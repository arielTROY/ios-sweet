#!/bin/bash
# Run a SHIM_DIAG shim as a subroutine of Linux and print its RAM log.
#   - boots the stock kernel with kvm-arm.mode=none if not already on it
#   - root: insmod sweetjump.ko, offline CPUs 1-7, stage IMAGE,
#     "go DIAG_MAGIC", online the CPUs again, print the sweetdiag log
# Usage: tools/stallion-diag.sh IMAGE [outfile]
# For diag images that reset the USB controller, adb over Wi-Fi is used
# (set PHONE_IP, default 192.168.0.173).
set -uo pipefail
cd "$(dirname "$0")/.."
IMG=$1
OUTF=${2:-logs/diag-$(date +%Y%m%d-%H%M%S).txt}
SU=/debug_ramdisk/su
NOKVM=shim/out-st/stock-nokvm.img
MAGIC=575349444741	# "SWDIAG", see start.S
mkdir -p logs
[ -f "$NOKVM" ] || python3 tools/mkcmdlineboot.py research/stallion-fw/boot.img "$NOKVM" "kvm-arm.mode=none"

if ! adb shell cat /proc/cmdline 2>/dev/null | grep -q "kvm-arm.mode=none"; then
	adb get-state >/dev/null 2>&1 && adb reboot bootloader
	until fastboot devices | grep -q .; do sleep 1; done
	fastboot boot "$NOKVM" >/dev/null 2>&1
	adb wait-for-device
fi
until [ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; do sleep 1; done
until adb shell "$SU -c id" 2>/dev/null | grep -q uid=0; do sleep 1; done
B=$(basename "$IMG")
adb push -q tools/stallion-ko/out/sweetjump.ko "$IMG" /data/local/tmp/ || exit 1
# Run detached on the phone so a USB reset during the test doesn't kill it;
# results land in a file we fetch afterwards (over USB or Wi-Fi).
adb shell "$SU -c 'rmmod sweetjump 2>/dev/null; insmod /data/local/tmp/sweetjump.ko; dmesg -c >/dev/null;
  nohup sh -c \"for c in 1 2 3 4 5 6 7; do echo 0 > /sys/devices/system/cpu/cpu\\\$c/online; done;
  cat /data/local/tmp/$B > /proc/sweetjump; echo go $MAGIC > /proc/sweetjump;
  for c in 1 2 3 4 5 6 7; do echo 1 > /sys/devices/system/cpu/cpu\\\$c/online; done;
  dmesg | grep -E sweet > /data/local/tmp/diag-out.txt\" >/dev/null 2>&1 &'"
sleep "${DIAG_WAIT:-8}"
for i in $(seq 1 60); do
	adb shell "$SU -c 'cat /data/local/tmp/diag-out.txt'" > "$OUTF" 2>/dev/null && [ -s "$OUTF" ] && break
	sleep 2
done
cat "$OUTF"
