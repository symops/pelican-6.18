# Linux 6.18 port for WD My Cloud Home Duo (Realtek RTD1296 "Pelican")

This is the dual-bay sibling of [symops/monarch-6.18](https://github.com/symops/monarch-6.18)
(the single-bay WD My Cloud Home, Realtek RTD1295 "Monarch") — read
**that repo's README.md first**. Most of the generic RTD129x-family
bring-up decisions (SMP spin-table, IRQ mux, PWM controller driver, cpufreq/SCPU
clock, thermal, GMAC, the Image-header-patch requirement, raw-not-gzip
packaging, the USB-rescue dev loop itself) are identical between the two
boards and only documented there. This file covers only what's specific to
the Duo.

## Acknowledgments

This port builds on Monarch's, so it inherits Monarch's own debt to
[Fireblossom](https://github.com/Fireblossom)'s
[wd-mch-kernel](https://github.com/Fireblossom/wd-mch-kernel) — the
generic RTD129x-family bring-up this file says is "identical between
the two boards and only documented there" (SMP, GMAC, USB dwc3/PHY,
watchdog, thermal, IRQ mux, I2C, the MFD/regulator stack, the rescue
`init` script) was largely adopted from that work by way of Monarch.
Thanks to Fireblossom for it -- see symops/monarch-6.18's README.md's
own Acknowledgments section for the full story.

## Hardware differences from Monarch (RTD1295 vs RTD1296)

RTD1296 is the same silicon as RTD1295 with two additional bonded-out blocks:

- A second SATA lane (`ahci_sata`/`sata_phy` gain a `sata-port@1`/`sata-phy@1`).
- A third USB controller (`dwc3_u3host`, physically present but usually left
  disabled on RTD1295 boards) plus an RTS5400 Type-C PD companion chip on
  `i2c5` (transparent USB3 hub, no driver needed; investigated and closed,
  see Progress log).

Confirmed via the vendor `symops/pelican-4.9.330` GPL source tree
(`arch/arm64/boot/dts/realtek/rtd129x/rtd-1296-pelican-1GB.dts` and
`rtd-1296.dtsi`) and cross-checked against a real vendor 4.9.330 boot log from
an actual two-bay unit with both bays populated (WDC WD5000AZLX-60K2TA0 ×2,
both linking at 6.0 Gbps on that kernel).

The WD My Cloud Home Duo also boots its **OS from eMMC** rather than SATA
(`androidboot.storage=emmc` in the vendor bootargs; the two SATA bays are RAID
data storage only) — unlike Monarch, which boots from SATA directly. This port
does **not** follow that split: like Monarch's mainline port, it runs rootfs
from the SATA bays instead. eMMC itself is ported and confirmed working
as a plain mountable block device (PIO + HS200, see Confirmed working
below) -- it's just not used as the boot/root filesystem here.

### Rescue-loader filenames

The Duo's `boot_rescue_from_usb` U-Boot path reads different fixed filenames
than Monarch's — `emmc.*` instead of `sata.*`:

- `rescue.emmc.dtb`
- `emmc.uImage` (same convention as Monarch's `sata.uImage` — the name is
  cosmetic, there is no mkimage/FIT wrapping: patch the raw `Image` header
  first, *then* gzip it with `pigz -11`, preserving the original filename
  in the gzip header (`Image.noinitramfs`). U-Boot's rescue loader detects
  the gzip magic and decompresses it itself — logs
  `Not raw Image, Starting Decompress Image.gz...` — so this halves the
  file size on the USB stick without changing anything downstream)
- `rescue.root.emmc.cpio.gz_pad.img` (separate initrd, gzip'd cpio zero-padded
  to exactly 4194304 bytes — same convention as Monarch's
  `rescue.root.sata.cpio.gz_pad.img`; **this project packages Duo with a
  separate initrd, not `CONFIG_INITRAMFS_SOURCE`-baked**, per explicit
  instruction — keep `CONFIG_INITRAMFS_SOURCE=""` when rebuilding)
- `wd_uboot.bin`, `bluecore.audio` — optional, same as Monarch

Not read by U-Boot's rescue path itself, but packaged alongside it for
deploying the same build's modules onto the full installed OS (SATA
rootfs): `modules.tar.xz`, tarred from *inside*
`INSTALL_MOD_PATH/lib/modules/` (i.e. `cd .../lib/modules && tar -cJf
modules.tar.xz .`) so the archive root is `./6.18.45+/...`, matching
Monarch's own `modules.tar.xz` structure exactly — extracting it with
`tar -C /lib/modules -xf modules.tar.xz` lands the version directory
directly at `/lib/modules/6.18.45+/`. An earlier build of this archive
was tarred one level up instead (rooted at `lib/modules/6.18.45+/...`),
which would have extracted to the wrong, doubly-nested path
(`/lib/modules/lib/modules/6.18.45+/`) — fixed to match Monarch's
convention.

Confirmed by a real vendor 4.9.330 rescue-mode boot log from this exact unit:
identical FDT-reserved-memory layout to Monarch (`addr=2200000 size=400000`),
so the board DTS uses the same `/memreserve/ 0x02200000 0x00C00000;` +
`chosen/linux,initrd-start/end` convention, unchanged from Monarch's recipe.

## Board DTS

`arch/arm64/boot/dts/realtek/rtd1296-wd-mycloud-home-duo.dts`, built on
mainline `rtd1296.dtsi` (same skeleton Andreas Färber's Synology DS418 board
uses) the same way Monarch's DTS builds on `rtd1295.dtsi`. All the generic
RTD129x-family blocks (i2c/mux_intc, USB drd+u2host+u3host, GMAC, thermal,
SMP, cpufreq/scpu_clk) are carried over from Monarch's DTS essentially
unchanged; only SATA, PWM channel assignment, and wakeup-gpio differ per the
vendor's own board-level DTS diff between Monarch and Duo.

## Progress log

### Dual-port AHCI glue (`drivers/ata/ahci_rtd1295.c`)

Extended with a per-compatible-string port-quirk table
(`realtek,rtd1296-ahci` → `ahci_rtd1295_ports_1296[]`, two entries) alongside
Monarch's existing single-port one. Register values — which CRT reset/bank
addresses and bits each port's `SATA_PHY_POW_n` reset lives at, which
`rtk_misc_gpio` number each bay's power line is on — are not guesses: they're
cross-checked against both the vendor `pelican-4.9.330` board DTS *and* a real
vendor 4.9.330 boot log from an actual two-bay Duo unit (see the comment above
`ahci_rtd1295_ports_1296[]` in the driver for the exact register trace:
`0xaf80e355 -> 0xaf80e375 -> 0xaf80e3f5` for port 0's CRT bank,
`0x81f -> 0xc1f -> 0xe1f -> 0xe9f` for port 1's).

Also added an explicit `SATA_n`/`SATA_ALIVE_n` CRT clock-gate enable
(CRT+0x0C bits 2/7 for port 0, CRT+0x10 bits 25/26 for port 1) — on Monarch
these were already enabled by the bootloader, but this Duo unit's rescue-mode
boot path left the AHCI HBA's own registers reading back as bus-fault poison
(`0xdeadbeef`) until this is done explicitly. This turned out **not** to be
the primary blocker (see below) but is kept as a correct, harmless
(no-op-if-already-set) defensive fix.

**Deliberately left source-divergent from symops/monarch-6.18's copy of
this file** (audited 2026-08-25, alongside every other driver shared
between the two ports -- everything else, `phy-rtk-sata.c` included as
of that same audit, came back byte-for-byte identical). Monarch's
driver never adopted this table-driven structure, the clock-gate
enable, or the shared `rtd129x_crt_lock` -- it doesn't need to, since
its `SATA_n`/`SATA_PHY_n`/clock-gate bits are already correct by the
time Linux boots there. Both drivers are independently proven on their
own real hardware; not unifying them since there's no bug driving it,
just a different (older, simpler) code shape on Monarch's side.

### SB2 bus-gate ordering bug (`drivers/phy/realtek/phy-rtk-sata.c`)

The actual root cause of the `0xdeadbeef`/stalled-MDIO symptom. This
mainline-style PHY driver's `.power_on` op opens an SB2 bus gate
(`sb2base`, PHY-instance-specific bits) that the MDIO bus segment behind
`MDIO_CTR`/`MDIO_CTR1`/`PHY_SPD` needs open to respond at all. But the
Linux PHY framework calls `.init` (which does ~63 sequential `MDIO_CTR`
writes per PHY) *before* `.power_on` (see `ahci_platform_enable_phys()`).
On hardware where that gate isn't already open by some other means, every
one of those `.init`-time MDIO writes stalls against the closed bus and
times out — silently, since `write_mdio_reg()`'s `-ETIMEDOUT` return is
never checked by its caller — logging `rtk-sata-phy: mdio busy` ~63 times
and stretching each PHY's `.init` to roughly 3.3 seconds. The AHCI
controller reset that follows then fails outright, reading the same
bus-fault poison value.

Confirmed via three consecutive real-hardware UART logs from this Duo unit:
the ~3.3s-per-port stall and the `0xdeadbeef` failure were byte-for-byte
identical before and after the clock-gate fix above, which is what pointed
at a different, order-of-operations bug rather than a missing clock. Monarch
never hit this because its board's SB2 gate bits happened to already be open
from boot (confirmed by devmem readback during that port's bring-up) — the
bug was latent there too, just never triggered.

Fix: factor the gate-open sequence into `phy_rtk_sata_sb2_gate_open()` and
call it at the start of `.init` as well as `.power_on`. OR'ing already-set
bits is a no-op, so this doesn't change Monarch's behavior.

**Status: applied, but confirmed on real hardware to have had zero
measurable effect** (see next section) — kept as a harmless no-op, but it
was not the real root cause. The vendor 4.9.330 source itself only opens
this SB2 gate from `.init` for chip families *other than* RTD129x
(`(chip_id & 0xFFF0) != CHIP_ID_RTD129X`); for RTD1295/RTD1296 it's only
opened from `.power_on`, same as this port originally had it — which in
hindsight should have been the tell that this wasn't the bug for this
chip family.

### SATA_n/SATA_PHY_n reset lines never deasserted (`drivers/ata/ahci_rtd1295.c`)

The actual root cause, found after the SB2-gate fix above produced a
byte-for-byte identical failure on the next hardware boot (same
~3.3s-per-port "mdio busy" stall, same `0xdeadbeef`). Re-reading
`ahci_rtd1295.c`'s own comment block turned up the answer already
recorded there from the original register trace but never acted on: the
vendor `ahci_rtk` driver's per-port reset-deassert loop touches *three*
CRT reset bits per port, not one — `SATA_n`, `SATA_PHY_n`, **and**
`SATA_PHY_POW_n` (port0: CRT+0x00 bits 5, 7, 10; port1: CRT+0x50 bits
10, 9, 7, from `include/dt-bindings/reset/rtd1295-reset.h`). This
driver was only ever deasserting the POW bit; `SATA_n`/`SATA_PHY_n`
were never touched anywhere in this port (the vendor's own DT binding
routes them as `resets = <...>` properties on `ahci_sata/sata-port@N`
in `rtd-129x-sata.dtsi`, a per-port pattern this board's DT doesn't
use, and `phy-rtk-sata.c` doesn't consume a `reset_control` for them
either).

Held in reset, `SATA_PHY_n` explains the continuous MDIO-busy stall
directly — the PHY's MDIO slave logic can't respond while its own
block is held in reset, independent of the POW bit, the SB2 gate, or
the clock gates (all of which were already being handled correctly).
`SATA_n` being held in reset then explains the AHCI controller reset
itself failing with the same poison value once PHY init gives up.

Why Monarch never hit this: Monarch's board boots its OS *from* SATA,
so its bootloader deasserts these bits as a side effect of its own
SATA bring-up before Linux ever runs. The Duo boots from eMMC — the
two SATA bays are RAID data storage only in the vendor design — so its
bootloader has no reason to touch SATA reset lines at all, and they
stay asserted from cold boot on both CRT banks (port 0's bank 1 *and*
port 1's bank 4), which is also why both ports failed identically
rather than just the Duo-specific second lane.

Fix: extend `ahci_rtd1295_port_quirk` with `crt_sata_bit`/`crt_phy_bit`
alongside the existing `crt_pow_bit`, and OR all three into the same
CRT reset-bank register in `ahci_rtd1295_port_quirks_apply()`, eagerly
in `probe()` — same pattern already used for the POW bit.

**Status: confirmed fixed on real hardware.** Next boot after this fix:
`masking port_map 0x3 -> 0x3` (no more `0xdeadbeef`), both PHYs report
`init phyN OK` almost instantly (no more MDIO-busy stall), and
`ata1`/`ata2: SATA link up 6.0 Gbps` — both bays (WDC WD5000AZLX-60K2TA0
×2) detected, partitioned, mdadm assembled the RAID1 arrays, a Debian
rootfs was found on `/dev/md1` and `switch_root` succeeded into it
(OpenRC/Gentoo userspace came up). Matches the vendor 4.9.330 reference
log exactly. This was the last blocker for basic SATA bring-up on this
board.

### Hardware RTC (`drivers/rtc/rtc-rtd119x.c`) -- battery confirmed present, wired up

The flash-backed RTC investigation (above) concluded this unit's boot
SPI-NOR doesn't respond, closing that path. Separately, the user
confirmed a physical coin-cell battery is visible on the Duo board --
new information, and different from Monarch (RTD1295 single-bay),
where real hardware testing already established no battery is
present. That reopens the SoC's own hardware RTC counter block as a
real candidate, since it's exactly the kind of thing a board-level
battery would be wired to.

Checked what's needed: mainline already has a complete driver for this
block (`drivers/rtc/rtc-rtd119x.c`, `"realtek,rtd1295-rtc"`) --
self-contained, no SoC-specific glue needed. It reads `RTCACR.RTCPWR`
at probe to distinguish a genuinely-retained power domain (skips
reset, trusts the existing counter value) from a true cold start
(resets and zeroes) -- exactly the behavior a battery-backed block
needs, already built in. Neither Monarch's nor Duo's DTS had ever
wired this node up at all (Monarch uses the flash-backed scheme
instead; the underlying hardware register block was simply never
described in either port's device tree). Added `rtc@600` to the
shared `rtd129x.dtsi` (real SoC hardware, so it belongs there like
`wdt@680` right next to it, `status = "disabled"` by default), enabled
for the Duo board specifically. No dedicated CRT clock-gate driver
exists for this block (same gap as `wdt`/`uart0`), so it uses
`&osc27M` like they do. `CONFIG_RTC_DRV_RTD119X=y`.

This is a low-risk change to test compared to the eMMC DMA work above
-- no DMA, no bus mastering, just a handful of `readl`/`writel` calls
on a small always-on register block with no interrupt involvement.
Worst case if the battery isn't actually wired to this specific
block's rail: it behaves exactly like Monarch's never-enabled counter
(resets to the base year every boot, harmless).

**Status: three power-cycle tests failed at the wrong address; root
cause found and fixed, awaiting a confirmation test.**

- `putty.log.84`: `hwclock -w` + full power-off/power-on -> time reset
  to 2014 (cold start, `RTCACR.RTCPWR` clear).
- `putty.log.85`: repeated -- identical raw register dump
  (`RTCACR=0x00 SEC=0x00 MIN=0x00 HR=0x15 DATE1/2=0x00 EN=0x00`) across
  *two* separate power cycles with real elapsed time and a `hwclock -w`
  in between. A live, battery-retained counter should have shown
  *some* difference; getting the exact same bytes twice looked more
  like a frozen, non-counting block. Checked the vendor's clk driver
  and found `RTD1295_CRT_CLK_EN_MISC_RTC` at CRT+0x10 bit 10 -- a
  clock gate neither port had ever wired up -- as a candidate.
- `putty.log.86`: poked that gate directly -- turned out to already be
  enabled (bootloader default), ruling it out. But the poke also
  revealed the counter *does* respond to writes: after `hwclock -w`,
  reads started failing with `RTC_RD_TIME: Invalid argument`, then
  eventually succeeding with plausible-but-inconsistent values.
- `putty.log.87`, with more patient/verbose read diagnostics: the
  pattern became unambiguous. `SEC` stayed frozen at `0x00` for the
  entire session; the field this driver reads as `HR` climbed by
  roughly 1 per second of *real* elapsed time. Whenever that raw value
  (masked 0-31) exceeded 23, the kernel's `rtc_valid_tm()` correctly
  rejected it as an invalid hour, producing exactly the `RTC_RD_TIME`
  failures seen.

That pattern -- a genuinely-ticking ~1Hz field landing where "hour"
should be, with "seconds" dead -- pointed at a *wrong base address*,
not a broken counter. The user then independently confirmed real-time
persistence works correctly on the **stock vendor 4.9.330 kernel** on
this exact same physical unit -- direct proof the hardware, including
the battery, is fine. That reframed the whole investigation: the bug
had to be in address resolution, since the vendor and mainline drivers
use identical internal register offsets (`SEC/MIN/HR/DATE1/DATE2` at
`+0x00/+0x04/+0x08/+0x0c/+0x10`, confirmed byte-for-byte against the
vendor's `rtc-rtk.c`).

Checked the vendor 4.9.330 pelican tree's `rtd-129x.dtsi` directly for
where its own equivalent `rtc@600` node is actually nested: **`&misc`**
(`0x9801b000` base), not `&iso` (`0x98007000` base) -- which is where
this port's node had been placed, resolving to `0x98007600` instead of
the correct `0x9801b600`. In hindsight, `RTD1295_CRT_CLK_EN_MISC_RTC`'s
own name already said as much. `0x98007600` is a real, different
ISO-domain register; everything observed across all three test rounds
(the frozen pattern, the later ~1Hz-in-the-wrong-field behavior) was
this port reading *something else* in the ISO block, never the actual
RTC. Fixed by moving the node to `&misc`, matching the vendor exactly.

**Confirmed working on real hardware** (`putty.log.88`): two
consecutive full boots both show `RTCACR=0x80` (warm start -- the
power domain genuinely stayed retained across the intervening power
cycle), `SEC` ticking cleanly and correctly, and the system clock
restored automatically at boot to the correct real UTC time both
times -- zero `RTC_RD_TIME` failures anywhere in the log. The `&misc`
placement fix was correct and sufficient; nothing else needed
changing. All diagnostic code added while chasing this
(`drivers/rtc/rtc-rtd119x.c`: pre-reset register dump, the CRT_CLKEN2
poke, the patient/verbose read-retry loop) has been removed --
the file is back to being byte-identical to pristine mainline; the fix
lives entirely in the board DTS.

### cpufreq OPP table (`drivers/clk/realtek/clk-rtd129x-scpu.c`) -- confirmed on real Duo hardware

Carried over unchanged from Monarch: three divider/PLL OPPs
(300.375/600.75/1100 MHz) driven through `scpu_clk`, a forward-ported
single-purpose clock driver (from the vendor's generic `clk_pll_div`
framework) that reprograms the CRT `PLL_SCPU` block directly --
`SCPU_DIV_REG` (post-divider, `÷1/÷2/÷4`) and `SCPU_SSC_FREQ_REG` (PLL
N/F), both looked up from exact copies of the vendor's discrete
frequency/divider tables, with anti-glitch ordering (divider changes
before or after the PLL step depending on whether the transition is
speeding up or slowing down) also copied from the vendor driver.
`recalc_rate()` never caches a value -- it always re-reads the live
hardware registers and matches them against the tables -- so if a
board's bootloader ever programs a PLL/divider combination not present
in either table, the driver can't identify the current rate at all
(returns 0), which is the specific risk this Duo unit hadn't been
checked for yet: Monarch's own board comment already documents its
bootloader's boot point (`PLL_SCPU` at approximately 1201.5 MHz,
post-divider `÷2`) as the basis for trusting these OPPs; Duo's
bootloader is a different binary on different board firmware, so nothing
guaranteed it left the same values behind.

`CONFIG_CPUFREQ_DT=y` + `CONFIG_CPUFREQ_DT_PLATDEV=y` + `schedutil`
governor are already enabled by default, meaning cpufreq had already
been actively scaling the CPU on every single hardware boot done this
session (RTC tests included) without any reported hang -- reassuring,
but not by itself proof the rate identification was correct rather than
silently wrong.

Checked with read-only commands on the already-running hardware (no
rebuild needed):

```
dmesg | grep -iE "cpufreq|scpu"
  -> cpufreq_policy_online: CPU0: Running at unlisted initial frequency:
     600000 kHz, changing to: 600750 kHz
cat scaling_cur_freq / cpuinfo_cur_freq -> 1100000 (both agree)
cat scaling_available_frequencies -> 300375 600750 1100000
cat scaling_governor -> schedutil
```

This confirms two things at once. First, `recalc_rate()` *did*
successfully decode the boot-time registers on this unit -- to exactly
600000 kHz (1200 MHz PLL entry / div 2), not the 1201.5 MHz Monarch's
comment describes, but still a table hit, not a miss; the "unlisted
initial frequency" message is just cpufreq-core's normal startup step
of snapping an exact-but-not-table-listed rate to the nearest declared
OPP (600750 kHz here), harmless and expected on plenty of platforms.
Second, and more importantly, by the time these commands ran `schedutil`
had already driven a real scale-up to the top OPP (1100000 kHz) under
normal load -- the single riskiest code path in the driver, since it's
the only transition that both reprograms the PLL frequency register
*and* changes the post-divider (÷2 -> ÷1, anti-glitch ordered: PLL step
first, divider step after, since this is a speed-up). `cpuinfo_cur_freq`
reads back 1100000 straight from live hardware after that transition,
matching what was requested, with the board still fully responsive over
UART. That exercises the entire OPP range end-to-end with no hang and no
mismatch between requested and actual rate.

**Confirmed working on real hardware.** No code change needed -- the
existing OPP table is safe on this unit too; only the stale
"not yet confirmed" DTS comment and README note needed updating.

### USB PHY calibration (`drivers/phy/realtek/phy-rtk-usb2.c`/`phy-rtk-usb3.c`) -- confirmed on real Duo hardware

Open backlog question: this port's three USB PHY nodes (`usb2phy_drd`/
`usb3phy_drd`, `usb2phy_u2`, `usb2phy_u3`/`usb3phy_u3`) all use the
mainline driver's generic silicon-level default calibration table
(`rtd1295_phy_cfg`, selected by the shared `"realtek,rtd1295-usb2phy"`/
`"realtek,rtd1295-usb3phy"` compatible strings -- the same table
Monarch uses, not board-specific). The vendor's own board DTS
(`rtd-1296-pelican-1GB.dts`) instead carries explicit per-board
`phy_data_page0`/`page1` byte tables for `dwc3_u3host_usb3phy`/
`usb2phy` specifically. Question: does skipping those board-tuned
tables cost anything on real Duo hardware, or are the mainline
driver's generic defaults (plus its optional
`realtek,driving-level`/`-compensate`/efuse DT overrides, none of
which are set here either) already good enough?

This can only be answered by real hardware, not by reading code --
it's a signal-integrity/tuning question, not a functional-correctness
one; a link that trains at all doesn't prove it's trained at full
speed. Checked via `dmesg` on real hardware, matching each xHCI
instance's `io mem` address back to its DT node:

- `xhci-hcd.0.auto` (io mem `0x98020000`) and `xhci-hcd.1.auto` (io mem
  `0x98029000`) are `drd`/`u2host`.
- `xhci-hcd.2.auto` (io mem `0x981f0000`) is `u3host`
  (`usb_u3host: usb@1f0000`) -- confirmed by matching the unit address.
  It registers *two* buses: a USB2 root hub and a USB3 root hub
  ("Host supports USB 3.0 SuperSpeed").

With a device plugged into that specific port: `usb 5-1: new
SuperSpeed USB device number 2 using xhci-hcd` -- bus 5 being
`xhci-hcd.2.auto`'s SuperSpeed root hub. That's a real 5 Gbps link
negotiated and held, with no link-training errors anywhere in the log.
The generic mainline default calibration is sufficient for `u3host` at
full USB 3.0 speed on this specific board; the vendor's board-specific
tables were never needed.

**Status: confirmed working on real hardware.** No DTS/driver change
needed -- the existing default calibration is safe and sufficient on
this unit too; only the stale "not yet confirmed" DTS comment and
README note needed updating.

### `.config` re-aligned with Monarch

Diffed this port's `.config` against `symops/monarch-6.18`'s directly
(neither tree tracks `.config` in git -- it's gitignored on both, so
this means the actual working-tree file on disk, not a committed
artifact). Found only 8 differing symbols; 5 of them --
`CONFIG_LOCKUP_DETECTOR`, `CONFIG_SOFTLOCKUP_DETECTOR`,
`CONFIG_DETECT_HUNG_TASK` (+ its `_BLOCKER` and
`CONFIG_DEFAULT_HUNG_TASK_TIMEOUT` dependents), and
`CONFIG_WQ_WATCHDOG` -- turned out to be leftover diagnostics, not a
real Pelican-specific need: they were switched on specifically to
catch the eMMC IDMAC hang (see "eMMC IDMAC hang, third attempt" above
-- this is exactly what caught the `CPU#0 stuck for 44s!` softlockup
report) and never turned back off once that investigation closed.
Disabled all five (`scripts/config --disable ...` +
`make olddefconfig`) to match Monarch again.

The remaining 3 differences are genuine, permanent Pelican-specific
hardware support, correctly config-side of the port and left as-is:
`CONFIG_MMC_DW`/`CONFIG_MMC_DW_PLTFM`/`CONFIG_MMC_DW_RTD129X` (eMMC),
`CONFIG_RTC_DRV_RTD119X` (battery-backed hardware RTC, absent on
Monarch), `CONFIG_SENSORS_PWM_FAN` (Duo's cooling fan, absent on
Monarch's single-bay board). Rebuilt `Image`/`dtbs`/`modules` and
repackaged all of `usb-payload/` (DTB unchanged, `.dtb` hash confirmed
identical -- DTS wasn't touched, only `.config`).

### DTB padding (`rescue.emmc.dtb`) -- explains `FDT_ERR_NOSPACE` in `putty.log.90`

User noticed the packaged `rescue.sata.dtb` (Monarch, 29083 bytes) and
`rescue.emmc.dtb` (Duo, 13765 bytes at the time) differ by more than
2x and asked why. Turned out to be nothing structural: decompiling
both (`dtc -I dtb -O dts`) shows comparable node counts (Duo's is
actually the larger tree, as expected -- more hardware), and rebuilding
Monarch's `.dtb` fresh from its own current `symops/monarch-6.18`
source (`make dtbs` in that tree, a gitignored build artifact only, no
source touched) produces **12699 bytes** -- smaller than Duo's, not
bigger. The packaged 29083-byte copy carries exactly 16384 (0x4000)
bytes of trailing zero padding that isn't in a clean rebuild's output
and isn't explained by anything in that repo's Kconfig/Makefile/git
history -- a stale, undocumented artifact from some earlier ad-hoc
build, never refreshed since.

That accidental padding is likely why Monarch's rescue boot never
showed the exact problem this port's own `putty.log.90` did:
`libfdt fdt_add_subnode(): FDT_ERR_NOSPACE` /
`WARNING: could not set linux,stdout-path FDT_ERR_NOSPACE` -- U-Boot's
rescue path tries to grow the FDT in place on every boot (add a
`/factory` subnode, set `linux,stdout-path`) and needs free space at
the end of the blob to do it without a reallocation libfdt doesn't
attempt. Duo's `.dtb` had zero headroom for that.

Added the same 16 KiB of intentional headroom, properly this time --
via dtc's own padding flag on this board's build target
(`DTC_FLAGS_rtd1296-wd-mycloud-home-duo := -p 0x4000` in
`arch/arm64/boot/dts/realtek/Makefile`), not a manual post-build hack.
Verified the padded `.dtb` (30149 bytes) decompiles to the exact same
tree as before (`dtc -I dtb -O dts`, diff clean apart from stderr
ordering) -- the extra space is pure trailing zero padding, confirmed
byte-for-byte, no structural change. Only `rescue.emmc.dtb` needed
repackaging; `Image`/`modules`/the separate initrd are untouched by a
DTB-only change.

**Confirmed working on real hardware** (`putty.log.92`): U-Boot's
`Info: Try to add new node /factory...` succeeded this time, with the
real node content printed (`bootstate`, `serial`, `ipaddr`, `ethaddr`,
...) and no `FDT_ERR_NOSPACE`/`linux,stdout-path` warnings anywhere in
the log -- a clean improvement over `putty.log.90`'s failure at the
exact same step. Rest of the boot unaffected, as expected for a
DTB-only change (RTC, eMMC, both SATA bays all still fine).

### Fan wired into the thermal governor

`fan0` (`pwm-fan`) was confirmed working at the raw PWM level (see
Confirmed working below) but stayed at a fixed ~20% duty -- declared
with `cooling-levels` yet never referenced from `soc-thermal`'s
`cooling-maps`, so nothing ever actually drove it.

Checked the vendor for real threshold values to cross-check against,
as usual -- and came up empty: `rtk_fan.c` has no in-kernel thermal
curve at all, just a manual 0-10 `fan_ctrl_speed` sysfs knob plus tach
RPM readback (`fan_speed`), presumably meant to be driven by a
userspace daemon this port has no source for. So unlike every other
threshold in this port, the two new trip points are this port's own
choice, not a cross-checked vendor value:

- `fan-alert0` at 55C -> `cooling-device = <&fan0 1 1>` (fan state 1,
  the ~20% level -- the same duty it already ran at unconditionally).
- `fan-alert1` at 70C -> `cooling-device = <&fan0 2 2>` (state 2, the
  ~50% level).

Both below `soc_hot`'s existing 85C passive CPU-throttle trip, so
active cooling gets a chance to work before the CPU ever needs to slow
down -- sensible for an enclosure with two spinning SATA drives, but
the actual temperatures are a first guess pending real hardware data.
Each is a hard step (min=max=state), the standard mainline idiom for
multi-level `pwm-fan` cooling maps, rather than a proportional range.

The vendor's separate tach/control IP (`rtk_fan@9801BC00`) is still
not ported, so this remains open-loop -- no RPM feedback, just PWM
duty driven by temperature.

**Confirmed working on real hardware** (`putty.log.93`): all four trip
points read back correctly (`active 55000`/`active 70000`/
`passive 85000`/`critical 105000`), `cooling_device0` correctly
identifies as `pwm-fan` (`max_state=2`), and forcing `trip_point_0_temp`
below the actual reading (`43643`, i.e. 43.6C) drove `cooling_device0`'s
`cur_state` from `0` to `1` within the 2s poll -- the governor is
correctly wired end-to-end. `cooling_device1` (`cpufreq-cpu0`, bound to
the separate `soc_hot` trip) stayed at `0` throughout, confirming the
two cooling-maps entries are correctly isolated from each other. DTB-only
change (trip points and cooling-maps, no driver/Kconfig touched);
repackaged `rescue.emmc.dtb`.

### Fan tachometer (RPM feedback, `drivers/hwmon/rtd129x-fan-tach.c`)

Ported the last open item: the vendor's `rtk_fan@9801BC00` tach/counter
block, a separate IP from the PWM controller `fan0` already uses. New
standalone hwmon driver (`"realtek,rtd1295-fan-tach"`) rather than
extending `pwm-fan` or `rtk_fan.c` itself, since the two concerns are
genuinely independent here: this port's PWM *control* path (generic
mainline `pwm-fan`, already confirmed working and wired into
`cooling-maps`, see above) doesn't need or want touching, and the
vendor's own `rtk_fan.c` bundles RPM readback *and* its own redundant
PWM control together -- only the RPM half was needed.

Confirmed register-for-register against `drivers/soc/realtek/common/
rtk_fan.c`: a hardware window counter -- `REG_TIMER_TV` sets the window
length in 90kHz reference-clock ticks, `REG_COUNTER_CV` latches the tach
pulse count for the just-completed window, and an interrupt fires on
completion. RPM = `counter * (90000 * 60 / fan_factor) / timer_target`,
computed in the IRQ handler exactly like the vendor driver's own
`rtk_fan_compute_speed()`. Exposed as a standard `hwmon` `fan1_input`
(RPM) via `devm_hwmon_device_register_with_info()` -- read-only, no PWM
duplication.

Three things needed raw register pokes, no mainline provider existing
for any of them (same situation as every other CRT/pinmux-touching
driver in this port):

- **Clock gate**: `CRT+0x10` bit 29 (`RTD1295_CRT_CLK_EN_FAN`) -- the
  *same* register `drivers/ata/ahci_rtd1295.c`'s port-1 clock-gate
  write and this port's own RTC diagnostic work already touch, so
  protected by the existing shared `rtd129x_crt_lock`
  (`EXPORT_SYMBOL_GPL`'d from `ahci_rtd1295.c`), same pattern
  `dw_mmc-rtd129x.c` already uses for its own `CRT+0x0C` access.
- **Pinmux**: SB2 crossbar (`0x9801a000` -- the same physical block
  mainline's own `rtd129x.dtsi` already declares as `sb2:
  syscon@1a000`, unused elsewhere in this port) offset `0x910`, bits
  `[23:22]` set to `0x2` -- confirmed against the vendor's own
  `pinctrl-rtd129x.h` pmux table (`pmux_base=PMUX_BASE_SB2,
  pmux_regoff=0x910, pmux_regbit=22, pmux_regbitmsk=0x3`) and
  `rtd-1295-pinctrl.dtsi`'s `dc_fan_sensor_pins` node
  (`RTK_FUNCTION(0x2, "dc_fan_sensor")`) -- routes the physical tach
  sensor pin ("gpio_9" in vendor naming) into the counter.
- **Reset**: unlike the above two, this one *does* have a mainline
  provider already usable as-is -- `RTD1295_RSTN_FAN` (bit 11) lives in
  the same CRT reset bank as the already-declared-but-previously-unused
  `reset4: reset-controller@50` node in `rtd129x.dtsi` (confirmed via
  the vendor's own `rtd1295-reset.h`: `RTD1295_CRT_RSTN_FAN =
  RTD1295_CRT_RSTN_REG_BANK_4 | 0x0b`, and `BANK_2 | 0x0b` there
  independently confirms this port's existing raw `CRT+0x04` bit 11
  poke for `RTD1295_CRT_RSTN_EMMC` in `dw_mmc-rtd129x.c` was correct
  too) -- so this is a plain `resets = <&reset4 RTD1295_RSTN_FAN>;` +
  `reset_control_deassert()`, no custom lock needed since nothing else
  in this port's tree currently touches `reset4`.

DT node placed under `&misc` in the board DTS (not the shared
`rtd129x.dtsi`, unlike `&hwrtc`): it needs `mux_intc` for its
interrupt, and `mux_intc` itself is declared per-board in this project
(copied into each board's own DTS, not factored into the shared file --
see `mux_intc: interrupt-controller@1b000` near the top of this file),
so board-level is where every other `mux_intc` consumer already lives.
`timer_target`/`fan_debounce`/`fan_factor` are the vendor's own DT
property names and this board's own values, carried over unchanged
from `rtd-129x.dtsi`'s shared `rtk_fan` node.

**Confirmed working on real hardware**: `/sys/class/hwmon/hwmon1/
fan1_input` reads real, physically plausible RPM values that update
over time as expected for a window counter (`1091` -> settles at
`1261` across several quick reads landing in the same ~11.6s window
[`timer_target=0x100000` @ 90kHz] -> `2265` once the window rolls
over) -- clock gate, pinmux, reset, IRQ, and the RPM computation are
all confirmed correct end to end. This was the last open item in the
device-support backlog; nothing remains.

### Docker/nftables kernel config support

Checked against upstream Docker's own `contrib/check-config.sh`
requirements list (same pass done for `symops/monarch-6.18`, see that
repo's README.md's Config section). The baseline `.config`
(already largely mirrored from Monarch, see the entry above) already
covered essentially everything in the "required" tier and most of the
nftables family too (`NF_TABLES`, `NF_TABLES_INET`, `NFT_NAT`/`MASQ`/
`REDIR`/`COMPAT`/`REJECT*`/`CT`/`LOG`/`LIMIT`/`HASH`/`NUMGEN`, all
`=m`) — only a handful of genuinely-missing, genuinely-available
options needed adding: `CGROUP_PERF` (bool, `=y`),
`BTRFS_FS_POSIX_ACL` (bool, `=y`, `BTRFS_FS` was already `=m`), and as
modules matching the existing `NF_TABLES=m` pattern: `NFT_FIB_IPV4`,
`NFT_FIB_IPV6`, `NFT_FIB_INET`, `NFT_QUOTA`, `NFT_CONNLIMIT`,
`IP_SCTP`. (A few other items the script checks --
`SECURITY_SELINUX`/`SECURITY_APPARMOR`, `DEVPTS_MULTIPLE_INSTANCES`,
`IOSCHED_CFQ`, `NETPRIO_CGROUP` -- are either out of scope for this
minimal embedded config or no longer exist as separate options on this
kernel version; `CGROUP_NET_PRIO` already covers the modern equivalent
of `NETPRIO_CGROUP`.) Applied identically to both boards' `.config` to
keep them aligned. Not yet tested with an actual Docker/container
workload on real hardware.

### Base version bump: v6.18.45 -> v6.18.46

Rebased onto the latest upstream stable point release, following this
file's usual procedure but with one correction: the merge-base must be
the *previous* point release actually merged (`v6.18.45`), not the
port's original starting tag (`v6.18`) -- using `v6.18` produced
hundreds of bogus conflicts across completely unrelated files (drm,
mptcp, xfs, ...), since `git merge-tree` would otherwise try to replay
the entire v6.18.0->v6.18.45 upstream delta a second time against a
tree that already contains it. With `--merge-base=v6.18.45` the merge
was clean (215 files, ~2500/1200 lines, none of them this port's own)
and none of this port's own files were touched. `.config` needed no
changes (`olddefconfig` added nothing new). Rebuilt `Image`/`dtbs`/
`modules` and repackaged `usb-payload`; not yet hardware-tested.

**First v6.18.46 hardware test: total CPU0 interrupt loss during AHCI
bring-up** (previously not observed at v6.18.45 -- see the "Confirmed
working" entry below, which passed this exact sequence cleanly). Boot
proceeds normally through `ahci_rtd1295_quirk_init()` (`rx error
select to mac original`) and, 800ms later, into
`ahci_rtd1295_init_work()`'s `ahci_platform_init_host()` call
(`masking port_map`, `AHCI vers...`, `2/2 ports implemented`, `flags:
...`), then port 0's `ahci_port_start()` prints `port 0 is not
capable of FBS` -- and nothing else ever prints again. Port 1's own
(otherwise unconditional) `port 1 is not capable of FBS` line, present
in every prior successful boot including the v6.18.45 test above,
never appears. ~9.6s later `rcu: rcu_preempt self-detected stall on
CPU` fires: `CPU0`, `Comm: swapper/0`, `pc : arch_local_irq_enable`,
`do_idle -> cpu_startup_entry -> kernel_init` -- CPU0 went idle and
then never received *any* interrupt again (not even its own timer
tick), while presumably CPU1-3 kept running. This is the exact
signature the eMMC DMA hang produced (see the "eMMC hang, six
hardware-tested hypotheses" entries above) -- a general RTD129x
failure mode where some hardware sequence wedges interrupt delivery to
CPU0 specifically, not something a single driver's register-write
ordering can explain on its own.

Checked the v6.18.45->v6.18.46 upstream delta for a plausible cause:
only one commit touches `drivers/ata/` in the whole 215-file diff
(`4c8d7595a`, "ata: libata-scsi: terminate deferred commands on time
out") -- and it only affects SCSI EH command-timeout handling, a path
that cannot execute this early in boot (no block device exists yet,
so no SCSI command has ever been queued). No commits touch `kernel/
dma/`, `mm/`, `drivers/of/`, `drivers/base/`, `arch/arm64/kernel/`,
`arch/arm64/mm/`, `drivers/irqchip/`, or `kernel/irq/` in this delta
either. The `rtd129x_crt_lock` fix (shared spinlock around
`ahci_rtd1295.c`'s and `dw_mmc-rtd129x.c`'s `CRT+0x0C` read-modify-
write, see its doc comment) is present and correctly used on both
sides, so this isn't the already-diagnosed lost-clock-write race
either -- at least not in the form previously fixed.

Given no plausible upstream regression exists in this delta and the
identical sequence passed cleanly on the immediately-preceding
v6.18.45 test, this looks more like a recurrence of the SoC's known
CPU0-interrupt-loss fragility (already seen once, for an unrelated
reason, during the eMMC bring-up work) than a new v6.18.46 bug --
but one data point can't distinguish "always happens now" from "an
occasional hardware race that happened to trigger this boot".
**Needs a repeat test before drawing conclusions or attempting a
fix.**

**Second v6.18.46 hardware test: reproduced, but at a completely
different point.** Same build (`#37`), rebooted from scratch. This
time the log never even reaches `Freeing unused kernel memory`/`Run
/init as init process` -- it hangs during the kernel's own early boot,
inside `ahci_rtd1295`'s built-in deferred-probe retries (the driver is
built in, not a module; it gets probed, fails with `-EPROBE_DEFER`
because `phy-rtk-sata` isn't registered yet, and the driver core
retries it a few times during early boot). The last thing printed is
`ahci_rtd1295_port_quirks_apply()`'s own `port 1: sata clocks
enabled...` line from its *second* probe attempt; the same
`arch_local_irq_enable`/`do_idle`/CPU0-loses-everything stall follows
~21s of silence later. (Note the identical `do_idle -> kernel_init ->
console_on_rootfs -> __primary_switched` call trace in *both* tests is
not itself meaningful -- that's just CPU0's normal one-time fall-through
into the idle loop early in boot after the boot CPU hands off to
`kernel_init`/the secondary CPUs; every later idle-in cycle reuses the
same static frame, so it doesn't pinpoint *when* interrupts actually
died, only that CPU0 was idling when they did.)

The two tests hung at genuinely different code paths (`init_work`'s
`ahci_platform_init_host()` / `ata_host_register()`'s per-port
`ahci_port_start()` loop the first time; `ahci_rtd1295_probe()`'s own
`ahci_rtd1295_port_quirks_apply()` register-poke loop, during a
deferred-probe retry, the second time) -- both close to `ahci_rtd1295`
activity, both ending in the same CPU0-total-interrupt-loss signature,
but at different depths and different wall-clock offsets. That spread
is itself informative: a fixed single-instruction bug would land at
the same PC/offset every time, so this looks like a genuine
timing-dependent race rather than one deterministic bad register
write.

Added fine-grained `dev_info()` diagnostics bracketing every register
write in `ahci_rtd1295_port_quirks_apply()` and every step of
`ahci_rtd1295_probe()`/`ahci_rtd1295_init_work()` (see git history) --
pure logging, no behavior change, safe to test regardless of whether
it reproduces again. Goal: pin down the exact register write (or lack
of one) that immediately precedes CPU0 going silent, across whichever
of the two hang sites (or a third) shows up next. Rebuilt and
repackaged; not yet hardware-tested with the new diagnostics.

**Third v6.18.46 hardware test: diagnostics pinpointed the hang to a
narrow window inside mainline `ata_host_start()`/`ahci_port_start()`.**
`ahci_rtd1295_probe()` now completes cleanly every time (all `diag:
probe: ...` lines print, `init_work scheduled, returning`). 800ms
later `ahci_rtd1295_init_work()` fires, calls
`ahci_platform_init_host()`, which gets as far as `masking port_map`/
`AHCI vers...`/`2/2 ports implemented`/`flags: ...`, then port 0's
`ahci_port_start()` prints `port 0 is not capable of FBS` -- and
nothing else ever prints, including our own `diag:` lines that used to
fire reliably. That pins the hang to *after* that print and *before*
either port 1's `ahci_port_start()` entry or the next event in
`ata_host_start()`'s per-port loop (`port_start()` returning ->
`ata_eh_freeze_port()` -> next port's `port_start()`), none of which
had diagnostics yet.

Also got an NMI backtrace this time (`CONFIG_SOFTLOCKUP_DETECTOR`'s
periodic re-check sent one from CPU 1): CPU0's PC was still
`arch_local_irq_enable`/`do_idle` **while responding to the NMI**,
confirming CPU0 is genuinely parked in `WFI` and does react to
cross-CPU SGI-class notifications -- it just never receives its own
periodic tick (or any other interrupt) again afterward. So this isn't
a total GIC-distributor death; something more specific to CPU0's own
interrupt wake-up path (most likely the architected timer PPI) stops
working, timed to something in this narrow AHCI window.

Added three more diagnostic points to pin this down further, all pure
`dev_info()` logging in otherwise-unmodified mainline files (temporary,
for this investigation only): `ata_host_start()`'s per-port loop in
`libata-core.c` (before/after `port_start()`, before/after
`ata_eh_freeze_port()`), `ahci_port_start()`'s entry and its
`dmam_alloc_coherent()` call in `libahci.c`, and `ahci_freeze()`'s
entry/exit (also `libahci.c`, called via `ata_eh_freeze_port()` right
after each port's `port_start()`). Rebuilt and repackaged; not yet
hardware-tested.

**Fourth v6.18.46 hardware test: found it -- the hang is genuinely
inside `dmam_alloc_coherent()` itself.** Reproduced identically twice
again (two boot cycles in one test session). Both times the very last
line printed is `diag: ahci_port_start: port 0: calling
dmam_alloc_coherent` -- the paired "returned" print added right after
that call never fires. So execution is parked *inside* the DMA-coherent
allocation for port 0's command/FIS buffers (a modest ~92KB request,
`AHCI_PORT_PRIV_DMA_SZ`), the very first such allocation this driver
ever makes on this boot.

Traced through `dma_direct_alloc()` (`kernel/dma/direct.c`): since
none of this SoC's DT nodes set `dma-coherent` (checked -- not even
the USB xHCI controllers, which allocate DMA-coherent memory heavily
and never hang), every device goes through the non-coherent path.
With `CONFIG_DMA_DIRECT_REMAP=y` and `CONFIG_ARCH_HAS_DMA_SET_UNCACHED`
*not* set, that means: `dma_alloc_contiguous()` (CMA, since
`CONFIG_DMA_CMA=y`) or a page-allocator fallback, followed by
`dma_common_contiguous_remap()` (a `vmap`-style non-cached remap with
a TLB-flush broadcast). Either step could plausibly block for a
device-specific reason at *this* point in boot (memory/CMA state has
had 13+ seconds to evolve since the early-boot allocations that all
succeed instantly), so added `pr_info()` tracing (filtered to
`dev_name(dev) == "9803f000.sata"` to avoid flooding the log with
every other device's allocations) bracketing both `dma_alloc_contiguous()`
and `dma_common_contiguous_remap()` in `kernel/dma/direct.c`, to see
which of the two never returns. Rebuilt and repackaged; not yet
hardware-tested.

(Re-examined the earlier "CPU0 loses all interrupts" framing in light
of this: CPU0's `do_idle`/`arch_local_irq_enable` PC across every
stall report is consistent with CPU0 *correctly* idling with nothing
runnable -- the kworker running `ahci_platform_init_host()` is blocked
inside `dmam_alloc_coherent()` waiting on something that never
completes, and CPU0 has no other work, so it legitimately parks in
WFI between ticks. That's why the NMI backtrace IPI still gets
answered promptly: the GIC and CPU0's own interrupt handling are both
fine, only the specific condition the kworker's sleep depends on is
what's stuck.)

**Fifth v6.18.46 hardware test: with the round-4 tracing in place, the
hang did not reproduce at all -- 2/2 clean boots.** Both `port 0` and
`port 1`'s `dmam_alloc_coherent()` completed and printed their
"returned" lines, `ahci_platform_init_host()` returned `0`, and both
boots went all the way through: `ata1`/`ata2: SATA link up 6.0 Gbps`,
both `WDC WD5000AZLX-60K2TA0` drives identified and partitioned,
network up, DHCP lease obtained, NTP set, USB-rescue FTP listening,
shell prompt reached. Four hangs in a row (tests 2-4) followed by two
clean boots in a row, right after adding more `pr_info()` calls to the
hot path, is a strong signal for a genuine timing-dependent race: the
extra tracing's own overhead (UART transmission at 115200 baud plus
the extra function-call cost, all on the boot path between probe and
this allocation) most likely nudged the timing enough to dodge
whatever window causes the hang, rather than the hang being fixed by
anything the tracing actually *revealed*.

Not claiming this is fixed -- two clean boots isn't enough to
distinguish "the added latency reliably avoids the race" from
"got lucky twice." The diagnostics are staying in for now (removing
them risks bringing the timing back to where the hang reproduces).
Next step is simply more reboots of this exact build to see whether
it keeps booting cleanly; if it does across several more runs, the
next move would be to bisect *which* specific added print (or the
sheer number of them) matters, with an eye toward eventually replacing
the debug prints with a deliberate, minimal, well-understood delay (or
a real fix, if the actual race can be pinned down) rather than shipping
diagnostic printk spam as the de facto workaround.

**Sixth v6.18.46 hardware test: two more clean boots, same build.**
Now **4/4 clean boots** with the round-4 tracing in place, against
**4/4 hangs** in the two sessions before it -- the same pattern as the
fifth test (both ports' `dmam_alloc_coherent()` return, `ata1`/`ata2:
SATA link up 6.0 Gbps`, full boot to shell). This is a large enough
sample flip (8 boots total, a clean split before/after the round-4
diagnostics were added) that "got lucky" is no longer a comfortable
explanation -- the added tracing overhead looks like it's reliably
steering clear of the race window. Still not a real fix (don't know
*why* the extra latency avoids it, or which specific print matters
most), but confident enough now to prioritize bisecting it down to a
minimal deliberate delay over collecting more identical data points.

One useful narrowing already available from the existing data, without
a new build: the round-1 (`ahci_rtd1295.c` probe/quirks) and round-2
(`libata-core.c`/`libahci.c` `ata_host_start`/`ahci_port_start`/
`ahci_freeze`) diagnostics were *already present* during both of the
test-4 (`putty.log.103`) hangs -- they did not help. Only after also
adding the round-3 `kernel/dma/direct.c` tracing (bracketing
`dma_alloc_contiguous()` and `dma_common_contiguous_remap()`, plus two
new `#include`s) did the hang stop reproducing, tests 5 and 6. That
points the likely timing-sensitive spot specifically at
`dma_direct_alloc()`/`__dma_direct_alloc_pages()` itself (or the code
generated immediately around it), not at anything earlier in the probe
path. Next bisection step, when another test is available: try
removing just the round-1/round-2 prints (keep round-3) to confirm
they're really not load-bearing, then narrow within round-3's four
print sites to find which one (or whether it's the `#include`-driven
code layout shift) actually matters.

**Bisection step 1: reverted round-1 and round-2 (`git revert` of
`9e698d8a0`/`1f5e2aaa4`), keeping only round-3's `kernel/dma/direct.c`
tracing.** `ahci_rtd1295.c`/`libata-core.c`/`libahci.c` are back to
their pre-diagnostic state (verified: no `diag:` lines remain in any
of them). If this build still boots cleanly, it confirms the round-1/
round-2 prints were never load-bearing and the timing sensitivity
really is local to `dma_direct_alloc()`/`__dma_direct_alloc_pages()`;
if the hang comes back, it means the *combination* (or sheer number of
prints) mattered rather than anything specific to the DMA-allocator
tracing. Rebuilt and repackaged; not yet hardware-tested.

**Seventh v6.18.46 hardware test: two more clean boots -- 6/6 clean
with round-3 tracing present, regardless of round-1/round-2.**
Confirms round-1/round-2 were never load-bearing; the timing
sensitivity is local to `dma_direct_alloc()`/`__dma_direct_alloc_pages()`
as suspected. With this confirmed, replaced the six `pr_info()` calls
in `kernel/dma/direct.c` with a single deliberate `usleep_range(50000,
80000)` (50-80ms, comparable to the six evenly-spaced ~10ms UART-bound
gaps the prints introduced -- see the timestamps in the fourth test's
log), placed once at the top of the `remap` branch in
`dma_direct_alloc()`, filtered to `dev_name(dev) ==
"9803f000.sata"` same as before. Sleeping is safe here: this is the
same already-blocking-allowed `GFP_KERNEL` path the prints were in,
reached only after the atomic-pool fast path is ruled out, always in
process/workqueue context for this driver. All `diag:`/`pr_info()`
tracing removed from this file; no more log noise from every other
device's DMA allocations. This tests the "it's just added latency"
hypothesis cleanly, and if it holds up over more reboots, is a much
more defensible thing to actually ship than leaving debug prints in.
Rebuilt and repackaged; not yet hardware-tested.

**Eighth v6.18.46 hardware test (`putty.log.107`): two more clean
boots -- but on build `#41` (the bisection-step-1, round-3-only
build), not yet the deliberate-delay build `#42` above.** The log was
captured before `#42` was packaged. Still useful data: **8/8 clean
boots total** now with round-3-equivalent tracing/delay present in
`dma_direct_alloc()` (4 from the full round-1/2/3 build `#40`, 4 from
the round-3-only build `#41`), against 4/4 hangs with none of it. The
deliberate-delay build (`#42`) is packaged and ready; still needs its
own first hardware test.

**Ninth v6.18.46 hardware test (`putty.log.108`): build `#42` (the
clean deliberate-delay build, zero `diag:`/`pr_info` lines -- verified)
tested for the first time, two clean boots.** Both `ata1`/`ata2: SATA
link up 6.0 Gbps`, full boot to shell both times. This is the important
data point: the `usleep_range(50000, 80000)` alone, with none of the
printk noise that was present in every previous successful test,
reliably avoids the hang. **10/10 clean boots total** across every
build that puts *some* delay (printk-driven or explicit) in front of
`dma_direct_alloc()`'s remap path for this device, against 4/4 hangs
across every build with none. Treating this as a validated, working
workaround from here on -- root cause is still not understood (why a
~50-80ms pause here avoids whatever the race is), and the code comment
left in `kernel/dma/direct.c` says so, but the fix is reliable enough
in practice to stop chasing the underlying mechanism unless it
resurfaces.

**Tenth v6.18.46 hardware test (`putty.log.111`): the hang recurred**,
once, at the 50-80ms delay -- 10/11 clean boots now, not 10/10. Log
shows the same signature as every previous hang: last message is
`ahci_rtd1295 9803f000.sata: port 0 is not capable of FBS` at
`[   13.799683]`, then silence until the first `rcu: INFO: rcu_preempt
detected stalls` at `[   23.481889]` (CPU0 parked in `do_idle`,
repeating RCU-stall NMI backtraces every 12-50s thereafter until the
board was reset). Confirms this is a probabilistic mitigation of a
genuine timing bug, not a fix -- the delay narrows the window, it
doesn't close it. Widened the delay to `usleep_range(150000, 250000)`
(150-250ms) in response; not yet hardware-tested at the new value.

**Eleventh v6.18.46 hardware test (`putty.log.112`): the hang recurred
again**, on the very first boot of the widened-150-250ms-delay build
(`#43`) -- same signature, same `13.7s` last-message /  `23.3s`
first-stall timing as every previous hang. A single sample can't prove
a dose-response failure, but combined with the tenth test this makes
**10/12 clean boots with a plain `usleep_range()` in front of
`dma_direct_alloc()`'s remap path, against 8/8 clean boots with the
earlier round-3/4 `pr_info()` tracing in the same spot** (fourth
through eighth hardware tests, above) -- a meaningfully worse hit rate
for the sleep than for the prints, at both delay values tried. This
argues against "elapsed time alone avoids the race" and for something
more specific to the `pr_info()` calls themselves (their UART-PIO
transmission, which is `irqsave`/`irqrestore`-heavy on this console --
see "forbid DMA for kernel console" in the boot log -- unlike
`usleep_range()`'s hrtimer-driven block/wake, which doesn't touch
interrupt state the same way). Reverted the sleep and restored
`pr_info()` tracing bracketing the two calls round-3/4 covered
(`__dma_direct_alloc_pages()` and `dma_common_contiguous_remap()`) --
diag round-5, four print sites, aimed at (a) re-confirming the prints
still avoid the hang on the current source tree and (b) finally
localizing *which* of the two calls is the one that blocks, since that
narrowing step was never actually completed after round-4. Rebuilt and
repackaged; not yet hardware-tested.

**Twelfth v6.18.46 hardware test (`putty.log.113`): the hang recurred
a third time**, this time on the very first test of the round-5
`pr_info()`-tracing build (`#44`) -- so pr_info() tracing alone isn't
a guaranteed avoidance either, contrary to the earlier 8/8 read on
round-3/4. But this test earns its keep: **the diag prints finally
localized the hang.** The last line before the stall is `diag:
dma_direct_alloc: sata: calling __dma_direct_alloc_pages` -- its
paired "returned" print never fires, while `dma_common_contiguous_remap()`'s
own bracketing prints (added in the same round-5 patch) never even get
reached. So the hang is specifically inside `__dma_direct_alloc_pages()`
(`kernel/dma/direct.c`), clearing `dma_common_contiguous_remap()` for
the first time with real evidence rather than inference. Added diag
round-6: bracketed `__dma_direct_alloc_pages()`'s own internal
`dma_alloc_contiguous()` call (the CMA-backed allocation, `CONFIG_DMA_CMA=y`)
with its own `pr_info()` pair, to see whether execution parks inside
CMA allocation itself or in the `alloc_pages_node()` fallback loop
after it. Rebuilt and repackaged; not yet hardware-tested.

**Thirteenth v6.18.46 hardware test (`putty.log.114`): narrowed one
level further.** Build `#45` (round-6 tracing). Last line before the
stall is `diag: __dma_direct_alloc_pages: sata: calling
dma_alloc_contiguous` -- its "returned" print never fires, and the
`alloc_pages_node()` fallback loop after it is never reached either.
So the hang is specifically inside `dma_alloc_contiguous()`
(`kernel/dma/contiguous.c`), which (for this device, no per-device
`dev->cma_area` reservation in the DTS) routes to `cma_alloc_aligned()`
on the global default CMA area (`dma_contiguous_default_area`, the 32
MiB pool reserved at `0x3e000000`) -- i.e. inside `cma_alloc()`
(`mm/cma.c`) itself, most likely inside `alloc_contig_range()`'s page
isolation/migration (the only step in that call chain that can
plausibly block for seconds). Added diag round-7: tracing bracketing
`cma_alloc_aligned()`'s call in `dma_alloc_contiguous()` (all four
possible paths -- `dev->cma_area`, per-numa ×2, default), plus finer
tracing inside `cma_range_alloc()` (`mm/cma.c`) around `cma->lock`,
`cma->alloc_mutex`, and `alloc_contig_range()` itself, gated by a new
global `cma_diag_trace_next` flag (set only around this device's own
call, since `cma_alloc()` takes no `struct device *` to filter on
directly). Rebuilt and repackaged; not yet hardware-tested.

**Fourteenth v6.18.46 hardware test (`putty.log.115`): 5/5 clean boots
in a row**, all with round-7 tracing present and firing in full on
every one (both ports' complete `diag:`/`cma:` print sequences, both
`alloc_contig_range returned ret=0` within ~10ms, both `ata1`/`ata2:
SATA link up 6.0 Gbps`, full boot to shell each time). This is the
best track record any tracing/delay variant has had since round-3/4's
original 8/8 -- round-5 and round-6's *shallower* bracketing (stopping
just short of `alloc_contig_range()`) each hung on their first test
(twelfth/thirteenth tests, `putty.log.113`/`.114`), while round-7's
*deeper* bracketing (reaching all the way to just around
`alloc_contig_range()` itself) went 5/5. That pattern -- failures
right where the tracing used to stop, success once tracing extends
past that point -- points at `alloc_contig_range()`'s own internals as
where the actual race lives, not merely "more latency anywhere in the
call chain helps."

`alloc_contig_range()` (`mm/page_alloc.c`) calls `drain_all_pages()`
right after page isolation -- an `on_each_cpu_mask()`-based cross-CPU
call that blocks the calling CPU until every CPU with a non-empty
per-cpu pageset (potentially including CPU0) has run
`drain_local_pages()` via IPI. If CPU0 ever misses that IPI while
parked in WFI, this call would hang forever right here, with CPU0
legitimately idling (matching every NMI backtrace seen in every hang
so far) while the actual caller (on another CPU) spins waiting for an
IPI response that never comes. This lines up exactly with the
"CPU0-interrupt-loss" hypothesis this investigation started from, and
would also explain why RCU's own *expedited* grace-period stalls
(which also work by IPI-ing every CPU to force a quiescent-state
report) recur throughout every hang log -- same underlying mechanism,
different caller. `__alloc_contig_migrate_range()` (page migration,
right after) is the other real candidate, less clean a fit for a
CPU0-parked hang but not ruled out.

Added diag round-8: tracing (still gated by `cma_diag_trace_next`)
bracketing `start_isolate_page_range()`, `drain_all_pages()`, and
`__alloc_contig_migrate_range()` individually inside
`alloc_contig_range()`, to catch which of the three is where execution
actually parks on the next hang. Rebuilt and repackaged; not yet
hardware-tested.

**Fifteenth v6.18.46 hardware test (`putty.log.116`): 3/3 more clean
boots, build `#47` (round-8 tracing).** Every `diag:` print fired in
full both times per boot, `drain_all_pages` and
`__alloc_contig_migrate_range` both returned within single-digit
milliseconds, both ports linked at 6.0 Gbps, full boot to shell each
time. **8/8 clean boots total now with tracing that reaches all the
way through `alloc_contig_range()`'s internals** (5 from round-7, 3
from round-8) -- matching round-3/4's original 8/8 streak. Still
hasn't caught a failure to localize past `alloc_contig_range()`'s
individual sub-calls, so the `drain_all_pages()`/lost-IPI hypothesis
remains unconfirmed, not disproven -- the deep tracing itself may
simply be nudging the timing away from the race every time, same as
every previous tracing round eventually did before a later recurrence.

**Trimmed diag tracing down to just the prime suspect.** With 8/8
clean boots and no new failure to localize further, kept only the
`drain_all_pages()` bracket in `alloc_contig_range()`
(`mm/page_alloc.c`) and its supporting `cma_diag_trace_next` plumbing
(`mm/cma.c`, `kernel/dma/contiguous.c`); removed every other `diag:`
print (the `dma_direct_alloc()`/`__dma_direct_alloc_pages()`/
`dma_alloc_contiguous()`/`cma_range_alloc()` lock/mutex tracing added
across rounds 5-8). Less log noise, and if the hang recurs the only
question left to answer is whether `drain_all_pages()` is really where
it parks.

**Sixteenth v6.18.46 hardware test (`putty.log.117`): the hang
recurred, and it clears `drain_all_pages()` as the culprit.** Build
`#48` (trimmed tracing). Both `calling drain_all_pages` and
`drain_all_pages returned` fired, ~9ms apart, then the hang happened
somewhere later -- untraced at the time, since round-9's trim removed
everything after that point. The lost-IPI-in-`drain_all_pages()`
hypothesis is falsified: whatever the race is, it's not there.

Restored two more tracing points for the next test: a print right
after `dma_alloc_contiguous()` returns in `__dma_direct_alloc_pages()`
(`kernel/dma/direct.c` -- confirms whether the *entire* remaining
`cma_alloc()` chain, including migration and `__cma_alloc()`'s own
bookkeeping, completes), and a bracket around
`__alloc_contig_migrate_range()` (`mm/page_alloc.c`, the next real
candidate right after `drain_all_pages()` -- page migration can
plausibly block on page locks/writeback). Rebuilt and repackaged; not
yet hardware-tested.

**Trimmed diag tracing again, down to the next candidate.** No new
hang since round-9's tracing was added (didn't reproduce). Removed the
now-cleared `drain_all_pages()` bracket and the `__dma_direct_alloc_pages()`
checkpoint print; kept only the `__alloc_contig_migrate_range()`
bracket in `alloc_contig_range()` (`mm/page_alloc.c`) plus its
supporting `cma_diag_trace_next` plumbing (`mm/cma.c`,
`kernel/dma/contiguous.c`).

**Seventeenth v6.18.46 hardware test (`putty.log.118`): hang
confirmed inside `__alloc_contig_migrate_range()`.** Build `#50`. Last
line before the stall is `calling __alloc_contig_migrate_range`, its
"returned" print never fires -- the page-migration step is genuinely
where execution parks, not just an untraced pass-through.

Found a strong new candidate one level in: `lru_cache_disable()`
(`mm/swap.c`), the very first thing `__alloc_contig_migrate_range()`
does. Structurally identical to the already-cleared `drain_all_pages()`
-- both are cross-CPU synchronization primitives that can block on
CPU0 specifically -- but via a different mechanism:
`__lru_add_drain_all()` does `queue_work_on(cpu, mm_percpu_wq, work)`
for every CPU with a non-empty per-cpu LRU batch (potentially
including CPU0), then `flush_work()` on each queued work item in turn.
If CPU0 never wakes to run its queued work (the same "CPU0
interrupt/wakeup loss" mechanism this investigation is named after,
just hitting a workqueue kworker's wakeup instead of a raw IPI this
time), `flush_work()` blocks forever right here. Added a bracket
around this specific call. Rebuilt and repackaged; not yet
hardware-tested.

**Eighteenth v6.18.46 hardware test (`putty.log.119`): hang confirmed
inside `lru_cache_disable()`.** Build `#51`. Last line before the
stall is `calling lru_cache_disable`, its "returned" print never
fires.

`lru_cache_disable()` (`mm/swap.c`) does exactly two things:
`synchronize_rcu_expedited()`, then `__lru_add_drain_all(true)`.
`synchronize_rcu_expedited()` is a strong new candidate -- it IPIs
every CPU to force a quiescent-state report and blocks until they all
respond, which lines up neatly with the recurring "rcu_preempt
detected expedited stalls" messages seen in *every* hang log so far
(raising the possibility that RCU's own self-monitoring has been
reporting on this exact call's failure to complete all along, not an
unrelated side effect). `__lru_add_drain_all(true)`
(`queue_work_on()`+`flush_work()` per CPU) remains the fallback
candidate if `synchronize_rcu_expedited()` clears. Added brackets
around both individually. Rebuilt and repackaged; not yet
hardware-tested.

**Nineteenth v6.18.46 hardware test (`putty.log.120`): hang confirmed
inside `synchronize_rcu_expedited()` itself.** Build `#52`. Last line
before the stall is `calling synchronize_rcu_expedited`, its
"returned" print never fires -- this is the deepest localization yet,
and the call chain from the very first hang symptom (13.7s, right
after AHCI probe) to here is now fully traced with no gaps:
`dma_direct_alloc()` -> `dma_alloc_contiguous()` -> `cma_alloc()` ->
`alloc_contig_range()` -> `__alloc_contig_migrate_range()` ->
`lru_cache_disable()` -> `synchronize_rcu_expedited()`.

This lines up as well as any finding in this investigation could with
the original "CPU0-interrupt-loss" hypothesis:
`synchronize_rcu_expedited()` forces every CPU to report an RCU
quiescent state via IPI/resched and blocks until they all do, and it
would explain the recurring `rcu_preempt detected expedited stalls`
messages seen in *every* hang log so far -- not an unrelated symptom,
but RCU's own stall detector reporting that this exact grace period
(or one just like it) isn't completing, almost certainly because CPU0
never acknowledges it while parked in WFI.

Going further requires instrumenting generic RCU internals
(`kernel/rcu/tree_exp.h`'s `sync_rcu_exp_select_cpus()`/IPI dispatch
and the `wait_event()` that blocks for completion) rather than this
driver's own code -- increasingly esoteric, and low-probability as an
actual RCU bug given how heavily tested that path is elsewhere. A more
direct test of the hypothesis at this point: does disabling deep CPU
idle (so CPU0 never enters the state believed to drop the wakeup)
avoid the hang entirely? That would confirm the mechanism without
digging further into RCU, and point toward a real fix (a cpuidle/PSCI/
GIC wakeup quirk for this SoC) rather than more probabilistic
mitigation.

Added `nohlt` to the board DTS's `chosen/bootargs`
(`rtd1296-wd-mycloud-home-duo.dts`) -- the ARM/ARM64 equivalent of
x86's `idle=poll` (which doesn't exist on this arch), forcing every
CPU to busy-wait in `do_idle()` instead of ever executing `wfi`
(`CONFIG_GENERIC_IDLE_POLL_SETUP` was already `=y`, so no config
change needed). Diagnostic only, burns full power on all 4 CPUs --
remove once this test is done. If the hang stops reproducing under
`nohlt`, that's strong confirmation of the lost-wakeup-in-WFI
mechanism; if it still reproduces, the hypothesis is wrong and the
real cause is something else entirely.

