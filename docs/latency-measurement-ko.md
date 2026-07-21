# VMS 지연 측정 기준

이 문서는 M9에서 사용하는 측정 기준이다. M1 clock offset 측정이 끝났다는 이유만으로
바로 실행하지 않는다. Qt viewer의 실제 재생 경로와 M3 VMS live server가 준비된 뒤
glass-to-glass 측정을 먼저 수행한다.

## 성능 목표와 현재 판정

live 영상의 end-to-end 목표는 다음과 같다.

```text
end_to_end = T7(Qt GUI render) - T0(camera encoder timestamp) <= 200ms
```

측정 보고에는 p50/p95/max를 모두 기록하고 각 값이 200ms 기준을 만족하는지 명시한다.
M1은 장치 시계 offset을 측정했을 뿐 T0와 T7을 계측하지 않았으므로 이
목표를 검증하지 못했다. 사용자가 육안으로 확인한 약 500ms도 정량 측정값은 아니지만,
실제값이 비슷하다면 200ms 목표의 약 2.5배이므로 이후 baseline 측정과 병목 분해가
필요하다.

다음 값은 단위가 모두 ms여도 서로 다른 지표다.

| 지표 | 이번 측정값 | 의미 | 200ms와 비교 |
| --- | ---: | --- | --- |
| `camera - Pi` clock offset | `-2422.600` | 카메라 시계가 Pi보다 약 2.423초 뒤처짐 | 비교 금지 |
| Pi -> 외부 NTP UDP RTT | 중앙값 `5.284` | 공통 NTP 서버 요청 왕복시간 | 비교 금지 |
| Pi NTP jitter | `2.199` | 과거 `systemd-timesyncd` peer jitter | 비교 금지 |
| SUNAPI HTTP RTT | 중앙값 `50.628` | date API 요청 왕복시간 | 비교 금지 |
| 영상 end-to-end | 미측정 | T0부터 T7까지 실제 영상 지연 | 목표 `<=200ms` |

## 시간 기준

시간대는 지연 계산과 무관하다. 내부 시각은 UTC로 저장하고 Qt 표시에서만
Asia/Seoul(UTC+9)로 변환한다.

두 종류의 clock을 구분한다.

```text
wall clock (UTC)
  장치 간 시각 비교, event/segment timeline, 로그 상관관계

monotonic clock
  한 장치 안의 함수/queue/decoder 소요시간
  NTP 보정이나 사용자의 시각 변경 영향을 받지 않음
```

카메라, Pi, Windows는 가능한 한 같은 NTP 서버를 사용한다. 동기화 주기를
애플리케이션이 직접 정하지 않고 chrony/Windows Time/camera NTP client가 계속
보정하게 한다. 운영 주기는 실제 offset과 drift를 관찰한 뒤 결정한다.

Hanwha camera의 현재 date/NTP 설정은 Digest 인증으로 읽는다.

```bash
curl --digest -u admin -H 'Accept: application/json' \
  'http://CAMERA_IP/stw-cgi/system.cgi?msubmenu=date&action=view'
```

비밀번호는 명령행에 넣지 않고 curl prompt에서 입력한다. 이 조회는 설정을 바꾸지
않는다. 응답에서 `NTPURLList`, `UTCTime`, `LocalTime`, `SyncType`와 모델별
`NTPStatus`, `NTPLastUpdatedTime`을 기록한다.

M1-A의 read-only 수집과 공통 NTP offset 측정은 다음 도구와 절차를 사용한다.

```text
tools/measure_time_offsets.py
docs/time-offset-measurement-ko.md
```

NTP 표본의 offset은 `reference - local`, 최종 장치 간 offset은
`left clock - right clock`으로 기록한다. SUNAPI `UTCTime`의 초 단위 해상도 때문에
카메라 상대 offset에는 기본 ±500ms 불확실성이 있다. SUNAPI 2.6.8은 요청 처리 중
`UTCTime`을 캡처하는 정확한 시점을 정의하지 않으므로, 도구가 출력하는 카메라
불확실성은 초 단위 양자화와 측정 RTT만 반영한 하한 추정치다.

## 실행 선행 조건

- Qt viewer가 camera direct, 기존 proxy, VMS live 경로를 재생할 수 있음
- 비교할 codec/profile/해상도/GOP 조건을 고정할 수 있음
- 동일 화면에서 실제 스톱워치와 Qt 영상을 함께 촬영할 수 있음

선행 조건이 없으면 수식이나 계측 코드부터 추가하지 않는다.

## 측정 구간

