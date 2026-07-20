# Raspberry Pi VMS

Hanwha RTSP camera의 원본 H.264/H.265 stream을 Raspberry Pi에서 녹화하고, 이후
live fan-out·event recording·playback으로 확장하는 VMS 프로젝트다.

현재 구현은 단일 채널 연속 녹화 PoC다.

```text
camera channel 0
  -> RTSP over TCP / Digest
  -> RTP depayload / codec parse
  -> MP4 segment
  -> VMS channel 1 storage + SQLite index
```

현재 상태와 다음 작업은 다음 두 문서를 기준으로 한다.

- `docs/current-status-ko.md`
- `docs/vms-development-roadmap-ko.md`

세부 설계 문서는 필요한 작업에서만 읽는다.

- 시간·지연: `docs/time-offset-measurement-ko.md`, `docs/latency-measurement-ko.md`
- SUNAPI event: `docs/sunapi-event-recording-ko.md`
- Git·CI: `docs/git-ci-plan-ko.md`

## 채널과 경로 기준

```text
camera_channel=0..3  camera RTSP/SUNAPI namespace
channel_id=1..4      VMS/DB/client/storage namespace
```

기본 mapping은 camera channel `0`을 VMS channel `1`로 저장한다.

```text
/mnt/vms-storage/
  index/media.db
  recordings/ch1/ch1_*.mp4
```

## Raspberry Pi package

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake pkg-config sqlite3 libsqlite3-dev \
  libgstreamer1.0-dev gstreamer1.0-tools \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad
```

## Build

활성 저장소 루트에서 실행한다.

```bash
make clean
make
```

또는:

```bash
cmake -S . -B build
cmake --build build -j
```

Makefile output은 `./app`, CMake output은 `build/rpi_vms`다.

## Raspberry Pi 반영

Windows의 활성 worktree를 source of truth로 사용하고 WSL2에서 실행한다.

```bash
bash tools/sync-to-pi.sh --dry-run
bash tools/sync-to-pi.sh --build
```

도구는 `.git`, `.github`, build/cache, recording, SQLite DB, credential, local 환경
문서를 Pi 전송에서 제외한다. `--clean`은 stale file 삭제가 필요한 경우에만 사용한다.

## 시간 offset 측정

M1-A 도구는 camera 설정을 변경하지 않고 Windows/Pi/camera wall clock 차이를
측정한다. 이 값은 영상 end-to-end latency가 아니다.

```powershell
py -3 tools/measure_time_offsets.py `
  --camera-host CAMERA_IP `
  --camera-user admin `
  --pi-target noc@noc `
  --ntp-server COMMON_NTP_SERVER_OR_IP `
  --samples 7 `
  --ntp-interval 15
```

SUNAPI password는 prompt에서 입력하며 명령행과 결과 파일에 저장하지 않는다.

## 실행 예시

실제 운영 녹화는 외장 ext4 mount를 필수로 확인한다. password는 예시처럼 직접
명령행에 고정하지 말고 배치별 보호된 입력 방식을 사용한다.

```bash
./app \
  --camera-host CAMERA_IP \
  --camera-port 554 \
  --camera-user USER \
  --camera-password "$CAMERA_PASSWORD" \
  --camera-path-template '/{camera_channel}/profile2/media.smp' \
  --camera-channel 0 \
  --storage-root /mnt/vms-storage \
  --channel-id 1 \
  --codec h264 \
  --segment-seconds 60 \
  --require-storage-mount \
  --log-level info
```

현재 코드는 camera URI를 내부에서 만들고 GStreamer `rtspsrc`가 Digest 인증을
처리한다. recording/live 경로는 transcoding하지 않는다. `ChannelIngest`, shared
live server, event gate, playback은 로드맵의 후속 구현이다.
