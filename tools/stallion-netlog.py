#!/usr/bin/env python3
"""Receive the XNU live console (UDP :5140, broadcast from the phone's
USB Ethernet, xnu 0077/0085/0086) and append it to a log file + stdout.
Usage: stallion-netlog.py OUTFILE [seconds]"""
import socket, sys, time
out = open(sys.argv[1], "ab", buffering=0)
end = time.time() + float(sys.argv[2]) if len(sys.argv) > 2 else None
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
s.bind(("", 5140))
s.settimeout(1.0)
while end is None or time.time() < end:
    try:
        data, _ = s.recvfrom(65536)
    except socket.timeout:
        continue
    out.write(data)
    sys.stdout.buffer.write(data)
    sys.stdout.flush()
