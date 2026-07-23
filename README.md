# Raspberry Pi VMS

Hanwha Vision Network camera의 원본 H.264/H.265 stream을 Raspberry Pi에서 녹화하고, 이후 live fan-out·event recording·playback으로 확장하는 VMS 프로젝트입니다.

현재 구현은 하나의 프로세스가 최대 4개의 `ChannelIngest`를 소유하는 `ChannelManager`와
이를 공유하는 RTSP/RTSPS live server입니다. 각 카메라 RTSP ingest는 다른 채널과 live
client session에서 독립적으로 실행되며 오류/EOS 뒤 해당 채널만 재연결합니다.

```text
ChannelManager (one rpi_vms process)
  camera 0 -> VMS 1 -> recordings/ch1 + /ch1
  camera 1 -> VMS 2 -> recordings/ch2 + /ch2
  camera 2 -> VMS 3 -> recordings/ch3 + /ch3
  camera 3 -> VMS 4 -> recordings/ch4 + /ch4

각 ingest
  -> RTSP over TCP / Digest
  -> RTP depayload / codec parse
  -> tee
     -> non-leaky recording queue -> MP4 segment -> shared SQLite index
     -> leaky/latest live queue -> client별 bounded queue
```

## 채널과 경로 기준

```text
camera_channel=0..3  camera RTSP/SUNAPI namespace
channel_id=1..4      VMS/DB/client/storage namespace
```

기본 mapping은 `0:1,1:2,2:3,3:4`이며 `--channel-map CAMERA:VMS,...`로 명시합니다.
1채널 부하 기준선은 `--channel-map 0:1`로 실행합니다.

```text
/mnt/vms-storage/
  index/media.db
  recordings/ch1/ch1_*.mp4
  recordings/ch2/ch2_*.mp4
  recordings/ch3/ch3_*.mp4
  recordings/ch4/ch4_*.mp4
```

SQLite writer는 프로세스 내부 mutex로 직렬화하고 WAL/NORMAL 모드와 기본
`busy_timeout=5000ms`를 사용합니다. 다른 프로세스가 writer lock을 잡으면 timeout
범위에서 대기합니다. camera↔VMS mapping은 migration version 2의 `vms_channels`에
기록합니다.

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
인자와 `ps` 출력에 남기지 않도록 표준 입력으로 전달합니다. 제품 기본 live listener는
plain RTSP를 끄고 RTSPS 8554를 사용하므로, 최초 개발 장치에서는 인증서를 한 번
생성합니다.

```bash
bash tools/generate-dev-tls-cert.sh
read -rsp 'Camera password: ' CAMERA_PASSWORD
echo
printf '%s\n' "$CAMERA_PASSWORD" | ./build/rpi_vms \
  --camera-host CAMERA_IP \
  --camera-port 554 \
  --camera-user USER \
  --camera-password-stdin \
  --camera-path-template '/{camera_channel}/profile2/media.smp' \
  --channel-map '0:1,1:2,2:3,3:4' \
  --storage-root /mnt/vms-storage \
  --db-busy-timeout-ms 5000 \
  --codec h264 \
  --segment-seconds 60 \
  --reconnect-delay-ms 2000 \
  --require-storage-mount \
  --rtsp-port 0 \
  --rtsps-port 8554 \
  --tls-cert certs/server.crt \
  --tls-key certs/server.key \
  --log-level info
unset CAMERA_PASSWORD
```

현재 코드는 userinfo가 없는 camera URI를 내부에서 만들고 credential은 `rtspsrc`
속성으로 전달하여 Digest 인증을 처리합니다. recording/live 경로는 transcoding하지
않습니다. 기본 live URL은 `rtsps://PI_IP:8554/ch1`부터 `/ch4`까지입니다. event gate와
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

