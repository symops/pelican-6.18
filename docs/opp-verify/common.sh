#!/bin/sh
# Shared helpers for the OPP-table verification scripts (docs/pr-6-9-duo-adaptation-plan.md,
# checklist items 2/3/4/5/7). Run as root, directly on hardware (Duo or Monarch -- see
# below) -- these poke real cpufreq/regulator sysfs, not meant for an emulator or
# container.
#
# Board-agnostic by design: every OPP frequency/voltage target and every regulator
# name is read LIVE from /sys/firmware/devicetree/base (the kernel's always-exposed
# live device tree -- same ABI as /proc/device-tree, no debugfs mount or special
# Kconfig needed). Nothing here is a maintained constant table, so these scripts
# reflect whatever DTB the board actually booted with, automatically -- on Monarch
# just as much as on Duo, since both carry the same operating-points-v2/cpu-supply
# structure (confirmed against the real compiled DTBs of both boards).

DT_BASE=/sys/firmware/devicetree/base
CPUFREQ_DIR=/sys/devices/system/cpu/cpu0/cpufreq

die() { echo "FATAL: $*" >&2; exit 1; }

require_root() {
	[ "$(id -u)" = "0" ] || die "must run as root"
	[ -d "$DT_BASE" ] || die "$DT_BASE not present -- CONFIG_PROC_DEVICETREE/OF sysfs missing?"
}

# ---- raw devicetree cell/string parsing --------------------------------------
#
# DT property files under /sys/firmware/devicetree/base are raw big-endian binary:
# ordinary integer cells are 4 bytes each; a property declared `/bits/ 64 <...>`
# in the DTS (opp-hz here) is two consecutive 4-byte cells forming one 64-bit
# value; string properties are plain NUL-terminated ASCII.

# Prints the decimal value of the Nth (0-based) 32-bit big-endian cell in a file.
dt_cell() {
	file="$1"; index="${2:-0}"
	od -An -tu4 --endian=big -j $((index * 4)) -N 4 "$file" | tr -d ' \n'
}

# Prints the decimal value of a 64-bit big-endian quantity (two consecutive
# cells, as produced by DTS `/bits/ 64 <...>`) starting at cell index N.
dt_cell64() {
	file="$1"; index="${2:-0}"
	hi=$(dt_cell "$file" "$index")
	lo=$(dt_cell "$file" $((index + 1)))
	echo $(( hi * 4294967296 + lo ))
}

dt_string() {
	tr -d '\0' < "$1"
}

# Finds the DT node directory whose own "phandle" cell equals the given number.
# Prints the node's directory path; returns 1 if nothing matches.
dt_find_by_phandle() {
	target="$1"
	for f in $(find "$DT_BASE" -name phandle 2>/dev/null); do
		[ "$(dt_cell "$f" 0)" = "$target" ] && { dirname "$f"; return 0; }
	done
	return 1
}

# Resolves a single-cell phandle-valued property on a node straight to the
# target node's directory (e.g. dt_resolve "$cpu0" cpu-supply).
dt_resolve() {
	node="$1"; prop="$2"
	[ -r "$node/$prop" ] || return 1
	dt_find_by_phandle "$(dt_cell "$node/$prop" 0)"
}

find_cpu0_node() {
	for d in "$DT_BASE"/cpus/cpu@0 "$DT_BASE"/cpus/cpu@0,0; do
		[ -d "$d" ] && { echo "$d"; return 0; }
	done
	for d in "$DT_BASE"/cpus/cpu@*; do
		[ -d "$d" ] || continue
		[ -r "$d/reg" ] && [ "$(dt_cell "$d/reg" 0)" = "0" ] && { echo "$d"; return 0; }
	done
	return 1
}

# ---- live discovery of the OPP table and the regulators involved -------------
#
# Populates (globals, space-separated):
#   OPP_LIST          "freq_khz:target_uv" pairs, ascending by frequency
#   OPP_FREQS_KHZ     just the freq_khz values, same order
#   CPU_SUPPLY_NAME   regulator-name of cpu0's cpu-supply (e.g. "cpudvs")
#   L2_SUPPLY_NAME    regulator-name of that regulator's coupled partner, if any
#                     (e.g. "ldo3") -- empty if the DTS has no coupling.
OPP_LIST=""
OPP_FREQS_KHZ=""
CPU_SUPPLY_NAME=""
L2_SUPPLY_NAME=""
_DT_DISCOVERED=0

