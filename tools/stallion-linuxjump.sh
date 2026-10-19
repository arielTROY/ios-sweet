#!/bin/bash
# Start a bare-metal image from running Android (Linux-initialized hardware):
#   1. fastboot boot the stock kernel with kvm-arm.mode=none (nothing flashed)
#   2. root: offline CPUs 1-7, insmod sweetjump.ko, stage IMAGE, "go 8a000000"
# Usage: tools/stallion-linuxjump.sh IMAGE(raw arm64 Image, not boot.img)
set -uo pipefail
cd "$(dirname "$0")/.."
IMG=$1
PAYLOAD=${2:-}
SU=/debug_ramdisk/su
NOKVM=shim/out-st/stock-nokvm.img
[ -f "$NOKVM" ] || python3 tools/mkcmdlineboot.py research/stallion-fw/boot.img "$NOKVM" "kvm-arm.mode=none"
say() { echo "[ljump $(date +%H:%M:%S)] $*"; }

# Already on the no-KVM kernel? Then skip the reboot.
if ! adb shell cat /proc/cmdline 2>/dev/null | grep -q "kvm-arm.mode=none"; then
	adb get-state >/dev/null 2>&1 && adb reboot bootloader
	until fastboot devices | grep -q .; do sleep 1; done
	fastboot boot "$NOKVM" >/dev/null 2>&1
fi
adb wait-for-device
until [ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; do sleep 1; done
until adb shell "$SU -c id" 2>/dev/null | grep -q uid=0; do sleep 1; done
say "android (kvm off) up"
adb push -q tools/stallion-ko/out/sweetjump.ko "$IMG" /data/local/tmp/ || exit 1
PCMD=""
if [ -n "$PAYLOAD" ]; then
	adb push -q "$PAYLOAD" /data/local/tmp/payload.bin || exit 1
	PCMD="cat /data/local/tmp/payload.bin > /proc/sweetpayload;"
fi
B=$(basename "$IMG")
adb shell "$SU -c 'for c in 1 2 3 4 5 6 7; do echo 0 > /sys/devices/system/cpu/cpu\$c/online; done; cat /sys/devices/system/cpu/online; rmmod sweetjump 2>/dev/null; insmod /data/local/tmp/sweetjump.ko; $PCMD cat /data/local/tmp/$B > /proc/sweetjump && echo jumping && (echo \"go 8a000000\" > /proc/sweetjump &) ; sleep 1'" 2>&1 | tr -d '\r'
t0=$(date +%s)
say "jumped; waiting for the phone to come back"
sleep 5
until adb devices | grep -qw device || fastboot devices | grep -q .; do sleep 1; done
say "back after $(( $(date +%s) - t0 ))s: $(adb devices | grep -w device | cut -f2)$(fastboot devices | cut -f2)"