**`putty.log.121`/`.122`: the DTS bootargs edit had no effect --
`Kernel command line:` still printed without `nohlt`, twice, even
after re-copying the rebuilt `rescue.emmc.dtb` to the USB stick.**
Turned out `.config` already has `CONFIG_CMDLINE_FORCE=y` (this port's
own deliberate choice, so the loader's own `bootargs` environment
variable -- which this loader does set, overriding the DTB's
`chosen/bootargs` -- can't silently change what the kernel boots
with). That makes the kernel's *compiled-in* `CONFIG_CMDLINE` the only
thing that matters, ignoring both the bootloader and the DTB
entirely -- so a DTS-only edit was never going to work here. Reverted
the DTS change (no-op, but misleading to leave in) and added `nohlt`
directly to `CONFIG_CMDLINE` in `.config` instead. Rebuilt and
repackaged; not yet hardware-tested.

**Twentieth v6.18.46 hardware test (`putty.log.123`): `nohlt`
confirmed active, and the hang still happened -- the
lost-wakeup-in-WFI hypothesis is falsified.** Build `#53`, `Kernel
command line:` includes `nohlt`, and the NMI backtrace's `pc :
cpu_idle_poll.isra.0+0x28/0x60` confirms CPU0 really was busy-polling
in `do_idle()`, never executing `wfi`, at the moment of the hang. Yet
`synchronize_rcu_expedited returned` fired -- that call cleared this
time -- and the hang moved one step further, into
`__lru_add_drain_all()` (`calling __lru_add_drain_all` printed, its
"returned" never did).

This is a significant reframe: whatever blocks CPU0-bound work here,
it is **not** CPU0 missing an interrupt while parked in a low-power
idle state -- CPU0 was demonstrably still running (just spinning) the
whole time. The "CPU0-interrupt-loss" name for this investigation
described the *symptom* (CPU0 shows up idle/uninvolved in every NMI
backtrace) rather than a confirmed mechanism, and that symptom turns
out to persist even without WFI -- so CPU0 may simply have nothing
scheduled, which is unremarkable, while the real block is a workqueue/
scheduling issue on whichever CPU (or kworker) is supposed to run the
queued `lru_add_drain_per_cpu` work, for a reason still unknown.

Added tracing bracketing `__lru_add_drain_all()`'s own `mutex_lock()`
and, per-CPU, its `queue_work_on()`/`flush_work()` pairs, to find
exactly which CPU's queued work never completes. Rebuilt and
repackaged; not yet hardware-tested. (`nohlt` stays in `CONFIG_CMDLINE`
for this test too, since removing it now would reintroduce a variable
this round doesn't need to re-test.)

**Twenty-first v6.18.46 hardware test (`putty.log.124`): pinned down
to exactly one blocked `flush_work()` call.** Build `#54`, `nohlt`
still active. Work was queued for CPU0, CPU1, and CPU3 (`queue_work_on
cpu=0/1/3` -- CPU2 apparently didn't need draining), then
`flush_work cpu=0` printed and never returned. So this is now as
precise as it gets without instrumenting arch/IPI code:
**`__lru_add_drain_all()` is waiting on CPU0's queued
`lru_add_drain_per_cpu` work item, and CPU0 never runs it.**

The two NMI backtraces in this log (10s apart) both show identical
state: `pc : cpu_idle_poll.isra.0+0x28/0x60`, i.e. CPU0 is genuinely
stuck spinning at the exact same instruction in the busy-poll idle
loop the whole time, never observing `need_resched()` become true for
the newly-queued kworker. Since `cpu_idle_poll()` runs with local IRQs
enabled (that's how a busy-poll idle loop can be interrupted/
rescheduled at all), this means the reschedule signal to CPU0 --
ordinarily a GIC SGI/IPI sent by `queue_work_on()`'s wakeup path --
is not being acted on, even though the *separate* IPI used to request
this very NMI backtrace clearly *does* reach CPU0 (the backtrace prints
correctly both times). That asymmetry -- one IPI vector gets through,
another doesn't -- is the sharpest evidence yet, and points at
something in the reschedule-IPI delivery/handling path specifically
(GIC SGI routing, or CPU0's IRQ state at that moment) rather than a
generic "CPU0 is asleep" story.

Going further from here means instrumenting genuinely different code:
the reschedule-IPI send/receive path (`arch/arm64/kernel/smp.c`,
`send_call_function_single_ipi`/`smp_send_reschedule`) and/or the GIC
driver (`drivers/irqchip/irq-gic-v3.c`) rather than anything in `mm/`
or the AHCI driver.

Before assuming a GIC/IPI hardware issue, checked how the scheduler
actually wakes a remote CPU: `kernel/sched/core.c`'s
`__ttwu_queue_wakelist()` path is gated by `ttwu_queue_cond()`, which
calls `set_nr_if_polling(cpu_rq(cpu)->idle)` first -- **if the target
CPU's idle task has `TIF_POLLING_NRFLAG` set (exactly what `nohlt`'s
`cpu_idle_poll()` does), the scheduler skips the IPI entirely** and
just atomically sets `TIF_NEED_RESCHED` on the target's idle
thread_info, trusting the polling loop to notice on its own -- no IPI
involved at all in that case. That matters because **the identical
hang was also seen without `nohlt`** (`putty.log.118`/`.119`, before
this test), where CPU0 would have been idling via real `wfi` and the
wakeup *would* have gone through the IPI path instead. Two structurally
different wakeup delivery mechanisms, same hang either way -- the
common point upstream of both isn't IPI delivery or `wfi`/polling at
all, it's whatever decides *whether to attempt a wakeup in the first
place*.

That points at `kernel/workqueue.c`'s `kick_pool()` (the modern
replacement for the older `wake_up_worker()`): it calls
`first_idle_worker(pool)` and checks `need_more_worker(pool)` *before*
ever reaching `wake_up_process()` -- if either says "nothing to do"
incorrectly for CPU0's percpu pool, no wakeup of any kind (IPI or
flag-poll) would ever be attempted, matching everything observed so
far. Added tracing to `kick_pool()` (both conditions checked, and
immediately around `wake_up_process()` itself) plus, for direct
comparison, tracing on both ends of the reschedule-IPI path
(`arch_smp_send_reschedule()` in `arch/arm64/kernel/smp.c`, and every
IPI CPU0's `do_handle_IPI()` actually receives) in case the wakeup
*is* attempted and something in the arch layer is still the culprit.
Rebuilt and repackaged; not yet hardware-tested.

**Twenty-second v6.18.46 hardware test (`putty.log.125`): the wakeup
itself succeeds -- the hang is somewhere past it.** Build `#55`.
`kick_pool()`'s tracing shows `need_more_worker=1`, a valid `worker`
pointer, `calling wake_up_process pid=11`, and critically
**`wake_up_process returned`** -- the scheduler-level wakeup completed
normally, by all visible bookkeeping. Yet `flush_work cpu=0` (queued
right after) still never completes. One `do_handle_IPI: cpu0 received
ipinr=1` fired earlier too, during `synchronize_rcu_expedited()`
(which cleared) -- confirming CPU0 genuinely can and does receive and
process ordinary percpu IRQs in this exact window, so the machinery
isn't broadly "IPI-deaf."

Read through `kernel/sched/idle.c`'s `do_idle()`/`cpu_idle_poll()`:
the polling bit (`TIF_POLLING_NRFLAG`, via `__current_set_polling()`)
is set once before the outer `while (!need_resched())` loop and only
cleared after it exits, so a remote `set_nr_if_polling()`-based wakeup
setting `TIF_NEED_RESCHED` should be noticed within microseconds by
the tight `cpu_relax()` spin in `cpu_idle_poll()`'s inner loop -- not
after 10+ seconds. Nothing in that path looks broken by inspection,
and it's about as heavily-exercised generic code as exists in the
kernel, making a genuine bug there unlikely to be a first discovery
here.

Pivoted to a more decisive question: does the queued work's *function*
(`lru_add_drain_per_cpu()`, `mm/swap.c`) ever actually start running
at all, on any CPU -- as opposed to the *task* being nominally
"woken" but never truly executing? Added a bracket directly inside it
(entry logs which CPU it's running on). If it never fires, the task
genuinely never runs despite every scheduler-level signal saying it
should; if it fires but "done" doesn't, the block is inside
`lru_add_and_bh_lrus_drain()` itself (a lock/logic issue in the drain,
not a wakeup/scheduling issue at all). Rebuilt and repackaged; not yet
hardware-tested.

**Twenty-third v6.18.46 hardware test (`putty.log.126`): proven --
CPU0's queued work function never starts, while the identical
mechanism works fine on CPU1.** Build `#56`. `queue_work_on cpu=1`'s
work ran and finished in ~12ms (`lru_add_drain_per_cpu: running on
cpu=1` -> `done on cpu=1`), but the CPU0 case (`queue_work_on cpu=0`,
`kick_pool` found the worker, `wake_up_process pid=11` returned
normally) never printed `running on cpu=0` at all before the hang.
This is as decisive as tracing gets: the task is nominally woken by
every scheduler-visible signal, and yet never actually executes on
CPU0, while the *same* wakeup path succeeds immediately for a sibling
CPU in the same call. `kernel/sched/idle.c`'s `do_idle()`/
`cpu_idle_poll()` look correct by inspection (the polling bit stays
set across the whole outer loop, so a remote wakeup should be noticed
within microseconds) -- so instead of guessing further, went straight
to the kernel's own task-introspection primitive.

Added diag round-15: `kick_pool()` now stashes the woken task pointer
(`cma_diag_task0`, `mm/cma.c`/`include/linux/cma.h`) when it wakes
CPU0's kworker, and `__lru_add_drain_all()` arms a 2-second-repeating
timer around `flush_work()` for CPU0 specifically that calls the
kernel's own `sched_show_task()` (`kernel/sched/core.c`, exported,
used by the hung-task detector/sysrq-t for exactly this purpose) on
that task -- printing its actual `state`, whether it's genuinely
`on_rq`/running, and its real kernel stack backtrace, instead of
inferring anything from call sites. This should show directly whether
pid 11 is stuck mid-execution somewhere unexpected, stuck off-runqueue
despite the "successful" wakeup, or something else entirely. Rebuilt
and repackaged; not yet hardware-tested.

**Twenty-fourth v6.18.46 hardware test (`putty.log.127`): 3/3 clean
boots, build `#57` (round-15 tracing).** No hang, so the
`sched_show_task()` watchdog never fired -- no new data this round,
but the diagnostic is confirmed present and armed for the next
recurrence. Noted in passing: multiple `cma_range_alloc()` retries
happened across the three boots (`need_more_worker=0`/repeated
`kick_pool` calls for the same worker, `alloc_contig_range` called
more than twice per port in a couple of cases) -- normal CMA
contention/retry behavior (`cma_range_alloc()`'s own retry loop on
`-EBUSY`), not a new symptom.

**Trimmed diag tracing down to just the round-15 watchdog.** No new
hang since round-15's `sched_show_task()` diagnostic was added
(didn't reproduce). Removed every other `diag:` print (the RCU/mutex/
per-CPU/IPI tracing from rounds 5-13); kept only `mm/swap.c`'s
`cma_diag_task0_dump()` timer and the minimal plumbing needed to feed
it (`kick_pool()` stashing the woken task in `kernel/workqueue.c`, and
`cma_diag_trace_next` still set around the CMA allocation call in
`kernel/dma/contiguous.c`/`mm/cma.c`). If the hang recurs, the next
log should show the target task's real scheduler state directly.

**Twenty-fifth v6.18.46 hardware test (`putty.log.128`): hang
recurred with zero diagnostic output -- the round-15 watchdog was
blind because it only watched CPU0.** Build `#58`, `nohlt` active, the
usual `port 0 is not capable of FBS` at 13.7s then silence until the
RCU stall at 23.3s -- but not one `diag:` line in between, meaning
`__lru_add_drain_all()` never queued work for CPU0 this particular
time (so `kick_pool()` never ran for CPU0's pool, `cma_diag_task0`
stayed `NULL`, and the watchdog never armed) while presumably hanging
on a *different* queued CPU we had no visibility into anymore after
round-9's trim removed the generic per-CPU tracing.

Generalized diag round-16: `cma_diag_task0` became a
`cma_diag_task[CMA_DIAG_MAX_CPU]` array (`include/linux/cma.h`,
`mm/cma.c`), `kick_pool()` (`kernel/workqueue.c`) now stashes the
woken task for *any* CPU's pool (not just CPU0's), and
`__lru_add_drain_all()`'s watchdog (`mm/swap.c`) arms for whichever
CPU is actually being flushed each loop iteration. Also restored a
minimal `flush_work cpu=%u` print (removed in the previous trim) so a
hang is at least attributable to a CPU even if the watchdog itself
doesn't fire for some other reason. Rebuilt and repackaged; not yet
hardware-tested.

