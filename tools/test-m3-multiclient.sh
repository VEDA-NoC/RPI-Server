#!/usr/bin/env bash
set -euo pipefail

app="${M3_APP:-./build/rpi_vms}"
camera_host="${M3_CAMERA_HOST:-192.168.0.5}"
storage_root="${M3_STORAGE_ROOT:-/mnt/vms-storage}"
app_log="${M3_APP_LOG:-/tmp/rpi-vms-m3-multiclient.log}"
slow_log="${M3_SLOW_LOG:-/tmp/rpi-vms-m3-slow-client.log}"
fast_log_prefix="${M3_FAST_LOG_PREFIX:-/tmp/rpi-vms-m3-fast-client}"

if [[ ! -x "$app" ]]; then
    echo "FAIL: app is not executable: $app" >&2
    exit 1
fi
for command in python3 gst-launch-1.0 sqlite3; do
    if ! command -v "$command" >/dev/null; then
        echo "FAIL: required command not found: $command" >&2
        exit 1
    fi
done
if pgrep -x rpi_vms >/dev/null; then
    echo 'FAIL: another rpi_vms process is already running' >&2
    pgrep -af rpi_vms >&2
    exit 1
fi

: >"$app_log"
: >"$slow_log"
: >"${fast_log_prefix}-1.log"
: >"${fast_log_prefix}-2.log"

read -rsp 'Camera password: ' camera_password
echo
if [[ -z "$camera_password" ]]; then
    echo 'FAIL: empty camera password' >&2
    exit 1
fi
exec 3<<<"$camera_password"
unset camera_password

