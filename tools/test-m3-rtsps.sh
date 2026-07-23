#!/usr/bin/env bash
set -euo pipefail

app="${M3_APP:-./build/rpi_vms}"
client="${M3_RTSPS_CLIENT:-./build/rtsps_test_client}"
camera_host="${M3_CAMERA_HOST:-192.168.0.5}"
storage_root="${M3_STORAGE_ROOT:-/mnt/vms-storage}"
tls_cert="${M3_TLS_CERT:-certs/server.crt}"
tls_key="${M3_TLS_KEY:-certs/server.key}"
app_log="${M3_APP_LOG:-/tmp/rpi-vms-m3-rtsps.log}"
client_log="${M3_CLIENT_LOG:-/tmp/rpi-vms-m3-rtsps-client.log}"

for executable in "$app" "$client"; do
    if [[ ! -x "$executable" ]]; then
        echo "FAIL: executable not found: $executable" >&2
        exit 1
    fi
done
for command in openssl timeout; do
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
: >"$client_log"

temporary_tls_dir=''
app_pid=''
stalled_client_pid=''
cleanup() {
    if [[ -n "$stalled_client_pid" ]] && kill -0 "$stalled_client_pid" 2>/dev/null; then
        kill "$stalled_client_pid" 2>/dev/null || true
        wait "$stalled_client_pid" 2>/dev/null || true
    fi
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

read -rsp 'Camera password: ' camera_password
echo
if [[ -z "$camera_password" ]]; then
    echo 'FAIL: empty camera password' >&2
    exit 1
fi
exec 3<<<"$camera_password"
unset camera_password

if [[ ! -r "$tls_cert" || ! -r "$tls_key" ]]; then
    temporary_tls_dir="$(mktemp -d /tmp/rpi-vms-m3-tls.XXXXXX)"
    tls_cert="$temporary_tls_dir/server.crt"
    tls_key="$temporary_tls_dir/server.key"
    openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days 1 \
        -subj '/CN=rpi-vms-m3-test' \
        -keyout "$tls_key" \
        -out "$tls_cert" \
        >/dev/null 2>&1
    chmod 600 "$tls_key"
    echo 'INFO: using a temporary self-signed certificate for this test'
fi

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
    --rtsp-port 0 \
    --rtsps-port 8554 \
    --tls-cert "$tls_cert" \
    --tls-key "$tls_key" \
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
    sed -n '1,120p' "$app_log" >&2
    exit 1
fi
echo 'PASS: ingest streaming'
attempts_before="$(grep -c 'state=connecting' "$app_log")"

timeout 3 bash -c 'exec 9<>/dev/tcp/127.0.0.1/8554; sleep 2' &
stalled_client_pid=$!
sleep 2
wait "$stalled_client_pid" 2>/dev/null || true
stalled_client_pid=''

set +e
"$client" rtsps://127.0.0.1:8554/ch1 10 >"$client_log" 2>&1
client_status=$?
set -e

kill -INT "$app_pid" 2>/dev/null || true
set +e
wait "$app_pid"
app_status=$?
set -e
app_pid=''
attempts_after="$(grep -c 'state=connecting' "$app_log")"
if [[ -n "$temporary_tls_dir" ]]; then
    rm -f -- "$temporary_tls_dir/server.crt" "$temporary_tls_dir/server.key"
    rmdir -- "$temporary_tls_dir" 2>/dev/null || true
    temporary_tls_dir=''
fi

failures=0
if [[ "$client_status" -eq 0 ]] && grep -q '^TLS connected$' "$client_log"; then
    echo 'PASS: TLS connected'
else
    echo "FAIL: RTSPS client exit status=$client_status"
    failures=$((failures + 1))
fi
if grep -Eq '^FINAL RTP packets: [1-9][0-9]*, bytes: [1-9][0-9]*' "$client_log"; then
    grep '^FINAL RTP packets:' "$client_log"
    echo 'PASS: RTP received over RTSPS'
else
    echo 'FAIL: no RTP received over RTSPS'
    failures=$((failures + 1))
fi
if grep -q 'state=listening transport=rtsps' "$app_log" && grep -q 'state=first_rtp' "$app_log"; then
    echo 'PASS: RTSPS listener served /ch1'
else
    echo 'FAIL: RTSPS listener or first RTP log missing'
    failures=$((failures + 1))
fi
if grep -q 'state=tls_handshake_failure' "$app_log"; then
    echo 'PASS: stalled TLS handshake timed out without blocking the listener'
else
    echo 'FAIL: stalled TLS handshake was not timed out'
    failures=$((failures + 1))
fi
if [[ "$app_status" -eq 0 ]]; then
    echo 'PASS: app exited cleanly'
else
    echo "FAIL: app exit status=$app_status"
    failures=$((failures + 1))
fi
if [[ "$attempts_before" -eq "$attempts_after" ]]; then
    echo "PASS: RTSPS clients reused ingest attempts=$attempts_after"
else
    echo "FAIL: ingest attempts changed during RTSPS test before=$attempts_before after=$attempts_after"
    failures=$((failures + 1))
fi

echo '=== RTSPS session log ==='
grep -E 'state=listening|state=session_created|state=play|state=first_rtp|state=session_closed' "$app_log" || true
if [[ "$failures" -ne 0 ]]; then
    echo "=== RTSPS client log ===" >&2
    sed -n '1,160p' "$client_log" >&2
    echo "FAIL: M3 RTSPS failures=$failures" >&2
    exit 1
fi
echo 'PASS: M3 RTSPS integration test'