discover_dt() {
	[ "$_DT_DISCOVERED" = 1 ] && return 0

	cpu0=$(find_cpu0_node) || die "could not find a cpu@0 node under $DT_BASE/cpus"

	reg_node=$(dt_resolve "$cpu0" cpu-supply) || die "cpu0 ($cpu0) has no resolvable cpu-supply phandle"
	[ -r "$reg_node/regulator-name" ] || die "regulator node $reg_node has no regulator-name property"
	CPU_SUPPLY_NAME=$(dt_string "$reg_node/regulator-name")

	if [ -r "$reg_node/regulator-coupled-with" ]; then
		l2_node=$(dt_resolve "$reg_node" regulator-coupled-with) && \
			[ -r "$l2_node/regulator-name" ] && \
			L2_SUPPLY_NAME=$(dt_string "$l2_node/regulator-name")
	fi

	opp_node=$(dt_resolve "$cpu0" operating-points-v2) || die "cpu0 ($cpu0) has no resolvable operating-points-v2 phandle"

	for d in "$opp_node"/opp-*; do
		[ -d "$d" ] && [ -r "$d/opp-hz" ] || continue
		hz=$(dt_cell64 "$d/opp-hz" 0)
		uv=$(dt_cell "$d/opp-microvolt" 0 2>/dev/null)
		[ -n "$uv" ] || uv=0
		OPP_LIST="$OPP_LIST $((hz/1000)):$uv"
	done
	[ -n "$OPP_LIST" ] || die "found OPP table node $opp_node but no opp-* children with an opp-hz property"
	OPP_LIST=$(printf '%s\n' $OPP_LIST | sort -t: -k1 -n | tr '\n' ' ')
	OPP_FREQS_KHZ=$(printf '%s\n' $OPP_LIST | sed 's/:.*//' | tr '\n' ' ')

	_DT_DISCOVERED=1
	echo "discovered from live devicetree ($cpu0):"
	for e in $OPP_LIST; do
		f=${e%%:*}; v=${e##*:}
		echo "  $((f/1000)) MHz -> ${v}uV target"
	done
	echo "  cpu-supply regulator-name: $CPU_SUPPLY_NAME"
	[ -n "$L2_SUPPLY_NAME" ] && echo "  coupled regulator-name:    $L2_SUPPLY_NAME"
}

# Prints the target microvolt value for a given frequency (kHz), or 0 if that
# frequency isn't one of the discovered OPPs.
opp_target_uv() {
	freq="$1"
	for e in $OPP_LIST; do
		[ "${e%%:*}" = "$freq" ] && { echo "${e##*:}"; return 0; }
	done
	echo 0
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
	discover_dt
	CPU_SUPP_DIR=$(find_regulator_dir "$CPU_SUPPLY_NAME") || die "$CPU_SUPPLY_NAME regulator not found under /sys/class/regulator -- is its driver (REGULATOR_G2227 or similar) enabled and probed?"
	echo "$CPU_SUPPLY_NAME -> $CPU_SUPP_DIR"
	if [ -n "$L2_SUPPLY_NAME" ]; then
		L2_SUPP_DIR=$(find_regulator_dir "$L2_SUPPLY_NAME") || die "$L2_SUPPLY_NAME (coupled regulator) not found under /sys/class/regulator"
		echo "$L2_SUPPLY_NAME -> $L2_SUPP_DIR"
	fi
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

# Reads the SoC temperature in degrees Celsius (one decimal place) from the
# standard kernel thermal framework (confirmed live on real RTD1295/RTD1296
# hardware: /sys/class/thermal/thermal_zone*/temp holds a raw integer in
# millidegrees C, e.g. "50642" == 50.6C). Any failure to find/read/parse a
# zone is reported loudly -- never silently printed as a fake "0.0".
read_temp_c() {
	for z in /sys/class/thermal/thermal_zone*/temp; do
		[ -r "$z" ] || continue
		v=$(cat "$z" 2>/dev/null)
		case "$v" in
			''|*[!0-9-]*)
				echo "temperature monitoring unavailable: $z contained non-numeric data ('$v')" >&2
				continue
				;;
		esac
		whole=$((v / 1000))
		frac=$(( (v % 1000) / 100 ))
		[ "$frac" -lt 0 ] && frac=$((-frac))
		echo "${whole}.${frac}"
		return 0
	done
	echo "temperature monitoring unavailable: no readable /sys/class/thermal/thermal_zone*/temp" >&2
	echo "?"
	return 1
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
