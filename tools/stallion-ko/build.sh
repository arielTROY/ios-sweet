#!/usr/bin/env bash
# Reproducible offline build of sweetmem.ko for the Pixel 10a running kernel
#   6.1.145-android14-11-gc1de4747ac59-ab14219743  (arm64 GKI)
#
# Approach (a): compile against the running kernel's own headers (extracted
# from its kheaders.ko) with Homebrew LLVM clang, mirroring Kbuild's arm64 GKI
# flags (kCFI, SCS-reserved x18, PAC+BTI branch protection, general-regs-only),
# then hand-assemble the module the way Kbuild's modpost would:
#   - compile sweetmem.c                -> sweetmem.o
#   - generate sweetmem.mod.c (this_module struct + __versions table, the
#     __versions CRCs taken from the device's own vendor_dlkm modules, which
#     are ground truth for this kernel's KMI) and compile it -> sweetmem.mod.o
#   - link `ld.lld -r` the two objects  -> out/sweetmem.ko
#
# No Android/Linux toolchain and no kernel tree are required; everything is
# driven from kheaders.ko + vendor_dlkm.tar. Nothing is signed: the module
# loads unsigned (tainting) because the kernel is MODULE_SIG but not SIG_FORCE.
#
# Usage: tools/stallion-ko/build.sh
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
IN="${SWEETMEM_INPUTS:-$REPO/research/stallion-ko}"
OUT="$HERE/out"
WORK="$OUT/work"
MODNAME=${MODNAME:-sweetmem}

# --- toolchain ------------------------------------------------------------
LLVM_BIN="${LLVM_BIN:-/opt/homebrew/Cellar/llvm@20/20.1.8/bin}"
LLD_BIN="${LLD_BIN:-/opt/homebrew/Cellar/lld@20/20.1.8/bin}"
CLANG="$LLVM_BIN/clang"
NM="$LLVM_BIN/llvm-nm"
OBJCOPY="$LLVM_BIN/llvm-objcopy"
READELF="$LLVM_BIN/llvm-readelf"
LD="$LLD_BIN/ld.lld"
[ -x "$CLANG" ] || { echo "no clang at $CLANG (brew install llvm@20)"; exit 1; }
[ -x "$LD" ]    || { LD="$LLVM_BIN/ld.lld"; }
[ -x "$LD" ]    || { echo "no ld.lld (brew install lld@20)"; exit 1; }

mkdir -p "$WORK"

# --- 1. extract the running kernel's headers from kheaders.ko -------------
T="$WORK/headers"
if [ ! -f "$T/include/generated/autoconf.h" ]; then
	echo ">> extracting kernel headers from kheaders.ko"
	rm -rf "$T"; mkdir -p "$T"
	# The embedded kheaders.tar.xz is the module's entire .rodata section:
	# symbol kernel_headers_data sits at .rodata+0 and kernel_headers_data_end
	# equals the section size, so dumping .rodata yields exactly the tarball.
	"$OBJCOPY" --dump-section .rodata="$WORK/kh.tar.xz" "$IN/kheaders.ko" /dev/null
	xz -dc "$WORK/kh.tar.xz" | tar -x -C "$T"
	rm -f "$WORK/kh.tar.xz"
fi
[ -f "$T/include/generated/autoconf.h" ] || { echo "header extraction failed"; exit 1; }

# --- 2. harvest symbol CRCs from vendor_dlkm modules ----------------------
CRCMAP="$WORK/crcmap.json"
echo ">> harvesting __versions CRCs from vendor_dlkm.tar"
VDIR="$WORK/vendor"
rm -rf "$VDIR"; mkdir -p "$VDIR"
tar -xf "$IN/vendor_dlkm.tar" -C "$VDIR"
VERS="$WORK/vers"; rm -rf "$VERS"; mkdir -p "$VERS"
while IFS= read -r ko; do
	b="$(basename "$ko" .ko)"
	"$OBJCOPY" --dump-section __versions="$VERS/$b.bin" "$ko" /dev/null 2>/dev/null || true
