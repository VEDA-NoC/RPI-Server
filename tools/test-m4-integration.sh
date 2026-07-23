#!/usr/bin/env bash
set -euo pipefail

app="${M4_APP:-./build/rpi_vms}"
camera_host="${M4_CAMERA_HOST:-192.168.0.5}"
storage_root="${M4_STORAGE_ROOT:-/mnt/vms-storage}"
run_seconds="${M4_RUN_SECONDS:-75}"
app_log="${M4_APP_LOG:-/tmp/rpi-vms-m4-integration.log}"

for command in gst-launch-1.0 python3 sqlite3; do
    command -v "$command" >/dev/null || {
        echo "FAIL: required command not found: $command" >&2
        exit 1
    }
done
[[ -x "$app" ]] || {
    echo "FAIL: app is not executable: $app" >&2
    exit 1
}
if pgrep -x rpi_vms >/dev/null; then
    echo 'FAIL: another rpi_vms process is already running' >&2
    pgrep -af rpi_vms >&2
    exit 1
fi

: >"$app_log"
read -rsp 'Camera password: ' camera_password
echo
[[ -n "$camera_password" ]] || {
    echo 'FAIL: empty camera password' >&2
    exit 1
}
exec 3<<<"$camera_password"
unset camera_password

app_pid=''
watchdog_pid=''
client_pids=()
cleanup() {
    for pid in "${client_pids[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill -INT "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    [[ -n "$watchdog_pid" ]] && kill "$watchdog_pid" 2>/dev/null || true
    if [[ -n "$app_pid" ]] && kill -0 "$app_pid" 2>/dev/null; then
        kill -INT "$app_pid" 2>/dev/null || true
        wait "$app_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

"$app" \
    --camera-host "$camera_host" \
    --camera-port 554 \
    --camera-user admin \
    --camera-password-stdin \
    --camera-path-template '/{camera_channel}/profile2/media.smp' \
    --channel-map '0:1,1:2,2:3,3:4' \
    --storage-root "$storage_root" \
    --codec h264 \
    --segment-seconds 60 \
    --db-busy-timeout-ms 5000 \
    --require-storage-mount \
    --rtsp-port 8554 \
    --rtsps-port 0 \
    --live-client-queue-frames 3 \
    --log-level info \
    <&3 >"$app_log" 2>&1 &
app_pid=$!
exec 3<&-

for _ in $(seq 1 60); do
    streaming_channels="$(sed -n \
        '/state=streaming first_buffer=yes/s/.*channel_id=\([1-4]\).*/\1/p' "$app_log" | sort -u | wc -l)"
    [[ "$streaming_channels" -eq 4 ]] && break
    kill -0 "$app_pid" 2>/dev/null || break
    sleep 1
done
if [[ "${streaming_channels:-0}" -ne 4 ]]; then
    echo "FAIL: expected four streaming channels, got ${streaming_channels:-0}" >&2
    grep -E 'state=connecting|first_buffer|pipeline_error|reconnecting|worker_failed' "$app_log" >&2 || true
    exit 1
fi
echo 'PASS: camera channels 0..3 are streaming as VMS channels 1..4'

(
    sleep "$run_seconds"
    kill -INT "$app_pid" 2>/dev/null || true
) &
watchdog_pid=$!

attempts_before="$(grep -c 'state=connecting' "$app_log")"
for channel_id in 1 2 3 4; do
    gst-launch-1.0 -q \
        rtspsrc location="rtsp://127.0.0.1:8554/ch${channel_id}" protocols=tcp latency=100 \
        ! rtph264depay ! h264parse ! fakesink sync=false \
        >"/tmp/rpi-vms-m4-ch${channel_id}.log" 2>&1 &
    client_pids+=("$!")
done
python3 tools/test_slow_rtsp_client.py \
    rtsp://127.0.0.1:8554/ch1 --pause-seconds 35 --receive-buffer 1024 \
    >/tmp/rpi-vms-m4-slow-client.log 2>&1 &
client_pids+=("$!")

sleep 40
for pid in "${client_pids[@]}"; do
    kill -INT "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
done
client_pids=()
attempts_after="$(grep -c 'state=connecting' "$app_log")"

invalid_status="$(python3 - <<'PY'
import socket
s = socket.create_connection(("127.0.0.1", 8554), timeout=2)
s.sendall(b"DESCRIBE rtsp://127.0.0.1:8554/ch5 RTSP/1.0\r\nCSeq: 1\r\n\r\n")
print(s.recv(256).split(b"\r\n", 1)[0].decode())
PY
)" || true

