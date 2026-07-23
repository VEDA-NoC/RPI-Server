#!/usr/bin/env bash
set -euo pipefail

app="${M4_APP:-./build/rpi_vms}"
client="${M4_RTSPS_CLIENT:-$(dirname -- "$app")/rtsps_test_client}"
camera_host="${M4_CAMERA_HOST:-192.168.0.5}"
storage_root="${M4_STORAGE_ROOT:-/mnt/vms-storage}"
tls_cert="${M4_TLS_CERT:-certs/server.crt}"
tls_key="${M4_TLS_KEY:-certs/server.key}"
run_seconds="${M4_RUN_SECONDS:-75}"
startup_timeout_ms="${M4_INGEST_STARTUP_TIMEOUT_MS:-45000}"
readiness_timeout_seconds="${M4_INGEST_READINESS_TIMEOUT_SECONDS:-150}"
channel_start_delay_ms="${M4_CHANNEL_START_DELAY_MS:-5000}"
startup_only="${M4_STARTUP_ONLY:-0}"
app_log="${M4_APP_LOG:-/tmp/rpi-vms-m4-integration.log}"

for command in openssl python3 sqlite3; do
    command -v "$command" >/dev/null || {
        echo "FAIL: required command not found: $command" >&2
        exit 1
    }
done
for executable in "$app" "$client"; do
    [[ -x "$executable" ]] || {
        echo "FAIL: executable is not available: $executable" >&2
        exit 1
    }
done
[[ "$startup_timeout_ms" =~ ^[1-9][0-9]*$ ]] || {
    echo 'FAIL: M4_INGEST_STARTUP_TIMEOUT_MS must be a positive integer' >&2
    exit 1
}
[[ "$readiness_timeout_seconds" =~ ^[1-9][0-9]*$ ]] || {
    echo 'FAIL: M4_INGEST_READINESS_TIMEOUT_SECONDS must be a positive integer' >&2
    exit 1
}
[[ "$channel_start_delay_ms" =~ ^[0-9]+$ ]] || {
    echo 'FAIL: M4_CHANNEL_START_DELAY_MS must be a non-negative integer' >&2
    exit 1
}
[[ "$startup_only" == 0 || "$startup_only" == 1 ]] || {
    echo 'FAIL: M4_STARTUP_ONLY must be 0 or 1' >&2
    exit 1
}
if pgrep -x rpi_vms >/dev/null; then
    echo 'FAIL: another rpi_vms process is already running' >&2
    pgrep -af rpi_vms >&2
    exit 1
fi

: >"$app_log"
temporary_tls_dir=''
if [[ ! -r "$tls_cert" || ! -r "$tls_key" ]]; then
    if [[ -e "$tls_cert" || -e "$tls_key" ]]; then
        echo 'FAIL: only one TLS file exists; refusing to replace it' >&2
        exit 1
    fi
    temporary_tls_dir="$(mktemp -d /tmp/rpi-vms-m4-tls.XXXXXX)"
    tls_cert="$temporary_tls_dir/server.crt"
    tls_key="$temporary_tls_dir/server.key"
    openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days 1 \
        -subj '/CN=localhost' \
        -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1' \
        -keyout "$tls_key" \
        -out "$tls_cert" \
        >/dev/null 2>&1
    chmod 600 "$tls_key"
    echo 'INFO: using a temporary self-signed certificate for this automated RTSPS test'
fi
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
client_labels=()
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
    if [[ -n "$temporary_tls_dir" ]]; then
        rm -f -- "$temporary_tls_dir/server.crt" "$temporary_tls_dir/server.key"
        rmdir -- "$temporary_tls_dir" 2>/dev/null || true
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
    --ingest-startup-timeout-ms "$startup_timeout_ms" \
    --channel-start-delay-ms "$channel_start_delay_ms" \
    --db-busy-timeout-ms 5000 \
    --require-storage-mount \
    --rtsp-port 0 \
    --rtsps-port 8554 \
    --tls-cert "$tls_cert" \
    --tls-key "$tls_key" \
    --live-client-queue-frames 3 \
    --log-level info \
    <&3 >"$app_log" 2>&1 &
app_pid=$!
exec 3<&-

echo "INFO: waiting up to ${readiness_timeout_seconds}s for all four camera ingest channels (first-buffer timeout ${startup_timeout_ms}ms, channel start delay ${channel_start_delay_ms}ms)"
previous_streaming_channels=-1
for elapsed_seconds in $(seq 0 $((readiness_timeout_seconds - 1))); do
    streaming_channels="$(sed -n \
        '/state=streaming first_buffer=yes/s/.*channel_id=\([1-4]\).*/\1/p' "$app_log" | sort -u | wc -l)"
    if [[ "$streaming_channels" -ne "$previous_streaming_channels" ]] || ((elapsed_seconds % 10 == 0)); then
        echo "INFO: ingest readiness elapsed=${elapsed_seconds}s streaming=${streaming_channels}/4"
        previous_streaming_channels="$streaming_channels"
    fi
    [[ "$streaming_channels" -eq 4 ]] && break
    kill -0 "$app_pid" 2>/dev/null || break
    sleep 1
