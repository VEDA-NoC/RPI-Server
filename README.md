# Raspberry Pi VMS

Hanwha Vision Network camera의 원본 H.264/H.265 stream을 Raspberry Pi에서 녹화하고, 이후 live fan-out·event recording·playback으로 확장하는 VMS 프로젝트입니다.

현재 구현은 프로세스가 소유하는 단일 채널 `ChannelIngest`와 이를 공유하는 RTSP/RTSPS
live server입니다. 카메라 RTSP ingest는 live client session과 독립적으로 유지되며
오류/EOS 뒤 재연결합니다.

```text
camera channel 0
  -> RTSP over TCP / Digest
  -> RTP depayload / codec parse
  -> tee
     -> non-leaky recording queue -> MP4 segment -> SQLite index
     -> leaky/latest live queue -> RTSP/RTSPS client별 bounded queue
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
  libssl-dev python3 \
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

실제 운영 녹화는 외장 ext4 mount를 필수로 확인합니다. 카메라 password는 프로세스
인자와 `ps` 출력에 남기지 않도록 표준 입력으로 전달합니다.

```bash
read -rsp 'Camera password: ' CAMERA_PASSWORD
echo
printf '%s\n' "$CAMERA_PASSWORD" | ./build/rpi_vms \
  --camera-host CAMERA_IP \
  --camera-port 554 \
  --camera-user USER \
  --camera-password-stdin \
  --camera-path-template '/{camera_channel}/profile2/media.smp' \
  --camera-channel 0 \
  --storage-root /mnt/vms-storage \
  --channel-id 1 \
  --codec h264 \
  --segment-seconds 60 \
  --reconnect-delay-ms 2000 \
  --require-storage-mount \
  --rtsp-port 8554 \
  --rtsps-port 0 \
  --log-level info
unset CAMERA_PASSWORD
```

현재 코드는 userinfo가 없는 camera URI를 내부에서 만들고 credential은 `rtspsrc`
속성으로 전달하여 Digest 인증을 처리합니다. recording/live 경로는 transcoding하지
않습니다. VMS channel 1의 기본 live URL은 `rtsp://PI_IP:8554/ch1`입니다. event gate와
playback은 아직 구현하지 않았습니다.

## M3 통합 테스트

Pi에서 warning-as-error build와 단위 테스트를 먼저 실행합니다.

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_COMPILE_WARNING_AS_ERROR=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

카메라와 외장 저장장치를 사용하는 bounded 통합 테스트는 각각 자동으로 앱을
종료합니다. 첫 번째는 60초 segment 전환과 DB 완료 처리를, 두 번째는 빠른 client
2개와 읽기를 멈추는 client 1개의 동시 접속 및 backpressure 격리를 확인합니다.

```bash
bash tools/test-m3-integration.sh
bash tools/test-m3-multiclient.sh
bash tools/test-m3-rtsps.sh
```

RTSPS 시험은 Git에서 제외된 `certs/server.crt`와 `certs/server.key`가 있으면 이를
사용합니다. 파일이 없으면 `/tmp`에 유효기간 1일의 시험용 self-signed 인증서를 만들고
시험 종료 시 삭제합니다. 다른 기존 인증서를 사용할 때는 `M3_TLS_CERT`와
`M3_TLS_KEY`로 경로만 지정합니다.

Windows Qt viewer의 기본 재생 호환성을 확인할 때는 Pi에서 다음 스크립트를 실행한
상태로 유지합니다. 출력된 `Qt Base URL`을 viewer 설정에 적용하면 현재 M3 범위에서는
CH1만 재생되고 아직 등록되지 않은 CH2~CH4는 404를 반환합니다.

```bash
bash tools/run-m3-qt-smoke.sh
```
