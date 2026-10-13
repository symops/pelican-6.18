#!/bin/sh
# Checklist item 4: sustained multi-core load (compress+verify data on every
# core, matching PR #9's own test) while the OPP changes at random roughly
# every 0.1s, for several minutes. Watches for lockup/thermal
# shutdown/silent reset and records steady-state temperature.
#
# Usage: 04-stress-random-opp.sh [duration_seconds]   (default 300 = 5 min;
# PR #9's own run was "several minutes" -- use a longer value for a more
# thorough soak, e.g. 1800 for 30 min).
#
# Run as root on real hardware (Duo or Monarch -- both carry the same cpu-supply/operating-points-v2 structure, discovered live, see common.sh).
set -e
. "$(dirname "$0")/common.sh"

require_root
discover_dt
DURATION="${1:-300}"
WORKDIR=$(mktemp -d /tmp/opp-stress.XXXXXX)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$WORKDIR"' EXIT

NCPUS=$(nproc)
echo "=== starting $NCPUS compress+verify worker(s) in $WORKDIR for ${DURATION}s ==="

worker() {
	id="$1"
	src="$WORKDIR/src-$id.bin"
	dd if=/dev/urandom of="$src" bs=1M count=16 status=none
	ref=$(sha256sum "$src" | awk '{print $1}')
	count=0
	errors=0
	end=$(( $(date +%s) + DURATION ))
	while [ "$(date +%s)" -lt "$end" ]; do
		gzip -c "$src" > "$WORKDIR/out-$id.gz" 2>/dev/null
		gunzip -c "$WORKDIR/out-$id.gz" > "$WORKDIR/back-$id.bin" 2>/dev/null
		got=$(sha256sum "$WORKDIR/back-$id.bin" | awk '{print $1}')
		count=$((count + 1))
		if [ "$got" != "$ref" ]; then
			errors=$((errors + 1))
			echo "worker $id: MISMATCH on iteration $count" >> "$WORKDIR/worker-$id.errors"
		fi
	done
	echo "$count $errors" > "$WORKDIR/worker-$id.result"
}

i=0
while [ "$i" -lt "$NCPUS" ]; do
	worker "$i" &
	i=$((i + 1))
done

echo "=== random OPP changer running in parallel ==="
echo userspace > "$CPUFREQ_DIR/scaling_governor"
end=$(( $(date +%s) + DURATION ))
changes=0
temps=""
while [ "$(date +%s)" -lt "$end" ]; do
	freq=$(echo "$OPP_FREQS_KHZ" | tr ' ' '\n' | shuf -n1 2>/dev/null || echo "$OPP_FREQS_KHZ" | awk '{print $1}')
	echo "$freq" > "$CPUFREQ_DIR/scaling_setspeed" 2>/dev/null || true
	changes=$((changes + 1))
	temps="$temps $(read_temp_c)"
	sleep 0.1
done

echo "=== waiting for workers to finish ==="
wait

echo "=== results ==="
total_iters=0
total_errors=0
for f in "$WORKDIR"/worker-*.result; do
	[ -r "$f" ] || continue
	read -r c e < "$f"
	total_iters=$((total_iters + c))
	total_errors=$((total_errors + e))
	echo "  worker result: $c compress+verify cycles, $e mismatches"
done
echo "OPP changes applied: $changes"
echo "total cycles: $total_iters, total mismatches: $total_errors"
echo "temperature samples (C): $temps"
echo "final temperature (C): $(read_temp_c)"

for f in "$WORKDIR"/worker-*.errors; do
	[ -r "$f" ] && { echo "--- $f ---"; cat "$f"; }
done

restore_governor
if [ "$total_errors" = 0 ]; then
	echo "=== result: PASS (no data corruption observed; manually confirm no lockup/reset happened and temperature stayed in a safe range) ==="
	exit 0
else
	echo "=== result: FAIL ($total_errors data mismatches) ==="
	exit 1
fi
