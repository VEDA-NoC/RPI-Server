#!/usr/bin/env bash
set -euo pipefail

app="${M4_APP:-./build/rpi_vms}"
client="${M4_RTSPS_CLIENT:-$(dirname -- "$app")/rtsps_test_client}"
camera_host="${M4_CAMERA_HOST:-192.168.0.5}"
storage_root="${M4_STORAGE_ROOT:-/mnt/vms-storage}"
tls_cert="${M4_TLS_CERT:-certs/server.crt}"
tls_key="${M4_TLS_KEY:-certs/server.key}"
duration="${M4_MEASURE_SECONDS:-120}"
result_dir="${M4_RESULT_DIR:-/tmp/rpi-vms-m4-load}"
enable_rtsps_clients="${M4_LOAD_RTSPS_CLIENTS:-1}"

for command in findmnt ip ping getconf awk sqlite3 openssl; do
    command -v "$command" >/dev/null || {
        echo "FAIL: required command not found: $command" >&2
        exit 1
    }
done
[[ -x "$app" ]] || {
    echo "FAIL: app is not executable: $app" >&2
    exit 1
}
if [[ "$enable_rtsps_clients" != 0 && "$enable_rtsps_clients" != 1 ]]; then
    echo 'FAIL: M4_LOAD_RTSPS_CLIENTS must be 0 or 1' >&2
    exit 1
fi
if [[ "$enable_rtsps_clients" == 1 && ! -x "$client" ]]; then
    echo "FAIL: RTSPS client is not executable: $client" >&2
    exit 1
fi
[[ "$duration" =~ ^[1-9][0-9]*$ ]] || {
    echo 'FAIL: M4_MEASURE_SECONDS must be a positive integer' >&2
    exit 1
}
pgrep -x rpi_vms >/dev/null && {
    echo 'FAIL: another rpi_vms process is already running' >&2
    exit 1
}

mkdir -p "$result_dir"
temporary_tls_dir=''
if [[ ! -r "$tls_cert" || ! -r "$tls_key" ]]; then
    if [[ -e "$tls_cert" || -e "$tls_key" ]]; then
        echo 'FAIL: only one TLS file exists; refusing to replace it' >&2
        exit 1
    fi
    temporary_tls_dir="$(mktemp -d /tmp/rpi-vms-m4-load-tls.XXXXXX)"
    tls_cert="$temporary_tls_dir/server.crt"
    tls_key="$temporary_tls_dir/server.key"
    openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days 1 \
        -subj '/CN=localhost' \
        -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1' \
        -keyout "$tls_key" \
        -out "$tls_cert" \
        >/dev/null 2>&1
    chmod 600 "$tls_key"
    echo 'INFO: using a temporary self-signed certificate for this RTSPS load comparison'
fi
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
client_pids=()
cleanup() {
    for pid in "${client_pids[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill -INT "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    if [[ -n "$app_pid" ]] && kill -0 "$app_pid" 2>/dev/null; then
        kill -INT "$app_pid" 2>/dev/null || true
        wait "$app_pid" 2>/dev/null || true
    fi
    rm -f "$password_file"
    if [[ -n "$temporary_tls_dir" ]]; then
        rm -f -- "$temporary_tls_dir/server.crt" "$temporary_tls_dir/server.key"
        rmdir -- "$temporary_tls_dir" 2>/dev/null || true
    fi
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
    local client_log_prefix="$result_dir/${label}-rtsps"

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
        --rtsp-port 0 \
        --rtsps-port 8554 \
        --tls-cert "$tls_cert" \
        --tls-key "$tls_key" \
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

    client_pids=()
    if [[ "$enable_rtsps_clients" == 1 ]]; then
        for channel_id in $(seq 1 "$expected_channels"); do
            "$client" "rtsps://127.0.0.1:8554/ch${channel_id}" "$duration" \
                >"${client_log_prefix}-ch${channel_id}.log" 2>/dev/null &
            client_pids+=("$!")
        done
        for _ in $(seq 1 15); do
            tls_ready=0
            for channel_id in $(seq 1 "$expected_channels"); do
                if grep -q '^TLS connected$' "${client_log_prefix}-ch${channel_id}.log"; then
                    tls_ready=$((tls_ready + 1))
                fi
            done
            [[ "$tls_ready" -eq "$expected_channels" ]] && break
            sleep 1
        done
        [[ "${tls_ready:-0}" -eq "$expected_channels" ]] || {
            echo "FAIL: $label RTSPS clients reached TLS on ${tls_ready:-0}/$expected_channels channels" >&2
            return 1
        }
    fi

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

    local rtsps_interleaved_bytes=0
    if [[ "$enable_rtsps_clients" == 1 ]]; then
        for pid in "${client_pids[@]}"; do
            set +e
            wait "$pid"
            client_status=$?
            set -e
            if [[ "$client_status" -ne 0 ]]; then
                echo "FAIL: $label RTSPS load client exit status=$client_status" >&2
                return 1
            fi
        done
        client_pids=()
        rtsps_interleaved_bytes="$(awk -F '[:,]' '/^FINAL RTP packets:/ {gsub(/[[:space:]]/, "", $4); sum += $4} END {print sum+0}' \
            "${client_log_prefix}"-ch*.log)"
        [[ "$rtsps_interleaved_bytes" -gt 0 ]] || {
            echo "FAIL: $label RTSPS clients received no interleaved RTP/RTCP bytes" >&2
            return 1
        }
    fi

    kill -INT "$app_pid"
    wait "$app_pid"
    app_pid=''

    local elapsed="$((end_time - start_time))"
    local cpu_average rss_average disk_bytes rx_bytes tx_bytes rtsps_client_count
    cpu_average="$(awk -F, 'NR>1 {sum+=$2; n++} END {printf "%.2f", sum/n}' "$samples")"
    rss_average="$(awk -F, 'NR>1 {sum+=$3; n++} END {printf "%.0f", sum/n}' "$samples")"
    disk_bytes="$(((end_disk - start_disk) * 512))"
    rx_bytes="$((end_rx - start_rx))"
    tx_bytes="$((end_tx - start_tx))"
    rtsps_client_count="$((enable_rtsps_clients * expected_channels))"
    echo "$label,$expected_channels,$rtsps_client_count,$elapsed,$cpu_average,$rss_average,$start_rss,$disk_bytes,$rx_bytes,$tx_bytes,$rtsps_interleaved_bytes,${ping_loss:-unknown}" \
        >>"$result_dir/summary.csv"
}

echo 'case,channels,rtsps_clients,elapsed_s,avg_cpu_percent,avg_rss_kib,start_rss_kib,usb_write_bytes,camera_net_rx_bytes,camera_net_tx_bytes,rtsps_interleaved_bytes,packet_loss' \
    >"$result_dir/summary.csv"
run_case one-channel '0:1' 1
sleep 5
run_case four-channel '0:1,1:2,2:3,3:4' 4

echo 'PASS: M4 load comparison complete'
column -s, -t "$result_dir/summary.csv" 2>/dev/null || cat "$result_dir/summary.csv"
echo "Raw samples and logs: $result_dir"
