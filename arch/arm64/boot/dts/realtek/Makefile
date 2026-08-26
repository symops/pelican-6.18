# SPDX-License-Identifier: GPL-2.0-only

dtb-$(CONFIG_ARCH_REALTEK) += rtd1293-ds418j.dtb

dtb-$(CONFIG_ARCH_REALTEK) += rtd1295-mele-v9.dtb
dtb-$(CONFIG_ARCH_REALTEK) += rtd1295-probox2-ava.dtb
dtb-$(CONFIG_ARCH_REALTEK) += rtd1295-wd-mycloud-home.dtb
dtb-$(CONFIG_ARCH_REALTEK) += rtd1295-xnano-x5.dtb
dtb-$(CONFIG_ARCH_REALTEK) += rtd1295-zidoo-x9s.dtb

dtb-$(CONFIG_ARCH_REALTEK) += rtd1296-ds418.dtb
dtb-$(CONFIG_ARCH_REALTEK) += rtd1296-wd-mycloud-home-duo.dtb

# 16 KiB of trailing free space so U-Boot's rescue path can grow the FDT in
# place (it tries to add a /factory subnode and set linux,stdout-path on
# every boot -- see README.pelican.md's ".config re-aligned with Monarch"
# entry for how this was found: a stale-but-still-working Monarch .dtb
# happened to carry the same amount of headroom, this board's had none,
# and U-Boot's own log showed `FDT_ERR_NOSPACE` because of it).
DTC_FLAGS_rtd1296-wd-mycloud-home-duo := -p 0x4000

dtb-$(CONFIG_ARCH_REALTEK) += rtd1395-bpi-m4.dtb
dtb-$(CONFIG_ARCH_REALTEK) += rtd1395-lionskin.dtb

dtb-$(CONFIG_ARCH_REALTEK) += rtd1619-mjolnir.dtb
