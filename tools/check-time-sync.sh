#!/usr/bin/env bash
set -u

echo "== UTC =="
date -u --iso-8601=ns

echo "== Local =="
date --iso-8601=ns

echo "== timedatectl =="
timedatectl status

if command -v chronyc >/dev/null 2>&1; then
    echo "== chrony tracking =="
    chronyc tracking
    echo "== chrony sources =="
    chronyc sources -v
else
    echo "chronyc: not installed"
fi

echo "== kernel clocksource =="
cat /sys/devices/system/clocksource/clocksource0/current_clocksource

