# Raspberry Pi VMS 개발 로드맵

## 현재 완료 상태

- Hanwha RTSP H.264 입력과 Digest 인증 성공
- RTP depayload/parse 후 MP4 segment 저장 성공
- ext4 외장 저장장치 mount 검증 성공
- `--require-storage-mount`로 SD카드 오기록 방지
- SQLite WAL 기반 media index 생성
- 카메라 채널 `0..3`과 VMS 채널 `1..4` 분리

현재 구현은 단일 채널 연속 녹화 PoC다. 아직 VMS용 상시 `ChannelIngest`,
live fan-out, playback server, SUNAPI event gate는 구현되지 않았다.

## 구현 순서

### M1. 시간 동기화와 지연 계측

1. 카메라, Raspberry Pi, Windows의 현재 시간 상태와 공통 NTP 기준 offset을
   read-only 도구로 수집한다.
2. 수집 결과를 검토한 뒤 세 장치가 같은 NTP 기준을 사용하게 한다.
3. 저장/로그/프로토콜 시각은 UTC로 통일한다.
4. 같은 장치 내부 구간 측정에는 monotonic clock을 사용한다.
5. Qt에서 packet receive, decode complete, GUI render 시각을 분리한다.
6. 카메라 직접 연결, 기존 RTSPS proxy, 새 VMS live 경로를 같은 조건으로 비교한다.

성능 목표:

- live 영상 end-to-end `T7 - T0`는 `200ms 이하`를 기준으로 한다.
- p50/p95/max를 함께 기록하고 각 값의 기준 충족 여부를 판정한다.
- clock offset, NTP/ICMP/HTTP RTT, Qt GUI queue 지연을 end-to-end 지연으로 대신하지
  않는다.

완료 조건:

- clock offset을 확인할 수 있다.
- Qt의 `delay`가 GUI queue 지연과 end-to-end 지연으로 구분된다.
- 평균뿐 아니라 p50/p95/max와 frame drop 수를 기록한다.
- 실제 end-to-end 결과를 200ms 기준과 비교한다.

### M2. 단일 채널 ChannelIngest

1. RTSP 연결을 프로세스 실행 시 열고 계속 유지한다.
2. depay/parse 이후 `tee`로 recording/live 계통을 분리한다.
3. recording queue는 drop하지 않는다.
4. live queue는 leaky 정책으로 최신 frame을 유지한다.
5. 첫 buffer, RTCP sync, reconnect, packet loss 상태를 계측한다.

### M3. Live RTSP/RTSPS 서버

1. 기존 proxy의 TLS, RTSP parser, Digest 관련 코드를 재사용한다.
2. client 접속 시 카메라에 새로 연결하지 않고 `ChannelIngest`를 구독한다.
3. client session별 RTP/RTCP 상태와 송신 queue를 분리한다.
4. live branch가 느려져도 recording branch에 backpressure가 전파되지 않게 한다.

### M4. 4채널 ChannelManager

1. 하나의 프로세스에서 VMS 채널 1..4를 관리한다.
2. 카메라 채널 0..3 매핑을 설정/DB로 이동한다.
3. SQLite writer 경합 정책과 `busy_timeout`을 정의한다.
4. 1채널과 4채널의 CPU, RSS, USB write, network, packet loss를 비교한다.

현재 단일 채널 `app` 네 개를 같은 DB에 연결하는 방식은 임시 부하 시험 외에는
사용하지 않는다.

### M5. Record profile과 Live substream

1. recording은 Record profile을 유지한다.
2. remote live는 Default/Mobile profile을 비교한다.
3. 동일 ingest fan-out과 별도 substream ingest의 CPU/대역폭을 비교한다.
4. Tailscale 구간 bitrate, `tailscaled` CPU, Qt decode 부하를 함께 측정한다.

### M6. systemd와 운영 로그

1. UUID 기반 ext4 mount와 `nofail`을 확정한다.
2. storage 없음 상태에서도 control/session 서비스는 기동한다.
3. recording은 mount ready 이후에만 시작한다.
4. 기본 로그는 journald로 보내고 rotation/보존 정책을 설정한다.
5. RTSP reconnect와 비정상 종료 복구를 시험한다.

### M7. SUNAPI event recording

1. `eventstatus.cgi?action=monitordiff` session을 유지한다.
2. motion start/stop을 `vms_events`에 UTC로 기록한다.
3. pre/post event buffer 정책을 구현한다.
4. 녹화가 없는 구간과 motion이 없는 구간을 timeline에서 구분한다.

### M8. Playback, export, control plane

1. 시간 범위로 segment를 검색한다.
2. playback media session을 live와 분리한다.
3. MP4 segment 직접 제공과 무손실 remux export를 지원한다.
4. Qt control TLS 연결에서 login, channel status, timeline, playback 명령을 처리한다.

## 바로 다음 작업

1. M1-B에서 Qt `StreamWorker`와 GStreamer ingest의 단계별 monotonic timestamp
   구조를 설계한다.
2. camera direct / existing proxy의 영상 end-to-end p50/p95/max를 측정하고 200ms
   기준과 비교한다.
3. 구간별 병목을 확인한 뒤 Qt queue와 VMS live branch의 latency 정책을 결정한다.
4. camera NTP/SyncType 설정은 별도 승인 전까지 변경하지 않는다.
5. 그 다음 단일 채널 `ChannelIngest`로 recorder 구조를 바꾼다.
