#!/usr/bin/env bash
# Phase B/Verification: exercises the nvfancontrol_ec thermal zone /
# nvfancontrol_fan cooling device end to end. Must run as root (module
# load/unload, sysfs mode/trip writes). Always leaves the zone disabled
# and overrides released (0xffff/0xffff) on exit, Ctrl-C, or a safety-temp
# abort -- safe to leave unattended once started.
set -uo pipefail

dev='/sys/bus/arm_ffa/devices/arm-ffa-17'
override_path="$dev/fan_override"
rpm_path="$dev/fan_rpm"
modparam='/sys/module/nvfancontrol/parameters'
safety_temp_c=90
zone=""
cdev=""

if [ "$(id -u)" -ne 0 ]; then
    echo "Run as root." >&2
    exit 1
fi

log() { printf '%s\n' "$*"; }
step() { printf '\n=== %s ===\n' "$*"; }

max_acpitz_temp() {
    sensors -j 2>/dev/null | python3 -c '
import json, sys
d = json.load(sys.stdin).get("acpitz-acpi-0", {})
vals = [v[k + "_input"] for k, v in d.items() if k.startswith("temp")]
print(max(vals) if vals else 0)
' 2>/dev/null || echo 0
}

find_zone_cdev() {
    local t
    for t in /sys/class/thermal/thermal_zone*; do
        [ "$(cat "$t/type" 2>/dev/null)" = "nvfancontrol_ec" ] && zone="$t"
    done
    for t in /sys/class/thermal/cooling_device*; do
        [ "$(cat "$t/type" 2>/dev/null)" = "nvfancontrol_fan" ] && cdev="$t"
    done
}

cleanup() {
    [ -n "$zone" ] && [ -w "$zone/mode" ] && echo disabled > "$zone/mode" 2>/dev/null
    echo "0xffff 0xffff" > "$override_path" 2>/dev/null
    [ -f "$modparam/failsafe_temp" ] && echo 92 > "$modparam/failsafe_temp" 2>/dev/null
    log "cleanup: override now: $(cat "$override_path" 2>/dev/null || echo unknown)"
}
trap cleanup EXIT INT TERM

sample() {
    local label="$1"
    echo "  [$label] fan_rpm: $(cat "$rpm_path") override: $(cat "$override_path")" \
         "zone_temp=$(cat "$zone/temp" 2>/dev/null)mC cdev_state=$(cat "$cdev/cur_state" 2>/dev/null)" \
         "max_acpitz=$(max_acpitz_temp)C"
}

watch_for() {
    local seconds="$1" label="$2" elapsed=0 t
    while [ "$elapsed" -lt "$seconds" ]; do
        sample "$label +${elapsed}s"
        t=$(max_acpitz_temp)
        awk -v t="$t" -v lim="$safety_temp_c" 'BEGIN{exit !(t+0>=lim)}' && {
            echo "SAFETY ABORT: max acpitz temp ${t}C >= ${safety_temp_c}C" >&2
            exit 1
        }
        sleep 5
        elapsed=$((elapsed + 5))
    done
}

step "load module"
cd /home/mlacage/dgx-spark-fan-override/nvfancontrol/usr/src/nvfancontrol || exit 1
if grep -q '^nvfancontrol ' /proc/modules; then
    rmmod nvfancontrol || exit 1
fi
insmod nvfancontrol.ko || exit 1
sleep 1
find_zone_cdev
if [ -z "$zone" ] || [ -z "$cdev" ]; then
    echo "could not find nvfancontrol_ec zone / nvfancontrol_fan cdev under /sys/class/thermal" >&2
    dmesg | tail -30
    exit 1
fi
log "zone=$zone cdev=$cdev"
log "mode: $(cat "$zone/mode")"
log "fan_override: $(cat "$override_path")"
log "--- dmesg since load ---"
dmesg | tail -25

step "zone temp vs sensors (disabled, idle)"
watch_for 10 idle-disabled

step "enable zone, idle observation"
echo enabled > "$zone/mode"
watch_for 15 enabled-idle

step "trip tunability: lower trip 0 near current temp, confirm governor reacts"
orig_trip0_temp=$(cat "$zone/trip_point_0_temp")
orig_trip0_hyst=$(cat "$zone/trip_point_0_hyst")
cur_mc=$(cat "$zone/temp")
new_trip0=$((cur_mc - 1000))
log "trip_point_0_temp: $orig_trip0_temp -> $new_trip0 (temporarily, current zone temp=$cur_mc)"
echo "$new_trip0" > "$zone/trip_point_0_temp"
watch_for 20 trip0-lowered
log "restoring trip_point_0_temp=$orig_trip0_temp trip_point_0_hyst=$orig_trip0_hyst"
echo "$orig_trip0_temp" > "$zone/trip_point_0_temp"
echo "$orig_trip0_hyst" > "$zone/trip_point_0_hyst"
watch_for 15 trip0-restored

step "stress-ng: watch trip crossings (75C/89C) for ~4 minutes"
stress-ng --cpu 0 --timeout 240s &
SPID=$!
for i in $(seq 1 48); do
    sample "stress +$((i * 5))s"
    t=$(max_acpitz_temp)
    awk -v t="$t" -v lim="$safety_temp_c" 'BEGIN{exit !(t+0>=lim)}' && {
        echo "SAFETY ABORT: max acpitz temp ${t}C >= ${safety_temp_c}C" >&2
        kill "$SPID" 2>/dev/null
        exit 1
    }
    sleep 5
done
wait "$SPID"

step "cooldown, hysteresis-delayed step-down"
watch_for 60 cooldown

step "failsafe: set failsafe_temp below current idle temp"
cur_c=$(max_acpitz_temp)
low_ft=$(awk -v c="$cur_c" 'BEGIN{printf "%d", c-3}')
log "setting failsafe_temp=$low_ft (current max_acpitz=${cur_c}C)"
echo "$low_ft" > "$modparam/failsafe_temp"
watch_for 15 failsafe-engaged
log "restoring failsafe_temp=92"
echo 92 > "$modparam/failsafe_temp"
watch_for 15 failsafe-cleared

step "legacy interlock: fan_override write must fail EBUSY while zone enabled"
if echo "2000 2000" > "$override_path" 2>/dev/null; then
    echo "UNEXPECTED: fan_override write succeeded while zone enabled" >&2
else
    log "OK: fan_override write refused while zone enabled (see dmesg for -EBUSY message)"
fi

step "disable zone, confirm interlock lifts and overrides released"
echo disabled > "$zone/mode"
sleep 1
log "fan_override: $(cat "$override_path")"
if echo "0xffff 0xffff" > "$override_path" 2>/dev/null; then
    log "OK: fan_override write succeeds again with zone disabled"
else
    echo "UNEXPECTED: fan_override write still failing with zone disabled" >&2
fi

step "rmmod, confirm clean unload"
cd /home/mlacage/dgx-spark-fan-override/nvfancontrol/usr/src/nvfancontrol || exit 1
rmmod nvfancontrol
log "rmmod OK"
dmesg | tail -10

trap - EXIT INT TERM
log "done."
