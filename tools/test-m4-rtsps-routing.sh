#!/usr/bin/env bash
set -euo pipefail

app="${M4_APP:-./build/rpi_vms}"
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root="$(mktemp -d /tmp/rpi-vms-m4-rtsps-routing.XXXXXX)"
tls_cert="$root/server.crt"
tls_key="$root/server.key"
app_log="$root/app.log"
port="$(python3 - <<'PY'
import socket
with socket.socket() as sock:
    sock.bind(("127.0.0.1", 0))
    print(sock.getsockname()[1])
PY
)"

for command in openssl python3; do
    command -v "$command" >/dev/null || {
        echo "FAIL: required command not found: $command" >&2
        exit 1
    }
done
[[ -x "$app" ]] || {
    echo "FAIL: app is not executable: $app" >&2
    exit 1
}

app_pid=''
cleanup() {
    if [[ -n "$app_pid" ]] && kill -0 "$app_pid" 2>/dev/null; then
        kill -INT "$app_pid" 2>/dev/null || true
        wait "$app_pid" 2>/dev/null || true
    fi
    case "$root" in
        /tmp/rpi-vms-m4-rtsps-routing.*)
            rm -rf -- "$root"
            ;;
    esac
}
trap cleanup EXIT INT TERM

openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days 1 \
    -subj '/CN=localhost' \
    -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1' \
    -keyout "$tls_key" \
    -out "$tls_cert" \
    >/dev/null 2>&1
mkdir -p "$root/storage"
random_password="$(openssl rand -hex 16)"
printf '%s\n' "$random_password" | "$app" \
    --camera-host 127.0.0.1 \
    --camera-port 1 \
    --camera-user unreachable-test-user \
    --camera-password-stdin \
    --camera-path-template '/{camera_channel}/unreachable' \
    --channel-map '0:1,1:2,2:3,3:4' \
    --storage-root "$root/storage" \
    --min-free-bytes 0 \
    --reconnect-delay-ms 50 \
    --ingest-startup-timeout-ms 1000 \
    --rtsp-port 0 \
    --rtsps-port "$port" \
    --tls-cert "$tls_cert" \
    --tls-key "$tls_key" \
    --log-level info \
    >"$app_log" 2>&1 &
app_pid=$!
unset random_password

for _ in $(seq 1 50); do
    grep -q 'state=listening transport=rtsps' "$app_log" && break
    kill -0 "$app_pid" 2>/dev/null || break
    sleep 0.1
done
if ! grep -q 'state=listening transport=rtsps' "$app_log"; then
    echo 'FAIL: RTSPS listener did not start' >&2
    sed -n '1,100p' "$app_log" >&2
    exit 1
fi

for channel_id in 1 2 3 4; do
    python3 "$script_dir/probe_rtsp_status.py" \
        "rtsps://127.0.0.1:${port}/ch${channel_id}" \
        --tls-insecure \
        --expect-status 200
done
python3 "$script_dir/probe_rtsp_status.py" \
    "rtsps://127.0.0.1:${port}/ch5" \
    --tls-insecure \
    --expect-status 404
python3 "$script_dir/test_slow_rtsp_client.py" \
    "rtsps://127.0.0.1:${port}/ch1" \
    --tls-insecure \
    --pause-seconds 0.1 \
    --receive-buffer 1024

kill -INT "$app_pid"
wait "$app_pid"
app_pid=''

grep -q 'state=tls_handshake_success' "$app_log" || {
    echo 'FAIL: server did not record a successful TLS handshake' >&2
    exit 1
}
grep -q 'state=play channel_id=1' "$app_log" || {
    echo 'FAIL: slow TLS client did not reach PLAY on /ch1' >&2
    exit 1
}
[[ ! -e "$root/storage/recordings/ch0" ]] || {
    echo 'FAIL: RTSPS routing test created forbidden ch0' >&2
    exit 1
}
echo 'PASS: M4 RTSPS routing, TLS slow-client control and clean shutdown'
