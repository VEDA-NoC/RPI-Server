#!/usr/bin/env bash
set -euo pipefail

app="${M3_APP:-./build/rpi_vms}"
camera_host="${M3_CAMERA_HOST:-192.168.0.5}"
storage_root="${M3_STORAGE_ROOT:-/mnt/vms-storage}"
run_seconds="${M3_RUN_SECONDS:-75}"
client_seconds="${M3_CLIENT_SECONDS:-20}"
app_log="${M3_APP_LOG:-/tmp/rpi-vms-m3-integration.log}"
client_log="${M3_CLIENT_LOG:-/tmp/rpi-vms-m3-client.log}"

if [[ ! -x "$app" ]]; then
    echo "FAIL: app is not executable: $app" >&2
    exit 1
fi
if pgrep -x rpi_vms >/dev/null; then
    echo "FAIL: another rpi_vms process is already running" >&2
    pgrep -af rpi_vms >&2
    exit 1
fi

: >"$app_log"
: >"$client_log"

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
cleanup() {
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
    sleep "$run_seconds"
    kill -INT "$app_pid" 2>/dev/null || true
) &
watchdog_pid=$!

attempts_before="$(grep -c 'state=connecting' "$app_log")"

gst-launch-1.0 -q \
    rtspsrc location=rtsp://127.0.0.1:8554/ch1 protocols=tcp latency=100 \
    ! rtph264depay \
    ! h264parse \
    ! fakesink sync=false \
    >"$client_log" 2>&1 &
client_pid=$!
sleep "$client_seconds"
kill -INT "$client_pid" 2>/dev/null || true
wait "$client_pid" 2>/dev/null || true

attempts_after="$(grep -c 'state=connecting' "$app_log")"

set +e
wait "$app_pid"
app_status=$?
set -e
app_pid=''
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
    echo "PASS: live client reused ingest attempts=$attempts_after"
else
    echo "FAIL: ingest attempts changed before=$attempts_before after=$attempts_after"
    failures=$((failures + 1))
fi

if grep -q 'state=first_rtp' "$app_log"; then
    echo 'PASS: first RTP transmitted'
else
    echo 'FAIL: no RTP was transmitted'
    failures=$((failures + 1))
fi

mapfile -t opened_files < <(grep 'opened segment:' "$app_log" | sed 's/^.*opened segment: //')
if [[ "${#opened_files[@]}" -ge 2 ]]; then
    echo 'PASS: recording crossed a 60-second segment boundary'
    printf '  %s\n' "${opened_files[@]}"
else
    echo "FAIL: expected at least 2 opened segments, got ${#opened_files[@]}"
    failures=$((failures + 1))
fi

if [[ -e "$storage_root/recordings/ch0" ]]; then
    echo 'FAIL: forbidden ch0 directory exists'
    failures=$((failures + 1))
else
    echo 'PASS: ch0 was not created'
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

echo '=== session log ==='
grep -E 'state=session_created|state=play|state=first_rtp|state=session_closed' "$app_log" || true

echo '=== latest DB rows ==='
sqlite3 -header -column "$storage_root/index/media.db" "
SELECT id, complete, size_bytes, start_wall_time_utc, end_wall_time_utc, file_path
FROM recording_segments
WHERE channel_id = 1
ORDER BY id DESC
LIMIT 3;
"

if [[ "$failures" -ne 0 ]]; then
    echo "FAIL: M3 integration failures=$failures" >&2
    exit 1
fi
echo 'PASS: M3 bounded integration test'
