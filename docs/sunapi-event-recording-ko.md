# SUNAPI event recording 설계

## 근거 문서

원문은 workspace의 `SUNAPI_Complete_Guide_2.6.8.pdf`다.

| 내용 | SUNAPI 절 | PDF page |
| --- | --- | ---: |
| motion sample | 158.20 Samples / MotionDetection | 1635 |
| event status | 161.1 Event Status | 1934 |
| 지원 channel event | EventType list | 1939 |
| status response | example | 1948 |
| monitor response | monitor/monitordiff | 1953 |
| schema response | SchemaBased event | 1962 |
| VMS event session | 256.4 Event Session | 2463-2464 |

## 제품 구조

`probe`는 최종 기능명이 아니다. camera 연결을 소유하는 단위는 `ChannelIngest`다.

```text
ChannelIngest
  camera RTSP connection 상시 유지
  RTP depayload와 codec parse 유지
  policy/event state가 recording branch 제어
  client session이 live branch 구독
  playback은 저장 media에서 별도 제공
```

motion이 시작된 뒤 camera에 연결하면 pre-event frame을 잃고 다음 keyframe까지 기다릴
수 있으므로 event recording에서도 upstream ingest는 유지한다.

## SUNAPI event source

```text
/stw-cgi/eventstatus.cgi?msubmenu=eventstatus&action=check
/stw-cgi/eventstatus.cgi?msubmenu=eventstatus&action=monitor
/stw-cgi/eventstatus.cgi?msubmenu=eventstatus&action=monitordiff
```

| action | 의미 |
| --- | --- |
| `check` | 현재 상태 한 번 조회 |
| `monitor` | 주기 및 상태 변경 시 응답 |
| `monitordiff` | 최초 상태 후 변경분 응답 |

VMS event gate의 첫 목표는 `monitordiff`다. 최초 구현은 `check`로 인증과 parser를
검증한 뒤 long-lived `monitordiff` session으로 확장한다.

camera channel `0`은 VMS/DB/storage channel `1`에 mapping한다. 두 값을 계산으로
암묵 변환하지 않고 channel configuration에 명시한다.

```text
/stw-cgi/eventstatus.cgi?msubmenu=eventstatus&action=monitordiff&Channel.0.EventType=MotionDetection
```

신규 SUNAPI는 `SchemaBased=True`를 권장한다. 초기 parser는 다음 text response도
지원할 수 있다.

```text
Channel.0.MotionDetection=False
Channel.0.MotionDetection.RegionID.1=False
Channel.0.MotionDetection.RegionID.1.Level=0
```

motion level sample은 진단과 UI tuning에 사용하며 recording trigger로 사용하지 않는다.

```text
/stw-cgi/eventsources.cgi?msubmenu=samples&action=check&EventSourceType=MotionDetection&MaxSamples=5&Channel=0&Index=1
```

## Recording policy

```text
mode = continuous | event | disabled
pre_event_seconds = 5
post_event_seconds = 10
record_profile = record
live_profile = default | mobile
```

- `continuous`: parsed video를 계속 저장한다.
- `event`: ingest는 유지하고 motion state로 영구 저장 구간을 제어한다.
- `disabled`: live ingest는 가능하지만 recording은 끈다.

pre-event 구현의 장기 목표는 memory buffer다. 초기에는 구현이 단순한 rolling temporary
segment를 사용할 수 있지만, 실제 외장 storage가 mount됐을 때만 허용한다. SD카드에는
연속 임시 segment를 쓰지 않는다.

## Timeline과 pipeline

event state transition은 video segment와 별도 `vms_events` row로 저장한다.

```text
motion start/stop
event source와 이름
UTC timestamp
optional metadata JSON
```

media가 없으면 UI가 마지막 frame을 실제 녹화처럼 표시하지 않도록 `미녹화` 상태를
명확히 표시한다.

```text
rtspsrc -> depay -> parse -> tee
                            -> recording queue  (drop 금지)
                            -> live queue       (leaky 허용)
                            -> timing/metadata branch
```

queue는 branch backpressure를 격리할 뿐 VPN bandwidth나 CPU 부족을 해결하지 않는다.

## 구현 순서

1. Digest HTTP helper와 `SunapiEventClient` 골격
2. `eventstatus.cgi?action=check` parser
3. long-lived `action=monitordiff` session
4. `False -> True`를 `motion_start`, `True -> False`를 `motion_stop`으로 변환
5. `vms_events` 기록
6. `RecordingPolicy` state machine
7. event state와 GStreamer recording branch 연결
