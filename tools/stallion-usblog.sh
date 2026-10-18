#!/bin/bash
# Live log from the stallion shim's USB CDC-ACM port (shim/usblog.c, built
# with SHIM_DEFS=-DSHIM_USBLOG). Waits for /dev/cu.usbmodem* to appear,
# copies it to the terminal and to logs/stallion-usblog-<time>.log, and
# goes back to waiting when the port disappears (phone reset / next run).
# Nothing to install: macOS binds AppleUSBACM to the port by itself.
#
# Usage: tools/stallion-usblog.sh [max-wait-seconds-per-attach (default: forever)]
#        USBLOG_DEV=/dev/cu.usbmodemXYZ tools/stallion-usblog.sh   (pin a port)
set -uo pipefail
cd "$(dirname "$0")/.."
mkdir -p logs
OUT="logs/stallion-usblog-$(date +%Y%m%d-%H%M%S).log"
MAXWAIT="${1:-0}"

say() { echo "[usblog $(date +%H:%M:%S)] $*" >&2; }

# Ports that already exist (other gadgets) are skipped unless pinned.
preexisting=" $(ls /dev/cu.usbmodem* 2>/dev/null | tr '\n' ' ') "

find_port() {
	if [ -n "${USBLOG_DEV:-}" ]; then
		[ -e "$USBLOG_DEV" ] && echo "$USBLOG_DEV"
		return
	fi
	local p
	for p in $(ls -t /dev/cu.usbmodem* 2>/dev/null); do
		case "$preexisting" in *" $p "*) continue ;; esac
		echo "$p"
		return
	done
}

say "logging to $OUT (Ctrl-C to stop)"
[ "$preexisting" != "  " ] && say "ignoring ports already present:$preexisting"
while :; do
	say "waiting for the shim's /dev/cu.usbmodem* ..."
	waited=0
	port=""
	while [ -z "$port" ]; do
		port=$(find_port)
		[ -n "$port" ] && break
		sleep 0.2
		waited=$((waited + 1))
		if [ "$MAXWAIT" -gt 0 ] && [ "$waited" -ge $((MAXWAIT * 5)) ]; then
			say "no port after ${MAXWAIT}s"
			exit 1
		fi
	done
	say "attached: $port"
	echo "==== $(date '+%F %T') attached $port ====" >> "$OUT"
	# raw: no echo back to the device, no CR/LF mangling; opening the port
	# raises DTR, which tells the shim to start sending.
	stty -f "$port" raw -echo 115200 2>/dev/null || true
	cat "$port" | tee -a "$OUT"
	say "port gone ($port)"
	echo "==== $(date '+%F %T') detached ====" >> "$OUT"
	# A re-enumerated shim is a new port, not a pre-existing one.
	preexisting=" "
done