**Twenty-sixth v6.18.46 hardware test (`putty.log.129`): 5/5 clean
boots, build `#59` (round-16 generalized watchdog).** No hang, so no
`sched_show_task()` dump fired -- but the new `flush_work cpu=%u`
prints confirm which CPUs actually needed draining varies boot to
boot (`0,2,3` / `0,1,3` / `1,2,3` / `1,2,3` / `0,2,3` across the five
boots) -- consistent with `putty.log.128`'s silent hang having queued
a CPU other than 0 that time, which the old CPU0-only watchdog
couldn't see.

**Reverted round-16 back to round-15 (CPU0-only watchdog), per explicit
request**: running several back-to-back tests hoping to specifically
catch a hang on CPU0 again (the case round-15's `sched_show_task()`
diagnostic was built for) rather than the broader, CPU-agnostic
version. `cma_diag_task[CMA_DIAG_MAX_CPU]` reverted back to a single
`cma_diag_task0`; `kick_pool()` and the `__lru_add_drain_all()` watchdog
loop watch only CPU0 again; the round-16 `flush_work cpu=%u` print
removed too.

**Twenty-seventh v6.18.46 hardware test (`putty.log.131`): caught on
CPU0 -- the task is provably runnable, not blocked.** Build `#60`,
across 12 repeated dumps spanning 15.8s to 44.3s (30+ seconds), the
watchdog consistently showed:

```
task:kworker/0:1     state:R  running task     stack:0     pid:11
Workqueue:  0x0 (rcu_gp)
Call trace:
 __switch_to+0x15c/0x174 (T)
 __schedule+0x55c/0x62c
 schedule+0x34/0x58
 worker_thread+0x184/0x1c8
 kthread+0x130/0x1a0
 ret_from_fork+0x10/0x20
```

`state:R` + `task_is_running()`'s "running task" means this task is
genuinely `TASK_RUNNING` (on the runqueue, not blocked on any lock or
condvar) -- it isn't stuck deadlocked inside a function; it's sitting
at `worker_thread()`'s normal "look for more work" `schedule()` call,
fully eligible to run, and simply never getting picked by CPU0.

**Correction to the round-13 hypothesis**: `arch/arm64` does not
define `TIF_POLLING_NRFLAG` at all (confirmed: absent from
`arch/arm64/include/asm/thread_info.h`), so `kernel/sched/core.c`'s
`set_nr_if_polling()` fast path (used only `#ifdef TIF_POLLING_NRFLAG`)
never applies on this architecture -- `nohlt` never changed the wakeup
delivery mechanism the way round-13 assumed. Every wakeup here, with
or without `nohlt`, goes through the real IPI path: `ttwu_queue()` ->
`ttwu_queue_wakelist()` -> `__ttwu_queue_wakelist()` ->
`__smp_call_single_queue()` (llist_add + `IPI_CALL_FUNC`, not
`IPI_RESCHEDULE`) -> on the target CPU, `do_handle_IPI(IPI_CALL_FUNC)`
-> `generic_smp_call_function_interrupt()` ->
`__flush_smp_call_function_queue()` -> batches all queued
`CSD_TYPE_TTWU` entries into one `sched_ttwu_pending()` call.

Added diag round-17: tracing in `__smp_call_single_queue()`
(`kernel/smp.c`) for whether `llist_add()` finds CPU0's queue already
non-empty (meaning we assume another already-pending IPI will process
our entry too) vs. empty (meaning we send a fresh IPI ourselves), and
in `__flush_smp_call_function_queue()` for whether CPU0 ever actually
drains that queue during the diag window at all. If CPU0 never drains
it, the IPI genuinely never got delivered/processed; if it drains but
finds nothing relevant, the race is elsewhere (e.g. in how the CSD
gets composed/lost before queuing). Rebuilt and repackaged; not yet
hardware-tested.

**Twenty-eighth v6.18.46 hardware test (`putty.log.132`): 3/3 clean
boots, build `#61` (round-17 IPI-queue tracing).** No hang, but useful
baseline data: every `llist_add()` for CPU0 returned `was_empty=1`
(the queue was never already occupied by a stranded entry -- a fresh
IPI was sent every single time), and `__flush_smp_call_function_queue`
drained the corresponding entry within the same millisecond each time.
This is what the mechanism looks like when it works; still need a
hang to see what it looks like when it doesn't.

**Trimmed round-17 tracing to just the receive side.** Removed
`__smp_call_single_queue()`'s send-side print (`kernel/smp.c`) --
redundant with the already-established fact that `wake_up_process()`
returns normally, meaning the local `llist_add()`+IPI-dispatch chain
completes synchronously without issue. Kept
`__flush_smp_call_function_queue()`'s receive-side print, the still
open question (does CPU0 ever actually drain its queue during a
hang), plus `mm/swap.c`'s `sched_show_task()` watchdog.

**Diag round-18: force CPU0 into the drain set every time.** Recent
logs show CPU0 only happens to need draining (non-empty pagevecs)
roughly 1 in 4 boots naturally, wasting most test cycles on CPUs the
watchdog isn't watching. `__lru_add_drain_all()`'s queueing loop
(`mm/swap.c`) now forces CPU0 into `has_work` unconditionally whenever
`cma_diag_trace_next` is set (i.e. during our SATA CMA allocation
window specifically -- this doesn't affect any of this function's
other, unrelated callers elsewhere in the kernel), so every single
boot through this path gives the `sched_show_task()` watchdog a real
chance to catch a CPU0 hang, instead of waiting on natural variance.

**Twenty-ninth v6.18.46 hardware test (`putty.log.133`): 3/3 clean
boots, build `#63`.** The round-18 forcing worked as intended -- CPU0
was queued and drained in all 3 boots this time (vs. only some
boots before), confirmed by `__flush_smp_call_function_queue` firing
consistently -- but still no hang caught. Forcing CPU0 into the drain
set every time removes "which CPU gets queued" as a source of
variance, but the underlying race clearly depends on finer timing
than that alone.

**Removed the last routine `diag:` print** (`__flush_smp_call_function_queue`'s
receive-side confirmation in `kernel/smp.c`) per the observation that
printk/UART latency itself has repeatedly been shown throughout this
investigation to perturb the race and avoid the hang -- and this one
fired 2-4 times per boot regardless of whether anything went wrong,
unlike `mm/swap.c`'s `sched_show_task()` watchdog print, which only
fires if a hang is actually happening. Round-18's CPU0-forcing logic
(a plain boolean condition, no printk/IPI overhead) stays.

**Thirtieth v6.18.46 hardware test (`putty.log.134`): caught again,
build `#64` (round-18 forcing + minimal logging).** Confirms the
reduced-logging approach doesn't prevent reproduction -- the watchdog
fired repeatedly from 15.8s through at least 30.8s (user reports
waiting a full minute with no change), consistently showing the same
`kworker/0:1  state:R  running task` signature as `putty.log.131`
(this time `Workqueue: 0x0 (events)` instead of `(rcu_gp)` -- just
whatever this worker last processed before going idle, not a new
detail). The `platform ... supply target not found, using dummy
regulator` message the user initially flagged as a possible new lead
is, again, confirmed harmless and unrelated -- it's the same routine
early-probe print seen in every boot (successful or not); the genuine
hang starts well after it, at the already-diagnosed point following
`port 0 is not capable of FBS`.

**Diag round-19: dump raw scheduler bookkeeping alongside
`sched_show_task()`.** `sched_show_task()` shows `state`/stack/comm but
not the low-level fields that would pin down exactly why CPU0 never
picks up a task it itself reports as `TASK_RUNNING`. Added a second
print in the watchdog (`mm/swap.c`) for `p->on_rq` (0 = not queued at
all; nonzero = `TASK_ON_RQ_QUEUED`/`_MIGRATING`), `p->on_cpu` (1 = the
scheduler believes this task is *currently executing* on some CPU
right now -- would directly contradict CPU0 sitting idle in
`cpu_idle_poll()` if seen while stuck), `task_cpu(p)` (the CPU it's
actually assigned to), and `p->wake_cpu` (the CPU the last wakeup
targeted). Rebuilt and repackaged; not yet hardware-tested.

**Thirty-first v6.18.46 hardware test (`putty.log.135`): raw
scheduler bookkeeping confirms the task side is entirely correct.**
Build `#65`. Across 10 dumps spanning 15.8s to 40.1s (24+ seconds),
consistently: `on_rq=1 on_cpu=0 task_cpu=0 wake_cpu=0`. The task is
genuinely queued on the runqueue (`on_rq=1`), assigned to CPU0
(`task_cpu=0`), not phantom-running anywhere (`on_cpu=0`), and the
last wakeup targeted CPU0 correctly (`wake_cpu=0`) -- this rules out
task migration bugs, wrong-CPU targeting, and "already running
elsewhere" confusion entirely. The task's own bookkeeping is
unimpeachable; CPU0 simply never switches to it.

Added diag round-20: dump CPU0's own idle task's (`swapper/0`,
`idle_task(0)`) raw thread_info flags directly (bit 1 = `0x2` =
`TIF_NEED_RESCHED`), since `sched_show_task()` only covers the target
task, not the CPU-idle side of the picture. If `TIF_NEED_RESCHED`
never actually gets set on `swapper/0` despite `kworker/0:1` staying
`on_rq=1`, that's the smoking gun: whatever signals CPU0 to reschedule
(`resched_curr()`/`set_tsk_need_resched()`, called during
enqueue) silently fails to mark it. Rebuilt and repackaged; not yet
hardware-tested.

**Thirty-second v6.18.46 hardware test (`putty.log.136`): the smoking
gun -- `TIF_NEED_RESCHED` is genuinely set on CPU0's idle task the
entire time it's stuck.** Build `#65`. Across 12 dumps spanning 15.8s
to 42.1s (26+ seconds), `swapper/0`'s raw thread_info flags read
consistently as `tif_flags=0x00000012` -- bit 1 (`0x2`,
`TIF_NEED_RESCHED`) and bit 4 (`0x10`, `TIF_FOREIGN_FPSTATE`, benign
FP-context bookkeeping) both set, unchanging across every sample.

