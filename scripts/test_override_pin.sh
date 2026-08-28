#!/usr/bin/env bash
# Phase A.2 verification: confirm that writing the same value to both EC
# override slots (fan_override) pins fan_rpm near that value, and that a
# value below the EC's own idle floor pulls RPM below the native curve.
# Must run as root (writes to fan_override). Restores 0xffff/0xffff (EC
# automatic curve) on exit, Ctrl-C, or if any sampled temp looks like it's
# running away, so this is safe to leave unattended.
set -euo pipefail

dev='/sys/bus/arm_ffa/devices/arm-ffa-17'
override_path="$dev/fan_override"
rpm_path="$dev/fan_rpm"
limits_path="$dev/fan_limits"
safety_temp_c=85

if [ "$(id -u)" -ne 0 ]; then
    echo "Run as root (writes fan_override)." >&2
    exit 1
fi

restore_auto() {
    echo "0xffff 0xffff" > "$override_path" 2>/dev/null || true
    echo "restored: $(cat "$override_path" 2>/dev/null || echo unknown)"
}
trap restore_auto EXIT INT TERM

max_acpitz_temp() {
    sensors -j 2>/dev/null | python3 -c '
import json, sys
d = json.load(sys.stdin).get("acpitz-acpi-0", {})
vals = [v[k + "_input"] for k, v in d.items() if k.startswith("temp")]
print(max(vals) if vals else 0)
' 2>/dev/null || echo 0
}

watch_for() {
    local seconds="$1" label="$2"
    local elapsed=0
    while [ "$elapsed" -lt "$seconds" ]; do
        t=$(max_acpitz_temp)
        echo "  [$label +${elapsed}s] fan_rpm: $(cat "$rpm_path") override: $(cat "$override_path") max_temp=${t}C"
        awk -v t="$t" -v lim="$safety_temp_c" 'BEGIN{exit !(t+0>=lim)}' && {
            echo "SAFETY ABORT: max acpitz temp ${t}C >= ${safety_temp_c}C, restoring auto and exiting" >&2
            exit 1
        }
        sleep 5
        elapsed=$((elapsed + 5))
    done
}

echo "=== fan_limits ==="
cat "$limits_path"

echo "=== baseline (auto) ==="
watch_for 10 baseline

echo "=== pin near max (3400/3400) ==="
echo "3400 3400" > "$override_path"
watch_for 60 pin-max

echo "=== pin near min (1350/1350) ==="
echo "1350 1350" > "$override_path"
watch_for 90 pin-min

echo "=== restore auto (0xffff/0xffff) ==="
echo "0xffff 0xffff" > "$override_path"
watch_for 30 restored

trap - EXIT INT TERM
echo "done."
