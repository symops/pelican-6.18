#!/bin/sh
# Checklist item 7: disk/network throughput unaffected while cycling OPPs
# (PR #9's own check was "NFS fills the gigabit link"; this script does a
# local disk read/write pass at every OPP by default, and an NFS write/read
# pass too if you give it a mounted NFS path -- closer to PR #9's own test).
#
# Usage: 07-throughput-under-opp-cycling.sh <test_dir> [nfs_test_dir]
#   test_dir     a directory on the board's real disk (not tmpfs) with a
#                few hundred MB free, e.g. /root or a mounted data volume.
#   nfs_test_dir optional: a directory under an already-mounted NFS share,
#                to mirror PR #9's own gigabit-NFS check specifically.
#
# Run as root on real hardware (Duo or Monarch -- both carry the same cpu-supply/operating-points-v2 structure, discovered live, see common.sh).
set -e
. "$(dirname "$0")/common.sh"

require_root
discover_dt
TEST_DIR="${1:?usage: $0 <test_dir-on-real-disk> [nfs_test_dir]}"
NFS_DIR="${2:-}"
SIZE_MB=512

[ -d "$TEST_DIR" ] || die "$TEST_DIR does not exist"

run_pass() {
	label="$1"; dir="$2"
	wfile="$dir/opp-throughput-test.bin"
	# Write
	sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
	wtime=$( { /usr/bin/time -f "%e" dd if=/dev/zero of="$wfile" bs=1M count="$SIZE_MB" conv=fdatasync status=none 2>&1; } 2>&1 )
	wmbps=$(awk -v s="$SIZE_MB" -v t="$wtime" 'BEGIN{ if (t>0) printf "%.1f", s/t; else print "?" }')
	# Read
	sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
	rtime=$( { /usr/bin/time -f "%e" dd if="$wfile" of=/dev/null bs=1M status=none 2>&1; } 2>&1 )
	rmbps=$(awk -v s="$SIZE_MB" -v t="$rtime" 'BEGIN{ if (t>0) printf "%.1f", s/t; else print "?" }')
	rm -f "$wfile"
	echo "$label write=${wmbps}MB/s read=${rmbps}MB/s"
}

echo userspace > "$CPUFREQ_DIR/scaling_governor"
echo "=== throughput at each OPP (disk: $TEST_DIR$( [ -n "$NFS_DIR" ] && echo ", NFS: $NFS_DIR")) ==="
for f in $OPP_FREQS_KHZ; do
	echo "$f" > "$CPUFREQ_DIR/scaling_setspeed"
	sleep 0.3
	cur=$(cat "$CPUFREQ_DIR/scaling_cur_freq")
	echo "--- OPP $((f/1000)) MHz (actual cur_freq=$((cur/1000)) MHz) ---"
	run_pass "  disk" "$TEST_DIR"
	if [ -n "$NFS_DIR" ]; then
		run_pass "  nfs " "$NFS_DIR"
	fi
done

restore_governor
echo "=== done -- compare the write/read MB/s lines above across OPPs by eye; PR #9's own bar was 'fills the gigabit link' (~110-118 MB/s) and no throughput drop tied to a specific OPP ==="
