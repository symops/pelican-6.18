#!/bin/sh
# Checklist item 3: a direct 1400 -> 600 MHz jump (skipping the intermediate
# points), same voltage read-back check as item 2 -- the transition most
# likely to expose a wrong regulator-ramp-delay/coupling-spread value.
# Run as root on real Duo hardware.
set -e
. "$(dirname "$0")/common.sh"

require_root
init_regulators

fail=0

echo "=== settle at 1400 MHz ==="
read -r freq cur cpu_uv l2_uv <<EOF
$(set_and_read_freq 1400000)
EOF
check_voltage "cpudvs (DC2) @1400MHz" "$(opp_target_uv 1400000)" "$cpu_uv" || fail=1
sleep 1

echo "=== direct jump 1400 -> 600 MHz ==="
t0=$(date +%s.%N 2>/dev/null || date +%s)
read -r freq cur cpu_uv l2_uv <<EOF
$(set_and_read_freq 600000)
EOF
t1=$(date +%s.%N 2>/dev/null || date +%s)
echo "transition observed in-script in: ${t0} -> ${t1} (coarse; see dmesg for the kernel-side timestamp if CPUFREQ debug is on)"
check_voltage "cpudvs (DC2) @600MHz after direct jump" "$(opp_target_uv 600000)" "$cpu_uv" || fail=1

echo "=== and back up 600 -> 1400 MHz (reverse direction) ==="
read -r freq cur cpu_uv l2_uv <<EOF
$(set_and_read_freq 1400000)
EOF
check_voltage "cpudvs (DC2) @1400MHz after direct jump back" "$(opp_target_uv 1400000)" "$cpu_uv" || fail=1

restore_governor
echo "=== result: $([ "$fail" = 0 ] && echo PASS || echo FAIL) ==="
echo "Also manually check: no lockup/log error during either jump (dmesg | grep -iE 'regulator|cpufreq')."
exit "$fail"
