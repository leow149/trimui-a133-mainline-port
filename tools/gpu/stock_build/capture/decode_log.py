#!/usr/bin/env python3
# Decode $PVRCAP_DIR/ioctl.log: python3 decode_log.py ioctl.log [--hex]
import struct, sys
KIND = {1: "IN struct", 2: "OUT struct", 3: "IN L1 ptr", 4: "IN L2 ptr", 5: "OUT L1 ptr", 6: "OUT L2 ptr"}
d = open(sys.argv[1], "rb").read(); o = 0; hexd = "--hex" in sys.argv
while o + 56 <= len(d):
    mg, k, sq, fd, a, b, n = struct.unpack_from("<7Q", d, o); o += 56
    fd = struct.unpack("<q", struct.pack("<Q", fd))[0]
    data = d[o:o + n]; o += n
    if k in (1, 2):
        extra = "req=0x%x%s" % (a, " ret=%d" % struct.unpack("<q", struct.pack("<Q", b))[0] if k == 2 else "")
    elif k in (3, 5): extra = "ptr=0x%x @struct+0x%x" % (a, b)
    else: extra = "ptr=0x%x @L1+0x%x @L2+0x%x" % (a, b >> 32, b & 0xffffffff)
    print("#%d fd=%d %s %s len=%d" % (sq, fd, KIND.get(k, k), extra, n))
    if hexd:
        for i in range(0, n, 16): print("   %04x  %s" % (i, data[i:i+16].hex(" ")))