임시 인증서는 자동화된 client 시험에만 사용합니다. Windows Qt처럼 반복 접속하는
interactive 시험에는 실행마다 인증서가 바뀌지 않도록 Pi에 유지되는 인증서를 먼저
만듭니다. 스크립트는 기존 파일을 덮어쓰지 않습니다.

```bash
bash tools/generate-dev-tls-cert.sh
```

M3 legacy Qt smoke는 `/ch1`만 등록합니다.

```bash
bash tools/run-m3-qt-smoke.sh
```

## M4 4채널 통합 및 부하 시험

먼저 camera channel `0..3`이 모두 같은 profile 경로를 제공하는지 M4 통합 시험의
`state=streaming` 로그로 확인합니다. 서버는 plain RTSP를 끄고 RTSPS 8554만
활성화합니다. `/ch1..ch4` 각각에서 TLS handshake, RTSP control, RTP 수신을 확인하고
느린 TLS client와 invalid `/ch5`의 404도 검증합니다. 기존 DB와 영상 파일은 삭제하지
않고 새 segment를 추가합니다.

```bash
M4_APP=./build-m4/rpi_vms bash tools/test-m4-integration.sh
```

정상 종료 시 다음 핵심 판정이 출력됩니다.

```text
PASS: camera channels 0..3 are streaming as VMS channels 1..4
PASS: RTSPS listener is active and plain RTSP is disabled
PASS: /ch1..ch4 transmitted RTP over TLS
PASS: invalid RTSPS /ch5 route returned 404
PASS: ch0 was not created
PASS: M4 four-channel RTSPS integration test
```

1채널과 4채널의 CPU, RSS, ext4 mount block write, camera NIC RX/TX, ICMP packet loss는
동일한 측정 시간으로 순차 수집합니다. 기본 측정 시간은 각 120초이며 결과는
`/tmp/rpi-vms-m4-load/summary.csv`와 raw sample/log에 저장됩니다. 기본값은 채널마다
로컬 RTSPS client 하나를 연결해 TLS 암호화와 live fan-out 부하도 포함하고, summary에
수신 RTP byte를 기록합니다. ingest-only 기준이 필요할 때만
`M4_LOAD_RTSPS_CLIENTS=0`을 명시합니다.

```bash
M4_APP=./build-m4/rpi_vms \
M4_MEASURE_SECONDS=120 \
bash tools/measure-m4-load.sh
```

Windows Qt 4채널 smoke는 같은 persistent 인증서로 서버를 유지합니다. 출력된
`Qt Base URL`을 viewer에 적용하고 CH1~CH4가 모두 `Playing`인지 확인합니다.

```bash
M4_APP=./build-m4/rpi_vms bash tools/run-m4-qt-smoke.sh
```

`run-m4-qt-smoke.sh`는 인증서가 없을 때 임시 인증서를 만들지 않고
`generate-dev-tls-cert.sh` 실행을 요구합니다. 자동 통합시험의 임시 인증서와 실제 Qt
접속 인증서를 구분하기 위한 동작입니다.

통합 시험 실패 시 다음 로그를 수집합니다.

```bash
grep -E 'state=connecting|state=streaming|pipeline_error|reconnecting|worker_failed|queue_drop' \
  /tmp/rpi-vms-m4-integration.log
for log in /tmp/rpi-vms-m4-rtsps-*.log; do echo "=== $log ==="; tail -n 30 "$log"; done
sqlite3 -header -column /mnt/vms-storage/index/media.db \
  'SELECT channel_id, camera_channel, updated_at_utc FROM vms_channels ORDER BY channel_id;'
sqlite3 -header -column /mnt/vms-storage/index/media.db \
  'SELECT channel_id, complete, size_bytes, file_path FROM recording_segments ORDER BY id DESC LIMIT 12;'
findmnt -no TARGET,FSTYPE,SOURCE /mnt/vms-storage
journalctl -k --since '-10 min' | grep -Ei 'usb|uas|scsi|ext4|reset|error'
```
