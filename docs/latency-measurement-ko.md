# VMS 지연 측정 기준

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
