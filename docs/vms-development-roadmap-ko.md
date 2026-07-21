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

### M1. 장치 시간 기준선 측정 (완료)

1. 카메라, Raspberry Pi, Windows의 현재 시간 상태와 공통 NTP 기준 offset을
   read-only 도구로 수집한다.
2. NTP 요청 간격, offset 계산 부호, 측정 불확실성을 검증한다.
3. clock offset, NTP/ICMP/HTTP RTT와 영상 지연을 서로 다른 값으로 기록한다.
4. 저장/로그/protocol timestamp는 UTC, 같은 장치 내부 지연은 monotonic clock을
   사용한다는 기준을 확정한다.

완료 조건:

- camera/Pi/Windows의 clock offset을 같은 NTP peer 기준으로 확인할 수 있다.
- 실제 측정 결과와 해석 한계를 문서에 기록한다.
- offset 값을 영상 end-to-end 지연으로 사용하지 않는다.

카메라 NTP 설정 변경과 영상 end-to-end 측정은 M1 완료 조건이 아니다.

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

### M9. Qt viewer 연동과 end-to-end 지연 검증

선행 조건:

- Qt viewer가 camera direct, 기존 proxy, VMS live 경로를 실제로 재생할 수 있다.
- M3 live server가 client session과 독립된 ingest를 제공한다.
- Pi와 Qt 내부 구간에 monotonic timestamp를 기록할 수 있다.

1. 카메라가 밀리초 스톱워치 또는 시계를 촬영하도록 한다.
2. 실제 화면과 Qt viewer를 한 프레임에 담는 glass-to-glass 방식으로 측정한다.
3. camera direct, 기존 proxy, VMS live 경로의 p50/p95/max와 frame drop을 비교한다.
4. live 영상 end-to-end 기준 `200ms 이하` 충족 여부를 판정한다.
5. 필요할 때만 T0..T7 구간 계측으로 병목을 세분화한다.

## 바로 다음 작업

1. M2에서 단일 채널 `ChannelIngest`로 recorder 구조를 바꾼다.
2. M3에서 client session과 독립된 live server 경로를 만든다.
3. Qt 연동과 end-to-end 측정은 선행 경로가 준비된 뒤 M9에서 수행한다.
4. camera NTP/SyncType 설정은 필요한 기능과 영향 범위를 정한 별도 task에서 다룬다.