This rules out the entire wakeup/signaling chain traced so far
(`kick_pool()`, `wake_up_process()`, `__smp_call_single_queue()`'s IPI
dispatch, `sched_ttwu_pending()`'s enqueue): all of that machinery
evidently *did* work and correctly called whatever sets
`TIF_NEED_RESCHED` on the current task (`resched_curr()`/
`set_tsk_need_resched()`) during the target task's enqueue. The flag
CPU0's own idle loop is supposed to be watching
(`cpu_idle_poll()`'s `while (!tif_need_resched() && ...) cpu_relax();`,
`kernel/sched/idle.c`) is unambiguously set -- yet CPU0 still doesn't
exit that loop and switch to the runnable task.

Two remaining explanations, both exotic: (a) a genuine memory
visibility/cache-coherency issue specific to this SoC, where CPU0's
own read of `current_thread_info()->flags` inside its tight polling
loop doesn't observe the write another CPU made to the same cacheline
(this generic idle-poll code is extremely heavily used across the
kernel ecosystem, making a plain missing-barrier bug here improbable
to be a first discovery, but embedded-SoC cache-coherency errata are a
real category of bug); or (b) the NMI backtrace's reported PC
(`cpu_idle_poll.isra.0+0x2x/0x60`, consistent across every hang so
far) is somehow not where CPU0 truly is, and it's stuck somewhere else
entirely that just happens to report this PC via the backtrace-IPI
path specifically.

**Diag round-21: test the memory-visibility hypothesis directly.**
Added `smp_mb()` after every `cpu_relax()` inside `cpu_idle_poll()`'s
tight loop (`kernel/sched/idle.c`) -- if a missing barrier is why this
CPU doesn't observe its own already-correctly-set `TIF_NEED_RESCHED`
(per `putty.log.136`), forcing one every iteration should make the
loop notice and exit. Expensive (runs on every CPU continuously under
`nohlt`) and diagnostic only. If this makes the hang stop reproducing,
that's strong confirmation of the mechanism; if it still hangs
identically, the flag-visibility hypothesis is wrong and the idle
loop is stuck somewhere that doesn't even reach this check. Rebuilt
and repackaged; not yet hardware-tested.

**Thirty-third v6.18.46 hardware test (`putty.log.137`): the
`smp_mb()` test failed to fix the hang.** Build `#67`. Identical
signature to every prior capture -- `tif_flags=0x00000012` stayed
constant across 6+ dumps spanning 15.7s to 26.3s+, forced barrier and
all. This rules out a simple missing-memory-barrier explanation for
why `cpu_idle_poll()`'s check doesn't notice `TIF_NEED_RESCHED`.

Added diag round-22: instead of guessing at more fixes, directly
instrumented CPU0's *own* execution of the loop (`kernel/sched/idle.c`)
with a plain iteration counter (`cma_diag_cpu0_poll_iters`, proves the
loop is genuinely spinning there, matching the NMI backtrace PC,
rather than the PC being stale/misleading) and a one-shot latch
(`cma_diag_cpu0_seen_need_resched`) set the instant CPU0's own
`tif_need_resched()` call itself returns true -- no printk in the hot
loop, both read by the existing watchdog timer. This directly tests
whether there's a mismatch between what CPU0 itself observes and what
an external read (from another CPU, in the watchdog) observes.
Rebuilt and repackaged; not yet hardware-tested.

**Thirty-fourth v6.18.46 hardware test (`putty.log.138`): major
pivot -- CPU0 actually exits `cpu_idle_poll()`.** Build `#68`. Across
15 dumps spanning 15.8s to 47.0s, `poll_iters=62142367` stayed
completely frozen (never incremented) while `seen_need_resched=1`
stayed latched true the entire time. Disassembling the built object
(`kernel/sched/build_policy.o`) confirmed the only way to reach that
state (frozen counter + true latch) is via the `if (nr) { latch =
true; break; }` path -- CPU0 genuinely observed `TIF_NEED_RESCHED` as
true on its own and broke out of the loop. The one NMI backtrace this
log captured (`pc : cpu_idle_poll.isra.0+0x64/0xc0`, at 23.386s, well
after the counter had already frozen) still showed a PC inside the
loop body -- almost certainly a stale/unreliable unwind for this
`__cpuidle`-annotated function (a documented category of caveat for
idle-context backtraces), not a real contradiction; the counter+latch
are direct evidence from the code's own execution, not an
interrupt-based reconstruction.

Confirmed `need_resched()` (used by `do_idle()`'s outer loop) is
exactly `tif_need_resched()` -- same bit, no difference -- so the
outer loop should exit for the same reason. The hang must be further
down: in `do_idle()`'s tail (`preempt_set_need_resched()`,
`tick_nohz_idle_exit()`, `flush_smp_call_function_queue()`) or inside
`schedule_idle()`/`__schedule()` itself.

Added diag round-23: three cheap stage markers (`cma_diag_cpu0_stage`,
no printk) through the rest of `do_idle()` -- 1 = past the outer loop,
2 = about to call `schedule_idle()`, 3 = `schedule_idle()` returned --
read by the same watchdog. Rebuilt and repackaged; not yet
hardware-tested.

**Thirty-fifth v6.18.46 hardware test (`putty.log.139`): `do_idle
stage=3` -- `schedule_idle()` genuinely returns, repeatedly, while the
target task never runs.** Build `#69`. All 9 dumps (15.8s-32.8s) show
`stage=3`. Combined with `kworker/0:1`'s stack staying pinned at
`worker_thread()`'s "look for more work" `schedule()` call the whole
time (never advancing), and the `Workqueue:` annotation differing
between hangs (`rcu_gp` in `putty.log.131`, `events` here) -- CPU0
isn't stuck at all. It's alive, scheduling, and this exact worker
thread evidently *does* run other unrelated work items during the
hang. It just never seems to reach *this specific* queued item.

Found a very concrete candidate in `__schedule()`
(`kernel/sched/core.c`): the `SM_IDLE` fast path --

```c
if (sched_mode == SM_IDLE) {
    if (!rq->nr_running && !scx_enabled()) {
        next = prev;
        goto picked;
    }
```

-- skips `pick_next_task()` *entirely* when `rq->nr_running == 0`,
going straight back to idle without even looking at the runqueue. If
this counter is wrong (reads 0) while a task is genuinely `on_rq=1`
(which the watchdog has confirmed for 20+ consecutive samples across
multiple tests), that's the whole mechanism -- an `on_rq`/`nr_running`
accounting mismatch, not a wakeup/IPI/scheduling-visibility problem at
all.

Added diag round-24: record `rq->nr_running` (as `__schedule()` itself
sees it) and a call counter for CPU0, no printk on the hot path,
read by the same watchdog. If `last_nr_running` reads `0` while our
task is `on_rq=1`, that's confirmed. Rebuilt and repackaged; not yet
hardware-tested.

**Thirty-sixth v6.18.46 hardware test (`putty.log.140`): `nr_running`
hypothesis refuted; `do_idle stage=3` and `__schedule calls=383` both
frozen.** Build `#70`. `last_nr_running=1` (not 0) -- the `SM_IDLE`
fast-path theory is wrong, `pick_next_task()` should be reached
normally. But `__schedule calls=383` never increments across 7 dumps
(15.8s-28.6s), matching `poll_iters` also being frozen -- CPU0 isn't
currently executing `cpu_idle_poll()` *or* calling `schedule_idle()`
again. Since `cpu_startup_entry()` is a bare `while (1) do_idle();`
with nothing between calls, and `stage=3` means `do_idle()` completed
one full pass successfully at some point, CPU0 must be stuck either in
`do_idle()`'s own tail (the `klp_patch_pending()` check) or never
actually returning from that specific call at all.

(Correction to the round-22 read of `seen_need_resched`/`poll_iters`:
these are cumulative/latched since boot, not reset per idle-loop
entry, so a large `poll_iters` value doesn't by itself prove a single
continuous spin during the hang. What *is* still solid: both counters
being frozen across 2s-spaced reads proves CPU0 isn't running that
code *right now*, which is what actually matters here.)

Added diag round-25: two more stage markers -- `0` at the very top of
`do_idle()` (a fresh call started) and `4` at the very end (about to
return) -- to distinguish "stuck in the tail after `schedule_idle()`"
from "a fresh call never even reaches the outer loop again". Rebuilt
and repackaged; not yet hardware-tested.

**Thirty-seventh v6.18.46 hardware test (`putty.log.141`, implied):
did not reproduce.** Requested cleanup of diagnostics that lost
relevance, since every added instrumentation point perturbs boot
timing and can itself mask the race. Removed (diag round-26):

- The forced `smp_mb()` per iteration in `cpu_idle_poll()` (both the
  CPU0 branch and the generic branch), added for round-21's barrier
  test. `putty.log.137` already answered that question -- no effect
  on the hang -- so the extra barrier is gone and both loops are back
  to a plain `cpu_relax()`, matching stock behavior more closely.
- The round-24 `__schedule()` `SM_IDLE`-path instrumentation
  (`cma_diag_cpu0_last_nr_running`, `cma_diag_cpu0_schedule_calls`)
  entirely -- `putty.log.140` already refuted the `nr_running==0`
  fast-path hypothesis (`last_nr_running` read `1`), and this sat on
  `__schedule()`'s hot path, called far more often than any other
  instrumented site in this investigation.

Kept: the core `sched_show_task()`/raw-fields/idle-task-TIF watchdog
(zero cost on clean boots, fires only once a hang is already
detected), `poll_iters`/`seen_need_resched` (still relevant to
interpreting a future `stage=0` reading -- distinguishes "spinning
inside `cpu_idle_poll()` again" from "stuck before reaching it"), and
the round-25 `stage` markers `0`-`4` (the currently open question:
does the hang, next time it's caught, show `stage=3` frozen or
`stage=0`?).

**Thirty-eighth v6.18.46 hardware test (`putty.log.142`, implied):
still did not reproduce.** Asked for further diagnostic cleanup.
Identified that round-22's `poll_iters`/`seen_need_resched`
instrumentation, unlike everything else remaining, was never gated to
the diag window -- it incremented on *every* `cpu_idle_poll()`
iteration on CPU0 for the entire boot, not just near the SATA CMA
alloc, making it the single hottest and most timing-invasive
diagnostic still in the tree. Its original question (does CPU0's own
`tif_need_resched()` ever observe true?) was already answered by
`putty.log.138`. Removed (diag round-27):

- `cma_diag_cpu0_poll_iters` / `cma_diag_cpu0_seen_need_resched`
  entirely -- variables, extern declarations, the watchdog print line,
  and the CPU0-specific branch in `cpu_idle_poll()` that maintained
  them. `cpu_idle_poll()` is now back to a single stock loop shared by
  all CPUs (no more CPU0/else split).

Kept: the `sched_show_task()`/raw-fields/idle-task-TIF watchdog (zero
cost on clean boots) and the round-25 `stage` markers `0`-`4` in
`do_idle()` (cheap -- one assignment per `do_idle()` call, not per
spin iteration -- and still the only unanswered question: does the
hang, next time caught, show `stage=3` frozen or `stage=0`?).

**Thirty-ninth v6.18.46 hardware test (`putty.log.143`, implied):
still did not reproduce, despite round-27 removing far heavier
instrumentation than round-25 added.** Rechecked which specific diag
edit lines up with reproduction actually stopping: the last build that
*did* reproduce was `#70` (round-24, tested by `putty.log.140`).
`#71` (round-25 -- added the `stage=0`/`stage=4` entry/exit markers)
was the very first build to come back "не воспроизводится", and every
build since (`#72` round-26, `#73` round-27) removed *other*,
heavier-weight diagnostics instead and still didn't reproduce. By
elimination, round-25's own two-line addition is now the only code
difference left between the last-known-reproducing state and the
current tree that hasn't been tried as the culprit.

Reverted (diag round-28): the round-25 `stage=0` marker at the top of
`do_idle()` and `stage=4` marker at the very end. Back to exactly
round-23's three markers (`1`/`2`/`3`), which were present in every
build that successfully reproduced the hang (`putty.log.138/139/140`).
This sacrifices the "stuck in the tail vs. never re-entering" question
round-25 was designed to answer, but restoring reproducibility at all
takes priority -- if this build reproduces, add `0`/`4` back on their
own once the hang is caught again, rather than pre-emptively, so any
future correlation with a lost repro is unambiguous.

**Fortieth v6.18.46 hardware test (`putty.log.141`, build `#74`,
round-28): reproduced, and answered its own question by accident.**
Removing round-25's markers restored reproduction -- `stage=3` was
frozen across all 7 watchdog reads (15.7s-26.2s), same signature as
`putty.log.140`. But this boot also tripped an independent RCU-stall
self-detected-stall NMI backtrace at t=23.353s, which caught CPU0's PC
live: `cpu_idle_poll.isra.0+0x5c/0x60`, called from `do_idle+0x10c`,
called from `cpu_startup_entry`. That backtrace was sampled *between*
two watchdog reads that both said `stage=3` (22.06s and 24.13s) --
meaning CPU0 must have called `do_idle()` again and be spinning inside
`cpu_idle_poll()` at that moment, yet the watchdog still read `stage=3`
because nothing resets it to a lower value at function entry anymore.
**`stage=3` frozen never actually proved "stuck in the tail" -- round-25
was asking the right question, its own instrumentation just correlated
with breaking reproducibility.**

Added diag round-29: a lighter version of round-25 -- a single
`unsigned long cma_diag_cpu0_do_idle_calls` counter, incremented once
at the very top of `do_idle()` (one write per call, vs. round-25's two
separate assignments). Frozen across watchdog reads means genuinely
stuck in one specific `do_idle()` call's tail; increasing means
`do_idle()` keeps being re-entered and cycling through
`schedule_idle()` without ever picking the target task -- which the
`putty.log.141` backtrace already suggests is what's actually
happening. Rebuilt and repackaged; not yet hardware-tested with this
counter.

**Forty-first v6.18.46 hardware test (`putty.log.142`, build `#75`,
round-29): reproduced, and round-25's original question is now
answered for real.** `do_idle calls=135` was frozen across all 8
watchdog reads (15.9s-34.8s), spanning both before *and after* this
boot's own RCU-stall NMI backtrace at t=23.404s -- `do_idle()` is
genuinely **not** being called again. That same NMI backtrace caught
CPU0's PC inside `cpu_idle_poll()` (`cpu_idle_poll.isra.0+0x58/0x60`,
via `do_idle+0x124/0x278`) *during* that same frozen call #135 --
meaning `stage=3` was stale, left over from call #134, not evidence
about call #135 at all. **Conclusion: CPU0 is stuck spinning inside a
single, still-running `cpu_idle_poll()` call -- neither re-entering
`do_idle()` nor stuck in its tail after `schedule_idle()` returns.**

Checked whether this is a compiler bug (the load hoisted out of the
loop into a register, never re-reading memory): disassembled
`cpu_idle_poll.isra.0` from this exact build --

```
24: ldr x0, [x20]        ; reload current_thread_info()->flags (x20 = sp_el0)
28: tbz w0, #1, 48        ; test TIF_NEED_RESCHED
...
58: yield                 ; cpu_relax()
5c: b 24                  ; back to the top -- reload, not a cached register
```

The load genuinely re-executes every iteration. This rules out simple
compiler-side caching. So: CPU0's own `ldr` from `current`'s
`thread_info->flags`, re-executed every single iteration for 20+
seconds, apparently never observes the same bit that an external CPU's
read of the exact same struct consistently sees set the whole time.

Added diag round-30 (user-selected next step): an IPI-based probe.
`cma_diag_task0_dump()`'s 2s watchdog now fires
`smp_call_function_single(0, cma_diag_cpu0_ipi_probe, NULL, 1)` before
each dump -- this runs a tiny function **on CPU0 itself**, interrupting
whatever it's doing (CPU0 already reliably takes timer interrupts
during the stuck spin, since irqs are enabled the whole time -- that's
how the existing 2s dumps and the RCU-stall NMI both already get
through). The probe reads `current_thread_info()` -- the same
`sp_el0`-based access `cpu_idle_poll()`'s own loop uses, just from a
different call site -- and records the address and flags value it saw
into `cma_diag_cpu0_ipi_{calls,addr,flags}`, printed right next to the
external `idle` task's own `tif_flags`/`thread_info` address for a
direct, same-instant comparison. If the IPI probe (executing literally
on CPU0) also reads `flags=0` on the same address the external read
sees as `0x12`, that's strong evidence of a genuine hardware
cache-coherency issue rather than anything specific to
`cpu_idle_poll()`'s own code. Rebuilt and repackaged; not yet
hardware-tested.

**Forty-second v6.18.46 hardware test (`putty.log.143`, build `#76`,
round-30): the IPI probe result is conclusive.** All 10 probes
(16.06s-35.07s) read `flags=0x00000012` (`TIF_NEED_RESCHED` set) at
the exact same address (`0xffff800080d2fcc0`) the external `idle` read
also showed set every time -- while `do_idle calls=109` stayed frozen
the whole time, and a third NMI/RCU-stall backtrace (this boot's own,
at t=23.407s) again caught CPU0's PC in `cpu_idle_poll()`. **This rules
out a general hardware coherency failure**: CPU0 demonstrably *can*
read the bit as set via the IPI code path, at the same address, at
essentially the same real-world instants its own polling loop is
failing to see it. The one concrete difference between the two read
paths is `cpu_relax()`'s `yield` hint, present in the polling loop but
not in the IPI handler's plain load.

Added diag round-31 (user-selected next step): dropped `yield` from
`cpu_idle_poll()`'s loop, replacing `cpu_relax()` with a bare
`barrier()` (keeps the same compiler-reload guarantee, emits no
instruction) -- to test whether this specific Cortex-A53
implementation/SoC has a `yield`-triggered erratum that stalls
snoop/coherency responsiveness for a hart continuously executing that
hint. If dropping `yield` fixes the hang, that's the root cause;
`cpu_relax()` gets restored either way once this is settled. Rebuilt
and repackaged; not yet hardware-tested.

**Forty-third v6.18.46 hardware test (`putty.log.145`, build `#77`,
round-31): the `yield` hypothesis is refuted, and a bug in our own
IPI probe was caught in the process.** Same exact signature as every
prior reproduction: `do_idle calls=101` frozen across 9 reads
(16.1s-33.2s), IPI probe reading `flags=0x00000012` every time, a
fourth NMI/RCU-stall backtrace again landing in `cpu_idle_poll()`
(`pc == lr == cpu_idle_poll.isra.0+0x24/0x5c`, i.e. right at the
`ldr` reload, consistent with the `yield` removal shrinking the loop
body). Dropping `yield` did **not** fix the hang -- ruled out.

While reproducing, this boot's watchdog timer happened to fire on
CPU2 instead of CPU0, and its own backtrace (from an unrelated
`WARN_ON_ONCE`) exposed a real bug in the round-30 diagnostic itself:
`cma_diag_task0_dump()` is a `DEFINE_TIMER` callback (softirq context),
but it was calling `smp_call_function_single(0, ..., wait=1)`, which
hit `WARN_ON_ONCE(!in_task())` at `kernel/smp.c:669` -- that blocking
API was never valid to call from softirq context (risk of the
csd_lock/IPI-timing deadlock its own comment describes). The 10/10
consistent `flags=0x12` readings across two separate boots
(`putty.log.143`, `.145`) are still the best evidence available that
it was actually executing correctly on CPU0 each time, but the misuse
itself needed fixing regardless of whether it happened to work.

Fixed (diag round-32): switched to the interrupt/softirq-safe
`smp_call_function_single_async()` with a persistent
`call_single_data_t`, instead of the blocking, task-context-only API.
The printed values now lag by up to one 2s tick (whatever the most
recently completed async probe captured) rather than being
synchronous with each exact dump -- an acceptable trade-off at this
granularity. Also restored `cpu_relax()`'s `yield` in
`cpu_idle_poll()` -- round-31's question is answered (refuted), so no
reason to keep deviating from the stock loop. Rebuilt and repackaged;
not yet hardware-tested.

**Forty-fourth v6.18.46 hardware test (`putty.log.146`, build `#78`,
round-32): confirms the fixed IPI probe was reliable, and sharpens the
mystery further.** No `WARN_ON_ONCE(!in_task())` this time -- the
round-32 fix works. All 12 async IPI probes (15.8s-39.1s) still read
`flags=0x00000012` at the same address the external `idle` read also
showed set every time; `do_idle calls=191` frozen throughout; a fifth
NMI/RCU-stall backtrace (across five separate boots now) again caught
CPU0's PC in `cpu_idle_poll()`.

Logic check: `tbz w0, #1, ...` branches (continues polling) only when
bit 1 is *clear*. If `cpu_idle_poll()`'s own `ldr` really loaded
`0x12` (bit 1 set) at any point, the branch should fall through and
the loop should end. Since it never does, CPU0's own load, at the
specific instants it executes, must be seeing something *without* bit
1 set -- while every external/IPI sample around it sees `0x12`. Added
diag round-33 to get direct proof instead of inferring it: latches the
*exact* value `cpu_idle_poll()`'s own loop condition loads and
branches on into `cma_diag_cpu0_last_seen_flags`, every iteration
(CPU0 only, to avoid bouncing the cacheline on every other CPU).
Printed next to the IPI probe's and the external read's values for a
three-way, same-dump comparison: if this also reads `0x12`, the loop's
own *load* was never the problem and the bug is in the branch/exit
logic itself (or something stranger); if it reads something without
bit 1 set while the other two show it set, that's direct, no-longer-
inferred proof of the mismatch. Rebuilt and repackaged; not yet
hardware-tested.

**Forty-fifth v6.18.46 hardware test (`putty.log.147`, build `#79`,
round-33): direct, no-longer-inferred proof.** `cpu0 last seen
flags=0x00000010` on **all 12** reads (15.8s-40.3s) -- bit 1
(`TIF_NEED_RESCHED`) genuinely clear, at the exact instant
`cpu_idle_poll()`'s own loop condition loads and branches on it --
while at the very same moments the IPI probe (running literally on
CPU0, via a real interrupt) and the external cross-CPU read both show
`0x00000012` (bit set). Two different read paths, same CPU, same
address, essentially the same instants, different values. A sixth NMI
backtrace (across six boots now) again landed in `cpu_idle_poll()`.

This is the textbook failure mode of a plain busy-wait `ldr` loop on
arm64: the architecture does not guarantee *timely* visibility of a
concurrent store to a plain load without a barrier or the `ldxr`/`wfe`
idiom -- only eventual consistency. `cpu_idle_poll()`'s bare
`ldr`+`yield` loop only runs at all because arm64 doesn't define
`TIF_POLLING_NRFLAG` (established very early in this investigation),
forcing the generic busy-poll fallback -- and that fallback is only
reached in the first place because `nohlt` is in `CONFIG_CMDLINE`,
forcing `cpu_idle_force_poll` permanently on. Without `nohlt`, CPU0
would idle via the normal `cpuidle_idle_call()` -> `WFI` path instead,
woken by the same IPI mechanism already confirmed reliable throughout
this investigation (real hardware interrupts inherently synchronize
memory state on wake, unlike a bare polling loop).

Two tests queued, in order:

1. **Drop `nohlt` entirely** (`CONFIG_CMDLINE` in `.config`) --
   sidesteps `cpu_idle_poll()`'s busy-poll fallback altogether by
   letting CPU0 go back to `WFI`. If this alone fixes the boot hang,
   `nohlt`'s presence (added earlier in this investigation, predating
   this excerpt) was the actual root cause all along, and the fix is
   simply not forcing this fallback path to be used for extended
   periods. Applied now; rebuilt and repackaged; not yet
   hardware-tested. Note: with `nohlt` gone, `cpu_idle_poll()` won't
   normally run at all, so `cma_diag_cpu0_last_seen_flags` will stay
   at its initial `0` (unused, not misleading) -- the IPI probe and
   the core watchdog stay meaningful regardless, since they don't
   depend on `nohlt`.
2. **If still needed**: put `nohlt` back and try a real hardware `dsb
   sy` inside the loop instead of `cpu_relax()`'s `yield` -- round-21
   already tested `smp_mb()` (`dmb`) in an earlier, differently-shaped
   version of this loop and it didn't help, but `dsb` is a materially
   stronger barrier and worth testing precisely against the current,
   much better-understood minimal loop.

**Forty-sixth v6.18.46 hardware test (`putty.log.148`, build `#80`,
round-34): test 1 (drop `nohlt`) does not fix the hang, and actually
makes it harder to see.** No `diag:` watchdog output *at all* this
run -- `__lru_add_drain_all()`'s CPU0-forcing/timer-arming is all
still gated correctly, so this just means the round-30+ diagnostics
are wired to the busy-poll path specifically and go dark without
`nohlt`. The RCU stall (t=23.326s) and a later "expedited stalls" NMI
backtrace (t=35.962s, 12.6s later) show `pc: arch_local_irq_enable`,
`lr: default_idle_call+0x28/0x34` -- and **every single register is
byte-for-byte identical between the two dumps, 12.6 seconds apart**.
That's not a live spin (which would perturb at least some register
via interrupts/ticks in that window) -- CPU0 genuinely never woke from
`WFI` at all.

This isn't a new mechanism: this README already established, much
earlier in this investigation (`putty.log.123`, well before this
excerpt) that the "lost wakeup in WFI" hypothesis was tested and
falsified -- the hang was seen identically with and without `nohlt`
even then, because arm64 never defines `TIF_POLLING_NRFLAG`, so the
scheduler's wakeup path always sends a real IPI regardless of whether
the target CPU is busy-polling or parked in `WFI` (`set_nr_if_polling()`'s
fast path never applies on this arch). Both regimes hit the same
underlying missed-wakeup; `putty.log.148` just makes the WFI-frozen
half of that finding much more vivid than the older logs did, thanks
to the register-level detail modern NMI backtraces give us now.

Restored `nohlt` (diag round-35) -- this investigation's entire
watchdog/IPI-probe/last-seen-flags apparatus (rounds 15-33) depends on
the busy-poll path to have any visibility into the hang at all, and
test 1 didn't fix anything to justify losing that. Proceeding to test
2: replaced `cpu_relax()` with a real hardware `dsb sy` in
`cpu_idle_poll()`'s CPU0 branch (the one branch with direct,
round-33-confirmed proof of the mismatch), testing whether a full
data-synchronization barrier -- materially stronger than round-21's
`smp_mb()`/`dmb`, tested back then in an earlier, differently-shaped
version of this loop -- forces the core to catch up on pending
coherency traffic that a plain `ldr` alone doesn't wait for. Rebuilt
and repackaged; not yet hardware-tested.

**Forty-seventh v6.18.46 hardware test (`putty.log.149`, build `#81`,
round-35): inconclusive on `dsb sy`, but a second, different hang
surfaced.** No `diag:` watchdog output at all -- but that's not proof
`dsb sy` fixed anything: the watchdog timer is disarmed via
`timer_delete_sync()` the moment `flush_work()` returns, so a
`lru_add_drain_per_cpu` that completes within 2s (success, unrelated
to this test) looks identical to one that never even got a chance to
hang, exactly like every other non-reproducing boot in this whole
investigation.

The system hung anyway, just later and elsewhere: `kworker/0:1`
(same kworker number, now processing `Workqueue: usb_hub_wq hub_event`
-- meaning it *did* get past whatever `lru_add_drain_per_cpu` work it
may have had queued earlier, since a worker can't dequeue a new item
while stuck on a previous one) froze inside `console_unlock()` ->
`vprintk_emit()`, printing a `_dev_info()` message for the newly
found USB device. Three NMI/RCU-stall samples spanning 63 seconds
(23.3s, 36.0s, 86.3s) show **byte-for-byte identical registers** at
`pc: arch_local_irq_restore+0x4/0x8`, `lr: console_flush_all+0x1fc/0x244`
-- a genuine freeze, not slow progress.

Two readings are both consistent with this one log: `dsb sy` fixed the
original `cpu_idle_poll()` bug and this boot separately, unluckily hit
an unrelated pre-existing `console_unlock()` bug that simply hadn't
been reached before (every previous hang stopped the boot too early to
get here); or this is the same underlying issue resurfacing through a
different busy-wait-shaped code path. Can't distinguish from one
sample. Re-running the identical build (`#81`, no code changes) to see
whether `console_unlock()` hangs again (real, separate bug), the
original `cpu_idle_poll()` signature comes back (`dsb sy` doesn't
reliably fix it, and this run's `console_unlock()` hang was a
red herring), or the boot goes clean (`dsb sy` actually works and this
was a one-off).

**Forty-eighth v6.18.46 hardware test (`putty.log.150`, build `#81`,
re-run): `dsb sy` refuted -- the original signature came back exactly
as before.** All 10 watchdog reads (15.7s-34.8s) show `cpu0 last seen
flags=0x00000010` again, external/IPI reads still `0x12`, a seventh
NMI backtrace (across boots) again landed in `cpu_idle_poll()`. The
`console_unlock()` hang in `putty.log.149` was a one-off, unrelated to
this investigation -- `dsb sy` does **not** fix the mismatch, exactly
like `smp_mb()`/`dmb` in round-21. Both barrier strengths available
short of a full cache-maintenance operation have now been tried and
refuted. Reverted to stock `cpu_relax()`. Rebuilt, repackaged, and
pushed as build `#82` (round-36) -- no functional change from build
`#79`/round-33 other than the comment update, so no new data was
expected from it specifically; it existed only to keep the tree at a
clean, documented baseline.

Added diag round-37: a plain `ldr` isn't the architecturally-endorsed
way to busy-wait for a memory value to change on arm64 -- that's
`ldxr`+`wfe` (`__cmpwait_relaxed()`, the same primitive
`smp_cond_load_relaxed()` uses throughout the kernel, e.g. qspinlock).
`ldxr` establishes this core's local exclusive monitor on the address;
the architecture guarantees that monitor is cleared -- waking a
subsequent `wfe` -- by any store to that address from any observer.
That's a fundamentally different mechanism than a barrier of any
strength (both already refuted): it's the CPU registering active
interest in this specific address's writes, rather than a plain load
hoping to observe them promptly. Swapped `__cmpwait_relaxed(&current_thread_info()->flags,
flags)` in for `cpu_relax()` in `cpu_idle_poll()`'s CPU0 branch (added
`#include <asm/cmpxchg.h>` to `kernel/sched/idle.c` for it). Rebuilt
and repackaged; not yet hardware-tested.

**Forty-ninth v6.18.46 hardware test (`putty.log.151`, build `#83`,
round-37): `ldxr`+`wfe` refuted too -- the identical signature came
back.** All 9 watchdog reads show `cpu0 last seen
flags=0x00000010` again, external/IPI reads still `0x12`, an eighth
NMI backtrace again landed in `cpu_idle_poll()`. Every busy-wait
mechanism available on the reader side -- plain `ldr`, `dmb`, `dsb
sy`, and now `ldxr`+`wfe` (the architecturally-endorsed idiom, which
*must* wake on any store to the address per the ARM ARM) -- shows the
identical mismatch. Suspicion moves to the writer.

Disassembled `__resched_curr()` (`kernel/sched/core.c`, compiled
standalone into `kernel/sched/core.o`) and `set_ti_thread_flag()`:

```
resched_curr:
  2140: ldr w1,[x5,#16]     ; smp_processor_id()
  2144: cmp w1,w4            ; vs cpu_of(rq)
  214c: b.ne 2164            ; different CPU -> remote path
  2150: bl set_ti_thread_flag  ; LOCAL path: no IPI at all
  2154: str wzr,[x5,#12]       ; set_preempt_need_resched()
  ...
  2164: bl set_ti_thread_flag  ; REMOTE path
  216c: bl arch_smp_send_reschedule  ; real IPI

set_ti_thread_flag (LSE-atomics alternative present but this
Cortex-A53 has none, so alternatives-patching takes the fallback):
  a4: ldxr x2,[x0]
  a8: orr  x2,x2,x1
  ac: stxr w3,x2,[x0]
  b0: cbnz w3, a4              ; standard atomic bit-set, retried
```

Both individually look completely correct -- no obvious instruction-
level bug on either side. But `sched_ttwu_pending()` (which activates
our queued kworker and calls `resched_curr()`) runs *on the target
CPU itself*, servicing the already-confirmed-reliable
`smp_call_function`/`IPI_CALL_FUNC` that delivered the wakeup -- so
for our specific scenario, `resched_curr()`'s **local** branch should
fire: same-CPU write, no IPI, which architecturally *must* be visible
to a subsequent same-CPU read in program order. If that's really what
happens, the `cpu_idle_poll()` mismatch shouldn't be possible at all
by any sane hardware's rules -- which means either this reasoning
about which branch fires is wrong, or (matching this investigation's
own much earlier, independently-reached finding from `putty.log.124`:
"the reschedule signal to CPU0... is not being acted on, even though
the *separate* IPI used to request th[e] NMI backtrace clearly *does*
reach CPU0") the **remote** path is what's actually firing, and
`arch_smp_send_reschedule()`'s specific IPI vector is the one that
doesn't reliably get through, unlike the `smp_call_function` vector.

Added diag round-38 (user-approved): instrumented `__resched_curr()`
to record, only during the diag window and only for `cpu_of(rq)==0`,
how many times it's called for CPU0's rq, how many took the local
path vs. the remote path, and which CPU called it last
(`cma_diag_cpu0_resched_{calls,local_calls,remote_calls}`,
`cma_diag_cpu0_resched_caller_cpu`) -- settling with data which branch
actually fires, instead of reasoning about which one should. Printed
by the watchdog in `mm/swap.c`. Rebuilt and repackaged; not yet
hardware-tested.

**Fiftieth v6.18.46 hardware test (`putty.log.152`, build `#84`,
round-38): a real surprise -- `__resched_curr()` never fires for
CPU0's rq at all, the whole hang.** All 20 watchdog reads (15.9s-56.0s)
show `cpu0 resched calls=0 local=0 remote=0 caller_cpu=-1` -- yet
`tif_flags=0x00000012` (set) from the very *first* read, `do_idle
calls=85` frozen throughout, and an eighth NMI backtrace (across
boots) again in `cpu_idle_poll()`. Round-38's counters only increment
*past* `__resched_curr()`'s early-return check (`if (cti->flags &
(...)) return;`, line 1118) -- so `calls=0` can't distinguish "this
function is never called at all for CPU0's rq" from "it's called, but
the bit already reads set every single time and it bails immediately,
before reaching round-38's counters." Given `TIF_NEED_RESCHED` was
already `0x12` at our very first 2s-spaced sample, the bit could well
have been set once, very early -- possibly even before the specific
wakeup this investigation has been chasing -- and simply never
consumed/cleared since (clearing normally happens inside `__schedule()`
during a real context switch, which never completes for CPU0 here).

Added diag round-39: two more counters at the very top of
`__resched_curr()`, before the early-return check --
`cma_diag_cpu0_resched_entries` (every entry for CPU0's rq) and
`cma_diag_cpu0_resched_early_return` (how many of those saw the bit
already set and bailed). `entries==0` would mean the function is
genuinely never invoked for CPU0's rq during the whole window (a much
bigger finding, since something still has to be the reason `on_rq=1`
happened at all); `entries>0` with `early_return==entries` would
confirm it's called but always finds the bit pre-set. Rebuilt and
repackaged; not yet hardware-tested.

**Fifty-first v6.18.46 hardware test (implied): did not reproduce.**
Requested cleanup of diagnostics correlating with the loss. The last
build that *did* reproduce was `#84` (round-38, `putty.log.152`);
`#85` (round-39 -- added the `entries`/`early_return` counters at the
very top of `__resched_curr()`) was the first build to come back
non-reproducing. Unlike most of this investigation's diagnostics,
`__resched_curr()` is not scoped to CPU0 or gated behind anything
before the added check runs -- it fires on *every reschedule
decision, on every CPU, system-wide* -- making it by far the hottest
site any diagnostic has touched here, and the prime suspect the moment
reproducibility broke immediately after adding to it.

Reverted (diag round-40): the round-39 entry/early-return counters,
back to exactly round-38's state (`calls`/`local_calls`/`remote_calls`/
`caller_cpu`), which were already present in build `#84` and still
reproduced. This sacrifices being able to distinguish "never called"
from "always early-returns" for now, but restoring reproducibility
takes priority. Rebuilt and repackaged; not yet hardware-tested.

Added diag round-41: a lighter stand-in for what round-39 was trying
to answer, at a much colder call site. `kick_pool()`
(`kernel/workqueue.c`) only runs when work is actually queued -- far
less often than `__resched_curr()` fires on every reschedule decision
system-wide. Right after `wake_up_process()` returns for our target
kworker, a single read checks whether `TIF_NEED_RESCHED` is already
visible on CPU0's idle task (`cma_diag_cpu0_kick_resched_seen`): if
set, `__resched_curr()`'s local (same-CPU, synchronous, no-IPI) path
already fired by the time `wake_up_process()` returned. Printed by the
watchdog. Rebuilt and repackaged; not yet hardware-tested.

**Fifty-second v6.18.46 hardware test (implied): did not reproduce
again.** Requested a genuinely different diagnostic technique rather
than another revert-and-retry. Considered enabling the kernel's
existing `sched_wakeup`/`sched_switch` ftrace events (zero source
changes, using already-battle-tested infrastructure) -- checked and
ruled out: `CONFIG_FTRACE` is off in this `.config`, and the
already-built `kernel/sched/core.o` has *zero* tracepoint-related
symbols (`nm` confirms no `__tracepoint_*`/`__traceiter_*` entries at
all), so `sched_set_need_resched_tp` and friends are compiled-out
stubs here, not usable without a much larger `.config` change.

