#!/system/bin/sh
# sweetmem client: one open fd for command + result.
exec 3<>/proc/sweetmem
echo "$*" >&3
cat <&3