kill -INT "$app_pid" 2>/dev/null || true
set +e
wait "$app_pid"
app_status=$?
set -e
app_pid=''
kill "$watchdog_pid" 2>/dev/null || true
wait "$watchdog_pid" 2>/dev/null || true
watchdog_pid=''

failures=0
if [[ "$app_status" -eq 0 ]]; then
    echo 'PASS: app exited cleanly'
else
    echo "FAIL: app exit status=$app_status"
    failures=$((failures + 1))
fi
if [[ "$attempts_before" -eq "$attempts_after" ]]; then
    echo "PASS: live clients did not create or reconnect camera ingest attempts=$attempts_after"
else
    echo "FAIL: ingest attempts changed before=$attempts_before after=$attempts_after"
    failures=$((failures + 1))
fi

first_rtp_channels="$(
    sed -n '/state=first_rtp channel_id=/s/.*channel_id=\([1-4]\).*/\1/p' "$app_log" | sort -u | wc -l
)"
if [[ "$first_rtp_channels" -eq 4 ]]; then
    echo 'PASS: /ch1..ch4 transmitted RTP'
else
    echo "FAIL: RTP observed on $first_rtp_channels of 4 routes"
    failures=$((failures + 1))
fi

if grep -Eq 'state=queue_drop|queue_drops=[1-9][0-9]*' "$app_log"; then
    echo 'PASS: slow client exercised bounded drop-oldest queue'
else
    echo 'FAIL: slow client did not exercise bounded queue policy'
    failures=$((failures + 1))
fi

for channel_id in 1 2 3 4; do
    output_dir="$storage_root/recordings/ch${channel_id}"
    if [[ -d "$output_dir" ]]; then
        echo "PASS: recording directory exists: $output_dir"
    else
        echo "FAIL: recording directory missing: $output_dir"
        failures=$((failures + 1))
    fi
    IFS='|' read -r complete size_bytes file_path <<<"$(sqlite3 "$storage_root/index/media.db" "
SELECT complete, size_bytes, file_path
FROM recording_segments
WHERE channel_id = $channel_id
ORDER BY id DESC
LIMIT 1;
")"
    if [[ "$complete" == 1 && "$size_bytes" =~ ^[1-9][0-9]*$ && "$file_path" == "$output_dir/"* ]]; then
        echo "PASS: channel_id=$channel_id latest segment complete size_bytes=$size_bytes"
    else
        echo "FAIL: channel_id=$channel_id complete=${complete:-missing} size=${size_bytes:-missing} path=${file_path:-missing}"
        failures=$((failures + 1))
    fi
done

if [[ -e "$storage_root/recordings/ch0" ]]; then
    echo 'FAIL: forbidden ch0 directory exists'
    failures=$((failures + 1))
else
    echo 'PASS: ch0 was not created'
fi

if [[ "$invalid_status" == *'404'* ]]; then
    echo 'PASS: invalid /ch5 route returned 404'
else
    echo "FAIL: invalid /ch5 route did not return 404 (${invalid_status:-no response})"
    failures=$((failures + 1))
fi

echo '=== channel and client log ==='
grep -E 'state=streaming|state=reconnecting|state=session_created|state=first_rtp|state=queue_drop|state=stopped' \
    "$app_log" || true
echo '=== latest DB rows ==='
sqlite3 -header -column "$storage_root/index/media.db" "
SELECT channel_id, complete, size_bytes, start_wall_time_utc, end_wall_time_utc, file_path
FROM recording_segments
WHERE channel_id BETWEEN 1 AND 4
ORDER BY id DESC
LIMIT 8;
"

if [[ "$failures" -ne 0 ]]; then
    echo "FAIL: M4 integration failures=$failures" >&2
    exit 1
fi
echo 'PASS: M4 four-channel integration test'
