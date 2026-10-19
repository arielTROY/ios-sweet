#!/usr/bin/env python3
"""Copy a boot.img v4 and set its header cmdline (offset 44, 1536 bytes).
ABL appends it to the vendor_boot/bootconfig command line, so later
parameters (e.g. kvm-arm.mode=none) override earlier ones.
Usage: mkcmdlineboot.py IN OUT "cmdline"
"""
import struct, sys
src, dst, cmd = sys.argv[1:4]
b = bytearray(open(src, "rb").read())
assert b[:8] == b"ANDROID!" and struct.unpack_from("<I", b, 40)[0] == 4
c = cmd.encode()
assert len(c) < 1536
b[44:44 + 1536] = c.ljust(1536, b"\0")
open(dst, "wb").write(b)
print(f"{dst}: cmdline {cmd!r}")
