#!/usr/bin/env bash
set -euo pipefail

app="${M4_APP:-./build/rpi_vms}"
camera_host="${M4_CAMERA_HOST:-192.168.0.5}"
storage_root="${M4_STORAGE_ROOT:-/mnt/vms-storage}"
tls_cert="${M4_TLS_CERT:-certs/server.crt}"
tls_key="${M4_TLS_KEY:-certs/server.key}"
startup_timeout_ms="${M4_INGEST_STARTUP_TIMEOUT_MS:-45000}"
channel_start_delay_ms="${M4_CHANNEL_START_DELAY_MS:-5000}"
app_log="${M4_APP_LOG:-/tmp/rpi-vms-m4-qt-smoke.log}"

for command in openssl awk; do
    command -v "$command" >/dev/null || {
        echo "FAIL: required command not found: $command" >&2
        exit 1
    }
done
[[ -x "$app" ]] || {
    echo "FAIL: app is not executable: $app" >&2
    exit 1
}
[[ "$channel_start_delay_ms" =~ ^[0-9]+$ ]] || {
    echo 'FAIL: M4_CHANNEL_START_DELAY_MS must be a non-negative integer' >&2
    exit 1
}
if [[ ! -r "$tls_cert" || ! -r "$tls_key" ]]; then
    echo 'FAIL: Qt smoke requires a persistent TLS certificate and private key' >&2
    echo "Expected certificate: $tls_cert" >&2
    echo "Expected private key: $tls_key" >&2
    echo 'Create the development certificate once with: bash tools/generate-dev-tls-cert.sh' >&2
    exit 1
fi
openssl x509 -in "$tls_cert" -noout >/dev/null
cert_public_key="$(openssl x509 -in "$tls_cert" -pubkey -noout | openssl sha256)"
private_public_key="$(openssl pkey -in "$tls_key" -pubout | openssl sha256)"
[[ "$cert_public_key" == "$private_public_key" ]] || {
    echo 'FAIL: TLS certificate and private key do not match' >&2
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
cleanup() {
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
    --codec h264 \
    --storage-root "$storage_root" \
    --segment-seconds 60 \
    --reconnect-delay-ms 2000 \
    --ingest-startup-timeout-ms "$startup_timeout_ms" \
    --channel-start-delay-ms "$channel_start_delay_ms" \
    --require-storage-mount \
    --rtsp-port 0 \
    --rtsps-port 8554 \
    --tls-cert "$tls_cert" \
    --tls-key "$tls_key" \
    --log-level info \
    <&3 >"$app_log" 2>&1 &
app_pid=$!
exec 3<&-

streaming_channels=0
for _ in $(seq 1 150); do
    streaming_channels="$(sed -n \
        '/state=streaming first_buffer=yes/s/.*channel_id=\([1-4]\).*/\1/p' "$app_log" | sort -u | wc -l)"
    [[ "$streaming_channels" -eq 4 ]] && break
    kill -0 "$app_pid" 2>/dev/null || break
    sleep 1
done
if [[ "$streaming_channels" -ne 4 ]]; then
    echo "FAIL: expected four streaming channels, got $streaming_channels" >&2
    grep -E 'state=connecting|first_buffer|pipeline_error|reconnecting|worker_failed' "$app_log" >&2 || true
    exit 1
fi

if command -v tailscale >/dev/null; then
    pi_address="$(tailscale ip -4 2>/dev/null | head -n 1)"
else
    pi_address=''
fi
[[ -n "$pi_address" ]] || pi_address="$(hostname -I | awk '{print $1}')"

echo 'PASS: M4 RTSPS server is ready for the Windows Qt smoke test'
echo "Qt Base URL: rtsps://${pi_address}:8554"
echo 'Expected: CH1, CH2, CH3 and CH4 all reach Playing.'
echo "Certificate SHA-256: $(openssl x509 -in "$tls_cert" -noout -fingerprint -sha256 | cut -d= -f2-)"
echo "App log: $app_log"
echo 'Press Ctrl+C after checking all four Qt channels.'

set +e
wait "$app_pid"
app_status=$?
set -e
app_pid=''
exit "$app_status"
