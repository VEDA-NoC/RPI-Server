#!/usr/bin/env bash
set -euo pipefail

app="${M3_APP:-./build/rpi_vms}"
camera_host="${M3_CAMERA_HOST:-192.168.0.5}"
storage_root="${M3_STORAGE_ROOT:-/mnt/vms-storage}"
tls_cert="${M3_TLS_CERT:-certs/server.crt}"
tls_key="${M3_TLS_KEY:-certs/server.key}"
app_log="${M3_APP_LOG:-/tmp/rpi-vms-m3-qt-smoke.log}"

if [[ ! -x "$app" ]]; then
    echo "FAIL: app is not executable: $app" >&2
    exit 1
fi
if ! command -v openssl >/dev/null; then
    echo 'FAIL: required command not found: openssl' >&2
    exit 1
fi
if pgrep -x rpi_vms >/dev/null; then
    echo 'FAIL: another rpi_vms process is already running' >&2
    pgrep -af rpi_vms >&2
    exit 1
fi

: >"$app_log"
temporary_tls_dir=''
app_pid=''
cleanup() {
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
    temporary_tls_dir="$(mktemp -d /tmp/rpi-vms-m3-qt-tls.XXXXXX)"
    tls_cert="$temporary_tls_dir/server.crt"
    tls_key="$temporary_tls_dir/server.key"
    openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days 1 \
        -subj '/CN=rpi-vms-m3-qt-test' \
        -keyout "$tls_key" \
        -out "$tls_cert" \
        >/dev/null 2>&1
    chmod 600 "$tls_key"
    echo 'INFO: using a temporary self-signed certificate for this run'
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

if command -v tailscale >/dev/null; then
    pi_address="$(tailscale ip -4 2>/dev/null | head -n 1)"
else
    pi_address=''
fi
if [[ -z "$pi_address" ]]; then
    pi_address="$(hostname -I | awk '{print $1}')"
fi

echo 'PASS: M3 RTSPS server is ready for the Windows Qt smoke test'
echo "Qt Base URL: rtsps://${pi_address}:8554"
echo 'Expected: CH1 plays; CH2, CH3 and CH4 return 404 until M4.'
echo "App log: $app_log"
echo 'Press Ctrl+C after checking the Qt video.'

set +e
wait "$app_pid"
app_status=$?
set -e
app_pid=''
exit "$app_status"