```text
T0 camera encoder timestamp
T1 RPi RTP receive
T2 RPi depay/parse output
T3 RPi live socket send
T4 Qt av_read_frame return
T5 Qt avcodec_receive_frame return
T6 Qt RGB conversion complete
T7 Qt GUI render
```

필요한 지표:

```text
camera_to_rpi       = T1 - T0
rpi_media_pipeline  = T3 - T1
network_to_qt       = T4 - T3
qt_decode           = T5 - T4
qt_convert          = T6 - T5
qt_gui_queue        = T7 - T6
end_to_end          = T7 - T0
```

T1..T3와 T4..T7은 각각 같은 장치의 monotonic clock으로 정확히 잴 수 있다.
서로 다른 장치의 값을 직접 빼려면 NTP offset 보정 또는 media timestamp 전달이
필요하다.

## RTP/RTCP 기준시각

RTP timestamp만으로는 UTC를 알 수 없다. RTCP Sender Report가 제공하는 RTP와
NTP의 대응 관계를 이용해야 한다.

GStreamer `rtspsrc`/`rtpjitterbuffer`는 RTCP 동기화 이후 reference timestamp
metadata를 붙일 수 있다. 이 기능은 설치된 GStreamer 버전과 카메라의 RTCP
Sender Report 제공 여부를 먼저 확인한다.

```text
rtspsrc ntp-sync=true add-reference-timestamp-meta=true
```

이 값은 camera encoder/media clock 기준에 가깝고 센서 노출 순간과 완전히 같은
시각이라고 단정하지 않는다. 최종 glass-to-glass 검증은 카메라가 밀리초 시계를
촬영하게 하고 Qt 화면과 비교하는 방법을 함께 사용한다.

## 현재 Qt 지표의 의미

현재 `StreamWorker`는 RGB 변환과 `QImage::copy()`가 끝난 뒤
`QDateTime::currentMSecsSinceEpoch()`를 `produced_ms`로 전달한다. `VideoPanel`의
`delay`는 GUI slot이 실행될 때까지의 차이다.

따라서 현재 값은 다음 하나만 나타낸다.

```text
current delay = Qt queued signal + GUI scheduling delay
```

카메라, 네트워크, FFmpeg demux, decoder 지연은 포함하지 않는다. 또한 wall clock을
사용하므로 Windows Time 보정 중 값이 튈 수 있다. 다음 구현에서는
`std::chrono::steady_clock` 또는 `QElapsedTimer` 기반 monotonic timestamp로 바꾼다.

## 비교 시나리오

동일 codec/profile/해상도/GOP에서 다음 경로를 각각 5분 이상 측정한다.

```text
A. Camera -> Qt
B. Camera -> existing RTSPS proxy -> Qt
C. Camera -> ChannelIngest/live server -> Qt
D. C + Tailscale
E. D + Mobile substream
```

각 시나리오에서 기록한다.

```text
latency p50/p95/max
decode/render FPS
dropped GUI frames
received bitrate
RPi app CPU/RSS
tailscaled CPU
USB write throughput
RTP loss/late/jitter
```

평균만 사용하지 않는다. live 영상은 순간적인 queue 증가가 체감 지연을 만들기
때문에 p95와 max, queue drop 횟수가 더 중요하다.

## 네트워크 RTT 해석

2026-07-17 M1-A 재측정의 `5.284ms`는 Pi에서 외부 Microsoft NTP peer
`52.231.114.183`로 보낸 UDP 요청의 RTT 중앙값이다. 과거 상태의 `2.199ms`는 내부망
RTT가 아니라 같은 NTP 상태 출력의 jitter다. 따라서 두 값을 “외부망 약 5ms, 내부망
약 2ms”의 직접 비교 근거로 사용하지 않는다.

2026-07-17 같은 Pi에서 같은 시점에 ICMP 10회로 다시 비교했다.

| 경로 | min/avg/max/mdev | packet loss |
| --- | --- | ---: |
| Pi -> camera `192.168.0.5` 내부 LAN | `0.209/0.347/0.719/0.152ms` | `0%` |
| Pi -> `52.231.114.183` 외부망 | `4.420/5.358/5.962/0.527ms` | `0%` |

내부 LAN 평균 RTT는 외부망보다 `5.011ms`, 약 93.5% 작았다. 따라서 내부망에서
network component가 줄어든다는 판단은 맞다. 다만 절대 절감량이 약 5ms이므로 육안
약 500ms의 주원인은 아니다. 카메라 encoder/GOP, RTSP jitter buffer, FFmpeg
demux/decode, GUI queue를 T0..T7로 나눠 측정해야 주된 병목을 판정할 수 있다.
