#!/usr/bin/env bash
# Quick diagnostic: load, find the zone, read temp a few times, disable,
# unload -- just to see the DIAG log lines from nvfancontrol_get_temp().
set -uo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "Run as root." >&2
    exit 1
fi

cd /home/mlacage/dgx-spark-fan-override/nvfancontrol/usr/src/nvfancontrol || exit 1
if grep -q '^nvfancontrol ' /proc/modules; then
    rmmod nvfancontrol || exit 1
fi
dmesg -C
insmod nvfancontrol.ko || exit 1
sleep 1

zone=""
for t in /sys/class/thermal/thermal_zone*; do
    [ "$(cat "$t/type" 2>/dev/null)" = "nvfancontrol_ec" ] && zone="$t"
done
echo "zone=$zone"

echo "--- temp read while disabled ---"
cat "$zone/temp" 2>&1
sleep 1
cat "$zone/temp" 2>&1

echo "--- also directly probing acpitz zones for comparison ---"
for t in /sys/class/thermal/thermal_zone*; do
    ty=$(cat "$t/type" 2>/dev/null)
    [ "$ty" = "acpitz" ] && echo "$t type=$ty temp=$(cat "$t/temp" 2>/dev/null)"
done

echo "--- enabling briefly ---"
echo enabled > "$zone/mode"
sleep 3
cat "$zone/temp" 2>&1
echo disabled > "$zone/mode"
sleep 1

echo "--- dmesg (since dmesg -C at start) ---"
dmesg

echo "--- unload ---"
rmmod nvfancontrol
echo done.
