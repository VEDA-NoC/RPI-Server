#!/usr/bin/env bash
set -u

run_if_available() {
    local command_name="$1"
    shift
    if command -v "$command_name" >/dev/null 2>&1; then
        "$@"
    else
        echo "$command_name: not installed"
    fi
}

echo "== collected_at_utc =="
date -u --iso-8601=seconds

echo "== board =="
tr -d '\0' </proc/device-tree/model
echo

echo "== cpu =="
lscpu

echo "== memory =="
free -h

echo "== os =="
cat /etc/os-release

echo "== kernel =="
uname -a

echo "== storage =="
lsblk -o NAME,SIZE,MODEL,TRAN,FSTYPE,MOUNTPOINTS,UUID

echo "== gstreamer =="
run_if_available gst-launch-1.0 gst-launch-1.0 --version

echo "== compiler =="
run_if_available g++ g++ --version

echo "== cmake =="
run_if_available cmake cmake --version

echo "== sqlite =="
run_if_available sqlite3 sqlite3 --version

echo "== openssl =="
run_if_available openssl openssl version -a

echo "== network =="
ip -brief address

echo "== throttling =="
run_if_available vcgencmd vcgencmd get_throttled

