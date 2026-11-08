#!/usr/bin/env python3
"""Read voltage/current from a FNIRSI FNB-58 USB power meter over /dev/hidrawN.

# Do not call this in a loop: re-opening the meter quickly has wedged it. Use tools/fnb58_logger.py.
Usage: fnb58_read.py [seconds] [/dev/hidrawN]
Prints one line per second (mean of the samples in that second) and a summary.
Needs read/write access to the hidraw node (tools/install_fnb58_udev_rule.sh).
Protocol as in the open FNIRSI loggers: init aa81/aa82/aa82, keepalive aa83 ~1/s,
data packets type 0x04 with 4 samples of 15 bytes (V, I little-endian u32 / 1e5).
"""
import glob, os, select, sys, time

def find_dev():
    for h in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        try:
            if "00002E3C:00005558" in open(h + "/device/uevent").read():
                return "/dev/" + os.path.basename(h)
        except OSError:
            pass
    sys.exit("FNB-58 not found")

def pkt(cmd, crc):
    return b"\x00" + bytes([0xAA, cmd]) + b"\x00" * 61 + bytes([crc])  # report id 0 + 64 bytes

def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 5
    dev = sys.argv[2] if len(sys.argv) > 2 else find_dev()
    fd = os.open(dev, os.O_RDWR)
    for c, k in ((0x81, 0x8E), (0x82, 0x96), (0x82, 0x96)):
        os.write(fd, pkt(c, k))
    end = time.time() + secs
    next_keep = time.time() + 1
    bucket, allv, alli = [], [], []
    bstart = time.time()
    while time.time() < end:
        if time.time() >= next_keep:
            os.write(fd, pkt(0x83, 0x9E))
            next_keep += 1
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            d = os.read(fd, 64)
            if len(d) >= 62 and d[1] == 0x04:
                for i in range(4):
                    o = 2 + 15 * i
                    v = int.from_bytes(d[o:o + 4], "little") / 1e5
                    a = int.from_bytes(d[o + 4:o + 8], "little") / 1e5
                    bucket.append((v, a))
        if time.time() - bstart >= 1 and bucket:
            v = sum(x[0] for x in bucket) / len(bucket)
            a = sum(x[1] for x in bucket) / len(bucket)
            allv.append(v); alli.append(a)
            print(f"{v:6.3f} V  {a*1000:7.1f} mA  {v*a*1000:7.1f} mW  (n={len(bucket)})", flush=True)
            bucket, bstart = [], time.time()
    os.close(fd)
    if alli:
        v = sum(allv) / len(allv); a = sum(alli) / len(alli)
        print(f"mean: {v:.3f} V  {a*1000:.1f} mA  {v*a*1000:.0f} mW  "
              f"(min {min(alli)*1000:.1f} / max {max(alli)*1000:.1f} mA, {len(alli)} s)")

if __name__ == "__main__":
    main()
