#!/bin/bash
# Live XNU console from the phone's USB Ethernet (xnu 0077/0084/0085): the
# kernel sends its log as UDP to port 5140; with sweetlogdst=0xffffffff it
# broadcasts, so this works on a self-assigned macOS interface without
# admin rights. Saves to logs/xnu-net-<time>.log until killed or MAXS.
# Usage: tools/stallion-netlog.sh [max-seconds]
cd "$(dirname "$0")/.."
mkdir -p logs
OUT="logs/xnu-net-$(date +%Y%m%d-%H%M%S).log"
echo "[netlog] UDP :5140 -> $OUT" >&2
if [ -n "${1:-}" ]; then
	timeout "$1" nc -ul 5140 | tee "$OUT"
else
	nc -ul 5140 | tee "$OUT"
fi
