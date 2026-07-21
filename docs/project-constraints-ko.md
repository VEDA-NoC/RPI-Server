# RPi VMS 고정 설계 기준

## 식별자와 시간

- camera RTSP/SUNAPI 채널은 `camera_channel=0..3`이다.
- VMS, DB, Qt, 저장 경로 채널은 `channel_id=1..4`다.
- DB, 로그, protocol timestamp는 UTC로 저장하고 UI에서만 KST로 변환한다.
- 같은 장치 내부 지연은 monotonic clock으로 측정한다.
- NTP offset, network RTT, 영상 end-to-end 지연을 같은 값으로 취급하지 않는다.

## ingest와 저장

- camera ingest는 client session과 독립적으로 유지한다.
- recording/live 경로에서는 transcoding을 기본으로 하지 않는다.
- recording 경로는 frame을 drop하지 않는다.
- live 경로는 latency 제한을 위해 leaky queue를 사용할 수 있다.
- 운영 녹화는 ext4 외장 USB mount를 요구하며 SD카드로 우회하지 않는다.

## 구현과 보안

- 기존 RTSPS proxy의 검증된 TLS, RTSP parser, Digest, logging 코드를 재사용한다.
- password, TLS private key, token을 source, 문서, unit에 hard-code하지 않는다.
- DB schema 변경은 migration으로 처리하고 확인되지 않은 DB·영상을 삭제하지 않는다.
- 실제 camera, Raspberry Pi, USB, network 검증은 x86 CI로 대체하지 않는다.

## 소스 경계

- RPi VMS source는 workspace의 `agent-rules/active-worktree-ko.md`에서 결정한다.
- Raspberry Pi 배치 경로는 `~/rpi-vms`다.
- Qt viewer source는 `C:\Users\shini\Documents\QtProjects\qt_4ch_viewer`다.
- Hanwha 원문은 workspace의 `SUNAPI_Complete_Guide_2.6.8.pdf`다.