done
if [[ "${streaming_channels:-0}" -ne 4 ]]; then
    if ! kill -0 "$app_pid" 2>/dev/null; then
        set +e
        wait "$app_pid"
        app_status=$?
        set -e
        app_pid=''
        echo "FAIL: app exited before all channels reached streaming, status=$app_status" >&2
        sed -n '1,160p' "$app_log" >&2
        exit 1
    fi
    echo "FAIL: expected four streaming channels, got ${streaming_channels:-0}" >&2
    grep -E 'state=connecting|first_buffer|pipeline_error|reconnecting|worker_failed' "$app_log" >&2 || true
    exit 1
fi
echo 'PASS: camera channels 0..3 are streaming as VMS channels 1..4'

if [[ "$startup_only" == 1 ]]; then
    initial_reconnects="$(grep -c 'state=reconnecting' "$app_log" || true)"
    kill -INT "$app_pid" 2>/dev/null || true
    set +e
    wait "$app_pid"
    app_status=$?
    set -e
    app_pid=''
    echo '=== startup log ==='
    grep -E 'channel-manager|state=connecting|state=streaming|pipeline_error|reconnecting|state=stopped' \
        "$app_log" || true
    if [[ "$app_status" -ne 0 ]]; then
        echo "FAIL: startup-only app exit status=$app_status" >&2
        exit 1
    fi
    if [[ "$initial_reconnects" -ne 0 ]]; then
        echo "FAIL: four channels became ready after initial reconnects=$initial_reconnects" >&2
        exit 1
    fi
    echo 'PASS: four channels became ready without an initial reconnect'
    exit 0
fi

(
    sleep "$run_seconds"
    kill -INT "$app_pid" 2>/dev/null || true
) &
watchdog_pid=$!

attempts_before="$(grep -c 'state=connecting' "$app_log")"
for channel_id in 1 2 3 4; do
    "$client" "rtsps://127.0.0.1:8554/ch${channel_id}" 20 \
        >"/tmp/rpi-vms-m4-rtsps-ch${channel_id}.log" 2>&1 &
    client_pids+=("$!")
    client_labels+=("ch${channel_id}")
done
python3 tools/test_slow_rtsp_client.py \
    rtsps://127.0.0.1:8554/ch1 --tls-insecure --pause-seconds 35 --receive-buffer 1024 \
    >/tmp/rpi-vms-m4-rtsps-slow-client.log 2>&1 &
client_pids+=("$!")
client_labels+=("slow-ch1")

echo 'INFO: running four 20s RTSPS clients and one 35s slow client'
for elapsed_seconds in 10 20 30 40; do
    sleep 10
    echo "INFO: RTSPS client phase elapsed=${elapsed_seconds}s/40s"
done
client_failures=0
for index in "${!client_pids[@]}"; do
    pid="${client_pids[$index]}"
    set +e
    wait "$pid"
    client_status=$?
    set -e
    if [[ "$client_status" -ne 0 ]]; then
        echo "FAIL: RTSPS client ${client_labels[$index]} exit status=$client_status"
        client_failures=$((client_failures + 1))
    fi
done
client_pids=()
client_labels=()
attempts_after="$(grep -c 'state=connecting' "$app_log")"

set +e
python3 tools/probe_rtsp_status.py \
    rtsps://127.0.0.1:8554/ch5 --tls-insecure --expect-status 404 \
    >/tmp/rpi-vms-m4-rtsps-invalid-route.log 2>&1
invalid_route_status=$?
set -e

kill -INT "$app_pid" 2>/dev/null || true
set +e
wait "$app_pid"
app_status=$?
set -e
app_pid=''
kill "$watchdog_pid" 2>/dev/null || true
wait "$watchdog_pid" 2>/dev/null || true
watchdog_pid=''

failures="$client_failures"
if grep -q 'state=listening transport=rtsps' "$app_log"; then
    echo 'PASS: RTSPS listener is active and plain RTSP is disabled'
else
    echo 'FAIL: RTSPS listener log is missing'
    failures=$((failures + 1))
fi
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
    echo 'PASS: /ch1..ch4 transmitted RTP over TLS'
else
    echo "FAIL: RTP observed on $first_rtp_channels of 4 routes"
    failures=$((failures + 1))
fi
for channel_id in 1 2 3 4; do
    client_log="/tmp/rpi-vms-m4-rtsps-ch${channel_id}.log"
    if grep -q '^TLS connected$' "$client_log" &&
        grep -Eq '^FINAL RTP packets: [1-9][0-9]*, bytes: [1-9][0-9]*' "$client_log"; then
        echo "PASS: RTSPS /ch${channel_id} completed TLS, RTSP control and RTP receive"
    else
        echo "FAIL: RTSPS /ch${channel_id} client evidence is incomplete"
        failures=$((failures + 1))
    fi
done
if grep -q '^PASS: RTSPS PLAY accepted' /tmp/rpi-vms-m4-rtsps-slow-client.log; then
    echo 'PASS: slow RTSPS client completed PLAY before pausing reads'
else
    echo 'FAIL: slow RTSPS client did not complete PLAY'
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

if [[ "$invalid_route_status" -eq 0 ]]; then
    echo 'PASS: invalid RTSPS /ch5 route returned 404'
else
    echo 'FAIL: invalid RTSPS /ch5 route did not return 404'
    sed -n '1,40p' /tmp/rpi-vms-m4-rtsps-invalid-route.log >&2
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
echo 'PASS: M4 four-channel RTSPS integration test'
