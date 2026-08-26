#!/usr/bin/env python3
"""Patch arch/arm64/boot/Image's header for this board's U-Boot 2015.07.

That bootloader copies the loaded Image to the address given by the
image header's own text_offset field -- for a normal relocatable arm64
Image (text_offset=0), that means "copy it on top of itself at 0x0",
which is silent, total, undebuggable failure: no console output at all,
not even a bootloader error. This was mistaken for a kernel-size limit
for a long time (see symops/monarch-6.18's README.md) before the real
cause was found.

Usage: patch-header.py arch/arm64/boot/Image
Run this on every rebuilt Image before packaging it for the board,
raw (never gzip -- this loader's built-in gzip decompression is
unreliable above a few MB, see symops/monarch-6.18's README.md).
"""
import struct
import sys

path = sys.argv[1]
with open(path, "rb") as f:
    kernel = bytearray(f.read())

struct.pack_into("<I", kernel, 0, 0x91005A4D)   # code0: valid AArch64 instruction + "MZ"
struct.pack_into("<Q", kernel, 8, 0x200000)     # text_offset = 0x200000 (not 0!)
struct.pack_into("<I", kernel, 60, 0x40)        # pe_offset

with open(path, "wb") as f:
    f.write(kernel)

print(f"patched {path}: code0={hex(struct.unpack_from('<I', kernel, 0)[0])} "
      f"text_offset={hex(struct.unpack_from('<Q', kernel, 8)[0])} "
      f"pe_offset={hex(struct.unpack_from('<I', kernel, 60)[0])}")