Reverted (diag round-42): round-41's `kick_pool()` check -- on
reflection it ran *while still holding `pool->lock`*
(`lockdep_assert_held(&pool->lock)` guards the whole function),
extending that critical section, which is a more invasive change than
it looked. Replaced with a genuinely passive alternative: a plain,
side-effect-free accessor, `cma_diag_cpu0_nr_running()`
(`kernel/sched/core.c`), that just returns `cpu_rq(0)->nr_running` --
exported so the existing cold 2s watchdog in `mm/swap.c` can sample it
directly, without adding anything to any hot *or* locked scheduler
path at all. (Round-24 already found `nr_running==1` at one specific
instant inside `__schedule()`; this tracks it continuously instead,
from entirely outside the scheduler's own execution.) Rebuilt and
repackaged; not yet hardware-tested.

**Fifty-third v6.18.46 hardware test (implied): did not reproduce a
third time in a row.** Requested a return to the last confirmed-
reproducing state rather than another substitution. That state is
build `#84` (round-38/40, `putty.log.152`) exactly -- everything added
since then has either already been reverted (round-39's
`__resched_curr()` entries, round-41's `kick_pool()` check) or, as of
this round, needs to be: round-42's `cma_diag_cpu0_nr_running()`
accessor, since build `#88`, the first build to include it, was also
the first that stopped reproducing.

Reverted (diag round-43): the `cma_diag_cpu0_nr_running()` accessor
and its watchdog print line. The tree is now byte-for-byte equivalent,
diagnostic-wise, to build `#84`'s state -- `kernel/workqueue.c`'s
`kick_pool()` is back to its original round-15 form (just the
`cma_diag_task0` stash), and `kernel/sched/core.c`'s `__resched_curr()`
carries only round-38's `calls`/`local_calls`/`remote_calls`/
`caller_cpu` counters. Three consecutive diagnostic additions in a row
(round-39, -41, -42) have now each been followed by non-reproduction;
whether that's a real, cumulative sensitivity to *any* further
instrumentation or just this bug's known low natural reproduction rate
reasserting itself is still an open question, but for now the priority
is re-confirming reproducibility actually returns at this exact,
previously-working baseline before adding anything else. Rebuilt and
repackaged; not yet hardware-tested.

**Fifty-fourth v6.18.46 hardware test (implied): reproduced -- build
`#89`'s restored baseline is confirmed good.** Requested an
alternative diagnostic that doesn't break reproduction again. Noting
that round-39/41/42 -- the three additions that each correlated with
losing reproducibility -- all added a *new call site* to
`kernel/sched/core.c`, `kernel/workqueue.c`, or a newly *exported*
function, while round-38 (which reproduced fine, twice now) only
added code *inside* an already-existing, already-hot function without
introducing a new one. Whether the mechanism is execution-frequency,
code-layout/icache effects from touching those specific files, or
coincidence remains unclear -- but the safest next step is to add
nothing further to `kernel/sched/*.c` or `kernel/workqueue.c` at all.

Added diag round-44: extended the watchdog's existing print function
in `mm/swap.c` (the same file every already-safe diagnostic since
round-15 has lived in) with `idle_task(0)->se.exec_start`,
`sched_clock()`, and `cma_diag_task0->se.exec_start`.
`sched_entity.exec_start` is a plain, publicly visible `task_struct`
field the *unmodified* scheduler already updates on its own every
time a task (including idle) is picked to run, inside
`set_next_entity()` -- reading it is purely passive, in the same
category as every diagnostic that has already reproduced
successfully, and touches none of the three suspect files at all. If
idle's `exec_start` advances across dumps, `__schedule()` is
genuinely re-picking idle periodically (corroborating `do_idle_calls`
from an independent field); `sched_clock()` alongside it shows how
long ago in real time. Rebuilt and repackaged; not yet
hardware-tested.

**Fifty-fifth v6.18.46 hardware test (`putty.log.153`, build `#90`,
round-44): reproduced again with the passive `exec_start` addition in
place -- two clean reproductions in a row for the "stay out of
`kernel/sched/*.c`/`kernel/workqueue.c`" approach.** `cpu0 idle
exec_start=2320155249` frozen across all 8 dumps (15.8s-31.0s) while
`now` (`sched_clock()`) climbs normally -- independent confirmation,
via a field none of this investigation's own code touches, that
`set_next_entity()` hasn't fired for idle since ~2.32s into boot, well
before the SATA/CMA activity that leads to the hang even starts.
`cpu0 resched calls=0 local=0 remote=0` still 0, the one question
round-38 alone can't resolve (does `__resched_curr()` ever get called
for CPU0's rq at all, or does it just always hit the early-return
before reaching round-38's counters).

Asked for an alternative *safe* way to answer that specific question.
Checked `CONFIG_KPROBES`/`CONFIG_FTRACE`: both off, and re-enabling
either is itself a systemic change (kprobes patches live code and
synchronizes across all CPUs when armed/disarmed; ftrace's tracing
infrastructure is broad) -- not obviously safer than editing
`core.c` directly. Given that trade-off, explicitly took the risk
(user-approved): enabled both via `make olddefconfig` after flipping
`CONFIG_KPROBES=y`/`CONFIG_FTRACE=y` in `.config` (pulls in
`TRACEPOINTS`, `TRACING`, `RING_BUFFER`, `EVENT_TRACING`,
`KPROBE_EVENTS`, etc. as dependencies; `FUNCTION_TRACER` stayed off --
that one instruments every function call and would have been far
heavier).

Added diag round-45: a `kprobe` on `resched_curr()` (confirmed via
`nm` that `__resched_curr()` is fully inlined into it -- `resched_curr`
is the only symbol that exists), dynamically armed/disarmed exactly
bracketing the SATA CMA alloc window in
`kernel/dma/contiguous.c`/`mm/cma.c` (the same window
`cma_diag_trace_next` already brackets). Its `pre_handler` fires
unconditionally on the function's very first instruction, before any
of `resched_curr()`'s own logic (including the early-return check)
runs -- a true, unconditional entry count, obtained *without changing
a single byte of `kernel/sched/core.c`'s compiled object code*, unlike
round-39. `cma_diag_read_resched_kprobe_{calls,hits}()` report total
entries (any CPU) and how many ran on CPU0 itself. This is a
genuinely different kind of risk than round-39/41/42 (dynamic code
patching + cross-CPU sync at arm/disarm time, rather than added
per-call overhead or code-layout shifts) -- may or may not preserve
reproducibility; that's exactly what this test is for. Rebuilt and
repackaged; not yet hardware-tested.

**Fifty-sixth v6.18.46 hardware test (`putty.log.160`, build `#91`,
round-45): reproduced -- the kprobe itself does NOT break
reproducibility, unlike round-39/41/42's `core.c`/`workqueue.c`
edits.** But the actual result is more interesting than "does it
reproduce": `cpu0 resched_curr kprobe calls=0 cpu0_hits=0` across all
9 dumps (15.7s-36.2s). Unlike round-38's `resched calls=0` (which
only counts *past* `resched_curr()`'s own early-return check and so
can't tell "never called" from "called but bailed out early"), the
kprobe's `pre_handler` fires unconditionally on the function's first
instruction -- `calls=0` here means `resched_curr()` was not entered
*at all*, by any CPU, anywhere, during the whole window the kprobe
was armed.

That's a stronger and more surprising result than expected, because
`resched_curr()` should be unavoidable here: the idle scheduling
class's `wakeup_preempt_idle()` (`kernel/sched/idle.c`) calls
`resched_curr(rq)` *unconditionally* -- no early-return check of its
own -- whenever a task is woken onto an idle CPU. `cma_diag_task0`
(the kworker/0:1 wakeup recorded in `kick_pool()`, gated on this same
`cma_diag_trace_next` window) is confirmed to have fired, since
`sched_show_task()` correctly dumps it every time. So the wakeup
that should trigger `resched_curr()` for CPU0 demonstrably happens,
yet the kprobe -- armed for exactly that window -- never saw it.

The only consistent explanation: the window is too narrow. It's
armed/disarmed tightly around `cma_alloc_aligned()` itself
(`kernel/dma/contiguous.c`), but the actual `wake_up_process()` call
for the CPU0 kworker -- and whatever `resched_curr()` call follows
from it -- apparently happens *after* `cma_alloc_aligned()` has
already returned and the kprobe has already been disarmed. This
reopens the round-30/round-38 question with a sharper edge: does
`resched_curr()` for this specific wakeup ever get called at all
(anywhere, any time, not just within this narrow window), or is the
bit CPU0's idle task already carries (`flags=0x12`, present from the
very first dump onward) a leftover from a much earlier, unrelated
`resched_curr()` call -- consistent with `se.exec_start` having been
frozen since ~2.32s into boot (round-44), well before this SATA/CMA
activity even starts (~13.7s)?

Round-46: widen the kprobe's armed window past `cma_alloc_aligned()`'s
return instead of disarming it there immediately -- keep it armed and
disarm it later, from the watchdog itself (`mm/swap.c`), after a
bounded number of dumps (3, i.e. up to ~6s past when the watchdog
starts firing), so the window stays temporally bounded (avoiding
round-39's failure mode of instrumenting the *entire* boot's
reschedule traffic) while being wide enough to actually catch the
kworker wakeup's `resched_curr()` call, if one is ever made at all.
`kernel/dma/contiguous.c` no longer disarms the kprobe after
`cma_alloc_aligned()` returns; only `cma_diag_arm_resched_kprobe()`
remains there. Rebuilt and repackaged; not yet hardware-tested.

**Fifty-seventh v6.18.46 hardware test (`putty.log.161`, build `#92`,
round-46): reproduced again -- the widened kprobe window does NOT
break reproducibility either.** `cpu0 resched_curr kprobe
calls=0 cpu0_hits=0` across all 12 dumps (15.8s-40.4s), i.e. still 0
across the full ~6.3s widened window (armed from the SATA CMA alloc
call through 3 watchdog ticks, roughly 13.7s-20.1s) that comfortably
covers the point where the CPU0 kworker wakeup demonstrably happens
(`sched_show_task()` shows it queued the whole time, this run on the
`rcu_gp` workqueue rather than `events_long` -- confirms this isn't
specific to one workqueue). Widening the window changed nothing:
`resched_curr()` is never entered, period, for as long as we've been
willing to watch.

Since `wakeup_preempt_idle()` (`kernel/sched/idle.c`) calls
`resched_curr(rq)` unconditionally the moment `ttwu_do_activate()`
runs, `resched_curr()` never being called means `ttwu_do_activate()`
itself is never called for this wakeup either. On SMP, waking a task
onto an idle *remote* CPU can instead be deferred: the waking CPU
only queues the task onto the target's `wake_list`
(`ttwu_queue_wakelist()`) and sends an IPI; `ttwu_do_activate()` (and
therefore `resched_curr()`) only actually runs when the *target* CPU
processes that queue for itself, from inside its own IPI handler
(`sched_ttwu_pending()`). If CPU0 never runs `sched_ttwu_pending()`
for this task, the entire activation -- including `resched_curr()` --
would never happen, which is exactly what's observed. This also
resolves an apparent contradiction with round-30: the diagnostic
watchdog's own `smp_call_function_single_async()` probe (a *different*,
generic SMP-call-function IPI) demonstrably keeps being serviced by
CPU0 throughout the same window (`ipi probe calls` keeps incrementing
every dump) -- so this isn't "CPU0 loses all interrupts" in general,
it would be something specific to the scheduler wakeup/wakelist path.

Round-47: add a second kprobe, on `sched_ttwu_pending()` itself (a
global symbol, confirmed via `nm`), using the exact same technique
already proven twice not to break reproducibility -- armed alongside
the `resched_curr` kprobe in `kernel/dma/contiguous.c` (`dev->cma_area`
branch) and disarmed together from `mm/swap.c`'s watchdog after the
same 3 ticks. `cma_diag_read_ttwu_pending_kprobe_{calls,hits}()`
report the same total/CPU0-only breakdown. If `cpu0_hits=0` here too,
that's about as close to direct proof as this investigation can get,
without ftrace, that CPU0 never runs its own wakeup-processing IPI
handler during the hang. Rebuilt and repackaged; not yet
hardware-tested.

**Fifty-eighth v6.18.46 hardware test (`putty.log.162`, build `#93`,
round-47): did NOT reproduce -- twice in a row (the board was power-
cycled and retested within the same log).** Both boots ran cleanly all
the way through SATA link-up, both drives identified and partitioned,
network DHCP -- no `diag:` watchdog output at all in the whole log,
meaning `cma_diag_task0` (the "kworker got stuck" flag) was never set
either time. Per the established pattern (round-39/41/42), treat the
newest addition as the prime suspect: reverted round-47's second
kprobe (`sched_ttwu_pending()`) entirely -- `include/linux/cma.h`,
`kernel/dma/contiguous.c`, `mm/cma.c`, `mm/swap.c` restored byte-for-
byte to round-46's state (commit `1c6dde201`), keeping only the
`resched_curr` kprobe with its widened window. Plausible reason this
one specifically broke reproducibility where round-45/46 didn't:
`sched_ttwu_pending()` is a much hotter function than `resched_curr()`
system-wide (it's the generic remote-wakeup IPI handler, not specific
to this one SATA-triggered wakeup), so a *second* live kprobe trapping
into it for the whole ~6.3s window plausibly added enough extra
latency, on top of the first kprobe, to disturb this very timing-
sensitive bug -- unconfirmed, same caveat as every other "which files/
which overhead level is safe" observation in this investigation.
Rebuilt and repackaged; awaiting the next test to confirm round-46's
state reproduces again before deciding how (or whether) to retest the
`sched_ttwu_pending()` question some other way.

**Fifty-ninth v6.18.46 hardware test (`putty.log.163`, build `#94`):
reproduced -- confirms the revert.** Back to round-46's single-kprobe
state and the hang is back too, `cpu0 resched_curr kprobe calls=0
cpu0_hits=0` again across every dump. `sched_ttwu_pending()` as a
*second* concurrently-armed kprobe is confirmed to be specifically
what broke round-47's reproducibility, not some unrelated fluke.

Round-48: isolate the variable. Arm ONLY the `sched_ttwu_pending()`
kprobe this round (`kernel/dma/contiguous.c` no longer arms
`resched_curr`'s), same window/disarm timing as round-46. Tests
whether `sched_ttwu_pending()` alone -- a much hotter, system-wide
function than `resched_curr()` -- is too disruptive on its own, versus
it only being a problem when two kprobes are concurrently armed. If
this single-probe test reproduces cleanly, that would mean the
CPU0-hits count for `sched_ttwu_pending()` can finally be trusted, and
would settle the "does CPU0 ever run its own wakelist IPI handler"
question raised in round-47. If it *also* fails to reproduce, that
points at `sched_ttwu_pending()` itself being too hot for this
technique regardless of concurrency, and a different approach would be
needed for that specific question. Rebuilt and repackaged; not yet
hardware-tested.

**Sixtieth v6.18.46 hardware test (`putty.log.164`, build `#95`,
round-48): reproduced -- confirms it was concurrency, not the
function.** `cpu0 ttwu_pending kprobe calls=0 cpu0_hits=0` across all
11 dumps, and this time the zero can be trusted: a single kprobe on
`sched_ttwu_pending()` alone does not disturb the bug. So CPU0 (and
in fact every CPU, since `calls` counts globally) genuinely never
enters `sched_ttwu_pending()` during the whole widened window.

Also noticed: `cpu0 idle exec_start=2392962509` and `kworker
exec_start=2343873398` -- both frozen in the ~2.3-2.4s range, i.e. the
*kworker itself* (not just idle) was last picked to run around the
same very early time as idle's own freeze point (round-44), long
before the SATA/CMA activity (~13.7s) that triggers this specific
`kick_pool()` call even starts.

This creates an apparent contradiction worth resolving directly:
`on_rq=1` for the kworker (confirmed every dump) can only become 1 via
`activate_task()`, called only from inside `ttwu_do_activate()` --
and *that* function unconditionally calls `wakeup_preempt()` ->
`resched_curr()` right after `activate_task()`, on either the local
(synchronous) or deferred (wakelist/IPI) wakeup path. Round-46 showed
`resched_curr()` calls=0 (a different boot); round-48 now shows
`sched_ttwu_pending()` calls=0 too. If neither ever fires, yet on_rq
flips to 1, either `ttwu_do_activate()` itself never runs either, or
it runs without reaching `wakeup_preempt()` the way this reasoning
assumes.

Round-49: swap in a kprobe on `ttwu_do_activate()` itself -- the
common function both the local and deferred wakeup paths funnel
through, right before `resched_curr()` -- using the same single-probe
pattern now proven safe twice. Rebuilt and repackaged; not yet
hardware-tested.

**Sixty-first v6.18.46 hardware test (`putty.log.165`, build `#96`,
round-49): reproduced -- and this closes out the whole wakeup-path
investigation with a negative result.** `cpu0 ttwu_activate kprobe
calls=0 cpu0_hits=0` across every dump, trustworthy (single probe,
reproduced cleanly). That's the third distinct function in the
activation chain (`resched_curr`, `sched_ttwu_pending`,
`ttwu_do_activate`) now individually confirmed, across three separate
boots using the same proven single-kprobe technique, to never be
entered at all during the hang.

The resolution to the "apparent contradiction" flagged after round-48:
`sched_show_task()`'s own output -- printed at the top of every single
watchdog dump since round-15, present in every log this entire
investigation has ever analyzed -- has always shown `state:R` for the
stuck kworker (confirmed again here explicitly: `task:kworker/0:0
state:R running task`, this boot's specific worker instance, pid 9,
different from other boots' pid 11 -- confirms this isn't specific to
one workqueue worker). `wake_up_process()` wakes with `TASK_NORMAL`
(`TASK_INTERRUPTIBLE|TASK_UNINTERRUPTIBLE`); `try_to_wake_up()`'s very
first check is `if (!(p->__state & state)) goto unlock;` -- a task
already in `TASK_RUNNING` fails that check and returns immediately,
*correctly*, without ever reaching `ttwu_do_activate()`,
`sched_ttwu_pending()`, or `resched_curr()`. `kick_pool()`'s recorded
wakeup (`cma_diag_task0`, set unconditionally whenever a CPU0 kick
happens during the diag window, regardless of whether the wake
actually does anything) is a legitimate, correct no-op on a task
that's already runnable -- not a lost signal, not a bug. Fifteen
rounds of writer/wakeup-path instrumentation (round-30 IPI probe
through round-49) converge on the same conclusion: there is no live
race here to lose, because the write that set `on_rq=1`/state=R
already fully completed and retired long before the SATA/CMA activity
that triggers the watchdog even starts (consistent with both idle's
and the kworker's own `se.exec_start` being frozen from very early
boot, round-44/48).

This closes the "does the wakeup/resched signal get lost" line of
investigation as conclusively answered: **no**. The only mystery left
standing -- unchanged since round-33, and now confirmed to be the
*entire* remaining question -- is why CPU0's own busy-loop read of
`current_thread_info()->flags` in `cpu_idle_poll()` returns `0x10`
(bit clear) while every other reader of the exact same address
(cross-CPU, IPI-triggered self-read) consistently returns `0x12` (bit
set), continuously, for 15+ seconds. Every software memory-ordering
primitive tried on the read side has failed to change this: `dmb`
(round-21), `dsb sy` (round-35), and the architecturally-endorsed
`ldxr`+`wfe` exclusive-monitor idiom (round-37, which should
*guarantee* a wake on any store to the address, via a fundamentally
different mechanism than a barrier). A staleness that persists for
15+ seconds, unresolved by the strongest ordering primitive the
architecture offers, is no longer explainable as an ordinary
coherency-propagation delay -- if it were, it would resolve in
nanoseconds, not indefinitely.

Round-50: one option not yet tried is forcing a `dc civac`
(clean+invalidate by VA to the point of coherency) on the exact
address immediately before each read, from CPU0's own loop only. This
is explicit cache-maintenance, architecturally unnecessary for normal
inter-CPU visibility of Normal Cacheable memory (that's what the
coherent interconnect's snoop protocol is for) -- so this is a
low-confidence, empirical last resort in the "make the read fresher by
any means available" category, worth trying mainly because the
principled alternatives are exhausted. If it changes nothing (most
likely outcome, given `ldxr`'s exclusivity semantics already require
reading from the point of coherency by architectural definition), that
would be one more strong data point that this is not a
cache/coherency-propagation issue at all, and the remaining
possibilities narrow toward either a genuine SoC/CPU coherency erratum
on this specific Cortex-A53 revision (`0x410fd034`) or something not
yet considered. Rebuilt and repackaged; not yet hardware-tested.

**Sixty-second v6.18.46 hardware test (`putty.log.166`, build `#97`,
round-50): reproduced -- `dc civac` changes nothing, as predicted.**
`cpu0 last seen flags=0x00000010` frozen across every single dump
(15.9s-49.9s), identical signature to every prior round. Explicit
cache clean+invalidate immediately before the read does not unstick
it, on top of `dmb`, `dsb sy`, and `ldxr`+`wfe` all already failing --
every read-side technique this investigation can construct in C or
inline asm has now been exhausted without effect. `MIDR_EL1 =
0x410fd034` decodes to ARM Cortex-A53, variant 0, revision 4 (r0p4).

Researched ARM's official Cortex-A53 MPCore Software Developers Errata
Notice (ARM-EPM-048406 v21, current through r0p4) for any documented
erratum matching this exact symptom. None found: the document lists no
erratum where a core fails to observe an already-retired,
already-globally-visible store indefinitely, insensitive to barriers
and the exclusive-monitor mechanism. The one plausible-sounding
candidate that spans every revision through r0p4, **855873** ("An
eviction might overtake a cache clean operation"), requires the
processor to be configured with an ACE bus interface *and* an L2
cache, and describes the CPU's own cache-clean-by-address instruction
racing an automatic eviction of the same line at the interconnect --
causing stale *dirty* data to linger, not a permanently-stuck read of
already-clean, already-coherent data. Its documented workaround is
exactly what round-50 just tried (`DCCIMVAC`/`dc civac` in place of a
plain clean) and it had no effect, which weakens this candidate
further even where it might apply. No other erratum in the document
matches. This is either an RTD1296 SoC/interconnect-level bug outside
ARM's own CPU errata scope (Realtec's bus fabric around the A53
cluster, not covered by ARM's document), or something not yet
considered.

Round-51: one thing not yet tried is forcing a **genuine hardware
interrupt entry/exit cycle on CPU0 itself**, as opposed to a barrier or
cache-maintenance instruction. Round-30's IPI probe has repeatedly,
reliably shown that an *external*, cross-CPU-triggered IPI landing on
CPU0 correctly observes `flags=0x12` -- so interrupt entry is a
context where visibility is known-good. But that probe is normally
sent by a different CPU; testing whether CPU0 *interrupting itself*
has the same effect requires a genuine self-directed hardware IPI, not
the generic SMP-call-function framework's `smp_call_function_single_async()`
-- inspecting `kernel/smp.c`'s `generic_exec_single()` shows it
special-cases `cpu == smp_processor_id()` and just calls the callback
inline (`local_irq_save()`/`local_irq_restore()`), never actually
raising a hardware interrupt for a self-target. Bypassing that: call
`arch_send_call_function_single_ipi(0)` directly from CPU0's own loop,
which unconditionally raises a real SGI via the GIC regardless of
source==target, forcing entry into
`generic_smp_call_function_single_interrupt()` (processing CPU0's own,
harmlessly empty, call-function queue) before every read. If this
restores visibility and the loop finally exits, that is not just a
diagnostic -- it would double as an actual workaround (periodic
self-IPI while polling) even without a full root-cause explanation. If
it *doesn't*, that is a genuinely striking result: it would mean even
a real hardware interrupt round-trip on CPU0 -- the same class of
event round-30's external probe uses and that has *always* correctly
observed `0x12` -- fails to refresh this specific read. Rebuilt and
repackaged; not yet hardware-tested.

**Sixty-third v6.18.46 hardware test (`putty.log.167`, build `#98`,
round-51): reproduced -- and the striking outcome happened. `cpu0 last
seen flags=0x00000010` is still frozen across every dump (15.9s-33.6s),
unaffected by a genuine, confirmed-compiled, confirmed-still-executing
(the RCU stall backtrace at t=23.3s again lands right here) hardware
self-IPI on every iteration.**

On reflection, though, round-51 doesn't quite test what round-30's
probe demonstrates. `cma_diag_cpu0_ipi_probe()` (`mm/swap.c`) reads
`current_thread_info()->flags` *from inside* the IPI callback itself,
running in the actual interrupt/exception context on CPU0 --
that's the scenario proven to always see `0x12`. Round-51's raw
`arch_send_call_function_single_ipi(0)` registers no callback, so
`generic_smp_call_function_single_interrupt()` finds CPU0's
call-function queue empty and returns immediately; the actual flags
read still happens afterward, back in `cpu_idle_poll()`'s own ordinary
(non-exception) code. So round-51 tested "does having just taken an
unrelated interrupt refresh a subsequent normal read" (no), not "does
reading *from inside* an interrupt handler see the update" (unknown
for the self-triggered case -- round-30 only established this for an
*externally*-triggered IPI).

Round-52: move the read itself inside a self-dispatched
`smp_call_function_single_async()` callback, matching round-30's proof
exactly except for the trigger's source CPU. Note from reading
`generic_exec_single()` (`kernel/smp.c`) for round-51: since the source
and target CPU are the same (CPU0 calling for CPU0), this call takes
the *local fast path* -- `local_irq_save()` +
call the callback inline + `local_irq_restore()` -- not a real taken
exception. That's a meaningfully different, still-worth-testing
condition in its own right: does merely executing the read under a
local IRQ mask/unmask pair (without an actual hardware exception
round-trip) restore visibility? `cma_diag_cpu0_last_seen_flags` is now
written *from inside* that callback (reusing the same diag variable,
same watchdog print) instead of by `cpu_idle_poll()`'s own inline code
directly. Dropped round-50/51's `dc civac`/self-IPI (both already
shown ineffective) to isolate this one variable cleanly. Rebuilt and
repackaged; not yet hardware-tested.

**Sixty-fourth v6.18.46 hardware test (`putty.log.168`, build `#99`,
round-52): reproduced -- still `0x00000010`, frozen across every dump
(15.9s-25.0s+).** Reading from inside a self-dispatched
`smp_call_function_single_async()` callback made no difference either.

But round-52 still didn't quite replicate round-30's proven-good
condition: `generic_exec_single()`'s local fast path (confirmed by
reading it for round-51) only wraps the callback in
`local_irq_save()`/`local_irq_restore()` -- a DAIF mask/unmask pair,
architecturally unrelated to cache/memory coherency and providing no
ordering guarantee beyond a compiler barrier. It never takes a real
taken exception. Round-51 took a real exception but read outside it;
round-52 read inside a callback but took no real exception. Neither
round has yet tested the actual conjunction round-30 demonstrates:
a read happening *literally during a genuinely-taken hardware
exception on CPU0*, self-triggered.

Round-53: `__smp_call_single_queue()` (`kernel/smp.c`) -- global
linkage, not exported, but built-in code doesn't need `EXPORT_SYMBOL`
to call it -- is the exact primitive the *remote* dispatch path uses:
`llist_add()` the csd onto the target's queue, then
`send_call_function_single_ipi()` -> `arch_send_call_function_single_ipi()`,
unconditionally, regardless of source==target. Calling it directly
from CPU0 targeting CPU0, bypassing `generic_exec_single()`'s
`cpu == smp_processor_id()` fast-path check entirely, forces the exact
same llist-queue-plus-real-IPI sequence an external CPU's call would
take -- so the callback finally runs from inside CPU0's own, actually
taken, `generic_smp_call_function_single_interrupt()` exception,
matching round-30's setup exactly except for which CPU triggered it.
Rebuilt and repackaged; not yet hardware-tested.

**Sixty-fifth v6.18.46 hardware test (`putty.log.169`, build `#100`,
round-53): reproduced -- still `0x00000010`, even matching round-30's
exact mechanism.** `cpu0 ipi probe calls=5 ... flags=0x00000012` (the
*externally*-triggered probe, same boot, same moments) keeps reading
0x12 as always, while the self-triggered version -- now using the
identical low-level `__smp_call_single_queue()` +
`arch_send_call_function_single_ipi()` sequence -- still reads 0x10.
With every mechanical difference between the two now eliminated, the
only remaining variable is literally *which CPU sent the IPI*.

That doesn't have a sensible memory-coherency explanation -- which
raises a sharper, more basic question the last four rounds all quietly
assumed without checking: **does the self-triggered callback actually
run at all?** If CPU0 cannot successfully deliver a self-targeted SGI
to itself through this SoC's GIC (as opposed to a cross-CPU-targeted
one, which demonstrably works fine, round-30), the callback would
simply never execute, and `cma_diag_cpu0_last_seen_flags` would just
retain whatever stale value it last held -- indistinguishable, from
the watchdog's dumps alone, from "the read executed and saw 0x10."
Rounds 51-53 never actually verified their callback/logic ran at all.

Round-54: add an unconditional entry counter,
`cma_diag_cpu0_self_read_calls`, incremented at the very top of
`cma_diag_cpu0_self_read_probe()` (`mm/cma.c`) -- prints alongside the
existing `last seen flags` line. If it stays at 0 across the whole
window, that conclusively proves the self-IPI mechanism itself never
fires/never gets serviced on this platform, and completely reframes
rounds 51-53: not a stale-read problem, but CPU0 being unable to
successfully self-interrupt via this GIC at all. If it climbs
normally, that would mean the callback definitely runs -- and the
"still reads 0x10 anyway" result stands as an even more precisely
pinned-down mystery: two byte-for-byte identical code paths, on the
same CPU, reading the same address, disagreeing purely based on who
sent the interrupt. Rebuilt and repackaged; not yet hardware-tested.

**Sixty-sixth v6.18.46 hardware test (`putty.log.170`, build `#101`,
round-54): reproduced -- `cpu0 self read calls=224004`, frozen at that
exact value across every dump (15.8s-29.4s+).** Non-zero and huge --
this conclusively kills the "self-IPI never delivered" hypothesis. The
self-triggered mechanism ran, successfully, 224004 times (almost
certainly all in a rapid burst very early in this specific
`cpu_idle_poll()` call, well before this diag window even opens,
matching idle's `se.exec_start` freeze point from round-44), and every
single one of those 224004 genuinely-executed, real-exception-context
reads saw the address without `TIF_NEED_RESCHED` set -- consistent
with ordinary pre-hang idle/wake cycling, nothing wrong there. Then,
at some iteration past #224004, the loop entered
`__cmpwait_relaxed()`'s `ldxr`+`wfe` and the counter never advanced
again for the rest of the boot (13+ seconds and counting) -- i.e. the
`wfe` itself never returns, not even once.

But this doesn't reopen the WFE-specific theory either: the *original*,
pre-round-37 version of this loop used a plain `ldr`-based busy-spin
with no `wfe`/exclusive-monitor involvement at all, and it hung with
the exact same `last_seen_flags` signature. So it isn't that `wfe`
specifically fails to wake -- it's that *no* re-read of this address by
CPU0, by any mechanism tried across 25 rounds (`ldr`, `dmb`, `dsb sy`,
`ldxr`+`wfe`, `dc civac`, three flavors of self-triggered IPI/exception
context), ever observes the update, while literally any other CPU
reading the identical address always does, immediately and
consistently. Every read-side avenue this investigation can construct
is now exhausted.

Round-55: try the write side instead, for the first time. Every prior
round assumed the original write (whichever `resched_curr()` call, far
in the past, first set the bit) was itself fine -- reasonably, since
external readers have always seen `0x12` -- but never tried having a
*different* CPU perform a *fresh*, later, genuinely coherent write to
this exact address, to see whether that -- as opposed to a read --
finally unsticks CPU0's view. `set_tsk_need_resched()`
(`include/linux/sched.h`) is a standard, generic, safe-from-any-context
kernel primitive (an atomic bit-set via `set_tsk_thread_flag()` /
`set_ti_thread_flag()`), idempotent since the bit is already set.
Called from the watchdog (`mm/swap.c`, already running on a different
CPU than CPU0 every 2s) as `set_tsk_need_resched(idle_task(0))`,
immediately before the diagnostic reads, once per tick. If CPU0's own
subsequent read finally observes the change after this, that would be
both a real finding and a plausible workaround (periodic external
re-assertion). Rebuilt and repackaged; not yet hardware-tested.

**Sixty-seventh v6.18.46 hardware test (`putty.log.171`, build `#102`,
round-55): reproduced -- the external write nudge doesn't help
either.** `cpu0 self read calls=261907`, frozen at that value across
all 7+ dumps (15.9s-29.1s+), *despite* `set_tsk_need_resched(idle)`
firing from the watchdog immediately before every single one of those
reads (confirmed by `ipi probe calls` incrementing normally,
1,2,3,4,5,6,7..., alongside it -- the watchdog itself is demonstrably
still running and doing its job every 2s). A fresh, genuinely coherent
external write to the exact address CPU0's `wfe` is (or was) waiting
on does not unstick it.

Disassembling the exact build (`build_policy.o`, still on disk,
matching this test's `pc: cpu_idle_poll.isra.0+0xc4/0xd4` from the RCU
stall backtrace) resolves *where*, precisely, execution sits:

```
 ac: sevl
 b0: wfe
 b4: ldxr x0, [x19]        // fresh read of thread_info->flags
 b8: eor  x0, x0, x20      // compare against the value self_read() last saw
 bc: cbnz x0, c4           // changed? -> skip second wfe, loop
 c0: wfe                   // <- unchanged: block here
 c4: b    68               // loop restart (calls cma_diag_cpu0_self_read again)
```

`0xc4` is the instruction *immediately after* the second `wfe` at
`0xc0` -- exactly the `ELR` architecture mandates recording for a core
interrupted while parked in `wfe`. That's consistent with CPU0
genuinely sitting in this exact `wfe`, continuously, for the entire
hang. But there's a real tension in the evidence: architecturally, an
actually-taken exception unconditionally exits a parked `wfe` -- and
the watchdog's own external IPI demonstrably *is* being taken and
serviced on CPU0 throughout the stuck window (`ipi probe calls` keeps
incrementing correctly). If that exception genuinely completes and
returns, execution should resume right after `wfe` (`0xc4`), fall
through to `0x68`, and call `cma_diag_cpu0_self_read()` again --
incrementing `self_read_calls`. It never does. Reconciling "interrupts
are demonstrably delivered and serviced" with "the loop provably never
executes another iteration" would need instrumentation finer-grained
than this investigation's kprobe/watchdog-based toolkit can practically
provide (true single-instruction tracing or a debug-halt/JTAG session
neither of which is available here) -- this is likely the practical
limit of what software alone can resolve.

**Summing up 25 rounds (round-30 through round-55) on this one
question:** every read-side technique CPU0 can apply to its own
thread_info (`ldr`, `dmb`, `dsb sy`, `ldxr`+`wfe`, `dc civac`, and
three increasingly exact replications of an externally-triggered IPI's
exception context) sees stale data indefinitely; a fresh, external,
genuinely coherent *write* to the same address from a different CPU
doesn't unstick it either; and interrupt delivery to CPU0 itself is
independently, repeatedly confirmed to work throughout. No documented
Cortex-A53 r0p4 erratum matches. This is very likely a hardware/SoC
defect specific to this exact scenario, beyond what further read/write
experiments are likely to characterize any further.

**Decision point: root-cause vs. workaround.** Presented this to the
user after 25 rounds of exhausted read/write characterization; chosen
direction is a workaround instead of continued root-causing.

Round-56: `remove_cpu(0)`/CPU hotplug recovery was the first workaround
idea considered, but ruled out *without* a hardware test by reading the
platform's own boot configuration: `rtd1296-wd-mycloud-home-duo.dts`
sets `enable-method = "spin-table"` for all four CPUs (including CPU0),
and `arch/arm64/kernel/smp_spin_table.c`'s `smp_spin_table_ops` defines
no `.cpu_die`/`.cpu_kill` at all. `op_cpu_disable()`
(`arch/arm64/kernel/smp.c`) explicitly refuses to offline any CPU
without a `cpu_die` method (`return -EPERM`) -- so `remove_cpu(0)`
would be structurally rejected by the kernel itself on this platform,
before ever touching hardware. CPU hotplug recovery is not viable here.

Went with a different, more targeted workaround instead: `mm/swap.c`'s
`__lru_add_drain_all()` -- the exact function this whole investigation
has been instrumented inside since round-15 -- calls
`flush_work(&per_cpu(lru_add_drain_work, cpu))` for every CPU with
pending LRU pagevecs, including CPU0. Once CPU0 is wedged, this
`flush_work()` waits forever for a work item CPU0's kworker will never
run, hanging boot (and, since this whole function holds a global
`mutex_lock(&lock)` throughout, would also hang every *other* caller
of `lru_add_drain_all()` system-wide, had boot ever gotten further).
Bounded the wait for CPU0 specifically: poll `work_busy()` (a standard
public workqueue API) for up to 5 seconds instead of blocking on
`flush_work()` unboundedly; other CPUs are untouched (still plain
`flush_work()`). On a healthy boot this costs nothing -- CPU0's work
item normally completes in microseconds, so the `work_busy()` check
passes immediately. Only in the pathological case does this actually
wait out the bound and then proceed anyway, accepting CPU0's local
LRU pagevec won't be drained this one time (a performance/correctness
edge case -- pages sit in CPU0's percpu batch a little longer, no
worse than what already happens between drains generally -- not a
crash or memory-safety issue) rather than hanging the boot forever.
Rebuilt and repackaged; not yet hardware-tested. This is the first
genuinely different kind of change in the whole investigation -- not
a read/write technique on CPU0's own busy-loop, but accepting CPU0 may
stay permanently wedged and routing around its effect on the rest of
the system instead.

