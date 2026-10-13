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

# "Normal load" -- a couple of busy loops per core is enough to give schedutil
# something to react to without pinning everything at max like item 4 does.
NCPUS=$(nproc)
i=0
while [ "$i" -lt "$NCPUS" ]; do
	( while :; do :; done ) &
	i=$((i + 1))
done
echo "started $NCPUS busy-loop background job(s) to give schedutil something to scale against"

last_freq=$(cat "$CPUFREQ_DIR/scaling_cur_freq")
manual_trans=0
elapsed=0
echo "polling every 2s, up to ${MAX_SECONDS}s, until >= $MIN_TRANS transitions..."
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
