# Raspberry Pi VMS

This is the VMS-oriented working copy derived from the original RTSPS proxy.

The current milestone is intentionally small:

```text
Hanwha RTSP camera channel 0
-> GStreamer RTP depayload / H264 or H265 parse
-> 60-second MP4 segments
-> VMS channel 1 storage and SQLite recording_segments index
```

The previous proxy code is still present in the tree for reference, but the default
`app` target now builds the VMS recorder entry point in `src/main.cpp`.

Design notes:

```text
docs/current-status-ko.md
docs/sunapi-event-recording.md
docs/latency-measurement-ko.md
docs/vms-development-roadmap-ko.md
docs/new-task-prompt-ko.md
docs/git-ci-plan-ko.md
```

The next architecture step is not a separate `probe` mode. It is `ChannelIngest`:
the camera RTSP connection stays open, while recording/live branches are controlled
by policy, SUNAPI events, and client sessions.

Camera connection options intentionally keep the original proxy style:

```text
--camera-host
--camera-port
--camera-user
--camera-password
--camera-path-template
```

Channel numbering is explicit:

```text
--camera-channel 0..3  Hanwha RTSP/SUNAPI channel index
--channel-id 1..4      VMS, DB, client, directory, and filename channel ID
```

The default mapping therefore reads camera channel `0` and stores it under
`recordings/ch1` with `recording_segments.channel_id = 1`. The legacy
`{channel}` camera-path placeholder remains accepted, but new configuration
should use `{camera_channel}`.

The app builds the RTSP URL internally and passes it to GStreamer `rtspsrc`.
Digest authentication is handled by `rtspsrc`, not by embedding a custom RTSP
client loop in this milestone.

## Install Packages On Raspberry Pi

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  cmake \
  pkg-config \
  sqlite3 \
  libsqlite3-dev \
  libgstreamer1.0-dev \
  gstreamer1.0-tools \
  gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad
```

If the camera uses H.265 and parsing fails, check that the Raspberry Pi image has
the needed GStreamer plugin package installed.

## Build

```bash
make clean
make
```

Or with CMake:

```bash
cmake -S . -B build
cmake --build build -j
```

## Raspberry Pi Synchronization

Run the deployment sync from WSL. The Windows repository remains the source of
truth, and the Raspberry Pi remains the native build and hardware-test target.

```bash
cd /mnt/c/Users/shini/Documents/Codex/2026-07-10/rtsps-codex-hanwha-rtsp-raspberry-pi/outputs/rpi-vms

# Inspect the changes first.
bash tools/sync-to-pi.sh --dry-run

# Synchronize source and run the native Pi build.
bash tools/sync-to-pi.sh --build
```

The script excludes `.git`, `.github`, recordings, SQLite databases,
credentials, the local environment document, and build directories. Use clean
mode only when stale files in `/home/noc/rpi-vms` should be removed:

```bash
bash tools/sync-to-pi.sh --clean --build
```

Clean mode protects the existing Pi `app`, `.env`, `certs/`, and
`docs/current-environment-local-ko.md`.

The Makefile output is:

```text
./app
```

The CMake output is:

```text
build/rpi_vms
```

## Run

Prepare a writable storage root first:

```bash
sudo mkdir -p /mnt/vms-storage
sudo chown "$USER:$USER" /mnt/vms-storage
```

H.264 example:

```bash
./app \
  --camera-host CAMERA_IP \
  --camera-port 554 \
  --camera-user USER \
  --camera-password 'PASSWORD' \
  --camera-path-template '/{camera_channel}/profile2/media.smp' \
  --camera-channel 0 \
  --storage-root /mnt/vms-storage \
  --channel-id 1 \
  --codec h264 \
  --segment-seconds 60 \
  --log-level debug
```

H.265 example:

```bash
./app \
  --camera-host CAMERA_IP \
  --camera-port 554 \
  --camera-user USER \
  --camera-password 'PASSWORD' \
  --camera-path-template '/{camera_channel}/profile2/media.smp' \
  --camera-channel 0 \
  --storage-root /mnt/vms-storage \
  --channel-id 1 \
  --codec h265 \
  --segment-seconds 60 \
  --log-level debug
```

For real recording service operation, require a real mounted storage device:

```bash
./app \
  --camera-host CAMERA_IP \
  --camera-port 554 \
  --camera-user USER \
  --camera-password 'PASSWORD' \
  --camera-path-template '/{camera_channel}/profile2/media.smp' \
  --camera-channel 0 \
  --storage-root /mnt/vms-storage \
  --channel-id 1 \
  --codec h264 \
  --segment-seconds 60 \
  --require-storage-mount \
  --log-level info
```

Output layout:

```text
/mnt/vms-storage/
  index/
    media.db
  recordings/
    ch1/
      ch1_20260710T090512Z_00000.mp4
      ch1_20260710T090512Z_00001.mp4
```

Inspect the DB:

```bash
sqlite3 /mnt/vms-storage/index/media.db \
  'select id, channel_id, file_path, codec, complete, size_bytes from recording_segments;'
```

## Notes

- This code does not transcode.
- It uses `rtspsrc protocols=tcp`.
- `StorageManager` warns when `--storage-root` is not a mount point.
- Use `--require-storage-mount` to fail instead of warning.
- Real UUID mount management is a later step.
- Playback and live fan-out are not implemented yet.
