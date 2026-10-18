#!/usr/bin/env python3
"""Pack a raw arm64 kernel Image (+ optional ramdisk) into an Android
boot.img v4 (GKI vendor format, struct boot_img_hdr_v4).

Layout (system/tools/mkbootimg bootimg.h):
  magic[8] "ANDROID!" | kernel_size | ramdisk_size | os_version |
  header_size | reserved[4] | header_version | cmdline[1536] |
  signature_size (v4 only)
then kernel, ramdisk, signature — each aligned to page_size (4096).
"""
import struct
import sys

def pack(kernel, ramdisk, out, cmdline=b"", page=4096):
    k = open(kernel, "rb").read()
    r = open(ramdisk, "rb").read() if ramdisk else b""
    os_version = 0x10 << 11 | 8 << 4 | 1   # 16.0.0, patch 2026-08-ish
    hdr_size = 8 + 4 * 9 + 1536 + 4
    hdr = struct.pack(
        "<8sIIIIIIIII1536sI",
        b"ANDROID!",
        len(k),            # kernel_size
        len(r),            # ramdisk_size
        os_version,
        hdr_size,          # header_size
        0, 0, 0, 0,        # reserved
        4,                 # header_version
        cmdline.ljust(1536, b"\0"),
        0,                 # signature_size
    )
    assert len(hdr) == hdr_size
    img = hdr.ljust(page, b"\0") + k
    img += b"\0" * (-len(img) % page) + r
    img += b"\0" * (-len(img) % page)
    open(out, "wb").write(img)
    print(f"{out}: kernel {len(k)} ramdisk {len(r)} total {len(img)}")

if __name__ == "__main__":
    pack(sys.argv[1], sys.argv[2] if sys.argv[2] != "-" else None, sys.argv[3])
