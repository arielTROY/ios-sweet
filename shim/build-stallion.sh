#!/bin/bash
# Build the shim for Pixel 10a (stallion) and wrap it in a boot.img v4.
# Pixel's ABL ignores the ramdisk in `fastboot boot`, so the XNU payload
# (tools/mkpayload.py output, optional $1) is appended to the Image itself;
# the loader finds it by scanning after _end for the SWEETXNU magic.
set -euo pipefail
cd "$(dirname "$0")"
CC=aarch64-elf-gcc
OBJCOPY=aarch64-elf-objcopy
CFLAGS="-DTARGET_STALLION ${SHIM_DEFS:-} -O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
mkdir -p out-st
for f in start.S shim.c console.c mmu.c fdt.c loader.c ufs.c log.c; do
	$CC $CFLAGS -c $f -o out-st/${f%.*}.o
done
abs=$(aarch64-elf-readelf -r out-st/shim.o out-st/console.o out-st/mmu.o out-st/fdt.o out-st/loader.o out-st/ufs.o out-st/log.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out-st/start.o out-st/shim.o out-st/console.o out-st/mmu.o out-st/fdt.o out-st/loader.o out-st/ufs.o out-st/log.o -o out-st/shim.elf
$OBJCOPY -O binary out-st/shim.elf out-st/Image
size=$(aarch64-elf-nm out-st/shim.elf | awk '$3=="_end"{print $1}' | python3 -c "import sys;print(int(sys.stdin.read().strip(),16))")
truncate -s "$size" out-st/Image
if [ -n "${1:-}" ]; then
	# 16-align the append point (loader scans in 8-byte steps after _end)
	pad=$(( (8 - size % 8) % 8 ))
	[ "$pad" = 0 ] || dd if=/dev/zero bs=1 count="$pad" >> out-st/Image 2>/dev/null
	cat "$1" >> out-st/Image
fi
# ABL only launches images that carry the stock AVB footer: wrap our
# Image in the stock boot.img (research/stallion-fw/boot.img).
python3 ../tools/mkhybridboot.py ../research/stallion-fw/boot.img out-st/Image out-st/shim-boot-st.img
ls -l out-st/Image out-st/shim-boot-st.img
