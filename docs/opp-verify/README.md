# OPP verification scripts (issue #10's PR #9, ported to Duo)

Companion scripts for the checklist in `../pr-6-9-duo-adaptation-plan.md`
("9b. OPP table") / `../pr-6-9-duo-adaptation-plan.ru.md`. **Run these
directly as root on real Duo hardware with the OPP-table changes booted
(`docs/pr-6-9-duo-adaptation-plan.md`'s DTS changes) -- not in an emulator
or container.** They poke real `cpufreq`/regulator sysfs and expect actual
voltage rails and CPU frequency scaling to respond.

| Script | Checklist item | What it does |
|---|---|---|
| `02-opp-sweep-readback.sh` | 2 | Steps through all 5 OPPs up then down via the userspace governor, reading back `cpudvs`/`ldo3` voltage after each step and checking it against the DTS `opp-microvolt` target (5% tolerance). |
| `03-direct-1400-to-600.sh` | 3 | Settles at 1400 MHz, jumps straight to 600 MHz (skipping the intermediate OPPs), then back up -- the transition most likely to expose a bad `regulator-ramp-delay`/coupling-spread value. |
| `04-stress-random-opp.sh` | 4 | Runs one compress+verify (gzip/gunzip + sha256sum) worker per core continuously while a second loop picks a random OPP every ~0.1s, for a configurable duration (default 5 min). Reports any data corruption and samples temperature. |
| `05-schedutil-soak.sh` | 5 | Switches to the real `schedutil` governor under a light background load, polls until at least 1000 frequency transitions are observed (via `cpufreq/stats/total_trans` if available, else manual polling), then checks `dmesg` for regulator/cpufreq errors and that the final voltage matches the governor's chosen OPP. |
| `07-throughput-under-opp-cycling.sh` | 7 | Runs a `dd` write+read throughput pass at every OPP against a real-disk path, and optionally against an NFS-mounted path too (closer to PR #9's own "fills the gigabit link" test) if you pass one. |

`common.sh` is shared by all of the above (not a standalone test) -- OPP
table / target-voltage definitions, regulator sysfs lookup by DTS
`regulator-name`, and small helpers. If the DTS OPP table ever changes,
update `OPP_FREQS_KHZ` and `opp_target_uv()` here first.

Checklist items 1 (boot test) and 6 (reboot/poweroff regression) are not
scripted here -- they're one-off manual actions (does it boot at all, does
`reboot`/`poweroff` still behave correctly), not repeatable utilities.

## Requirements on the board

- Root shell, Debian 13 userspace (matches PR #9's own test environment).
- `CONFIG_REGULATOR` sysfs present at `/sys/class/regulator/regulator.*`
  (standard; not gated behind a debugfs mount).
- `userspace` and `schedutil` cpufreq governors built in.
- `CONFIG_CPU_FREQ_STAT` for `05-schedutil-soak.sh`'s transition counter
  (falls back to manual polling if absent, just less precise).
- Usual coreutils (`dd`, `gzip`, `sha256sum`, `awk`) plus `/usr/bin/time`
  (from the `time` package) for `07-throughput-under-opp-cycling.sh`.

## Suggested run order

`02` -> `03` -> `07` (quick, a few minutes each) -> `04` and `05` (longer
soaks, tens of minutes) last, since those are the ones most likely to
surface a real problem under sustained load.
