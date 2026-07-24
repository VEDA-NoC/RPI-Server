#!/usr/bin/env bash
set -euo pipefail

app="${M4_APP:-./build/rpi_vms}"
camera_host="${M4_CAMERA_HOST:-192.168.0.5}"
storage_root="${M4_STORAGE_ROOT:-/mnt/vms-storage}"
startup_timeout_ms="${M4_INGEST_STARTUP_TIMEOUT_MS:-5000}"
case_timeout_seconds="${M4_CAMERA_CASE_TIMEOUT_SECONDS:-60}"
settle_seconds="${M4_CAMERA_SETTLE_SECONDS:-5}"
result_dir="${M4_CAMERA_STARTUP_RESULT_DIR:-/tmp/rpi-vms-m4-camera-startup}"
camera_channels="${M4_CAMERA_CHANNELS:-0 1 2 3}"

[[ -x "$app" ]] || {
    echo "FAIL: app is not executable: $app" >&2
    exit 1
}
mountpoint -q "$storage_root" || {
    echo "FAIL: storage root is not mounted: $storage_root" >&2
    exit 1
}
[[ "$(findmnt -n -o FSTYPE -T "$storage_root")" == 'ext4' ]] || {
    echo "FAIL: storage root must use ext4: $storage_root" >&2
    exit 1
}
for value_name in startup_timeout_ms case_timeout_seconds settle_seconds; do
    value="${!value_name}"
    [[ "$value" =~ ^[0-9]+$ ]] || {
        echo "FAIL: $value_name must be a non-negative integer" >&2
        exit 1
    }
done
[[ "$startup_timeout_ms" -ge 1000 ]] || {
    echo 'FAIL: M4_INGEST_STARTUP_TIMEOUT_MS must be at least 1000' >&2
    exit 1
}
[[ "$case_timeout_seconds" -ge 1 ]] || {
    echo 'FAIL: M4_CAMERA_CASE_TIMEOUT_SECONDS must be at least 1' >&2
    exit 1
}
if pgrep -x rpi_vms >/dev/null; then
    echo 'FAIL: another rpi_vms process is already running' >&2
    pgrep -af rpi_vms >&2
    exit 1
fi
read -r -a selected_channels <<<"$camera_channels"
[[ "${#selected_channels[@]}" -gt 0 ]] || {
    echo 'FAIL: M4_CAMERA_CHANNELS must contain at least one camera channel' >&2
    exit 1
}
seen_channels=''
for camera_channel in "${selected_channels[@]}"; do
    [[ "$camera_channel" =~ ^[0-3]$ ]] || {
        echo 'FAIL: M4_CAMERA_CHANNELS entries must be in 0..3' >&2
        exit 1
    }
    [[ " $seen_channels " != *" $camera_channel "* ]] || {
        echo 'FAIL: M4_CAMERA_CHANNELS must not contain duplicates' >&2
        exit 1
    }
    seen_channels+=" $camera_channel"
done

mkdir -p "$result_dir"
password_file="$(mktemp /tmp/rpi-vms-m4-camera-password.XXXXXX)"
chmod 600 "$password_file"
read -rsp 'Camera password: ' camera_password
echo
[[ -n "$camera_password" ]] || {
    rm -f -- "$password_file"
    echo 'FAIL: empty camera password' >&2
    exit 1
}
printf '%s\n' "$camera_password" >"$password_file"
unset camera_password

app_pid=''
cleanup() {
    if [[ -n "$app_pid" ]] && kill -0 "$app_pid" 2>/dev/null; then
        kill -INT "$app_pid" 2>/dev/null || true
        wait "$app_pid" 2>/dev/null || true
    fi
    rm -f -- "$password_file"
}
trap cleanup EXIT INT TERM

echo 'camera_channel,channel_id,ready,elapsed_s,attempts,reconnects,pipeline_errors,app_status,log'
failures=0
last_index=$((${#selected_channels[@]} - 1))
for index in "${!selected_channels[@]}"; do
    camera_channel="${selected_channels[$index]}"
    channel_id=$((camera_channel + 1))
    log="$result_dir/camera-${camera_channel}.log"
    : >"$log"

    "$app" \
        --camera-host "$camera_host" \
        --camera-port 554 \
        --camera-user admin \
        --camera-password-stdin \
        --camera-path-template '/{camera_channel}/profile2/media.smp' \
        --channel-map "${camera_channel}:${channel_id}" \
        --storage-root "$storage_root" \
        --codec h264 \
        --segment-seconds 60 \
        --ingest-startup-timeout-ms "$startup_timeout_ms" \
        --channel-start-delay-ms 0 \
        --db-busy-timeout-ms 5000 \
        --require-storage-mount \
        --live-listen-host 127.0.0.1 \
        --rtsp-port 8554 \
        --rtsps-port 0 \
        --log-level info \
        <"$password_file" >"$log" 2>&1 &
    app_pid=$!

    ready=0
    elapsed_seconds=0
    for elapsed_seconds in $(seq 0 $((case_timeout_seconds - 1))); do
        if grep -q 'state=streaming first_buffer=yes' "$log"; then
            ready=1
            break
        fi
        kill -0 "$app_pid" 2>/dev/null || break
        sleep 1
    done

    if kill -0 "$app_pid" 2>/dev/null; then
        kill -INT "$app_pid" 2>/dev/null || true
    fi
    set +e
    wait "$app_pid"
    app_status=$?
    set -e
    app_pid=''

    attempts="$(grep -c 'state=connecting' "$log" || true)"
    reconnects="$(grep -c 'state=reconnecting' "$log" || true)"
    pipeline_errors="$(grep -c 'state=pipeline_error' "$log" || true)"
    echo "${camera_channel},${channel_id},${ready},${elapsed_seconds},${attempts},${reconnects},${pipeline_errors},${app_status},${log}"

    if [[ "$ready" -ne 1 || "$app_status" -ne 0 ]]; then
        failures=$((failures + 1))
        grep -E '\[storage\]|\[db\]|state=connecting|state=streaming|pipeline_error|reconnecting|worker_failed|fatal|error' \
            "$log" >&2 || true
    fi
    if [[ "$index" -lt "$last_index" && "$settle_seconds" -gt 0 ]]; then
        sleep "$settle_seconds"
    fi
done

if [[ "$failures" -ne 0 ]]; then
    echo "FAIL: single-channel camera startup failures=$failures" >&2
    exit 1
fi
echo "PASS: selected camera channels reached streaming in isolation: $camera_channels"
