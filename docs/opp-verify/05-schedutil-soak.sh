#!/bin/sh
# Checklist item 5: real schedutil governor under normal load, left running
# for at least as long as PR #9's own test (1000+ transitions). Checks
# dmesg for any regulator/cpufreq error and that cpuinfo_cur_freq / voltage
# readback stay consistent with whatever the governor picked.
#
# Usage: 05-schedutil-soak.sh [min_transitions] [max_seconds]
#   min_transitions default 1000 (PR #9's own bar), max_seconds default 1800
#   (safety cap so the script doesn't run forever if the load is too light
#   to make schedutil switch often -- raise it, or raise the load, if you
#   hit the cap without reaching min_transitions).
#
# Run as root on real hardware (Duo or Monarch -- both carry the same cpu-supply/operating-points-v2 structure, discovered live, see common.sh).
set -e
. "$(dirname "$0")/common.sh"

require_root
init_regulators

MIN_TRANS="${1:-1000}"
MAX_SECONDS="${2:-1800}"
STATS_FILE="$CPUFREQ_DIR/stats/total_trans"

if ! grep -qw schedutil "$CPUFREQ_DIR/scaling_available_governors" 2>/dev/null; then
	die "schedutil governor not available (CONFIG_CPU_FREQ_GOV_SCHEDUTIL not built in?)"
fi
echo schedutil > "$CPUFREQ_DIR/scaling_governor"
echo "governor set to schedutil"

# Mark the dmesg point we care about (so we only look at NEW lines).
dmesg -c > /dev/null 2>&1 || true   # clears the ring buffer if we can; harmless if not permitted

if [ -r "$STATS_FILE" ]; then
	start_trans=$(cat "$STATS_FILE")
else
	echo "WARNING: $STATS_FILE not present (CONFIG_CPU_FREQ_STAT not built in?)"
	echo "falling back to manual scaling_cur_freq polling to count transitions."
	start_trans=0
fi

WORKDIR=$(mktemp -d /tmp/opp-soak.XXXXXX)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$WORKDIR"' EXIT

# "Normal load" -- a flat 100%-busy loop gives schedutil no reason to ever
# back off once it hits max (confirmed on real Monarch hardware: only 2
# transitions in a full 1800s cap, settling at 1400MHz immediately and
# staying there, vs PR #9's own 1035 transitions). schedutil only reacts to
# *changes* in utilization, so each core needs real idle gaps, not constant
# saturation. Oscillate each core between a short CPU-bound burst and a
# sleep so utilization actually swings and schedutil has something to react
# to. (Deliberately not timed via GNU date's %N here -- %s-only, same as
# the rest of this repo's scripts -- so the burst is a coarse ~0-1s rather
# than a precise ~0.3s; that's fine, the idle gap is what matters.)
NCPUS=$(nproc)
burn_then_idle() {
	while :; do
		end=$(( $(date +%s) + 1 ))
		x=0
		while :; do
			x=$((x + 1))
			if [ $((x % 2000)) -eq 0 ] && [ "$(date +%s)" -ge "$end" ]; then
				break
			fi
		done
		sleep 0.2
	done
}
i=0
while [ "$i" -lt "$NCPUS" ]; do
	burn_then_idle &
	i=$((i + 1))
done
echo "started $NCPUS busy/idle oscillating background job(s) (~0-1s burst / ~0.2s idle per core) to give schedutil utilization swings to react to"

last_freq=$(cat "$CPUFREQ_DIR/scaling_cur_freq")
manual_trans=0
elapsed=0
last_report=0
REPORT_INTERVAL=180
echo "polling every 2s, up to ${MAX_SECONDS}s, until >= $MIN_TRANS transitions (progress printed every ${REPORT_INTERVAL}s)..."
while [ "$elapsed" -lt "$MAX_SECONDS" ]; do
	sleep 2
	elapsed=$((elapsed + 2))
	cur=$(cat "$CPUFREQ_DIR/scaling_cur_freq")
	if [ "$cur" != "$last_freq" ]; then
		manual_trans=$((manual_trans + 1))
		last_freq="$cur"
	fi
	if [ -r "$STATS_FILE" ]; then
		now_trans=$(cat "$STATS_FILE")
		count=$(( now_trans - start_trans ))
	else
		count=$manual_trans
	fi
	if [ $((elapsed - last_report)) -ge "$REPORT_INTERVAL" ]; then
		echo "progress: ${count} transitions after ${elapsed}s (bar: >= $MIN_TRANS, cap: ${MAX_SECONDS}s)"
		last_report=$elapsed
	fi
	if [ "$count" -ge "$MIN_TRANS" ]; then
		echo "reached $count transitions after ${elapsed}s"
		break
	fi
done

kill $(jobs -p) 2>/dev/null || true

cpu_uv=$(read_microvolts "$CPU_SUPP_DIR")
cur_khz=$(cat "$CPUFREQ_DIR/scaling_cur_freq")
echo "final cur_freq=$((cur_khz/1000)) MHz, cpudvs readback=${cpu_uv}uV"
target=$(opp_target_uv "$cur_khz")
fail=0
if [ "$target" != 0 ]; then
	check_voltage "cpudvs (DC2) at final schedutil-chosen OPP" "$target" "$cpu_uv" || fail=1
fi

echo "=== dmesg since governor switch (filtered) ==="
err_lines=$(dmesg 2>/dev/null | grep -iE "regulator|cpufreq|g2227" | grep -iE "error|warn|fail" || true)
if [ -n "$err_lines" ]; then
	echo "$err_lines"
	echo "FOUND regulator/cpufreq error or warning lines above"
	fail=1
else
	echo "(none found)"
fi

echo "observed/reported transitions: ${count:-$manual_trans} (bar: >= $MIN_TRANS)"
if [ "${count:-$manual_trans}" -lt "$MIN_TRANS" ]; then
	echo "NOTE: did not reach $MIN_TRANS transitions within ${MAX_SECONDS}s -- re-run with a longer MAX_SECONDS or heavier/more variable load."
fi

restore_governor
echo "=== result: $([ "$fail" = 0 ] && echo PASS || echo FAIL) ==="
exit "$fail"
