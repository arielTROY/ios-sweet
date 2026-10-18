#!/usr/bin/env python3
"""Build a stallion boot.img that the Pixel ABL will actually launch.

ABL drops bare boot.img v4 images (no AVB footer) before jumping to the
kernel; a stock boot.img whose kernel bytes are swapped for ours runs
(measured 2026-10-18). So: take the stock boot.img, replace the kernel
area with our Image, patch kernel_size, zero the rest of the old kernel
and keep everything else (header, AVB footer) as is.

Usage: mkhybridboot.py STOCK_BOOT_IMG IMAGE OUT
"""
import struct
import sys

PAGE = 4096


def main():
    stock, image, out = sys.argv[1:4]
    st = bytearray(open(stock, "rb").read())
    if st[:8] != b"ANDROID!" or struct.unpack_from("<I", st, 40)[0] != 4:
        sys.exit("stock image is not a boot.img v4")
    if st[-64:-60] != b"AVBf":
        sys.exit("stock image has no AVB footer")
    ksz = struct.unpack_from("<I", st, 8)[0]
    k = open(image, "rb").read()
    if len(k) > ksz:
        sys.exit(f"Image {len(k)} bytes > stock kernel area {ksz}")
    st[PAGE:PAGE + ksz] = k + b"\0" * (ksz - len(k))
    struct.pack_into("<I", st, 8, len(k))
    open(out, "wb").write(st)
    print(f"{out}: kernel {len(k)} in stock area {ksz}, total {len(st)}")


if __name__ == "__main__":
    main()