app_pid=''
watchdog_pid=''
slow_pid=''
fast_pids=()
cleanup() {
    for pid in "${fast_pids[@]}" "$slow_pid"; do
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            kill -INT "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    if [[ -n "$watchdog_pid" ]]; then
        kill "$watchdog_pid" 2>/dev/null || true
    fi
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
    --camera-channel 0 \
    --channel-id 1 \
    --codec h264 \
    --storage-root "$storage_root" \
    --segment-seconds 60 \
    --reconnect-delay-ms 2000 \
    --ingest-startup-timeout-ms 15000 \
    --require-storage-mount \
    --rtsp-port 8554 \
    --rtsps-port 0 \
    --live-client-queue-frames 3 \
    --live-client-queue-bytes 8388608 \
    --log-level info \
    <&3 >"$app_log" 2>&1 &
app_pid=$!
exec 3<&-

for _ in $(seq 1 45); do
    if grep -q 'state=streaming first_buffer=yes' "$app_log"; then
        break
    fi
    if ! kill -0 "$app_pid" 2>/dev/null; then
        break
    fi
    sleep 1
done
if ! grep -q 'state=streaming first_buffer=yes' "$app_log"; then
    echo 'FAIL: ingest did not reach streaming' >&2
    grep -E 'state=connecting|pipeline_error|reconnecting|state=stopped' "$app_log" >&2 || true
    exit 1
fi
echo 'PASS: ingest streaming'

(
    sleep 50
    kill -INT "$app_pid" 2>/dev/null || true
) &
watchdog_pid=$!

attempts_before="$(grep -c 'state=connecting' "$app_log")"

python3 tools/test_slow_rtsp_client.py \
    rtsp://127.0.0.1:8554/ch1 \
    --pause-seconds 35 \
    --receive-buffer 1024 \
    >"$slow_log" 2>&1 &
slow_pid=$!
sleep 2

for client_number in 1 2; do
    gst-launch-1.0 -q \
        rtspsrc location=rtsp://127.0.0.1:8554/ch1 protocols=tcp latency=100 \
        ! rtph264depay \
        ! h264parse \
        ! fakesink sync=false \
        >"${fast_log_prefix}-${client_number}.log" 2>&1 &
    fast_pids+=("$!")
done

sleep 25
for pid in "${fast_pids[@]}"; do
    kill -INT "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
done
fast_pids=()

set +e
wait "$slow_pid"
slow_status=$?
set -e
slow_pid=''

kill -INT "$app_pid" 2>/dev/null || true
set +e
wait "$app_pid"
app_status=$?
set -e
app_pid=''
kill "$watchdog_pid" 2>/dev/null || true
wait "$watchdog_pid" 2>/dev/null || true
watchdog_pid=''

attempts_after="$(grep -c 'state=connecting' "$app_log")"
sessions_created="$(grep -c 'state=session_created' "$app_log")"
first_rtp="$(grep -c 'state=first_rtp' "$app_log")"
failures=0

if [[ "$app_status" -eq 0 ]]; then
    echo 'PASS: app exited cleanly'
else
    echo "FAIL: app exit status=$app_status"
    failures=$((failures + 1))
fi
if [[ "$slow_status" -eq 0 ]]; then
    echo 'PASS: slow client completed its no-read interval'
else
    echo "FAIL: slow client exit status=$slow_status"
    sed -n '1,80p' "$slow_log"
    failures=$((failures + 1))
fi
if [[ "$sessions_created" -ge 3 && "$first_rtp" -ge 3 ]]; then
    echo "PASS: concurrent clients received RTP sessions=$sessions_created first_rtp=$first_rtp"
else
    echo "FAIL: expected >=3 sessions and first RTP events, got sessions=$sessions_created first_rtp=$first_rtp"
    failures=$((failures + 1))
fi
if [[ "$attempts_before" -eq "$attempts_after" ]]; then
    echo "PASS: all clients reused ingest attempts=$attempts_after"
else
    echo "FAIL: ingest attempts changed before=$attempts_before after=$attempts_after"
    failures=$((failures + 1))
fi
if grep -Eq 'state=queue_drop|queue_drops=[1-9][0-9]*' "$app_log"; then
    echo 'PASS: slow-client backpressure was isolated by bounded queue drops'
else
    echo 'FAIL: slow client did not produce a recorded queue drop'
    failures=$((failures + 1))
fi
if grep -Eq 'pipeline_error|state=reconnecting' "$app_log"; then
    echo 'FAIL: ingest pipeline failed or reconnected during client load'
    failures=$((failures + 1))
else
    echo 'PASS: ingest remained streaming during client load'
fi
if grep -q 'closed segment:' "$app_log"; then
    echo 'PASS: recording segment closed cleanly'
else
    echo 'FAIL: no recording segment was closed'
    failures=$((failures + 1))
fi
IFS='|' read -r db_complete db_size < <(sqlite3 "$storage_root/index/media.db" "
SELECT complete, size_bytes
FROM recording_segments
WHERE channel_id = 1
ORDER BY id DESC
LIMIT 1;
")
if [[ "$db_complete" == '1' && "$db_size" =~ ^[1-9][0-9]*$ ]]; then
    echo "PASS: latest DB segment is complete size_bytes=$db_size"
else
    echo "FAIL: latest DB segment complete=${db_complete:-missing} size_bytes=${db_size:-missing}"
    failures=$((failures + 1))
fi
if [[ -e "$storage_root/recordings/ch0" ]]; then
    echo 'FAIL: forbidden ch0 directory exists'
    failures=$((failures + 1))
else
    echo 'PASS: ch0 was not created'
fi

echo '=== session and backpressure log ==='
grep -E 'state=session_created|state=play|state=first_rtp|state=queue_drop|state=write_failure_disconnect|state=session_closed' \
    "$app_log" || true
echo '=== latest DB row ==='
sqlite3 -header -column "$storage_root/index/media.db" "
SELECT id, complete, size_bytes, start_wall_time_utc, end_wall_time_utc, file_path
FROM recording_segments
WHERE channel_id = 1
ORDER BY id DESC
LIMIT 1;
"

if [[ "$failures" -ne 0 ]]; then
    echo "FAIL: M3 multi-client failures=$failures" >&2
    exit 1
fi
echo 'PASS: M3 multi-client and slow-client test'
