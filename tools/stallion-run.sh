#!/bin/bash
# Autonomous stallion bring-up loop for ios-sweet.
#
# One run:
#   1. wait for the phone on adb, pull Android-side recon (/proc/iomem,
#      /proc/cmdline, /proc/device-tree, pstore leftovers)
#   2. adb reboot bootloader -> fastboot boot <image>  (temporary, nothing flashed)
#   3. the shim runs its probes, writes its log into the ramoops console prz
#      @0xfd3ff000, then PSCI-resets back into Android
#   4. wait for adb again, pull /sys/fs/pstore/console-ramoops-0
#
# Nothing is flashed: `fastboot boot` only. Safe to run unattended.
# Usage: tools/stallion-run.sh [boot-image] [outdir]
set -uo pipefail
cd "$(dirname "$0")/.."

IMG="${1:-shim/out-st/shim-boot-st.img}"
OUT="${2:-logs/stallion-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT"

say() { echo "[stallion-run $(date +%H:%M:%S)] $*"; }

wait_adb() {
	say "waiting for adb..."
	adb wait-for-device
	adb get-state >/dev/null
	say "adb up: $(adb shell getprop ro.product.device 2>/dev/null | tr -d '\r')"
}

wait_fastboot() {
	say "waiting for fastboot..."
	for i in $(seq 1 120); do
		fastboot devices | grep -q fastboot && return 0
		sleep 1
	done
	say "ERROR: no fastboot device after 120s"
	return 1
}

pull_recon() {
	say "pulling android-side recon..."
	adb shell "cat /proc/cmdline" > "$OUT/cmdline.txt" 2>/dev/null
	adb shell "cat /proc/iomem" > "$OUT/iomem.txt" 2>/dev/null
	adb shell "uname -a" > "$OUT/uname.txt" 2>/dev/null
	adb shell "getprop ro.build.fingerprint; getprop ro.product.device; getprop ro.hardware" \
		> "$OUT/props.txt" 2>/dev/null
	# device tree the running kernel sees (tar through stdout; root not needed)
	adb shell "cd /proc/device-tree && tar cf - . 2>/dev/null" > "$OUT/device-tree.tar" 2>/dev/null
	# leftover pstore records
	adb shell "ls /sys/fs/pstore/ 2>/dev/null" > "$OUT/pstore-list.txt" 2>/dev/null
	for f in $(cat "$OUT/pstore-list.txt" | tr -d '\r'); do
		adb shell "cat /sys/fs/pstore/$f" > "$OUT/pstore-$f" 2>/dev/null
	done
	say "recon in $OUT"
}

pull_log() {
	say "pulling pstore..."
	adb shell "ls /sys/fs/pstore/" > "$OUT/pstore-list2.txt" 2>/dev/null
	for f in $(cat "$OUT/pstore-list2.txt" | tr -d '\r'); do
		adb shell "cat /sys/fs/pstore/$f" > "$OUT/shim-$f" 2>/dev/null
	done
	if [ -s "$OUT/shim-console-ramoops-0" ]; then
		say "=== shim log (first 60 lines) ==="
		head -60 "$OUT/shim-console-ramoops-0"
		say "=== full log: $OUT/shim-console-ramoops-0 ==="
	else
		say "WARN: no console-ramoops-0 record; shim log may not have landed"
		ls -l "$OUT"/shim-* 2>/dev/null
	fi
}

say "image: $IMG"
say "out:   $OUT"

# Phase 0: if the phone is already in fastboot, skip the adb recon.
if fastboot devices | grep -q fastboot; then
	say "device already in fastboot"
else
	wait_adb
	pull_recon
	say "rebooting to bootloader..."
	adb reboot bootloader
	wait_fastboot || exit 1
fi

say "fastboot boot $IMG"
fastboot boot "$IMG" 2>&1 | tail -5

# The shim resets into Android; give it a while (recon run is ~1-2 min).
sleep 10
wait_adb
sleep 5		# let pstore mount/appear
pull_log

say "done."
