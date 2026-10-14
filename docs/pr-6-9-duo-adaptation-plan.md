# Adapting symops/monarch-6.18 issue #10 (PRs #6-#9) for Duo/RTD1296

Context: issue #10 on symops/monarch-6.18 summarizes four PRs, all tested only
on real Monarch (RTD1295) hardware by their author (who has no Duo unit).
This repo already carries the directly-portable pieces:

- **#7 (jumbo frames)**: applied as-is, commit `58cf4904d` -- same
  `r8169soc.c` file, no board-specific content, zero risk.
- **#9's DTS/DVFS infrastructure**: applied, commit `5239264a9` -- the G2227
  PMIC is wired identically on this board (`pmic@12` on i2c0, confirmed by
  diffing both boards' DTS), so the pinctrl mux, regulator-coupling and
  `cpu-supply` wiring all carry over directly. **The specific OPP voltage
  table does not** -- see "9b. OPP table" below.

This document covers what's deliberately **not** applied, and what would be
needed to adapt it.

## #8 (MISC GPIO controller + gpiod VBUS/drive-bay power) -- not applicable

**Do not port this.** The problem #8 solves does not exist on Duo. Verified
by diffing both boards' current DTS: Duo's `wd,usb-vbus-gpios` property
already lives entirely on `rtk_iso_gpio` (lines 34 and 26, one per drive
bay), which is a real mainline `gpiolib` controller
(`drivers/gpio/gpio-rtd.c`) with normal `gpiod` consumers end to end.
Monarch's problem was that *half* of its VBUS wiring (one port) sat on
`misc-gpio`, a block with no DT node and no gpiolib controller in this port
at all -- forcing a raw-MMIO poke instead of a normal `gpiod_set_value()`
call. Duo never had that half-raw-MMIO situation to begin with (see
`wdmc-poweroff.c`'s own header comment, which already documents this
asymmetry between the two boards). There is nothing to adapt: if someone
revisits this later, re-derive this conclusion from the DTS diff first,
don't assume #8 is "just unported."

## #6 (real power-off via audio-CPU IPC + PMIC sleep) -- needs board-specific work + real hardware

**STATUS UPDATE: ported and hardware-confirmed.** Applied in commit
`03eaa4df8` (+ console diagnostics in `747bc0612`); `shutdown -P` on a real
Duo (booted from an eMMC firmware slot) now drops consumption to 0 W. What
was done: `reg` entries for the watchdog and UART0, `memory-region =
<&rpc_comm>` and `&rpc_comm { no-map; }` in the board DTS, and the driver
as in Monarch's PR #6 without #8's misc-gpio. The PMIC rails go off through
the g2227 regulator driver's `regulator-state-coldboot` states, so that
driver (a module on Pelican) must be loaded. An earlier test build left the
board at 2.6 W with identical logic; the cause of that one-off was not found
(see the history of this branch). The original analysis below is kept for
context.

**Originally not applied.** Plausible in principle (the underlying `rpc_comm` IPC
region is a SoC-level node in the shared `rtd129x.dtsi`, present on both
boards identically), but requires work this repo's author cannot safely do
blind, and the PR's own author has no Duo hardware to validate on.

### What would need to change

1. **DTS**: Duo's board file currently has no `&rpc_comm { ... }` override
   at all (Monarch's PR #6 added one). Needs the same `no-map` addition
   (Monarch's commit, not yet ported here) plus whatever `poweroff@...`
   node changes mirror #6's `reg`/`memory-region` additions -- but *without*
   #8's misc-gpio-specific VBUS reg entries, since those don't apply here
   (see above). The `poweroff` node's existing `reg = <0x70d0 0x4>` plus
   Duo's own two-bay `wd,usb-vbus-gpios` stay as-is; only the
   watchdog-control + UART0 `reg` entries and `memory-region = <&rpc_comm>;`
   from #6 would need adding.
2. **Driver** (`drivers/power/reset/wdmc-poweroff.c`): the file is currently
   byte-identical between both repos (confirmed before this session's PR
   work started). #6's driver changes (the IPC wait loop, the UART0
   loopback drain, the PMIC `iso_mux` poke via `of_find_i2c_device_by_node`)
   are not board-specific in the code itself -- they'd carry over as part of
   whatever commit also adds the DTS `&rpc_comm` override, since the driver
   reads the mapping from DT rather than hardcoding addresses.

### What to test before trusting it on Duo

Same risk class as Monarch's version: a wrong IPC/PMIC poke sequence can
leave a board in a bad power state (not bricked -- the audio CPU firmware
and PMIC are not touched by anything that can't be undone by re-flashing or
a hard power cycle -- but potentially stuck not-booting until someone pulls
the plug). Do not merge this sight-unseen. Minimum bar, mirroring #6's own
tested scope:

- Cold boot still works after the DTS change (the `rpc_comm` `no-map`
  reservation must not collide with anything else's memory layout on this
  board -- check `/proc/iomem` before/after).
- **Regression-critical**: `reboot` must NOT power off the board. #6's
  driver gates the PMIC/IPC sequence on `system_state != SYSTEM_POWER_OFF`;
  confirm this still holds with Duo's `.shutdown()` call path (same
  `wdmc-poweroff.c`, should be identical, but verify on real hardware, not
  just by code inspection).
- A real `poweroff` actually cuts power (board stays off until the power
  cable is pulled and reinserted), not just LED/USB/CPU-halt as today.
- The firmware's wake-timer path (same mechanism #6 tested on Monarch) still
  wakes the board on this board's audio CPU firmware revision, if one is
  present/flashable on Duo units.
- **Duo-specific**: verify the new power-off sequence doesn't interfere with
  the *existing*, already-working two-bay drive power sequencing (the two
  `rtk_iso_gpio` VBUS lines). Specifically check ordering -- does the
  PMIC-cutoff IPC request happen before or after the existing GPIO-based
  bay power-down in `wdmc_poweroff_shutdown()`, and does a real power-off
  cycle leave either drive bay in an inconsistent electrical state (confirm
  via a second clean boot immediately after, both drives enumerate).

Needs real Duo hardware access. Nobody has reported doing this.

## #9b. OPP table -- infrastructure ported, voltage table VERIFIED on Duo

**STATUS UPDATE (2026-10-14): verified on a real Duo (RTD1296)**, kernel
6.18.56 with the PMIC drivers as modules, scripts in `docs/opp-verify/`:
02 sweep up/down (all 5 OPPs reached, `cpudvs` within 5% of the DTS target
at each, `ldo3` follows through coupling, spread <= 100 mV), 03 direct
1400->600->1400 MHz jump, 04 five-minute 4-core compress+verify stress with
1963 random OPP changes (0 data mismatches, 49-63 C), 05 real `schedutil`
(1006+ transitions, no regulator/cpufreq errors in dmesg), 07 disk
throughput at every OPP (no drop tied to an OPP). Not covered: the NFS
variant of 07. The original analysis below is kept for context.


The DTS/driver *infrastructure* from #9 (pinctrl-single I2C0 mux, PMIC
regulator-coupling between `cpu_supp`/DC2 and `l2_supp`/LDO3, `cpu-supply`
wired to all four `&cpuN` nodes) is already applied in this repo
(`5239264a9`) because it's generic and the PMIC wiring is identical on both
boards. The **specific OPP table values** (5 points, 600-1400 MHz at
825-1100 mV) are copied as-is from Monarch's real-hardware-tested table and
are flagged in both the DTS comment and this document: **RTD1296 may have
different voltage/binning requirements than RTD1295** -- these numbers are
unverified on this silicon.

