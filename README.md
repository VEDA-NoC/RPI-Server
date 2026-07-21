# Raspberry Pi VMS

Hanwha Vision Network camera의 원본 H.264/H.265 stream을 Raspberry Pi에서 녹화하고, 이후 live fan-out·event recording·playback으로 확장하는 VMS 프로젝트입니다.

현재 구현은 단일 채널 연속 녹화 PoC입니다.

```text
camera channel 0
  -> RTSP over TCP / Digest
  -> RTP depayload / codec parse
  -> MP4 segment
  -> VMS channel 1 storage + SQLite index
```

## 채널과 경로 기준

```text
camera_channel=0..3  camera RTSP/SUNAPI namespace
channel_id=1..4      VMS/DB/client/storage namespace
```

기본 mapping은 camera channel `0`을 VMS channel `1`로 저장합니다.

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

활성 저장소 루트에서 실행합니다.

```bash
make clean
make
```

혹은:

```bash
cmake -S . -B build
cmake --build build -j
```

Makefile output은 `./app`, CMake output은 `build/rpi_vms`입니다.

## Raspberry Pi 반영

Windows의 활성 worktree를 source of truth로 사용하고 WSL2에서 실행합니다.

```bash
bash tools/sync-to-pi.sh --dry-run
bash tools/sync-to-pi.sh --build
```

도구는 `.git`, `.github`, build/cache, recording, SQLite DB, credential, local 환경 문서를 Pi 전송에서 제외합니다. `--clean`은 stale file 삭제가 필요한 경우에만 사용합니다.

## 실행 예시

실제 운영 녹화는 외장 ext4 mount를 필수로 확인합니다. password는 예시처럼 직접
명령행에 고정하지 말고 배치별 보호된 입력 방식을 사용합니다.

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
처리합니다. recording/live 경로는 transcoding하지 않습니다. `ChannelIngest`, shared live server, event gate, playback를 구현해야 합니다.
