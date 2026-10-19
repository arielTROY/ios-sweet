#!/bin/bash
# One XNU boot on stallion, launched from Linux (Path B):
#   ADT (mkadt --target stallion, RAM window 0x8c0000000 + 3.75 GiB)
#   + kernel -> payload (mkpayload) ; shim with USB log ; sweetjump go
# Logs: shim over CDC-ACM (tools/stallion-usblog.sh), XNU over USB
# Ethernet UDP broadcast (tools/stallion-netlog.sh).
# Usage: tools/stallion-xnu-run.sh [kernel] [extra boot-args]
set -uo pipefail
cd "$(dirname "$0")/.."
KERNEL=${1:-xnu/out-st/kernel.development.sweet}
EXTRA=${2:-}
O=shim/out-st
python3 tools/mkadt.py $O/xnu.adt --target stallion --mem 0x8c0000000:0xf0000000 --panic-reset 5000 2>/dev/null || exit 1
python3 tools/mkpayload.py --cmdline "debug=0x8 serial=3 sweetlogdst=0xa9feffff sweetlinklocal=1 cpus=1 -v -noprogress $EXTRA" \
	"$KERNEL" $O/xnu.adt $O/payload-xnu.bin >/dev/null || exit 1
SHIM_DEFS="-DSHIM_USBLOG -DNO_FDT_DUMP -DSENTINEL_S=120 -DSENTINEL_XNU_S=150" \
	bash shim/build-stallion.sh >/dev/null 2>&1 || exit 1
cp $O/Image $O/Image-xnu
pkill -f stallion-usblog.sh; pkill -f "nc -ul 5140"; pkill -f stallion-netlog.py
tools/stallion-usblog.sh 900 > logs/xnu-usblog-listener.txt 2>&1 &
python3 tools/stallion-netlog.py logs/xnu-net-$(date +%Y%m%d-%H%M%S).log 900 > /dev/null 2>logs/xnu-netlog-listener.txt &
sleep 1
tools/stallion-linuxjump.sh $O/Image-xnu $O/payload-xnu.bin