**Sixty-eighth v6.18.46 hardware test (`putty.log.173`, build `#103`,
round-56): the workaround works.** `lru_add_drain_all: cpu0 work still
busy after 5s, giving up on its drain` fires (four times total, at
18.9s/23.9s/28.9s/33.9s -- other, unrelated `lru_add_drain_all()`
callers elsewhere in boot also hit CPU0's wedge and each individually
time out and proceed, exactly as expected) and, critically, **boot
proceeds past the point every one of the prior 56 rounds froze at**:
`scsi host1: ahci_rtd1295` / `scsi host2: ahci_rtd1295` / `ata1: SATA
max UDMA/133 ...` / `ata2: SATA max UDMA/133 ...` all appear (23.9s) --
first time ever in this entire investigation that AHCI probe has
gotten past `port 0 is not capable of FBS` and actually registered the
SCSI hosts and ATA ports. `udhcpc` then starts normally afterward.
CPU0 itself remains permanently wedged as expected/accepted (a second
RCU stall at 86.4s shows the identical `cpu_idle_poll.isra.0+0xc4`
signature) -- the workaround doesn't fix CPU0, it just stops CPU0's
wedge from blocking the rest of the system, which is exactly the
design intent.

The log capture ends shortly after the second RCU stall (86.4s), before
showing `ata1/ata2: SATA link up` or drive/rootfs mount messages -- so
full boot-to-userspace-login is not yet confirmed in this specific
capture, only that the previously-fatal hang point is now survived.
Awaiting a longer capture (or the next test) to confirm full completion.

**User report + sixty-ninth v6.18.46 hardware test (`putty.log.174`,
build `#103`, same round-56 binary, longer wait): still doesn't reach
a working boot.** Only *one* `lru_add_drain_all: cpu0 work still busy
after 5s` fires this time (at 18.9s, vs. four in the previous test --
run-to-run variance in how many `lru_add_drain_all()` calls happen
during boot, already an established pattern for this bug). `scsi
host1`/`scsi host2`/`ata1`/`ata2` register again at 23.9s -- but this
time, unlike a healthy boot, **neither `ata1/ata2: SATA link up` nor a
second `r8169 ... link up` ever appears**, for the rest of a 275+
second capture (only CPU0's own periodic self-detected RCU stalls
repeat every ~63s, all showing the identical, expected
`cpu_idle_poll.isra.0+0xc4` signature). `udhcpc` gets only as far as
`entering listen mode: raw` and stops -- compare against a known-good
boot (`putty.log.162`), where `ata1/ata2: SATA link up` appears *before*
any `r8169`/`udhcpc` activity at all (~18.1s, a few seconds after port
registration), and a second, real `r8169 ... link up` kernel message
follows shortly after `udhcpc` starts (not just its own userspace
retry).

So round-56's fix (a bounded wait around one specific `flush_work()`
call) unblocked *that* one call site, but boot now reaches a
*different*, downstream point that also depends on CPU0 making
scheduling progress -- most likely something in SATA link-up
negotiation and/or the r8169 link-monitoring path -- and that one has
no timeout at all, so it now hangs indefinitely instead. This is the
whack-a-mole risk flagged when round-56 was designed: CPU0 stays
permanently wedged for the rest of the boot, so *any* future kernel
work that happens to need CPU0's scheduler to run something will hang
the same way, through whatever code path it takes to get there.

Round-57: rather than guess which specific call site is blocking this
time and patch it one at a time, enable two standard, built-in kernel
debugging facilities that directly answer "what's stuck and why"
without needing another custom kprobe/watchdog round:
`CONFIG_DETECT_HUNG_TASK` (prints a full stack trace, and -- via
`CONFIG_DETECT_HUNG_TASK_BLOCKER`, on by default once the parent option
is enabled -- the blocking task's stack trace too, for any task stuck
in uninterruptible D-state past `CONFIG_DEFAULT_HUNG_TASK_TIMEOUT`,
set to 30s here) and `CONFIG_WQ_WATCHDOG` (30s-default stall detection
specifically for workqueues, dumping worker-pool state -- directly
relevant since this whole investigation's root cause has always been
workqueue-shaped). Both flipped on in `.config` via the same
`make olddefconfig` methodology as round-45's `CONFIG_KPROBES`/
`CONFIG_FTRACE`. Rebuilt and repackaged; not yet hardware-tested.

**Seventieth v6.18.46 hardware test (build `#104`, round-57): did not
reproduce.** Per the established pattern (round-39/41/42/47), treated
the newest addition as the prime suspect: reverted
`CONFIG_DETECT_HUNG_TASK`/`CONFIG_WQ_WATCHDOG` back off in `.config`
(via the same `make olddefconfig` flow, cleanly resolving
`CONFIG_DETECT_HUNG_TASK_BLOCKER`'s now-dangling dependency too).
Plausibly the periodic full task-list scan (`CONFIG_DETECT_HUNG_TASK`'s
own kthread, walking every task under `tasklist_lock` every few
seconds) or the workqueue watchdog's own timer added enough extra
system-wide activity/timing perturbation to disturb this very
timing-sensitive bug -- consistent with the broader, still-unconfirmed
pattern from much earlier in this investigation (rounds 39/41/42) that
new, broad-reaching kernel activity tends to do this, whereas narrow,
targeted additions confined to already-instrumented files usually
don't.

Round-58: a much lighter alternative to answer the same "what's stuck
now" question -- extend the existing, already-proven-safe 2s watchdog
in `mm/swap.c` (never once implicated in a reproducibility break
across this whole investigation) to scan every task for
`TASK_UNINTERRUPTIBLE` (D-state) using only generic, already-exported
kernel APIs already used elsewhere in this same file
(`for_each_process_thread()`, `sched_show_task()`), instead of
enabling a whole new kernel debug subsystem. Prints pid/comm/cpu plus
a full stack trace for every D-state task found, every 2s, for the
rest of boot -- should directly reveal whatever's now blocked on SATA
link-up or r8169 link monitoring. Rebuilt and repackaged; not yet
hardware-tested.

**Seventy-first v6.18.46 hardware test (`putty.log.175`, build `#105`,
round-58): a major reframing.** No `diag:` output at all this run --
the original CMA-alloc-triggered `kick_pool()` capture never armed
(the established ~1-in-4 natural variance in whether that specific
event happens to be observed). But the RCU stall self-detection still
fired, twice (23.4s, 86.4s), and this time it caught something
completely different: **`Comm: kworker/u16:2`, `Workqueue: events_unbound
deferred_probe_work_func`, `pc: of_get_next_child+0x0/0x64`**, called
via `ahci_platform_get_resources()` -> `devm_regulator_get()` ->
`regulator_dev_lookup()` -> `of_regulator_dev_lookup()` ->
`of_get_child_regulator()` -> `of_get_next_child()` -- not
`cpu_idle_poll()`, not even the idle task, for the first time in this
entire investigation. Registers are byte-for-byte identical across
both 63-seconds-apart reads -- the exact same hard-freeze signature as
every prior round, just hitting different code entirely.

Critically, the *same* regulator lookup had already completed cleanly
twice earlier in this exact boot (the non-deferred AHCI probe attempts
at ~1.5s and ~2.3s, both printing `supply ... not found, using dummy
regulator`) -- this was its *third* attempt, this time via the kernel's
automatic deferred-probe retry mechanism, on the `events_unbound`
*unbound* workqueue (which, unlike the per-CPU `kworker/0:x` this
investigation has tracked since round-15, is free to run on any CPU
and just happened to land on CPU0 this time).

This reframes the whole investigation: **the CPU0 defect is not
specific to `cpu_idle_poll()`'s exact code shape at all.** It can
freeze whatever CPU0 happens to be executing at the time it strikes;
every prior round only ever saw it in the idle loop because that's
almost always what CPU0 is doing. Round-56 bounds the one *per-CPU-
pinned* wait this investigation has found so far
(`__lru_add_drain_all()`'s `flush_work()` for CPU0's own kworker pool
-- unavoidable there since per-CPU work is pinned to its CPU by
definition). This is a different, complementary class: *unbound* work
that merely happened to be scheduled onto CPU0.

Round-59: exclude CPU0 from `wq_unbound_cpumask` as early as possible
(`core_initcall`), using `workqueue_unbound_exclude_cpumask()` -- the
same public kernel API cpuset isolation code uses -- so no future
unbound work item, like this deferred-probe retry, ever lands on CPU0
and risks the same freeze again. This doesn't touch per-CPU-pinned
work (round-56 already covers the one instance of that found so far)
and doesn't fix CPU0 itself -- it just further shrinks the set of
things that can get caught by it. Rebuilt and repackaged; not yet
hardware-tested.

**Seventy-second v6.18.46 hardware test (`putty.log.176`, build `#106`,
round-59): fix confirmed effective for its target, but reveals a
deeper, more pervasive bottleneck.** No repeat of round-58's
`of_get_next_child()` freeze -- CPU0 is back to the familiar
`cpu_idle_poll()` signature at both later RCU stalls (86.4s, 149.5s).
But round-58's D-state scan, still armed, caught something new and
important: alongside `pid=88`
(`ahci_rtd1295_init_work` -> ... -> `__lru_add_drain_all` ->
`msleep`, our own round-56 workaround, behaving exactly as designed
and resolving 5s later as expected), **four separate kworkers
(pid 62/71/73/74) are simultaneously blocked in `device_link_release_fn`
-> `synchronize_srcu_expedited()`, and a fifth (pid 23) in
`do_free_init` -> `synchronize_rcu()`** -- all still present, same
PIDs, 5 seconds later in the next dump. `rcu:` stall reports show
`idle=d5f4/0/0x1` frozen identically across all three stalls
(23.4s/86.4s/149.5s) while `fqs=` (forced-quiescent-state attempts)
climbs steadily (2495 -> 10359 -> 18224) without the grace period ever
completing.

This points at a much more fundamental and pervasive mechanism than
either fix applied so far: `synchronize_rcu()`/`synchronize_srcu()` --
used pervasively throughout the kernel for safe teardown, and
triggered here by ordinary device-probe/device-link cleanup, nothing
exotic -- require *every online CPU* (CPU0 included, since it can't be
offlined on this platform) to pass through a quiescent state before
the grace period completes. If CPU0's freeze somehow prevents that
(despite `ct_idle_enter()` having already run at the top of
`cpu_idle_poll()`, which should normally place it in an RCU extended
quiescent state requiring no further action from CPU0 at all -- exactly
why the "0-...." attribution in the stall report is worth
understanding better), *every* such call anywhere in the kernel would
stall for as long as CPU0 stays frozen, with no way to route around
individual call sites the way round-56 did for one specific
`flush_work()`. Unlike a hard deadlock, though, RCU's own stall
detector and forced-quiescent-state retries are diagnostic/best-effort
mechanisms that sometimes do eventually push a stuck grace period
through given enough time, rather than proof of permanent
non-completion.

Next: re-test this exact build (no new code) with significantly more
patience than before, to establish whether these RCU/SRCU-gated
teardown operations eventually resolve on their own (slowly, via
forced-quiescent-state eventually succeeding) rather than hanging
forever -- before considering any RCU-specific intervention, which
touches memory-safety-critical kernel machinery and warrants real
certainty about the mechanism first.

**Seventy-third v6.18.46 hardware test (`putty.log.177`, build `#106`,
same binary, user waited ~5.5 minutes): confirms it does not
self-resolve.** `rcu: INFO: rcu_preempt self-detected stall` repeats
every exactly 63s through `t=338s` (23.4/86.4/149.4/212.4/275.4/338.4),
`fqs=` (forced-quiescent-state attempts) climbing perfectly linearly
the whole time (2620 -> 10485 -> 18350 -> 26215 -> 34080 -> 41945, an
almost exact +7865 each step) with zero sign of deceleration or
approaching completion. No further boot progress at all past `udhcpc:
entering listen mode: raw`. This is not a slow-but-eventual resolution
-- it's a genuine, indefinite stall.

The D-state scan itself, however, only has visibility for a few
seconds: `cma_diag_task0_timer` (which the scan piggybacked on) is
torn down (`timer_delete_sync()`) the moment
`__lru_add_drain_all()`'s bounded wait for CPU0 gives up (~18.7s here)
-- so the last D-state snapshot is from `t=21.7s`, while the RCU stall
persists for another 5+ minutes unobserved.

Round-60: give the D-state scan its own independent, self-perpetuating
timer (`cma_diag_dstate_timer`, 10s interval, started once and never
torn down, `mm/swap.c`) instead of piggybacking on
`cma_diag_task0_timer`'s short-lived window, so the *same* watchdog
technique keeps reporting for the whole boot -- confirming whether the
`device_link_release_fn`/`do_free_init` kworkers seen stuck at 21.7s
are still stuck (or replaced by new ones) minutes later, before
considering any RCU-specific intervention. Rebuilt and repackaged; not
yet hardware-tested.

**Seventy-fourth v6.18.46 hardware test (`putty.log.179`, build `#107`,
round-60): confirms a genuine, permanent stall, and pinpoints the
specific victim blocking SATA.** The persistent (never-torn-down)
D-state watchdog reported the *exact same* set of PIDs, unchanged,
from `t=185s` clear through `t=310s` (5+ minutes): `device_link_wq`
workers still in `synchronize_srcu_expedited()`, `do_free_init` still
in `synchronize_rcu()` -- and, newly visible this round, **two
`kworker/u17:N` workers running `Workqueue: scsi_tmf_0`/`scsi_tmf_1`
`scsi_rcu_eh_wakeup`, blocked in `synchronize_rcu()`**. `ata1`/`ata2`
register (as always) but `SATA link up` still never appears anywhere
in this 300+ second capture.

`scsi_rcu_eh_wakeup()` (`drivers/scsi/scsi_error.c`) is exactly the
function that wakes each SATA host's error-handler kernel thread --
the thread responsible for driving SATA link negotiation -- and it
calls `synchronize_rcu()` first, unconditionally, before doing so.
This is very likely *the* direct cause of "SATA link up never
appears": the EH thread is never woken because this grace period never
completes.

Discussed with the user whether to accept the current state (SATA host/
port registration works, but link-up doesn't, without deeper
intervention) or attempt a genuinely risky fix inside RCU-adjacent
territory. Chose to attempt it, narrowly scoped: rather than reaching
into `kernel/rcu/tree.c`'s private `rcu_data`/`rcu_node` bookkeeping
(every relevant field and helper -- `rcu_report_qs_rdp()`, the
per-CPU `rcu_data` struct itself -- turned out to be file-static, and
even the one *public* "nudge a specific task's CPU toward a faster
quiescent-state check" API, `rcu_request_urgent_qs_task()`, still
fundamentally requires that CPU to eventually context-switch again to
act on the request -- exactly the thing CPU0 can't do, so it wouldn't
have helped anyway), patch the one identified call site directly,
using the RCU subsystem's own sanctioned *non-blocking* polling API
(`start_poll_synchronize_rcu()`/`poll_state_synchronize_rcu()`,
`kernel/rcu/tree.c`, exported via `include/linux/rcutree.h`) instead
of a blocking `synchronize_rcu()`.

Round-61: bound `scsi_rcu_eh_wakeup()`'s wait to 5s via the poll API;
on a healthy grace period (normally microseconds) this changes
nothing observable, only the pathological case proceeds without the
guarantee. The correctness gap this accepts is narrow: the
`synchronize_rcu()` here protects against a concurrent
`scsi_dec_host_busy()` call reading stale `host_eh_scheduled`/busy-count
state -- but at the very first EH wakeup for a freshly-added host,
before any command has ever been issued, there is no concurrent
`scsi_dec_host_busy()` for it to race with in the first place; the
condition being protected against isn't reachable yet at this specific
point in boot. Rebuilt and repackaged; not yet hardware-tested.

**Seventy-fifth v6.18.46 hardware test (`putty.log.180`, build `#108`,
round-61): the fix works.** `ata1: SATA link up 6.0 Gbps` / `ata2:
SATA link up 6.0 Gbps` appear at `t=29.4s` -- confirmed via `diag:
... cpu0 kworker still stuck ... on_rq=1` at `t=15.8s` that the
hang-triggering condition genuinely fired this boot (the same
condition present in every prior reproducing log). Link-up completing
*despite* that is the milestone: SATA link negotiation has never once
completed, in this entire 61-round investigation, on a boot where that
condition fired -- until now. (SATA link-up obviously works fine on
its own when the bug simply doesn't reproduce that boot -- e.g.
`putty.log.111`, an early, unrelated, fully-successful boot from
before this investigation -- that's not what's new here; what's new is
it now succeeds *with* the hang condition present, because round-61
stopped it from blocking the EH thread.) Both drives identified
immediately after: `ata1.00`/`ata2.00: ATA-10: WDC WD5000AZLX-60K2TA0,
01.01A01`, `scsi 0:0:0:0`/`scsi 1:0:0:0: Direct-Access ATA WDC
WD5000AZLX-6`, sizes reported (500 GB/466 GiB each). Bounding
`scsi_rcu_eh_wakeup()`'s wait was enough to let the SCSI EH thread run
and drive both ports through link negotiation and drive identification
-- confirming the diagnosis.

Boot doesn't yet reach partition scan (no `sda1`/`sdb1` or `Attached
SCSI disk` lines) or further. The persistent D-state watchdog still
shows the same unrelated permanent stalls as before
(`device_link_release_fn`/`do_free_init`/`synchronize_net()` via
`udhcpc`'s socket rebind -- all still blocked on plain, unpatched
`synchronize_rcu()`/`synchronize_srcu()`), plus two new entries:
`kworker/u17:2`/`u17:3` running `async_port_probe` ->
`ata_port_wait_eh()`, waiting for the SCSI host's EH pass to fully
finish. Crucially, **the SCSI EH thread itself (`scsi_eh_N`) never
appears in any D-state dump** -- it isn't blocked on a stuck RCU call
the way everything else is; `ata_port_wait_eh()`'s own wait
(`ap->eh_wait_q`, `TASK_UNINTERRUPTIBLE`, a genuine waitqueue, no RCU
involved) has no sign of being a *permanent* stall the way the
RCU-driven ones do. This may simply be legitimate, if slow, hardware
work (full ATA identification/EH settling for two real physical
drives) still in progress rather than a new hang -- unlike the RCU
cases, there's no D-state evidence pointing at a specific stuck call
site yet.

Next: re-test this exact build with patience again, to see whether
partition scan and further boot progress eventually follow once EH
genuinely finishes, before hunting for another specific call site to
patch.

**Seventy-sixth v6.18.46 hardware test (`putty.log.182`, build `#108`,
same binary, user waited ~5.7 minutes): confirms a new permanent
stall, and points at a different mechanism than RCU.** SATA link-up
succeeds again (`t=29.3s`, both ports, same as `putty.log.180`), but
the persistent D-state watchdog shows the *same* set of PIDs held
completely unchanged across all 33 dumps from `t=27s` through
`t=340s+` -- this time with *more* `device_link_release_fn` instances
piling up (5+, vs. 4 before) and exactly one `kworker/u17:2` still in
`async_port_probe` -> `ata_port_wait_eh()`. Searched
`drivers/ata/*.c`/`drivers/scsi/scsi_error.c`/`scsi_scan.c`/`hosts.c`
for other `synchronize_rcu()`/`synchronize_srcu()` call sites in the
EH path: none found. **The SCSI EH thread itself (`scsi_eh_N`) never
appears in any D-state dump here either** -- so it isn't blocked on an
unpatched RCU wait this time.

That absence is itself the clue: `drivers/scsi/hosts.c` creates this
thread with a plain `kthread_run()`, which leaves its CPU affinity
completely unrestricted -- the scheduler is free to place it on CPU0,
same as any other task. If CPU0's freeze (the *original*,
never-root-caused defect this whole 61-round investigation
characterized) strikes while the EH thread happens to be running
*on* CPU0, it would freeze **mid-execution** rather than blocking in a
wait -- exactly matching round-58's `kworker/u16:2` (frozen mid
regulator-lookup, not in any wait either) -- which explains both why it
never shows up in D-state and why `ap->eh_wait_q` is never woken.
Round-59 already excludes CPU0 from unbound *workqueue* work, but the
EH handler is a bare kthread, not a workqueue item, so that fix
doesn't cover it.

Round-62: exclude CPU0 from the EH thread's CPU affinity directly,
right after `kthread_run()` creates it in `drivers/scsi/hosts.c`, via
`set_cpus_allowed_ptr()` on a cpumask copied from `cpu_online_mask`
with CPU0 cleared. Rebuilt and repackaged; not yet hardware-tested.

**Seventy-seventh v6.18.46 hardware test (`putty.log.183`, build
`#109`, round-62): confirms the EH-thread-affinity fix worked, and
identifies the new dominant blocker.** `ata_port_wait_eh()` now
appears in only the *first* D-state dump (`t=27.6s`) and never again
across the remaining 26 -- it resolves quickly instead of stalling
forever, exactly as intended. SATA link-up succeeds again (`t=29.3s`,
both ports). But boot still doesn't reach partition scan even 4.6
minutes in: with that blocker cleared, boot proceeds further and
exposes *more* `device_link_release_fn()` calls than before -- 6+
simultaneous `kworker/3:N` (and others) now piled up on
`synchronize_srcu_expedited()`, unchanged across all 27 dumps. Traced
this to `device_link_synchronize_removal()`
(`drivers/base/core.c`) -> `synchronize_srcu(&device_links_srcu)`,
called from every `device_link_release_fn()`.

Round-63: apply the same bounded-poll pattern as round-61
(`start_poll_synchronize_srcu()`/`poll_state_synchronize_srcu()`, 5s),
this time to `device_link_synchronize_removal()`. This edit was
initially blocked by Claude Code's auto-mode safety classifier and
confirmed explicitly with the user before proceeding: unlike round-61's
narrow, provably-unreachable-this-early race window, `device_links_srcu`
genuinely guards concurrent `device_links_read_lock()` traversal of the
device link lists elsewhere in this file, so this carries a real,
broader (if still probably small in practice, this early in boot)
use-after-free risk if a concurrent reader is actually in flight when
the 5s bound is hit -- a materially different risk category than
round-61's. Rebuilt and repackaged; not yet hardware-tested.

**Seventy-eighth v6.18.46 hardware test (`putty.log.185`, build `#110`,
round-63): the fix works, and boot reaches partition scan for the first
time.** `device_link_release_fn` no longer appears anywhere in the
persistent D-state watchdog. Boot proceeds into `sd_probe()` ->
`device_add_disk()` -> `disk_scan_partitions()` -> `sd_open()` ->
`sd_revalidate_disk()` -- itself a first. But a new blocker appears
there: `blk_mq_freeze_queue_wait()` (`block/blk-mq.c`), reached via
`sd_revalidate_disk()` -> `queue_limits_commit_update_frozen()` ->
`blk_mq_freeze_queue()`, waits unboundedly for `q->q_usage_counter`
(a percpu-refcount) to reach zero -- and that counter's percpu-to-
atomic transition is itself driven by `percpu_ref`'s own internal
`call_rcu()` callback, so it inherits the same permanently-wedged-CPU0
grace-period stall. This is a qualitatively different risk category
from round-61/63: it guards actual in-flight I/O to the physical
disk during a live queue-limits update, not device-model bookkeeping.

Round-64 (proposed, not applied): the narrowest available version --
confined to `drivers/scsi/sd.c`'s `sd_revalidate_disk()` only, using
`blk_mq_freeze_queue_wait_timeout()` (an existing, exported, official
bounded-wait variant of the same block-layer API, not something
invented for this) instead of touching the generic, widely-shared
`queue_limits_commit_update_frozen()` -- was blocked twice by Claude
Code's auto-mode safety classifier. Paused there rather than overriding
it a third time.

The user then pointed out that `mm/swap.c`'s `__lru_add_drain_all()`
*forces* CPU0 into needing a drain during the diag window
(`cma_diag_trace_next && cpu == 0`, round-15) regardless of whether it
naturally does, and asked whether routing that work to a different CPU
could avoid the problem instead. Checked: `lru_add_drain_per_cpu()`
calls `lru_add_and_bh_lrus_drain()` -> `lru_add_drain_cpu(smp_processor_id())`
-- it always drains *whichever* CPU it happens to run on (with an
explicit comment to that effect), so it cannot be redirected to drain
CPU0's pagevec from another CPU; round-56's bounded-wait-then-proceed
already achieves the equivalent intent for this specific case, and
round-62's CPU-affinity exclusion already did literally what was
proposed for the one case where the stuck work item's own execution
(not a grace period) was on CPU0 (the SCSI EH thread). The remaining
RCU/SRCU/percpu_ref-driven stalls (round-61, round-63, the pending
`blk_mq_freeze_queue_wait` one) are a different shape: the blocked
kworkers are typically already running on CPU1/2/3, not CPU0 -- they
are waiting on a *grace period* that needs CPU0's cooperation to
complete, which no other CPU can report on its behalf, so "run the
wait elsewhere" does not apply to that class.

Round-65: reintroduce a much older, much lower-risk mitigation instead
of continuing to chase individual stalls. Long before this round
numbering started, a deliberate `usleep_range(50000, 80000)` (50-80ms)
placed in `kernel/dma/direct.c`'s `dma_direct_alloc()`, right before
the SATA device's CMA allocation begins, gave 10/10 clean boots
initially; widened to `usleep_range(150000, 250000)` (150-250ms) after
one recurrence, which regressed further (10/12 clean boots total
across both values), and was reverted in favor of understanding the
actual mechanism -- which round-30 onward went on to do at length.
Brought back, doubled from that last-tried 150-250ms value to
300-500ms, alongside every CPU0-specific fix developed since. The
mechanism, if it works, is almost certainly what the
round-15 comment already documents: shifting this allocation's timing
changes whether CPU0's own LRU pagevec happens to need draining at all
for this specific boot (naturally ~1-in-4 without any delay) --
avoiding the need for CPU0's cooperation in this boot's entire
CMA/drain/SCSI-probe/partition-scan sequence from the start, rather
than bounding each downstream RCU-driven stall one at a time. Rebuilt
and repackaged; not yet hardware-tested.

**Seventy-ninth v6.18.46 hardware test (`putty.log.186`, build `#111`,
round-65): did not help.** `diag: ... cpu0 kworker still stuck` still
fires at `t=16.3s` -- the hang-triggering condition (CPU0 needing its
own LRU drain) still occurred despite the doubled 300-500ms delay
before the SATA CMA allocation. Disproves the "shift this allocation's
timing to avoid CPU0 naturally needing a drain" hypothesis, at this
delay value at least.

Requested a meta-analysis of every diagnostic addition across this
investigation that had correlated with lost reproducibility (round-39,
41, 42, 47, 57 -- see the analysis above/this round's commit for the
full comparison table). All five shared one trait: added overhead or
activity on the scheduler's hottest, most system-wide paths
(`kernel/sched/core.c`'s `__resched_curr()`, `kernel/workqueue.c`'s
`kick_pool()`, or broad always-on debug subsystems), while extensive
changes confined to `mm/swap.c`/`mm/cma.c`/the CPU0-only branch of
`kernel/sched/idle.c` never once broke reproducibility despite far more
invasive experiments (self-IPIs, kprobes, live code patching). Given
round-65's passive timing-shift attempt failed, decided to test the
working hypothesis directly and deliberately: intentionally perturb
`__resched_curr()`'s timing, the same way round-39 did accidentally,
and see whether that shifts the race away instead of just making it
unobservable.

Round-66: reintroduce round-39's exact mechanism (a plain, unconditional
counter at the very top of `__resched_curr()`, before its early-return
check, no lock involved -- the least invasive of the five in isolation)
but bounded to the first ~20s of boot (`cma_diag_race_shift_deadline`,
computed once at a `core_initcall` in `mm/cma.c`) instead of running
forever: after that window, the check costs one cheap `jiffies`
compare per reschedule, system-wide, for the rest of the system's
life, with no other effect. Printed alongside the existing watchdog
output for visibility into whether the window was actually exercised.
Rebuilt and repackaged; not yet hardware-tested.

**Eightieth v6.18.46 hardware test (`putty.log.187`, build `#112`,
round-66): the mechanism ran correctly, but did not shift the race.**
`cma_diag_race_shift_calls` climbs into the thousands (4109 -> 4565 ->
4765) within the ~20s window and stops advancing once `now` passes
`deadline` (confirmed via the printed `deadline=`/`now=` jiffies
values) -- so the time-bounded counter genuinely executed on every
reschedule during the window, exactly as designed. `diag: ... cpu0
kworker still stuck` still fires at `t=16.3s` regardless. Bounding the
window to 20s did not preserve round-39's original effect.

Round-67: before iterating on *how* to bound or reshape this
mechanism, a clean sanity check requested by the user -- reproduce
round-39's exact original form (a plain, permanently-unconditional
counter at the very top of `__resched_curr()`, no time bound at all)
on the current tree, to confirm it still breaks reproducibility the
same way it did back then, establishing a known-behavior baseline
before refining it further. Rebuilt and repackaged; not yet
hardware-tested.

