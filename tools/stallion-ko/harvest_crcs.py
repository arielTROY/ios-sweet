#!/usr/bin/env python3
# Build a {symbol: "0x<crc>"} map from the __versions sections dumped out of the
# device's vendor_dlkm modules. Each __versions record is 64 bytes:
#   u64 little-endian crc  +  56-byte NUL-padded symbol name.
# These CRCs are the ground truth for this kernel's exported-symbol ABI.
# Aborts if any symbol carries conflicting CRCs across modules (never seen, but
# that would mean the modules came from different KMIs).

import glob, json, os, struct, sys


def main():
    vers_dir, out = sys.argv[1], sys.argv[2]
    crcs = {}
    for f in sorted(glob.glob(os.path.join(vers_dir, "*.bin"))):
        data = open(f, "rb").read()
        for i in range(len(data) // 64):
            rec = data[i * 64:(i + 1) * 64]
            crc = struct.unpack_from("<Q", rec, 0)[0]
            name = rec[8:].split(b"\x00")[0].decode("latin1")
            if not name:
                continue
            prev = crcs.get(name)
            if prev is not None and prev != crc:
                sys.stderr.write(
                    "FATAL: CRC conflict for %s: %#x vs %#x\n"
                    % (name, prev, crc))
                sys.exit(1)
            crcs[name] = crc
    with open(out, "w") as fh:
        json.dump({k: "0x%016x" % v for k, v in crcs.items()}, fh, indent=0)
    sys.stderr.write("harvest_crcs: %d symbols -> %s\n" % (len(crcs), out))


if __name__ == "__main__":
    main()
