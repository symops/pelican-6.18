#!/bin/sh
# Shared helpers for the OPP-table verification scripts (docs/pr-6-9-duo-adaptation-plan.md,
# checklist items 2/3/4/5/7). Run as root, directly on Duo hardware -- these poke real
# cpufreq/regulator sysfs, not meant for an emulator or container.
#
# Expected (opp-hz kHz : target microvolts) pairs, from the ported OPP table in
# arch/arm64/boot/dts/realtek/rtd1296-wd-mycloud-home-duo.dts. Edit here if the
# DTS table ever changes.
OPP_FREQS_KHZ="600000 800000 1000000 1200000 1400000"
opp_target_uv() {
	case "$1" in
		600000)  echo 825000 ;;
		800000)  echo 875000 ;;
		1000000) echo 925000 ;;
		1200000) echo 1012500 ;;
		1400000) echo 1100000 ;;
		*) echo 0 ;;
	esac
}

CPUFREQ_DIR=/sys/devices/system/cpu/cpu0/cpufreq

die() { echo "FATAL: $*" >&2; exit 1; }

require_root() {
	[ "$(id -u)" = "0" ] || die "must run as root"
}

# Finds a regulator's sysfs dir by its DTS regulator-name (e.g. "cpudvs", "ldo3").
# Prints the full /sys/class/regulator/regulator.N path, or nothing + exit 1 if not found.
find_regulator_dir() {
	name="$1"
	for d in /sys/class/regulator/regulator.*; do
		[ -r "$d/name" ] || continue
		if [ "$(cat "$d/name" 2>/dev/null)" = "$name" ]; then
			echo "$d"
			return 0
		fi
	done
	return 1
}

# Reads a regulator's current output in microvolts. Echoes the value, or "?" if unreadable.
read_microvolts() {
	dir="$1"
	if [ -r "$dir/microvolts" ]; then
		cat "$dir/microvolts"
	else
		echo "?"
	fi
}

CPU_SUPP_DIR=""
L2_SUPP_DIR=""
init_regulators() {
	CPU_SUPP_DIR=$(find_regulator_dir cpudvs) || die "cpudvs (cpu_supp/DC2) regulator not found under /sys/class/regulator -- is REGULATOR_G2227 enabled and probed?"
	L2_SUPP_DIR=$(find_regulator_dir ldo3) || die "ldo3 (l2_supp) regulator not found under /sys/class/regulator"
	echo "cpudvs  -> $CPU_SUPP_DIR"
	echo "ldo3/l2 -> $L2_SUPP_DIR"
}

# Pin a specific frequency (kHz) via the userspace governor. Waits briefly for the
# transition, then prints: freq_khz cur_khz cpudvs_uv l2_uv
set_and_read_freq() {
	freq="$1"
	echo userspace > "$CPUFREQ_DIR/scaling_governor" 2>/dev/null \
		|| die "could not set userspace governor (is CPUFREQ_GOV_USERSPACE built in?)"
	echo "$freq" > "$CPUFREQ_DIR/scaling_setspeed" || die "could not set scaling_setspeed=$freq"
	sleep 0.3
	cur=$(cat "$CPUFREQ_DIR/scaling_cur_freq")
	cpu_uv=$(read_microvolts "$CPU_SUPP_DIR")
	l2_uv=$(read_microvolts "$L2_SUPP_DIR")
	echo "$freq $cur $cpu_uv $l2_uv"
}

# Checks a read-back microvolt value against the DTS target within a tolerance (default 5%).
# Returns 0 (pass) or 1 (fail), prints a verdict line either way.
check_voltage() {
	label="$1"; target="$2"; actual="$3"; tol_pct="${4:-5}"
	if [ "$actual" = "?" ]; then
		echo "  $label: UNREADABLE (target ${target}uV)"
		return 1
	fi
	# integer-only arithmetic (no bc dependency)
	diff=$(( actual - target )); [ "$diff" -lt 0 ] && diff=$(( -diff ))
	tol=$(( target * tol_pct / 100 ))
	if [ "$diff" -le "$tol" ]; then
		echo "  $label: ${actual}uV (target ${target}uV, within ${tol_pct}%) -- OK"
		return 0
	else
		echo "  $label: ${actual}uV (target ${target}uV, within ${tol_pct}%) -- OUT OF RANGE"
		return 1
	fi
}

read_temp_c() {
	for z in /sys/class/thermal/thermal_zone*/temp; do
		[ -r "$z" ] || continue
		v=$(cat "$z")
		echo "scale=1; $v/1000" | awk '{printf "%.1f\n", $1/1000}' 2>/dev/null || echo "$((v/1000))"
		return
	done
	echo "?"
}

restore_governor() {
	# Leave the board in a sane state (ondemand is the kernel default if present,
	# else schedutil, else performance) rather than stuck on userspace.
	for g in schedutil ondemand performance; do
		if grep -qw "$g" "$CPUFREQ_DIR/scaling_available_governors" 2>/dev/null; then
			echo "$g" > "$CPUFREQ_DIR/scaling_governor" 2>/dev/null && return
		fi
	done
}
