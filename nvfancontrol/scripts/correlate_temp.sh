#!/usr/bin/env bash
# Phase A.1 correlation helper: samples fan_telemetry hex + acpitz temps
# across an idle -> CPU-stress -> cooldown cycle, so the raw telemetry
# bytes can be correlated against known SoC temperatures by inspection.
set -euo pipefail

cat > /tmp/nvfc_temps.py <<'PYEOF'
import json, sys
d = json.load(sys.stdin)["acpitz-acpi-0"]
print(",".join("%s=%s" % (k, v[k + "_input"]) for k, v in d.items() if k.startswith("temp")))
PYEOF

tel='/sys/bus/arm_ffa/devices/arm-ffa-17/fan_telemetry'
out=/tmp/nvfc_correlate.log
: > "$out"

sample() {
  ts=$(date +%s)
  hex=$(cat "$tel")
  t=$(sensors -j | python3 /tmp/nvfc_temps.py)
  echo "$ts $hex $t" | tee -a "$out"
}

echo "=== idle baseline ===" | tee -a "$out"
for i in $(seq 1 5); do sample; sleep 2; done

echo "=== stressing ===" | tee -a "$out"
stress-ng --cpu 0 --timeout 90s &
SPID=$!
for i in $(seq 1 30); do sample; sleep 3; done
wait "$SPID"

echo "=== cooldown ===" | tee -a "$out"
for i in $(seq 1 20); do sample; sleep 3; done

echo "log saved to $out"
