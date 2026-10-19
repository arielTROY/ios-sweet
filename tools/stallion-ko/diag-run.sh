#!/system/bin/sh
# On-phone half of tools/stallion-diag.sh (runs as root, detached).
# $1 = image file in /data/local/tmp, $2 = x0 for the image (hex).
cd /data/local/tmp || exit 1
OUT=/data/local/tmp/diag-out.txt
rm -f $OUT
rmmod sweetjump 2>/dev/null
insmod /data/local/tmp/sweetjump.ko || { echo "insmod failed" > $OUT; exit 1; }
dmesg -c > /dev/null
for c in 1 2 3 4 5 6 7; do echo 0 > /sys/devices/system/cpu/cpu$c/online; done
cat "$1" > /proc/sweetjump
echo "go $2" > /proc/sweetjump
for c in 1 2 3 4 5 6 7; do echo 1 > /sys/devices/system/cpu/cpu$c/online; done
dmesg | grep -E "sweet" > $OUT.tmp
mv $OUT.tmp $OUT
# A diag that took over the USB controller leaves Linux's gadget dead:
# rebind the UDC so adb comes back.
echo "" > /config/usb_gadget/g1/UDC
sleep 1
echo 11210000.dwc3 > /config/usb_gadget/g1/UDC
