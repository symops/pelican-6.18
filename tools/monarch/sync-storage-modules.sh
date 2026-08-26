#!/bin/sh
# Copy the three modules the initramfs insmods by hand (see initramfs/init)
# from the just-built kernel tree into initramfs/lib/modules/, so the next
# `make Image` bakes in copies whose vermagic actually matches this build.
#
# These are never committed to git (initramfs/lib/modules/.gitignore) --
# they're tied to the exact kernel version and go stale on every rebuild.
# Run this after `make modules` and before `make Image`.
set -e
cd "$(dirname "$0")/../.."

for pair in \
	"drivers/phy/realtek/phy-rtk-sata.ko" \
	"drivers/usb/storage/usb-storage.ko" \
	"drivers/usb/storage/uas.ko"
do
	if [ ! -f "$pair" ]; then
		echo "sync-storage-modules: missing $pair -- run 'make modules' first" >&2
		exit 1
	fi
	cp "$pair" "initramfs/lib/modules/$(basename "$pair")"
	echo "synced $pair -> initramfs/lib/modules/$(basename "$pair")"
done