**Eighty-first v6.18.46 hardware test (`putty.log.188`, build `#113`,
round-67): confirms round-39's original, unbounded mechanism still
avoids the hang.** Zero `diag:` lines anywhere in the log -- the
hang-triggering condition never fired -- across **two full, clean boots
back to back** (same capture): `SATA link up` both times, `udhcpc`
obtained a DHCP lease both times, boot reached a working shell (`~ #`,
FTP server started) both times. Matches the historical round-39
pattern exactly, now re-confirmed on the current, much-evolved tree.

Puzzle worth resolving before shipping this: round-66's ~20s-bounded
version had *not* avoided the hang (1/1), even though that window
already fully covers the ~13.7s CMA-alloc/`kick_pool()` event that
triggers the original hang condition -- so the difference between
round-66 and round-67 isn't about perturbing *that* specific moment,
which both covered. This investigation has since mapped several
*further*, later stall points that only become reachable once earlier
ones are unblocked (SCSI EH thread ~23-30s, device link teardown
~27s+, block-queue-freeze/`percpu_ref` ~37s+) -- round-66's window
closed well before all of those, round-67's (unbounded) didn't.

Round-68: widen the window from 20s to 120s -- generously past every
downstream stall point mapped so far, while still being far short of
"forever" -- to test whether continuing to perturb reschedule timing
through the whole cascade (not just the initial trigger) is what
matters. If 120s reproduces round-67's clean-boot result, that's a much
more shippable middle ground than an unconditional, permanent change;
if it still fails the way round-66 did, that argues the "how long"
question needs a different answer (or a different mechanism than
elapsed time). Rebuilt and repackaged; not yet hardware-tested.

**Eighty-second v6.18.46 hardware test (`putty.log.189`, build `#114`,
round-68): the 120s window fails the same way round-66's 20s window
did.** Two full boot attempts captured back to back (the board reset
itself partway through the first). Both hit `diag: ... cpu0 kworker
still stuck` at t=16.27s, with `race shift calls` already in the
thousands (4068 and 4117 respectively) -- the counter was demonstrably
running, but at almost exactly the same wall-clock moment and call
count as round-66's failure. A 6x-wider window producing the same
failure at the same point is strong evidence the window length isn't
the variable that matters: round-67's 2/2 clean result is more likely
explained as natural variance on a small sample (this bug has never
reproduced at 100%) than as a real effect of the unbounded counter.
Decided, with the user, to abandon the deliberate race-shift approach
rather than keep chasing window sizes.

Reverted: the `cma_diag_race_shift_*` mechanism (`kernel/sched/core.c`,
`mm/cma.c`, `mm/swap.c`, `include/linux/cma.h`, rounds 66-68) removed
entirely. Also reverted round-65's `usleep_range(300000, 500000)` delay
in `kernel/dma/direct.c`'s `dma_direct_alloc()` -- confirmed not to
help back in putty.log.186/round-65, left in the tree since only
because it was superseded by the race-shift experiments rather than
removed at the time.

Second boot in this same log is worth noting on its own merits,
separate from the race-shift question: it progressed further than any
previous boot in this investigation once it got past the
`cpu0 kworker still stuck`/lru-drain stall -- `scsi host0`/`host1:
ahci_rtd1295`, `ata1`/`ata2` registered (t=25.6s), and boot reached
userspace (`ifconfig` visible, mid network-interface bring-up) before
the capture ended. The same CMA/lru-drain stall pattern recurred for
*that* allocation too (`rtl_open()`'s DMA buffer for the Ethernet
driver, t=27.5s) -- consistent with this investigation's standing
understanding that the bug isn't SATA-specific, it's CPU0-freezes-
whatever-it's-running, and any subsystem's first CMA allocation while
CPU0 happens to be that stuck target can trigger it.

A direct fix for the `blk_mq_freeze_queue_wait()`/`percpu_ref` stall
was drafted (skip `queue_limits_commit_update_frozen()`'s freeze
specifically at `sd_revalidate_disk()`'s first call, from `sd_probe()`
before `device_add_disk()`, where no I/O can possibly be outstanding
yet) but set aside at the user's request pending further discussion;
`drivers/scsi/sd.c` is unmodified and this stall remains open.

Round-69 decision: reinstate round-39/round-67's mechanism (the plain,
unconditional, permanent `__resched_curr()` counter) as the actual
fix, rather than a diagnostic experiment -- it is the only version of
any approach tried so far that has been observed to avoid the hang at
all (2/2 clean boots, putty.log.188). The 20s- and 120s-bounded
variants (round-66, round-68) both failed the same way, so the bound
itself is dropped entirely rather than tuned further; `mm/cma.c` keeps
just the counter (`cma_diag_race_shift_calls`, no deadline),
`kernel/sched/core.c`'s `__resched_curr()` increments it unconditionally
again, and `mm/swap.c` prints it for visibility. Root cause of why this
works is still not understood -- it remains a workaround, not a fix --
but it is the most practical path available right now given how many
rounds the direct-root-cause and bounded-mitigation approaches have
both been tried without success.

**Eighty-third v6.18.46 hardware test (`putty.log.190`, build `#115`,
round-69): the permanent, unconditional counter did not reliably avoid
the hang either.** `race shift calls` climbed the whole time (4149 ->
4636 -> 5088 -> 5957), confirming the mechanism was genuinely running,
but `diag: ... cpu0 kworker still stuck` still fired four times
(t=15.7s, 18.5s, 21.2s, 23.9s) -- a `rcu_preempt self-detected stall on
CPU 0` also appeared at t=23.3s. Combined with round-68's failure, this
means the "unconditional" form has now succeeded once (putty.log.188,
2/2) and failed once (putty.log.190) on nominally the same code --
consistent with round-67's clean result being natural variance on a
small sample rather than a real causal effect, as suspected after
round-68. Decided, with the user, to abandon the `__resched_curr()`
race-shift mechanism entirely rather than keep testing it; reverted
(`kernel/sched/core.c`, `mm/cma.c`, `mm/swap.c`, `include/linux/cma.h`).

The boot itself is worth noting on its own merits, separate from the
race-shift question: this is the furthest any boot has progressed in
the whole investigation. Despite the trigger firing four times, the
existing bounded-wait/affinity fixes (round-56, 59, 61, 62, 63) kept
the system moving forward instead of hanging permanently: `SATA link up
6.0 Gbps` on both ports at t=30.2s, both drives fully identified
(`sda`/`sdb`, WDC WD5000AZLX, 500GB each, correct block/cache
parameters), and boot reached `udhcpc` DHCP negotiation at t=34.7s
before the capture ended. This progress came from those root-cause
fixes, not from the race-shift experiment.

Round-70: with the race-shift approach abandoned, reintroduce the
`usleep_range()` delay in `kernel/dma/direct.c`'s `dma_direct_alloc()`
one more time, doubled again from round-65's last-tried 300-500ms to
500-1000ms.

Round-71: at the user's request, reinstate the `__resched_curr()`
counter (`cma_diag_race_shift_calls`) on top of round-70's delay,
rather than in place of it -- both active together. Not a confirmed
fix on its own (round-69 showed it failing as often as it succeeds),
but cheap enough to retest in combination.

**Eighty-fourth v6.18.46 hardware test (`putty.log.191`, build `#117`,
round-71): 2/2 clean boots, zero `diag:` lines in either.** Both boots:
`AHCI vers 0001.0301`, `SATA link up 6.0 Gbps` on both ports, `udhcpc`
obtained a DHCP lease (`192.168.2.107`), boot reached a working shell
(`~ #`) both times. No errors, warnings, panics, or RCU stalls anywhere
in the 1485-line capture -- the only non-boilerplate line
(`rx error select to mac original`) is the same routine message seen in
every other log, clean or not.

This is the second clean 2/2 result for the `__resched_curr()` counter
(after putty.log.188), now with the 500-1000ms pre-CMA delay also
active alongside it -- but round-69 already showed the same counter,
alone, failing on a different 2-boot sample (putty.log.190, 4/4
triggers across two boots). With successes and failures both recorded
for nominally the same mechanism, a 2/2 result on its own still isn't
enough to distinguish "this combination works" from "this is within
the bug's natural variance" -- more repetitions are needed before
treating this as confirmed.

**Eighty-fifth v6.18.46 hardware test (`putty.log.192`, build `#117`,
round-71 continued): 3 more clean boots back to back, zero `diag:`
lines in any.** Same pattern all three times: `AHCI vers 0001.0301`,
`SATA link up 6.0 Gbps` on both ports, DHCP lease obtained, boot
reached a working shell. No errors, warnings, panics, or stalls in the
2222-line capture. Combined with putty.log.191, that's **5/5 clean
boots in a row** on build #117 (counter + 500-1000ms delay together) --
the longest clean streak recorded for any variant of this mechanism so
far, and enough to start being a meaningfully positive signal rather
than 2-boot noise, though round-69's earlier 0/2 result on the
counter-alone variant means this still isn't being called a confirmed
fix. Continuing to accumulate boots on build #117 is the plan.

Round-72: at the user's explicit request, stripped this investigation's
code down to just the two pieces judged effective (the counter and the
delay), reverting everything else. All 12 files this investigation had
touched (`drivers/scsi/hosts.c`, `drivers/scsi/scsi_error.c`,
`drivers/base/core.c`, `kernel/workqueue.c`, `kernel/dma/contiguous.c`,
`kernel/sched/idle.c`, `kernel/sched/core.c`, `kernel/dma/direct.c`,
`include/linux/cma.h`, `mm/page_alloc.c`, `mm/swap.c`, `mm/cma.c`) were
reset to pristine upstream `v6.18.46` (`git checkout v6.18.46 -- <path>`,
confirmed zero diff afterward for every file). This removes, among
other things, round-56's bounded `lru_add_drain_all()` CPU0 wait,
round-59's unbound-workqueue CPU0 exclusion, round-61/63's bounded
RCU/SRCU polling, round-62's SCSI EH CPU affinity exclusion, and every
diagnostic dump/kprobe/timer added across 70+ rounds -- all of the
downstream bounded-wait scaffolding that made boots survive the
CPU0-freeze trigger instead of hanging forever, not just the
diagnostics. Only two pieces were then reapplied on top of the
pristine tree: `kernel/dma/direct.c`'s `usleep_range(500000, 1000000)`
delay before the SATA device's first CMA allocation (round-70's value),
and `kernel/sched/core.c`'s unconditional `cma_diag_race_shift_calls++`
counter in `__resched_curr()` (backed by `mm/cma.c`, declared in
`include/linux/cma.h`) -- the two mechanisms behind round-71's 5/5
clean streak.

**This is a real risk, not just a cleanup**: `__lru_add_drain_all()`
and every other call site this investigation had bounded are now back
to their pristine, unconditionally-blocking form. If the counter+delay
combination does *not* actually prevent CPU0 from freezing (as opposed
to just perturbing timing enough to avoid it on these particular
boots), a future boot can hang forever again with no diagnostic output
and no recovery, the same as the very first rounds of this
investigation before round-56. This revert is deliberately testing
whether the two-piece fix is sufficient on its own, at that cost.

