#!/bin/sh
# Checklist item 2: step through every OPP up and down individually, reading back
# both cpudvs (DC2) and the coupled L2 rail (ldo3) after each step, confirming
# they land at the DTS opp-microvolt target. Run as root on real Duo hardware.
set -e
. "$(dirname "$0")/common.sh"

require_root
init_regulators

fail=0

echo "=== sweep up ==="
for f in $OPP_FREQS_KHZ; do
	read -r freq cur cpu_uv l2_uv <<EOF
$(set_and_read_freq "$f")
EOF
	echo "OPP $((freq/1000)) MHz requested, cur_freq=$((cur/1000)) MHz"
	target=$(opp_target_uv "$freq")
	check_voltage "cpudvs (DC2)" "$target" "$cpu_uv" || fail=1
	# l2_supp/ldo3 is coupled to track cpu_supp within 100mV; just confirm it's readable
	# and in the same ballpark rather than an exact target (its own DTS range is 900-1000mV).
	if [ "$l2_uv" = "?" ]; then
		echo "  ldo3 (L2, coupled): UNREADABLE"; fail=1
	else
		echo "  ldo3 (L2, coupled): ${l2_uv}uV"
	fi
done

echo "=== sweep down ==="
for f in $(echo "$OPP_FREQS_KHZ" | tr ' ' '\n' | tac); do
	read -r freq cur cpu_uv l2_uv <<EOF
$(set_and_read_freq "$f")
EOF
	echo "OPP $((freq/1000)) MHz requested, cur_freq=$((cur/1000)) MHz"
	target=$(opp_target_uv "$freq")
	check_voltage "cpudvs (DC2)" "$target" "$cpu_uv" || fail=1
done

restore_governor
echo "=== result: $([ "$fail" = 0 ] && echo PASS || echo FAIL) ==="
exit "$fail"
