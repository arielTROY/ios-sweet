#!/bin/bash
# One unattended stallion boot experiment.
#   1. get the phone into fastboot (adb reboot bootloader if in Android)
#   2. fastboot boot IMAGE (nothing flashed)
#   3. time how long until it is back on adb or fastboot (ABL's armed
#      watchdog recovers hangs; boot_a must be stock, see NOTES.md)
#   4. save the bootloader log of the next fastboot session, whose [PRE]
#      lines summarize the reset that ended our run
# Usage: tools/stallion-try.sh IMAGE [LABEL]
set -uo pipefail
cd "$(dirname "$0")/.."
IMG=$1
LABEL=${2:-$(basename "$IMG" .img)}
OUT=logs/st-try; mkdir -p "$OUT"
say() { echo "[try $(date +%H:%M:%S)] $*"; }

to_fastboot() {
	fastboot devices | grep -q fastboot && return 0
	adb wait-for-device; adb reboot bootloader
	for i in $(seq 90); do fastboot devices | grep -q fastboot && return 0; sleep 1; done
	return 1
}

to_fastboot || { say "no fastboot"; exit 1; }
t0=$(date +%s)
fastboot boot "$IMG" >/dev/null 2>&1
sleep 3
where=none
for i in $(seq 1 480); do
	if adb devices | grep -qw device; then where=adb; break; fi
	if fastboot devices | grep -q fastboot; then where=fastboot; break; fi
	sleep 1
done
dt=$(( $(date +%s) - t0 ))
say "$LABEL: back on $where after ${dt}s"
to_fastboot && fastboot oem dmesg > "$OUT/$LABEL.txt" 2>&1
grep -E "^\[PRE\] (reset message|RST_STAT|Reboot reason|Reboot mode)|Last AVB: avb_error_parts" "$OUT/$LABEL.txt"
echo "$(date +%F_%T) $LABEL $where ${dt}s" >> "$OUT/summary.txt"