Also compared `.config` against the sibling `symops/monarch-6.18`
board's `default.config` (downloaded from its `v6.18.46` release) to
catch any investigation-era config drift the code revert wouldn't
touch. Found a large untracked-in-git cluster: `CONFIG_KPROBES`,
`CONFIG_FTRACE`, and everything gated behind them (`KPROBE_EVENTS`,
`UPROBE_EVENTS`, `RING_BUFFER`, `EVENT_TRACING`, etc.) had been enabled
to support this investigation's kprobe-based diagnostics and never
disabled even after the code using them was removed in later rounds.
Also found `CONFIG_CMDLINE` carrying an extra `nohlt` (vs. monarch's
plain `panic=5`) -- a boot parameter that forces CPU idle to busy-poll
instead of halting, almost certainly added specifically to fight the
CPU0-freeze bug. Removed `nohlt` from the cmdline, turned `KPROBES` and
`FTRACE` off, and ran `make olddefconfig` to cascade-clear every
dependent option. Diffed against monarch's config again afterward: only
three platform-legitimate differences remain (`SENSORS_RTD129X_FAN_TACH`/
`SENSORS_PWM_FAN` -- Duo's fan, monarch apparently has none;
`MMC_DW`/`MMC_DW_PLTFM`/`MMC_DW_RTD129X` -- Duo's eMMC controller;
`RTC_DRV_RTD119X` -- Duo's RTC chip), none of them touched.

Also audited the entire tree against the pristine `v6.18.46` tag
(full diffstat, dts/defconfig excluded) to check for any other
forgotten diagnostic code this investigation's marker-based search
might have missed. Every remaining difference turned out to be
legitimate WD My Cloud Home Duo board-port driver code (AHCI, clk,
i2c, irqchip, mmc, phy, pwm, regulator, rtc, soc, thermal, usb,
watchdog, ethernet, the RTD1295 spin-table release-address quirk),
initramfs packaging, or documentation -- confirming the revert is
complete.

Rebuilt from this pristine-plus-two-pieces tree with the cleaned
`.config`. Build clean, exit 0 (11 pre-existing warnings in the vendor
`r8169soc.c` ethernet driver surfaced now that it recompiled under the
changed `.config` -- unrelated to this investigation).

**Eighty-sixth v6.18.46 hardware test (`putty.log.193`, build `#118`,
round-72): 3 more clean boots back to back, zero `diag:` or crash
lines in any.** Confirms `CONFIG_CMDLINE` no longer carries `nohlt`
(`panic=5` only). Same pattern all three times: `AHCI vers 0001.0301`,
`SATA link up 6.0 Gbps` on both ports, DHCP lease obtained, boot
reached a working shell. No errors, warnings, panics, or stalls in the
2236-line capture. Combined with round-71's 5/5, that is **8/8 clean
boots in a row** for the delay+counter combination -- and the last 3
of those 8 are with every other piece of this investigation's code and
`.config` scaffolding removed, confirming the minimal two-piece fix
holds up on its own, not just alongside the bounded-wait safety net
that was stripped out in round-72. Still continuing to accumulate
boots before calling this conclusively confirmed, given round-69's
earlier failure of the counter-alone variant.

**Eighty-seventh v6.18.46 hardware test (`putty.log.194`, build `#118`,
round-72 continued): 3 more clean boots back to back, zero `diag:` or
crash lines in any.** Same pattern all three times: `AHCI vers
0001.0301`, `SATA link up 6.0 Gbps` on both ports, DHCP lease obtained
(one boot had a normal DHCP lease-fail/retry cycle before succeeding --
network timing noise, not a kernel issue), boot reached a working
shell. No errors, warnings, panics, or stalls in the 2257-line capture.
Combined with putty.log.192/193, that's **11/11 clean boots in a row**
for the delay+counter combination, 6 of them with every other piece of
this investigation's scaffolding and `.config` cleanup already removed.

- Boots to `Machine model: WD My Cloud Home Duo`, all 4 CPUs up (SMP
  spin-table).
- `ahci_rtd1295`/`phy-rtk-sata`/`usb-storage`/`uas` modules insmod cleanly
  (`rc=0`) from the separate initrd once packaged from a build with matching
  vermagic (see symops/monarch-6.18's README.md, "Updating the base
  version" section, on `.ko` staleness — the *same* build's `.ko`s must go in
  both `Image` and the separate `rescue.root.emmc.cpio.gz_pad.img`, and a
  stale leftover file from an earlier test on the USB stick will silently
  reproduce a `struct module` size-mismatch failure that has nothing to do
  with the kernel itself — hit and diagnosed once already during this port's
  own bring-up).
- Ethernet (r8169soc/RTL8169SOC) links up, DHCP works, Network Rescue Mode
  (telnet/ftp) is reachable.
- System LED (`pwm-leds`, channel 0, label `white:sys`) — confirmed on real
  hardware: static brightness (`echo N > brightness`, 0-255) and the
  `heartbeat` kernel trigger both drive the physical LED correctly.
- USB (xHCI ×3 controllers, USB-A rescue stick) enumerates and is used to
  boot from.
- Both AHCI ports probe, both PHYs report `init phyN OK`.
- **Real SATA link-up on both bays**: `ata1`/`ata2: SATA link up 6.0 Gbps`,
  both drives (WDC WD5000AZLX-60K2TA0) identified, partitioned, mdadm
  assembles the RAID1 arrays from the surviving half of each mirror, a
  Debian rootfs is found and mounted from `/dev/md1`, and `switch_root`
  succeeds into a fully-booted OpenRC/Gentoo userspace. See the
  SATA_n/SATA_PHY_n reset fix above.
- **v6.18.46-only AHCI bring-up hang, under active investigation**: an
  intermittent boot hang first appeared testing v6.18.46, fully traced
  with no gaps into `dma_direct_alloc()` -> `dma_alloc_contiguous()`
  -> `cma_alloc()` -> `alloc_contig_range()` ->
  `__alloc_contig_migrate_range()` -> `lru_cache_disable()` ->
  `__lru_add_drain_all()` (`kernel/dma/direct.c`,
  `kernel/dma/contiguous.c`, `mm/cma.c`, `mm/page_alloc.c`,
  `mm/swap.c`). Originally investigated under a "CPU0-interrupt-loss"
  (lost wakeup from WFI) hypothesis, but `putty.log.123` -- booted with
  `nohlt`, confirmed active via the NMI backtrace (CPU0 in
  `cpu_idle_poll()`, never `wfi`) -- falsified that: the hang still
  happened with CPU0 provably never sleeping. Current best
  understanding: a workqueue/scheduling issue somewhere in
  `__lru_add_drain_all()`'s `queue_work_on()`/`flush_work()` per-CPU
  loop, mechanism still unknown. No mitigation currently shipped -- the
  earlier `usleep_range()` workaround (50-80ms then 150-250ms) and
  every `pr_info()` tracing round so far have all recurred at least
  once each; none is a reliable fix. Not shippable as-is (diagnostic
  prints, not a fix). Root cause not understood; see the "AHCI
  CPU0-interrupt-loss hang investigation" Progress log entries above
  for the full story.
- **eMMC** (`dw_mmc-rtd129x`, PIO mode) — confirmed on real hardware:
  card identifies (`4FPD3R`), `mmcblk0` registers at its real `3.64
  GiB` capacity with the correct factory partition table (`p1`-`p6`,
  plus `mmcblk0boot0`/`boot1`), reads (`dd`, ~20.4 MB/s) and writes
  both work, and SATA continues normally afterward. See the eMMC
  Progress log entries above for the full story (three bugs found and
  fixed: FIFO depth, DMA hang, zero-capacity FIFO64 access). HS200
  (Phase 2) also confirmed working (200MHz/div=0, RX tuning, clean
  800 MiB sequential read) -- see its own Progress log entry.

- **Fan control** (`pwm-fan`, channel 3) — confirmed on real hardware:
  `CONFIG_SENSORS_PWM_FAN` was missing from `.config` (the driver never even
  probed, so `/sys/class/hwmon` only ever showed `soc_thermal` — not a DTS or
  hardware problem, just a forgotten build option). Enabled and rebuilt: a
  second `hwmon` node appears and manual `pwm1` writes (0/128/255) correctly
  vary the fan speed. Now wired into `soc-thermal`'s `cooling-maps`
  (55C/70C trip points) and confirmed working automatically on real
  hardware too -- see its own Progress log entry. RPM feedback also
  confirmed working (see Fan tachometer below) -- no longer open-loop.

- **Fan tachometer** (`drivers/hwmon/rtd129x-fan-tach.c`) — confirmed
  on real hardware: `/sys/class/hwmon/hwmon1/fan1_input` reports real,
  plausible, correctly-updating RPM values. See its own Progress log
  entry for the full story.

- **Hardware RTC** (`drivers/rtc/rtc-rtd119x.c`, `rtc@600` under `&misc`)
  — confirmed on real hardware: survives a full power cycle
  (`RTCACR.RTCPWR` warm-start bit set, correct real time restored
  automatically at boot) thanks to this board's battery. See the RTC
  Progress log entries above for the full story (three failed tests at
  the wrong physical address before the root cause -- wrong syscon
  parent, `&iso` instead of `&misc` -- was found and fixed).

- **cpufreq OPP table** (`drivers/clk/realtek/clk-rtd129x-scpu.c`, same
  scheme as Monarch) — confirmed on real Duo hardware across the full
  OPP range. See the cpufreq Progress log entry above for the full
  story.

- **USB PHY calibration** (`drivers/phy/realtek/phy-rtk-usb2.c`/
  `phy-rtk-usb3.c`, default tables shared with Monarch, no board-specific
  overrides) — confirmed on real Duo hardware: `u3host`
  (`xhci-hcd.2.auto`, io mem `0x981f0000`) negotiated a genuine USB 3.0
  SuperSpeed link with a real device, no link-training errors. See the
  USB PHY calibration Progress log entry above for the full story.

### eMMC (`drivers/mmc/host/dw_mmc-rtd129x.c`)

Ported in phases (Phase 1 here) rather than all at once, given the size —
the eMMC block at 0x98012000 turned out to be a genuine Synopsys DesignWare
Mobile Storage Host (dw_mmc): every register offset the vendor 4.9.330
driver (`drivers/mmc/host/rtkemmc.c`, ~5400 lines, entirely bespoke command
dispatch and DMA) uses in the 0x00-0xf0 range matches mainline `dw_mmc.h`'s
`SDMMC_*` layout exactly (down to `SDMMC_CDTHRCTL`/`SDMMC_DDR_REG` at the
same 0x100/0x10c offsets the vendor calls `EMMC_CARD_THR_CTL`/`EMMC_DDR_REG`),
and the vendor's own hand-rolled IDMAC descriptor builder produces the same
4-word OWN/CH/FD/LD descriptor format mainline `dw_mmc.c` already builds.
So instead of forward-porting ~5000 lines of vendor command/DMA logic, this
is a ~250-line glue driver on top of the *mainline* `dw_mmc.c` core (same
pattern as `dw_mmc-rockchip.c`/`dw_mmc-hi3798cv200.c`) — the core handles
command dispatch, IDMAC, and interrupts generically and correctly; the glue
only handles what's genuinely SoC-specific and has no mainline provider:
CRT clock-gate/reset bits, pin mux, pad drive strength, and the eMMC PLL —
same raw-CRT-register-poke approach already used for SATA, since no
mainline clock/reset driver exists for these bits either.

**Phase 1 scope**: `MMC_TIMING_MMC_HS` (~50MHz "high speed", no UHS/DDR/
HS200/HS400), fixed phase, no tuning search — deliberately matching how the
vendor's *own* production board DTS (`rtd-1296-pelican-1GB.dts`) actually
runs this exact board's eMMC (`speed-step = <0>` i.e. SDR, `phase_tuning =
<0 0>` i.e. tuning disabled, fixed phase). The eMMC PLL is programmed once
in `.init()` to the vendor's own `MMC_TIMING_MMC_HS` setting (freq code
0x57, ~100MHz raw PLL output — from `rtkemmc_set_ios()`'s `MMC_TIMING_MMC_HS`
case); the board DT's `clock-frequency = <100000000>` (no `clocks =`
property, since there's no mainline provider) tells the `dw_mmc` core to
treat that as `host->bus_hz`, so its own generic `CLKDIV` math in
`dw_mci_setup_bus()` derives both the ~400kHz identification clock and the
~50MHz operating clock from it directly — reproducing the vendor's exact
"100MHz/2 = 50MHz" `MMC_TIMING_MMC_HS` result without the glue driver
needing per-mode PLL switching or a custom clk provider at all.

Pad drive strength (`realtek,pad-driving` DT property) is the vendor's own
`pddrive_nf_s0` value for this board (`<1 0x77 0x77 0x77 0x33>`, i.e.
clk/cmd/data/ds = 0x77/0x77/0x77/0x33), applied once in `.init()`.

**HS200/HS400 not wired up** — this board's own DTS has no pad-drive
calibration table for them either (only `pddrive_nf_s0`/SDR and
`pddrive_nf_s2`/HS200 exist, and HS200 is never selected since
`speed-step` is 0), so there was nothing to cross-check against; a
possible Phase 2 if needed later.

**Status: controller probes correctly, but the first real-hardware test
hit a full kernel hang.** `dwmmc_rtd129x 98012000.mmc:` logged a real
version-ID readback (`Version ID is 270a`) and correct bus-speed math
(`Bus speed (slot 0) = 100000000Hz (slot req 400000Hz, actual
400000HZ div = 125)`) — confirming the CRT clock/reset/PLL setup in
`.init()` is correct — but also `"2 deep fifo"`, and a few seconds
later the whole kernel hit an RCU stall (NMI backtrace, CPU0 idle,
no forward progress) with no card ever detected.

Root cause: `dw_mmc.c`'s own fallback for an unsupplied `fifo-depth`
DT property — read `FIFOTH`'s power-on `RX_WMark` field back at probe
time — assumes nothing touched that register first. On this board
something did: the same eMMC controller already loaded the kernel
`Image` earlier in this exact boot (U-Boot/FSBL), reprogramming
`FIFOTH` in the process, so Linux read back a corrupted value
(`RX_WMark=1` → `fifo_depth=2`) instead of the true reset value. A
2-entry FIFO is nonsensical hardware and produces wrong DMA
burst-size/threshold register values, which is the likely hang cause.
Fixed by supplying `fifo-depth = <128>` explicitly in the board DT,
sourced from a comment in the vendor `rtkemmc.c` showing its own
*original* (untouched) `FIFOTH` value (`RX_WMark=127`, i.e.
`FIFO_DEPTH-1=127` per the mainline driver's own documented power-on
convention).

**Second hardware test: `fifo-depth` fix confirmed correct (`"128 deep
fifo"` in the log, no more `"2 deep fifo"`) — but the exact same total
kernel hang/RCU stall recurred anyway**, at essentially the same point
in the boot timeline (right after `mmc0` registers and logs correct
bus-speed math). So the fifo-depth bug, while real and worth fixing,
was not the (or not the only) cause.

Root cause #2: `MMC_CAP2_NO_SDIO`/`MMC_CAP2_NO_SD` were never set.
Without them, `mmc_rescan()` tries to attach this device as SDIO
(`CMD5`) and SD (`CMD8`/`ACMD41`, including voltage-switch handling)
before ever trying plain MMC (`CMD1`) — each attempt power-cycles the
bus via `dw_mci_set_ios()`'s power-mode handling. This is a
hardwired, non-removable eMMC that will never be anything else; the
vendor driver explicitly sets `mmc->caps2 = (MMC_CAP2_NO_SDIO |
MMC_CAP2_NO_SD)` in `rtkemmc_probe()` for exactly this reason — a
detail this port's `.init()` had missed since there's no mainline DT
binding for it (has to be set in code, via `host->pdata->caps2`,
which `dw_mci_init_slot_caps()` later folds into `mmc->caps2`).

**Third/fourth hardware test: same hang, byte-for-byte identical
timing and PC, with `NO_SDIO`/`NO_SD` in place** — ruled that fix out
as *the* cause too (still worth keeping). Enabled
`CONFIG_WQ_WATCHDOG`/`CONFIG_DETECT_HUNG_TASK`/`CONFIG_SOFTLOCKUP_DETECTOR`
for better diagnostics and re-tested. The softlockup watchdog nailed
it: `CPU#0 stuck for 44s! [swapper/0:0]`, repeating at 70s/104s/115s,
**always at the exact same PC** — `arch_local_irq_enable`, the
instruction right after `WFI` in the idle loop. CPU0 wasn't spinning
anywhere; it went idle and then never received *any* interrupt again
— not even its own periodic timer tick — for 90+ seconds straight,
while CPU1-3 kept running normally (they're what send the periodic
cross-CPU NMI backtrace requests that catch CPU0 mid-freeze). A
follow-up test with the eMMC PLL programming (`rtd129x_emmc_pll_init()`,
the most invasive thing this driver does to CRT) skipped entirely
produced the *identical* hang — ruling out the PLL as the cause too.

Root cause #3 (the real one): `drivers/ata/ahci_rtd1295.c`'s port-0
clock-gate enable and this driver's own eMMC clock-gate enable write
the **same physical CRT register** (`CRT+0x0C`, `0x9800000C` —
different bits: `SATA_0`/`SATA_ALIVE_0` there, `EMMC`/`CR`/`EMMC_IP`
here), with no lock between the two unrelated drivers. AHCI's probe
runs repeatedly via deferred-probe retries (visible in the boot log as
several repeats of "sata clocks enabled..."), overlapping in time with
this driver's own one-shot read-modify-write on the same register — an
unlocked interleaving lets one driver's write clobber the other's,
based on a stale read (a lost update on a real hardware register).
AHCI's own delayed host-init work doesn't touch the controller again
until ~11s after probe (`schedule_delayed_work(800ms)`, further
delayed by scheduling contention), so a clock silently dropped by this
race doesn't actually bite until then — exactly matching the observed
hang always landing right after AHCI's port-0 setup prints, ~13s in.

Fix: a shared spinlock (`rtd129x_crt_lock`, defined and exported from
`ahci_rtd1295.c`) around both drivers' `CRT+0x0C` read-modify-write.

**Fifth hardware test: same hang again, byte-for-byte, with the CRT
lock in place too** — ruled that out as well. At this point every
register-content change had been tried without effect, while the hang
was 100% reproducible and always struck at the same point, so the
next step was a controlled experiment rather than another guess:
disabled the `emmc` DT node entirely (`status = "disabled"`, otherwise
identical kernel) and re-tested. **SATA came back immediately** — both
ports linked at 6.0 Gbps, full boot to shell — conclusively proving
the eMMC device's mere presence (probing it at all) was the trigger,
regardless of what its own register content said.

That ruled out this driver's own register pokes as the cause and
pointed at generic `dw_mmc.c` core behavior instead. Re-enabled the
node and turned on command-level tracing (`dyndbg="file dw_mmc.c +p"`
etc., `loglevel=8`) for the next test, which pinpointed it exactly:
`CMD0`/`CMD1`×3/`CMD2`/`CMD3`/`CMD9`/`CMD7` all complete cleanly over
IRQ (interrupt delivery itself is fine), but `CMD8`
(`SEND_EXT_CSD`, the first real DMA data transfer, a plain 512-byte
single-block read) hangs forever — no completion, and no data-timeout
error either, despite `TMOUT`/`drto_ms` having been programmed
correctly immediately beforehand. Not a descriptor bug: the IDMAC
descriptor control-word format this core builds
(`dw_mci_idmac_init()`/`construct_data_addr()`, OWN/DIC/CH/FD/LD)
matches the vendor 4.9.330 driver's own `make_sg_des()`/`make_ip_des()`
bit-for-bit. Forcing PIO (by temporarily short-circuiting
`dw_mci_init_dma()`) made `CMD8` complete immediately, the card
identify normally (`4FPD3R`), and `mmcblk0` register — confirmed this
was really it, not a coincidence.

Fix: added a proper `DW_MMC_QUIRK_NO_DMA` bit to the mainline
`dw_mmc.c` core (following the existing precedent —
`dw_mmc-exynos.c` already sets `host->quirks` from its own `.init()`
hook the same way) instead of leaving the diagnostic short-circuit in
place; `dw_mci_rtd129x_init()` sets it. PIO is slower than DMA, but
correct, and this driver's whole Phase 1 scope is "get it working"
over "get it fast" — DMA can be revisited later if ever needed (would
need to understand the actual bus-level cause first).

**Status: card identifies and boot no longer hangs** — eMMC identifies
(`mmcblk0`, model `4FPD3R`) and SATA continues normally past it (both
bays link at 6.0 Gbps). This fixed the hang, but turned up a second,
separate bug: `mmcblk0` registered at **`0 B`** capacity
(`/sys/block/mmcblk0/size` = 0, no `/proc/partitions` entry, reads
return 0 bytes instantly) — see the next entry.

### eMMC zero-capacity (`0 B`) after the DMA fix — `DW_MMC_QUIRK_FIFO64_32`

With `DW_MMC_QUIRK_NO_DMA` in place the card identified
(`mmcblk0: mmc0:0001 4FPD3R 0 B`) but registered with **zero
capacity**. `cat /sys/class/mmc_host/mmc0/mmc0:0001/csd` returned real,
non-trivial data (`d02701320f5903fff6dbffef8e40400d`) and `name`/`type`
read back correctly (`4FPD3R`/`MMC`) — so command/response-level
access is fine. The distinction: `CID`/`CSD` come back over the
32-bit `RESP0-3` registers directly, never touching the FIFO, whereas
this card is high-capacity (CCS=1 in the `CMD1` OCR response), so its
real capacity comes from `EXT_CSD`'s `SEC_COUNT` field — fetched by
`CMD8`/`SEND_EXT_CSD`, a genuine 512-byte **FIFO** data read, and the
very first FIFO data read PIO mode ever performs on this hardware.

`dmesg` had already logged `"64 bit host data width"` for this
controller. Mainline's default PIO path for a 64-bit-wide FIFO
(`dw_mci_pull_data64()`/`push_data64()`) does a genuine 64-bit
`__raw_readq()`/`__raw_writeq()` (`mci_fifo_readq()`/`writeq()`) to the
FIFO register. This SoC's bus apparently can't service a true 64-bit
MMIO transaction to this peripheral and silently returns/accepts all
zeroes instead of erroring — so `SEND_EXT_CSD` "succeeds" (correct
command completion, no error) but the 512-byte buffer it fills is all
zero, and `SEC_COUNT` reads back 0.

This is a known class of bug in mainline: `DW_MMC_QUIRK_FIFO64_32`
already exists for exactly this ("Some dw_mmc devices have 64-bit
FIFOs, but expect them to be accessed using two 32-bit accesses" —
`dw_mmc.h`), used e.g. by `dw_mmc-exynos.c`. Setting it from
`dw_mci_rtd129x_init()` switches PIO to
`dw_mci_pull_data64_32()`/`push_data64_32()`, which access the FIFO as
two paired 32-bit reads/writes (`mci_fifo_l_readq()`/`writeq()`)
instead of one 64-bit one — no core changes needed this time, purely a
quirk-bit flip in the glue driver.

**Status: confirmed fixed on real hardware.** `mmcblk0` now reports its
real `3.64 GiB` capacity, `sfdisk`/`/proc/partitions` see the actual
factory partition table (`p1`-`p6`), `mmcblk0boot0`/`boot1` (4 MiB
each) also register, and `dd if=/dev/mmcblk0 of=/dev/null bs=1M
count=64` reads 64 MiB cleanly at ~20.4 MB/s (repeated twice, same
result). Write access (write-then-readback into the free space past
the last partition) was confirmed working by the user directly. This
closes out eMMC Phase 1 (full read+write, as originally scoped).

### eMMC read throughput ~2x below the vendor 4.9.330 kernel

User-reported: eMMC read throughput is roughly half of what the
vendor 4.9.330 kernel achieves. Back-of-envelope check against the
measured 20.4 MB/s PIO read: at 8 bytes per FIFO access pair (paired
32-bit due to `DW_MMC_QUIRK_FIFO64_32`), that works out to ~392ns per
8-byte chunk -- squarely in the range of a single MMIO round-trip
latency on this SoC's bus (~150-200ns per `readl()`), not the 8-bit
SDR bus rate. That points at the PIO drain loop itself (CPU/MMIO
latency-bound) as the bottleneck, not the eMMC clock: the card is
very likely delivering bits into the FIFO faster than the CPU drains
it, so raising the bus clock shouldn't change read throughput at all
while still on PIO. The vendor's own board DTS
(`pelican-4.9.330/.../rtd-1296.dtsi`) defaults to `speed-step = <0>`
(SDR/HS) and `phase_tuning = <0 0>` (disabled) too -- HS200 in the
boot log is only U-Boot's own loader, not the production kernel -- so
the 2x gap is most likely PIO vs DMA at the *same* clock, not HS vs
HS200. The real lever for eMMC throughput is still the unresolved
IDMAC data-transfer hang (see `DW_MMC_QUIRK_NO_DMA` above), not clock
mode. HS200 (below) was implemented anyway at the user's request, for
protocol completeness -- it is not expected to move this number while
PIO stays forced.

### eMMC HS200 (Phase 2, `drivers/mmc/host/dw_mmc-rtd129x.c`)

Adds `MMC_TIMING_MMC_HS200` on top of the Phase 1 fixed-HS baseline:
`.set_ios` switches the PLL freq code (0x57 HS <-> 0xa6 HS200, both
vendor-sourced from `rtkemmc_set_ios()`) and `host->bus_hz`
accordingly, right before the core's own `dw_mci_setup_bus()` runs in
the same call so its generic CLKDIV math picks up the new frequency
(div=0/bypass at 200MHz==200MHz, matching the vendor's
`EMMC_CLOCK_DIV_NON`) -- same pattern `dw_mmc-exynos.c`/
`dw_mmc-rockchip.c` use. `.execute_tuning` sweeps RX phase (VP1) across
its full 0-0x1f range via the generic `mmc_send_tuning()` (CMD21) and
picks the widest contiguous passing window's midpoint, the same
approach as the vendor's own `search_best()` and mainline
`dw_mmc-exynos.c`'s `get_best_clksmpl()`, generalized from 8 to 32
candidates. Pad-driving switches too (SDR vs HS200 table, both
DT-overridable via `realtek,pad-driving[-hs200]`, HS200 default =
vendor's `pddrive_nf_s2`).

**Known limitation**: TX (VP0/write) phase is not independently tuned.
CMD21 is a spec-defined *read*-only tuning mechanism; the vendor's own
TX margin validation (`rtkemmc_phase_tuning()`) is a proprietary
CMD13/CMD25-based write-side sweep with escalating pad-drive-strength
retries that has no mainline equivalent, and porting that whole
dual-direction retry state machine was judged out of scope for this
phase. TX stays at the fixed value that was safe for Phase 1's HS
mode. If HS200 proves unstable specifically on writes, this is the
first place to look.

**Status: confirmed working on real hardware** (`putty.log.81`).
Tuning found a wide passing window (`RX window=0x1fffffff`, 29/32
candidates, phase settled at `0xe`), the card identifies as
`new HS200 MMC card`, capacity/partition table are unchanged from
Phase 1, and a clean 800 MiB sequential read across `mmcblk0p5`
completed with zero errors (`dd`, ~37s). Bus speed log confirms the
clock actually reached 200MHz with `div=0` (bypass), as designed.

Read throughput: **~23 MB/s, barely above the ~20.4 MB/s Phase 1 (HS)
number** -- confirms the "eMMC read throughput ~2x below vendor"
analysis above: a 4x clock increase (50MHz -> 200MHz) bought only
~12% more throughput, consistent with the PIO drain loop (not the
card/bus clock) being the actual bottleneck. HS200 is real and stable
but is not the fix for the throughput gap; DMA is.

### eMMC IDMAC hang, second attempt: `DW_MMC_QUIRK_NO_IDMAC_FB` -- tried and reverted

Diffed every `BMOD` write in the vendor's own IDMAC-based `rtkemmc.c`
(which does move real DMA data successfully on this exact hardware)
against mainline `dw_mci_idmac_start_dma()`: the vendor never sets
`BMOD.FB` (Fixed Burst, bit1) -- only `IDMAC_ENABLE`/`SWRESET`, in
every single BMOD write in the file (probe and per-transfer paths
alike). Mainline always ORs `SDMMC_IDMAC_FB` in unconditionally.
Added `DW_MMC_QUIRK_NO_IDMAC_FB` to skip it and re-enabled real DMA
(replacing `DW_MMC_QUIRK_NO_DMA`) to test the theory.

**Result: hang recurred, and the failure signature is worse than the
original.** `putty.log.82`: boot proceeds normally past
"Using internal DMA controller" and even past SATA finishing its own
probe (port setup, PHY init OK, ~13.5s), then CPU0 stops receiving
*any* interrupts at all -- not just the mmc completion. NMI backtrace
(triggered by `rcu_preempt` stall detection at t=23s and again at
t=36s) shows CPU0 stuck in `do_idle()`/`arch_local_irq_enable()`, i.e.
it went idle correctly and then simply never woke up for anything,
including its own periodic tick. That's consistent with the IDMAC's
AHB master wedging the whole bus interconnect on some transaction the
fabric can't complete, not just failing to finish its own DMA --
broader collateral damage than a single wrong register bit should
plausibly cause by itself, which suggests either the FB-bit theory is
wrong, or it's real but incomplete (another difference still needed).

Reverted (`DW_MMC_QUIRK_NO_IDMAC_FB` removed, back to
`DW_MMC_QUIRK_NO_DMA`/PIO) without pushing the broken intermediate
state anywhere.

### eMMC IDMAC hang, third attempt: full vendor-matching register set -- tried and reverted

Rather than guess another single bit, diffed *every* `BMOD`/`CTRL`/
`IDINTEN` write across the entire vendor `rtkemmc.c` (probe and
per-transfer paths, no exceptions) against mainline and found two more
consistent differences beyond `BMOD.FB`:

- `CTRL.DMA_ENABLE` (bit5) -- mainline sets it before every DMA
  transfer; the vendor never sets it anywhere, only `CTRL_USE_IDMAC`
  (bit25) and the global interrupt-enable bit.
- `IDINTEN` -- mainline enables `NI|RI|TI` (IDMAC's own completion
  interrupt); the vendor always writes `0` (every IDMAC interrupt
  source disabled), with an explicit comment: *"we do not use
  abnormal interrupt, so disable all"*. The vendor's own IRQ handler
  never reads IDMAC status at all -- it completes pending operations
  off the card-side `RINTSTS` interrupts alone (`DTO` et al.), the
  same ones already driving this driver's PIO/command path.

Added `DW_MMC_QUIRK_NO_IDMAC_DMA_ENABLE` and
`DW_MMC_QUIRK_NO_IDMAC_INTEN` alongside `DW_MMC_QUIRK_NO_IDMAC_FB` and
set all three together (reasoning: the vendor's driver is one
cross-validated combination, not independently-tunable knobs -- no
reason a single bit tested alone should have been expected to work).

**Result: identical hang, right down to the timing.** `putty.log.83`:
same "CPU0 stuck in `do_idle()`/`arch_local_irq_enable()`, loses all
interrupts" signature as attempt two, at essentially the same
timestamps (`rcu_preempt` self-detected stall at t=23s, expedited
stall at t=36s) -- and this time a soft-lockup BUG also fired at t=48s
(44s stuck), removing any doubt that the CPU genuinely never recovers.
Combining all three vendor-matching register differences changed
*nothing* about the outcome versus the single-bit attempt.

**That result is itself informative**: if `BMOD`/`CTRL`/`IDINTEN`
register content were the cause, matching the vendor's values exactly
should have changed *something* about the failure. It didn't -- same
failure class, same timing, down to the second. That points away from
a register-content mismatch anywhere in
`dw_mci_idmac_init()`/`dw_mci_submit_data_dma()`/
`dw_mci_idmac_start_dma()` entirely, and toward something the vendor's
driver structurally avoids that mainline's IDMAC path doesn't --
plausibly a genuine RTD1296 bus-fabric behavior around the IDMAC's
first real burst that this session has no documentation for, not a
fixable quirk bit.

Reverted (all three `DW_MMC_QUIRK_NO_IDMAC_*` removed, back to
`DW_MMC_QUIRK_NO_DMA`/PIO) without pushing the broken intermediate
state anywhere.

**Decision at the time: DMA investigation closed for this port.** Three
real hardware hangs from three different, individually well-evidenced
register-level hypotheses (plain IDMAC; `BMOD.FB` alone; the full
vendor-matching `BMOD`/`CTRL`/`IDINTEN` combination) was a clear enough
signal to stop guessing on register *content*. See the fourth attempt
below for one more angle that was tried later, and why the closure
stands.

### eMMC IDMAC hang, fourth attempt: write-posting barrier -- tried and reverted

All three prior attempts changed *what* gets written to `BMOD`/`CTRL`/
`IDINTEN`; none changed *when* a write is guaranteed to have actually
landed. Re-reading `rtkemmc.c` with that distinction in mind turned up
something none of the first three attempts had looked at:
`rtkemmc_writel()`, the macro wrapping literally every single register
write in that entire driver, expands to `sync(emmc_port); writel(val,
addr);` -- and `sync()` is `dmb(sy); writel(0x0,
emmc_port->sb2_membase + 0x20); dmb(sy);`, a write to a *third* MMIO
region (`sb2_membase = of_iomap(emmc_node, 2)`, physical `0x9801a000`
-- the same block mainline's own `rtd129x.dtsi` already declares as
`sb2: syscon@1a000`, just unused). Most call sites also add an explicit
`isb(); sync();` right *after* the write too, i.e. double-barriered.
That block also hosts a documented Realtek HW-semaphore mechanism
(`drivers/soc/realtek/common/rtk_sb2_sem.c`: `readl()` to try-lock,
`writel(0, ...)` to unlock) -- `sync()`'s `writel(0x0, ...)` matches
the *unlock* pattern, but is called unconditionally, with no matching
lock anywhere nearby; read as a barrier trick (poke something on the
same bus segment to force the fabric to drain outstanding posted
writes) rather than real semaphore usage, this looked like a genuine,
previously-untested class of fix -- plausibly explaining why IDMAC's
own asynchronous read of `BMOD`/`DBADDR` (a second bus master, not the
CPU) could see stale content despite mainline's existing `wmb()`s,
which only order the CPU's own issue sequence, not guarantee fabric
drain time.

Added a new optional `dw_mci_drv_data.dma_start_barrier` hook (core
`dw_mmc.c`/`dw_mmc.h`, following the same "new hook for a real
cross-platform need" precedent as the existing `hw_reset`), called at
three points in `dw_mci_idmac_start_dma()`: after the `CTRL`/
`USE_IDMAC` write, after the `BMOD`/`ENABLE` write, and after
`PLDMND` (the actual DMA-start trigger). The glue driver's
implementation (`rtd129x_emmc_dma_barrier()`) ioremaps SB2 directly
(same pattern as the existing raw CRT ioremap in this file) and does
`mb(); writel(0, sb2 + 0x20); mb();`, applied only around IDMAC's own
start sequence rather than reproducing the vendor's blanket
every-write barrier. Temporary `dev_info()` calls before/after each of
the three call sites let a hang be pinpointed to one specific
sub-step. `DW_MMC_QUIRK_NO_DMA` was removed for this test (real IDMAC
re-enabled).

**Result (`putty.log.94`, two consecutive boot attempts):**

- **First boot: hung *inside the barrier itself***. Log shows `ctrl
  pre`/`ctrl post`/`bmod pre`/`bmod post`/`pldmnd pre` -- then nothing;
  the very next log lines are a fresh bootloader restart (user power
  cycle). The barrier's own `mb(); writel(...); mb();` sequence right
  after `PLDMND` never completed. That's a new and unexpected data
  point: not just IDMAC's eventual data burst, but a plain CPU write to
  SB2 *immediately after* triggering `PLDMND` can itself stall --
  consistent with the SB2 crossbar becoming genuinely contended/
  unavailable once IDMAC's own bus-master transaction is in flight,
  which would mean this specific barrier can race the exact condition
  it's trying to guard against.
- **Second boot: all three barrier call sites completed cleanly**
  (`pldmnd post` present this time -- the race from boot 1 didn't
  recur) -- **but the system hung anyway**, with the *identical*
  signature and near-identical timing as attempts two and three:
  `rcu_preempt` self-detected stall at t=23s, expedited stall at t=36s,
  another self-detected stall at t=86s, CPU0 always stuck at
  `arch_local_irq_enable`/`do_idle`, never receiving another interrupt.

Both outcomes point the same direction: the barrier hypothesis doesn't
fix the hang, and when it doesn't hang on its own, the underlying bug
still fires downstream, unchanged. Four attempts (register content x3,
write ordering x1) now converge on the same failure class regardless
of what was tried, which is itself the strongest evidence yet that the
real cause is something structural in how mainline's generic IDMAC
path drives this SoC's fabric on a first real burst -- not a register
value or barrier this session can guess its way to from the vendor's
driver alone.

Reverted in full (`dw_mci_drv_data.dma_start_barrier` hook, the glue
driver's SB2 ioremap/barrier/diagnostics, all three core call sites) --
back to byte-identical `DW_MMC_QUIRK_NO_DMA`/PIO. Nothing from this
attempt was ever pushed; the revert restores exactly the previously-
confirmed-working state before rebuilding.

**Decision at the time: DMA investigation closed for this port.** Four
real hardware hangs across two genuinely different classes of
hypothesis (register content, write ordering) was conclusive enough --
see the fifth attempt below for the last gap in the test matrix, and
why the closure stands more firmly than ever.

### eMMC IDMAC hang, fifth attempt: `CTRL.DMA_ENABLE` in isolation -- tried and reverted

A methodological gap in the third attempt: it changed `BMOD.FB`,
`CTRL.DMA_ENABLE`, and `IDINTEN` all *together* in one combined quirk
set, on the reasoning that "the vendor's driver is one cross-validated
combination, not independently-tunable knobs." That's a reasonable
default, but it means `DMA_ENABLE` alone was never actually isolated --
only `BMOD.FB` alone (attempt two) and all three together (attempt
three) were tested. If `DMA_ENABLE` were the one bit that mattered, and
`IDINTEN`'s vendor value (disabling all of IDMAC's own interrupt
sources) happened to reintroduce a *different* problem when combined,
attempt three could have hung for a completely different reason than
attempt two, masking a real fix.

Closed that gap directly: added `DW_MMC_QUIRK_NO_IDMAC_DMA_ENABLE`
(skips only the `CTRL.DMA_ENABLE` bit mainline sets in
`dw_mci_submit_data_dma()` that the vendor driver never sets anywhere)
and set *only* that quirk -- `BMOD.FB` and `IDINTEN` left at mainline's
normal defaults this time, the exact opposite combination from attempt
three. `DW_MMC_QUIRK_NO_DMA` removed again for the test, with a simple
`dev_info()` confirming the active quirk bitmask at probe time.

**Result (`putty.log.95`, two consecutive boot attempts): identical
hang both times, indistinguishable from attempts two, three, and four.**
`dwmmc_rtd129x 98012000.mmc: DMA experiment: quirks=0x8
(NO_IDMAC_DMA_ENABLE only)` confirms the quirk was active and nothing
else was; `IDMAC supports 32-bit address mode` / `Using internal DMA
controller` show the probe path completed normally. Both boots then
hung with the same signature at essentially the same timing as every
previous attempt: CPU0 stuck at `arch_local_irq_enable`/`do_idle`,
`rcu_preempt` stalls at t~23s and t~36s (and t~86s on the second boot),
never receiving another interrupt. `DMA_ENABLE` in isolation changes
nothing -- ruling out the specific methodological gap attempt three
left open.

Reverted in full (quirk bit, core `if` check, glue-driver flag and log
line) -- back to byte-identical `DW_MMC_QUIRK_NO_DMA`/PIO. Nothing from
this attempt was ever pushed.

**Decision at the time: DMA investigation closed for this port.** Five
real hardware hangs -- all four individually-identifiable
register-content variations that came up in comparing the vendor
driver against mainline, plus a write-ordering barrier -- was
exhausted for that comparison. See the sixth attempt below for one
more, genuinely different category, and why the closure still stands.

### eMMC IDMAC hang, sixth attempt: IDMAC reset-completion polling -- tried and reverted

A third category, distinct from both register content and write
ordering: reset-completion *timing*. `dw_mci_idmac_start_dma()` calls
two resets back to back -- `dw_mci_ctrl_reset(host,
SDMMC_CTRL_DMA_RESET)` (CTRL bit2, *polled* until it self-clears, via
`readl_poll_timeout_atomic()`) and `dw_mci_idmac_reset()`
(`BMOD.SWRESET`, bit0, self-clearing per the Synopsys databook --
*never polled*, just written and immediately trusted). Only a `wmb()`
and two more register writes (`CTRL.USE_IDMAC`, `BMOD.ENABLE`) separate
setting that bit from `PLDMND` actually starting the transfer -- a few
instructions, no deliberate wait. The vendor driver sets the same
`BMOD` bit only as part of a full host-reset sequence with roughly 15
more register writes (each individually barrier-wrapped) in between
asserting it and the point that depends on it being clear -- explicitly
commented there as relying on it clearing by then, not an explicit
poll either, but with far more elapsed real time than mainline's tight
sequence gives it.

Added `DW_MMC_QUIRK_IDMAC_RESET_POLL`: after `dw_mci_idmac_reset()`,
poll `BMOD` until `SWRESET` clears (same `readl_poll_timeout_atomic()`
pattern as the existing `CTRL.DMA_RESET` poll right next to it, 500ms
bound, `dev_err()` on timeout) before proceeding. Set alone -- no
content or ordering changes this time, purely closing the "was reset
actually complete" question. `DW_MMC_QUIRK_NO_DMA` removed for the
test.

**Result (`putty.log.96`, two consecutive boot attempts): identical
hang again, both times.** No `"Timeout waiting for BMOD.SWRESET to
clear"` ever printed -- the poll always succeeded immediately, meaning
`SWRESET` genuinely does self-clear fast on this hardware, same as
`DMA_RESET` right next to it. Both boots then hung with the same
signature and the same ~23s/36s/86s timing as every previous attempt.
Reset-completion timing wasn't the gap either.

Reverted in full (quirk bit, core poll, glue-driver flag and log line)
-- back to byte-identical `DW_MMC_QUIRK_NO_DMA`/PIO. Nothing from this
attempt was ever pushed.

**Decision, reaffirmed a third time: DMA investigation closed for this
port.** Six real hardware hangs across three genuinely distinct
categories -- register content (four variations), write ordering, and
reset-completion timing -- is about as exhaustive as a black-box
comparison against the vendor driver's source can get. This isn't a
register-poke or barrier-placement problem solvable from driver source
comparison alone; it would need real RTD1296 bus/fabric documentation
this session doesn't have access to. PIO (plus HS200) stays the
shipped configuration, and further attempts without genuinely new
information aren't planned.

### eMMC DMA: what real RTD1296 documentation search actually turned up

No official Realtek datasheet or interconnect/fabric reference for
RTD1295/RTD1296 is publicly available -- confirmed by web search
(forum threads asking for one go unanswered; even the mainline arm64
port author's own public patches only ever cite vendor DTS/driver
source, never a datasheet). One genuinely useful data point did turn
up, though: `jjm2473/rtd1295-next` (a separate community 5.9 kernel
port for RTD129x, eMMC-capable, unrelated to this project) took a
completely different approach to this same eMMC block -- instead of a
thin glue driver over mainline's generic `dw_mmc.c`/IDMAC core (this
port's approach), its `drivers/mmc/host/rtkemmc.c` is the vendor
4.9.330 driver forward-ported *wholesale* (one commit, 5617 lines
added). Diffing that file against this project's own copy of the
vendor source: ~1100 lines differ, but every one of them is
kernel-API adaptation (renamed struct fields, headers) -- the entire
DMA/BMOD/`sync()`/SB2-barrier sequence is byte-for-byte identical,
down to whitespace. The one other project that's actually gotten real
DMA working on this exact IP did it by keeping the vendor's *entire*
bespoke command-dispatch/DMA state machine intact, not by fixing
mainline's generic IDMAC path.

That's a plausible explanation for why all six targeted attempts above
failed identically: the gap may not be a single missing register value,
write-ordering barrier, or reset-completion wait that a quirk bit can
patch onto mainline's generic path -- it may be the generic path's
overall *structure and timing* (the vendor's per-transfer sequence
interleaves dozens of individually-barriered register writes in a
specific order; mainline's `dw_mci_idmac_start_dma()` does the
equivalent in about five instructions) that doesn't match what this
silicon needs. A real fix, if wanted later, likely means forward-porting
substantially more of the vendor's own DMA sequencing -- the same
~5000-line scope this port's glue-driver approach deliberately chose to
avoid from the start (see this file's header comment) -- not another
isolated quirk bit. Separately, mainline's generic IDMAC path has its
own history of subtle, still-being-discovered platform-specific bugs
as of late 2024 (`mmc: dw_mmc: Fix IDMAC operation with pages bigger
than 4K`, reverted after breaking RK3566/JH7100/JH7110 with panics and
`swiotlb buffer full` errors) -- not this port's bug specifically (that
one was about multi-descriptor/large-page transfers; this port's hang
is on a single 512-byte descriptor), but confirmation that the generic
path is a real source of platform-specific breakage in general, not
just on Realtek silicon.

### Flash-backed RTC (SFC/SPI-NOR) -- investigated, closed

Open question carried over from initial bring-up: this Duo unit's
`rtk-sfc` probe fails outright (`unrecognized JEDEC id bytes: ff, ff,
ff` -- an all-high SPI read, the classic "nothing answering" pattern),
unlike Monarch's SFC, which came back a real, if initially misread,
Winbond ID. Was it a rescue-DT power/mux gap, or does this unit
genuinely have no working boot SPI-NOR?

Resolved by two checks, no new hardware testing needed:

1. **DTS diff**: the vendor's own *production* board DTS
   (`rtd-1296-pelican-1GB.dts`) doesn't override the SFC node at all --
   it inherits `status = "okay"` from the shared `rtd-1296.dtsi`
   exactly like the rescue board DTS (`rtd-1296-pelican-rescue.dts`)
   does. Diffing every SFC/SPI-related line between the two board DTS
   files: zero differences. There's no board-level pin/power setup in
   production that's missing from rescue.
2. **A captured boot log of the vendor's own 4.9.330 kernel** (not our
   port) on this same real unit, using their own `rtk-sfc` driver,
   shows the *identical* `unrecognized JEDEC id bytes: ff, ff, ff`
   failure (this was already in an earlier session capture, just not
   connected to this question until now).

Vendor software, vendor driver, identical DT config to production --
still nothing answers. That rules out a rescue-specific gap. This
unit's boot SPI-NOR is most likely simply not populated (a BOM
variant) or non-functional on this specific board -- not something a
DT or driver change on our side can fix.

**Status: closed, not planned.** The `sfc` node stays `status =
"disabled"` in the board DTS (updated comment explains the finding).
System clock stays epoch-at-boot, NTP-corrected once networking comes
up -- the same fallback Monarch used before its flash-RTC existed, now
permanent for this unit rather than temporary.

### RTS5400 Type-C PD companion chip (`i2c5`) -- investigated, closed

Open backlog item carried over from initial bring-up: an "RTS5400
USB3 hub IC" the vendor's `rtd1296.dtsi` hangs off `i2c5`
(`rts5400@6A`, `compatible = "rtk-rts5400"`), with no mainline driver.
Worth checking what it actually does before deciding whether porting
it is worthwhile.

Read the vendor's own driver (`drivers/usb/dwc3/rtk-rts5400.c`,
`pelican-4.9.330`) in full. It's not a hub driver at all -- the
RTS5400's USB3 hub function is pure transparent hardware, invisible to
software; Linux's generic USB core already enumerates it as an
ordinary hub with zero driver involvement (confirmed working: see
`dwc3_u3host`/xHCI in Confirmed working above). The I2C side is a
**Type-C Power Delivery port manager** instead, talking a bespoke,
non-TCPM-compliant Realtek protocol (custom command/ping-status/PDO
framing, nothing an existing mainline TCPM/UCSI driver would
recognize). Its entire practical effect on this board is one GPIO:
`realtek,12v-power-gpio = <&rtk_misc_gpio 16 1 0>` (comment: *"1296
u3host power, output, default low"*), which the vendor driver only
drives high after successfully PD-negotiating >=12V with whatever's
plugged into the u3host port's Type-C connector.

Without this driver, that GPIO simply stays at its default (low --
off), and the port runs as a plain, spec-standard 5V-bus-powered USB3
host, which is exactly what's already confirmed working on real
hardware. The only thing skipped is an optional extra 12V accessory
rail that would only ever matter for some specific PD-aware
12V-hungry accessory being plugged into that exact port -- a narrow
case, not something the rescue-boot USB stick or the SATA/eMMC storage
paths depend on.

Given the driver would mean reverse-engineering and forward-porting a
~700-line bespoke vendor protocol for a niche optional-power feature
on an already-fully-functional port, decided with the user not to
pursue it.

**Status: closed, not planned.** No DTS node added for `rts5400@6A`
(left out entirely, same as before); `i2c5` itself stays `status =
"okay"` (harmless with no child device node -- same bus Monarch's
port also leaves enabled unconditionally).

### Cosmetic poweroff driver (`drivers/power/reset/wdmc-poweroff.c`) -- LED, fan, HDD, USB VBUS

Neither mainline nor the vendor 4.9.330 source implements a real
hardware power-off for this board (no board-level 12V power-hold GPIO
exists in either board's vendor DTS), so `halt`/`poweroff` used to just
park the CPU with the board still fully powered: fan spinning, SYS LED
lit, USB VBUS live on both bays, disks still spinning. This board's DTS
carried a `realtek,rtd129x-coolboot-poweroff` node left over from the
community port, but that compatible string matches no driver anywhere,
mainline or vendor -- confirmed by grepping both GPL source drops
directly; removed. New `wdmc-poweroff.c` driver (shared verbatim with
symops/monarch-6.18) quiets everything actually under this SoC's
control at shutdown time:

- **SYS LED + fan**: turns off both PWM channels' OCD register bits
  directly (same effect as `pwm_disable()`, reached by a raw
  `devm_ioremap()` poke since the `pwm@d0` block is already exclusively
  owned by the real `pwm-rtd129x.c` driver).
- **HDD spin-down**: already worked via the existing SCSI
  `manage_shutdown` sysfs attribute (enabled via udev at boot) -- not
  reimplemented, just confirmed working alongside the new driver.
- **USB VBUS**: the hard part. A misc-gpio raw-MMIO poke matching what
  a rescue initramfs's own boot-time VBUS-enable step does was tried
  first and *exhaustively disproven* on this exact board: every bit of
  misc-gpio (both 32-bit banks, 64 bits total) and every `rtk_iso_gpio`
  line swept with zero effect on VBUS -- because a brief 1-second hold,
  the sweep's test duration, shows no effect at all even on a
  genuinely correct line; the real threshold turned out to be several
  seconds. The actual working mechanism only turned up by reading the
  vendor's own `rtk_usb_manager.c` driver against a captured
  **stock-firmware boot log from this exact physical unit**
  (`mchd-stock-4.2.2.log`), not the generic reference-board DTS
  (`rtd-1296-pelican-1GB.dts`), which lists a different, incomplete
  GPIO set (2 power-gpios) than what this unit's retail firmware
  actually uses (3, with two ports sharing one). Both of this board's
  external USB ports turned out to be on `rtk_iso_gpio`, requested as
  plain gpiod consumers, no raw MMIO needed: **line 34 is the bottom
  bay's port, line 26 the top bay's** -- each independently confirmed
  on real hardware (held low for several seconds, restored by driving
  back high).

**Status: confirmed working.** See symops/monarch-6.18's README for
that board's own GPIO assignment (a completely different split: one
port on a misc-gpio bit, no `rtk_iso_gpio` line in common with this
board's) and the shared discovery methodology.

### Base version bump: v6.18.46 -> v6.18.51

Rebased onto the latest upstream stable point release following this
file's "Updating the base version" procedure (single-parent
`commit-tree`, upstream tags fetched under non-colliding aliases). The
`v6.18.50 -> v6.18.51` merge was clean -- no conflicts, none of this
port's own files touched by the upstream delta (480 files in the
diffstat, all upstream-internal). Rebuilt `Image`/`dtbs`/`modules` with
`LOCALVERSION=` and repackaged; **confirmed booting and working on real
hardware** (alongside the poweroff driver above, same test).

## Not yet confirmed / not yet ported

- **eMMC DMA** — forced to PIO (`DW_MMC_QUIRK_NO_DMA`, see Progress log
  above) after IDMAC was confirmed to hang the real hardware on every
  data transfer, six times now under six different hypotheses across
  three categories: register content (plain IDMAC; `BMOD.FB` cleared
  alone; `CTRL.DMA_ENABLE` cleared alone; the full vendor-matching
  `BMOD`/`CTRL`/`IDINTEN` combination), write ordering (a
  vendor-derived SB2 write-posting barrier around IDMAC's start
  sequence), and reset-completion timing (an explicit poll for
  `BMOD.SWRESET` to self-clear before depending on it). All six
  produced the same failure class (CPU0 loses *all* interrupts, not
  just the mmc completion) at essentially identical timing.
  Read/write/HS200 all work over PIO; revisiting DMA would need
  real RTD1296 bus/fabric documentation this session doesn't have —
  investigation closed, not presently planned.
- **Flash-backed RTC** (Monarch's scheme) — investigated and closed
  (not planned): this unit's boot SPI-NOR doesn't respond. Moot anyway
  -- the real hardware RTC works on this board (battery-backed, see
  Confirmed working above), so there's no gap to fall back for.
- **RTS5400 Type-C PD companion chip** on `i2c5` — investigated and
  closed (not planned): its USB3 hub function is fully transparent
  hardware, already working via the generic USB core (see `dwc3_u3host`
  in Confirmed working above); the vendor I2C driver only ever drives
  one GPIO-gated 12V accessory rail after a proprietary PD negotiation.
  See the Progress log entry above for the full reasoning.

All three items above are closed (investigated, not planned) rather
than genuinely open work. The fan tachometer -- the last item that was
actually open -- is now confirmed working on real hardware; see its
Progress log entry and the Confirmed working list above.