### Verification checklist (match PR #9's own tested bar)

1. Boot with the new OPP table active; confirm the board comes up at all
   (if the boot-time OPP's voltage is wrong, this could hang before any
   console output -- have serial console access before trying).
2. Step through every OPP up and down individually (`cpupower frequency-set`
   or raw sysfs), reading back both `cpudvs` (DC2) and the coupled L2 rail
   (LDO3) via the regulator debugfs/sysfs after each step, confirming they
   land at the DTS-specified `opp-microvolt` target each time.
   Script: `opp-verify/02-opp-sweep-readback.sh`.
3. A direct 1400 -> 600 MHz jump (skipping intermediate points), same
   read-back check -- this is the transition most likely to expose a wrong
   `regulator-ramp-delay`/coupling-spread value.
   Script: `opp-verify/03-direct-1400-to-600.sh`.
4. Multi-core sustained load (PR #9 used 4 cores compressing/verifying data)
   with the OPP changing at random every ~0.1s for several minutes; watch
   for any lockup, thermal shutdown, or silent reset. Record steady-state
   temperature.
   Script: `opp-verify/04-stress-random-opp.sh`.
5. Real `schedutil` governor, left running under normal load for at least
   as long as PR #9's test (1000+ transitions); check `dmesg` for any
   regulator or cpufreq error, and that `cpuinfo_cur_freq` / voltage
   readback stay consistent with the governor's choice throughout.
   Script: `opp-verify/05-schedutil-soak.sh`.
6. `reboot` and `poweroff` both still work correctly after the OPP changes
   (regulator `regulator-state-coldboot` entries must not leave a rail
   wrongly off/on across a reboot).
7. Disk/network throughput unaffected (PR #9's own NFS-fills-gigabit check)
   -- run while cycling OPPs, not only at a fixed frequency.
   Script: `opp-verify/07-throughput-under-opp-cycling.sh`.

All five scripts and their shared helpers live under `opp-verify/` next to
this document; see `opp-verify/README.md` for requirements and usage.

Until this checklist has been run on real Duo hardware, treat the OPP
voltage values as a reasonable starting point ported from a related but
not identical chip, not as validated for production.
