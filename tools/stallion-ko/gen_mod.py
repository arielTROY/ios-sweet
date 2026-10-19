#!/usr/bin/env python3
# Generate the modpost-equivalent <mod>.mod.c for an out-of-tree arm64 GKI
# module: the .gnu.linkonce.this_module struct and the __versions (modversion)
# table, filled with symbol CRCs harvested from the device's own vendor_dlkm
# modules (ground truth for this kernel's KMI).
#
# Usage:
#   gen_mod.py --syms syms.txt --crcmap crcmap.json --name sweetmem \
#              --out sweetmem.mod.c
#
# syms.txt   : one undefined symbol name per line (from `llvm-nm -u obj.o`)
# crcmap.json: { "symbol": "0x<64-bit crc>", ... }
#
# modpost always adds a __versions entry for `module_layout` (the struct-module
# ABI guard); we do the same. Every symbol must resolve to a CRC or we abort,
# because a missing/forced CRC is exactly what makes a module fail to load.

import argparse, json, sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--syms", required=True)
    ap.add_argument("--crcmap", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--vermagic", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    with open(args.crcmap) as f:
        crc = json.load(f)

    syms = []
    with open(args.syms) as f:
        for line in f:
            s = line.strip()
            if s:
                syms.append(s)

    # module_layout is always imported by any module (struct module ABI).
    if "module_layout" not in syms:
        syms.append("module_layout")

    # stable, deterministic order
    syms = sorted(set(syms))

    missing = [s for s in syms if s not in crc]
    if missing:
        sys.stderr.write("FATAL: no vendor CRC for: %s\n" % ", ".join(missing))
        sys.exit(1)

    entries = []
    for s in syms:
        c = int(crc[s], 16)
        # name[] is 56 bytes; modpost rejects longer names too.
        if len(s) >= 56:
            sys.stderr.write("FATAL: symbol name too long: %s\n" % s)
            sys.exit(1)
        entries.append('\t{ 0x%08x, "%s" },' % (c & 0xffffffffffffffff, s))

    tmpl = """\
#include <linux/module.h>
#include <linux/compiler.h>

/* defined in the main object (aliases of the module init/exit functions) */
extern int init_module(void);
extern void cleanup_module(void);

/* vermagic copied verbatim from the device's vendor_dlkm modules. With
 * CONFIG_MODVERSIONS the kernel's same_magic() ignores the first token (the
 * release/scmversion) and compares only this flag suffix, relying on the
 * __versions CRCs below for ABI compatibility. */
MODULE_INFO(vermagic, "__VERMAGIC__");
MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

static const struct modversion_info ____versions[]
__used __section("__versions") = {
__ENTRIES__
};

MODULE_INFO(depends, "");
"""
    body = (tmpl
            .replace("__VERMAGIC__", args.vermagic)
            .replace("__ENTRIES__", "\n".join(entries)))

    with open(args.out, "w") as f:
        f.write(body)

    sys.stderr.write("gen_mod: wrote %s with %d __versions entries\n"
                     % (args.out, len(entries)))


if __name__ == "__main__":
    main()