done < <(find "$VDIR" -name '*.ko')
python3 -I "$HERE/harvest_crcs.py" "$VERS" "$CRCMAP"

# vermagic: copy verbatim from a vendor module (proven to load on this kernel).
# With CONFIG_MODVERSIONS the kernel's same_magic() ignores the first token
# (the release/scmversion) and compares only the flag suffix, relying on the
# __versions CRCs for ABI; so the vendor string is the safe, correct choice.
VENDOR_KO="$(find "$VDIR" -name '*.ko' | head -1)"
"$OBJCOPY" --dump-section .modinfo="$WORK/vmi.bin" "$VENDOR_KO" /dev/null 2>/dev/null
VERMAGIC="$(tr '\0' '\n' < "$WORK/vmi.bin" | sed -n 's/^vermagic=//p')"
echo ">> vermagic: $VERMAGIC"

# --- 3. compile flags (mirror arm64 GKI Kbuild) ---------------------------
RES="$("$CLANG" -print-resource-dir)"
CFLAGS=(
	-target aarch64-linux-gnu -std=gnu11 -O2
	-fsanitize=kcfi                 # CONFIG_CFI_CLANG (kCFI type-id prologues)
	-mbranch-protection=standard    # CONFIG_ARM64_BTI + PTR_AUTH (paciasp/bti)
	-ffixed-x18                     # CONFIG_SHADOW_CALL_STACK reserves x18
	-mgeneral-regs-only             # no FP/SIMD in kernel code
	-mcmodel=small -mno-outline-atomics
	-fno-pie -fno-common -fno-stack-protector
	-fno-asynchronous-unwind-tables -fno-unwind-tables
	-Wall -Wundef -Werror=implicit-function-declaration
	-nostdinc -isystem "$RES/include"
	-I"$T/arch/arm64/include" -I"$T/arch/arm64/include/generated"
	-I"$T/include" -I"$T/include/generated"
	-I"$T/arch/arm64/include/uapi" -I"$T/arch/arm64/include/generated/uapi"
	-I"$T/include/uapi" -I"$T/include/generated/uapi"
	-include "$T/include/linux/kconfig.h"
	-include "$T/include/linux/compiler-version.h"
	-D__KERNEL__ -DMODULE
	-DKBUILD_MODNAME="\"$MODNAME\""
	-DKBUILD_BASENAME="\"$MODNAME\""
	-DKBUILD_MODFILE="\"$MODNAME\""
)

# --- 4. compile the module object -----------------------------------------
echo ">> compiling $MODNAME.c"
"$CLANG" "${CFLAGS[@]}" -c "$HERE/$MODNAME.c" -o "$WORK/$MODNAME.o"

# --- 5. generate + compile the modpost-equivalent object ------------------
echo ">> generating $MODNAME.mod.c"
"$NM" -u "$WORK/$MODNAME.o" | awk '{print $NF}' > "$WORK/syms.txt"
python3 -I "$HERE/gen_mod.py" \
	--syms "$WORK/syms.txt" --crcmap "$CRCMAP" \
	--name "$MODNAME" --vermagic "$VERMAGIC" --out "$WORK/$MODNAME.mod.c"
"$CLANG" "${CFLAGS[@]}" -c "$WORK/$MODNAME.mod.c" -o "$WORK/$MODNAME.mod.o"

# --- 6. link the relocatable module ---------------------------------------
echo ">> linking $MODNAME.ko"
"$LD" -r -T "$HERE/module.lds" -o "$OUT/$MODNAME.ko" "$WORK/$MODNAME.o" "$WORK/$MODNAME.mod.o"

echo ">> built: $OUT/$MODNAME.ko"
"$READELF" -h "$OUT/$MODNAME.ko" | sed -n '2,6p' || true
echo ">> insmod it on device (as root) with:  insmod /data/local/tmp/$MODNAME.ko"
