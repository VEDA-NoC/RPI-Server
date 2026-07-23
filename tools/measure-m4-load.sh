#!/usr/bin/env bash
set -euo pipefail

app="${M4_APP:-./build/rpi_vms}"
camera_host="${M4_CAMERA_HOST:-192.168.0.5}"
storage_root="${M4_STORAGE_ROOT:-/mnt/vms-storage}"
duration="${M4_MEASURE_SECONDS:-120}"
result_dir="${M4_RESULT_DIR:-/tmp/rpi-vms-m4-load}"

for command in findmnt ip ping getconf awk sqlite3; do
    command -v "$command" >/dev/null || {
        echo "FAIL: required command not found: $command" >&2
        exit 1
    }
done
[[ -x "$app" ]] || {
    echo "FAIL: app is not executable: $app" >&2
    exit 1
}
[[ "$duration" =~ ^[1-9][0-9]*$ ]] || {
    echo 'FAIL: M4_MEASURE_SECONDS must be a positive integer' >&2
    exit 1
}
pgrep -x rpi_vms >/dev/null && {
    echo 'FAIL: another rpi_vms process is already running' >&2
    exit 1
}

mkdir -p "$result_dir"
password_file="$(mktemp /tmp/rpi-vms-m4-password.XXXXXX)"
chmod 600 "$password_file"
read -rsp 'Camera password: ' camera_password
echo
[[ -n "$camera_password" ]] || {
    rm -f "$password_file"
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
    rm -f "$password_file"
}
trap cleanup EXIT INT TERM

mount_source="$(findmnt -no SOURCE --target "$storage_root")"
block_name="$(basename "$mount_source")"
block_stat="/sys/class/block/$block_name/stat"
[[ -r "$block_stat" ]] || {
    echo "FAIL: block statistics unavailable for $mount_source ($block_stat)" >&2
    exit 1
}
interface="$(ip route get "$camera_host" | awk '/dev/ {for (i=1;i<=NF;i++) if ($i=="dev") {print $(i+1); exit}}')"
[[ -n "$interface" ]] || {
    echo "FAIL: no route to camera $camera_host" >&2
    exit 1
}
clock_ticks="$(getconf CLK_TCK)"

read_counter() {
    local path="$1"
    tr -d '[:space:]' <"$path"
}

disk_sectors_written() {
    awk '{print $7}' "$block_stat"
}

run_case() {
    local label="$1"
    local mapping="$2"
    local expected_channels="$3"
    local log="$result_dir/${label}.log"
    local samples="$result_dir/${label}-samples.csv"

    "$app" \
        --camera-host "$camera_host" \
        --camera-user admin \
        --camera-password-stdin \
        --camera-path-template '/{camera_channel}/profile2/media.smp' \
        --channel-map "$mapping" \
        --storage-root "$storage_root" \
        --segment-seconds 60 \
        --db-busy-timeout-ms 5000 \
        --require-storage-mount \
        --rtsp-port 8554 \
        --rtsps-port 0 \
        --log-level info \
        <"$password_file" >"$log" 2>&1 &
    app_pid=$!

    local streaming=0
    for _ in $(seq 1 60); do
        streaming="$(sed -n \
            '/state=streaming first_buffer=yes/s/.*channel_id=\([1-4]\).*/\1/p' "$log" | sort -u | wc -l)"
        [[ "$streaming" -eq "$expected_channels" ]] && break
        kill -0 "$app_pid" 2>/dev/null || break
        sleep 1
    done
    [[ "$streaming" -eq "$expected_channels" ]] || {
        echo "FAIL: $label reached $streaming/$expected_channels streaming channels" >&2
        grep -E 'state=connecting|pipeline_error|reconnecting|worker_failed' "$log" >&2 || true
        return 1
    }

    local start_ticks start_rss start_disk start_rx start_tx start_time
    start_ticks="$(awk '{print $14+$15}' "/proc/$app_pid/stat")"
    start_rss="$(awk '/VmRSS:/ {print $2}' "/proc/$app_pid/status")"
    start_disk="$(disk_sectors_written)"
    start_rx="$(read_counter "/sys/class/net/$interface/statistics/rx_bytes")"
    start_tx="$(read_counter "/sys/class/net/$interface/statistics/tx_bytes")"
    start_time="$(date +%s)"
    echo 'elapsed_s,cpu_percent,rss_kib' >"$samples"

    local previous_ticks="$start_ticks"
    local previous_time="$start_time"
    for second in $(seq 1 "$duration"); do
        sleep 1
        kill -0 "$app_pid" 2>/dev/null || {
            echo "FAIL: $label app exited during measurement" >&2
            return 1
        }
        local now_ticks now_time rss cpu
        now_ticks="$(awk '{print $14+$15}' "/proc/$app_pid/stat")"
        now_time="$(date +%s)"
        rss="$(awk '/VmRSS:/ {print $2}' "/proc/$app_pid/status")"
        cpu="$(awk -v ticks="$((now_ticks - previous_ticks))" -v hz="$clock_ticks" \
            -v elapsed="$((now_time - previous_time))" 'BEGIN {printf "%.2f", 100*ticks/hz/elapsed}')"
        echo "$second,$cpu,$rss" >>"$samples"
        previous_ticks="$now_ticks"
        previous_time="$now_time"
    done

    local end_ticks end_disk end_rx end_tx end_time ping_loss
    end_ticks="$(awk '{print $14+$15}' "/proc/$app_pid/stat")"
    end_disk="$(disk_sectors_written)"
    end_rx="$(read_counter "/sys/class/net/$interface/statistics/rx_bytes")"
    end_tx="$(read_counter "/sys/class/net/$interface/statistics/tx_bytes")"
    end_time="$(date +%s)"
    ping_loss="$(ping -c 10 -W 1 "$camera_host" | sed -n 's/.* \([0-9.]*%\) packet loss.*/\1/p')"

    kill -INT "$app_pid"
    wait "$app_pid"
    app_pid=''

    local elapsed="$((end_time - start_time))"
    local cpu_average rss_average disk_bytes rx_bytes tx_bytes
    cpu_average="$(awk -F, 'NR>1 {sum+=$2; n++} END {printf "%.2f", sum/n}' "$samples")"
    rss_average="$(awk -F, 'NR>1 {sum+=$3; n++} END {printf "%.0f", sum/n}' "$samples")"
    disk_bytes="$(((end_disk - start_disk) * 512))"
    rx_bytes="$((end_rx - start_rx))"
    tx_bytes="$((end_tx - start_tx))"
    echo "$label,$expected_channels,$elapsed,$cpu_average,$rss_average,$start_rss,$disk_bytes,$rx_bytes,$tx_bytes,${ping_loss:-unknown}" \
        >>"$result_dir/summary.csv"
}

echo 'case,channels,elapsed_s,avg_cpu_percent,avg_rss_kib,start_rss_kib,usb_write_bytes,network_rx_bytes,network_tx_bytes,packet_loss' \
    >"$result_dir/summary.csv"
run_case one-channel '0:1' 1
sleep 5
run_case four-channel '0:1,1:2,2:3,3:4' 4

echo 'PASS: M4 load comparison complete'
column -s, -t "$result_dir/summary.csv" 2>/dev/null || cat "$result_dir/summary.csv"
echo "Raw samples and logs: $result_dir"
