#!/bin/bash
# Boot IMAGE once (fastboot boot, nothing flashed), wait for Android, load
# sweetmem.ko and run the given sweetmem commands. Usage:
#   tools/stallion-run-read.sh IMAGE "r 9c0000000 10" "rl 1121c120 1" ...
set -uo pipefail
cd "$(dirname "$0")/.."
IMG=$1; shift
SU=/debug_ramdisk/su
adb get-state >/dev/null 2>&1 && adb reboot bootloader
until fastboot devices | grep -q .; do sleep 1; done
t0=$(date +%s)
fastboot boot "$IMG" >/dev/null 2>&1
sleep 3
until adb devices | grep -qw device; do
	fastboot devices | grep -q . && { echo "landed in fastboot"; fastboot reboot >/dev/null 2>&1; }
	sleep 1
done
until [ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; do sleep 1; done
echo "back in Android after $(( $(date +%s) - t0 ))s"
until adb shell "$SU -c id" 2>/dev/null | grep -q uid=0; do sleep 1; done
adb push -q tools/stallion-ko/out/sweetmem.ko tools/stallion-ko/sm.sh /data/local/tmp/
adb shell "$SU -c 'cp /data/local/tmp/sm.sh /data/local/tmp/sm; chmod 755 /data/local/tmp/sm; grep -q sweetmem /proc/modules || insmod /data/local/tmp/sweetmem.ko'"
for c in "$@"; do
	echo "== $c"
	adb exec-out "$SU -c '/data/local/tmp/sm $c'"
	echo
done
