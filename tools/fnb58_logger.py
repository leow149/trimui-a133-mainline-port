#!/usr/bin/env python3
"""Continuous FNIRSI FNB-58 logger: open the meter ONCE and keep streaming.

Usage: fnb58_logger.py LOGFILE [/dev/hidrawN]
Appends one CSV line per second: unix_time,volts,milliamps,milliwatts,samples
Read it with tail/awk instead of re-opening the meter: repeatedly opening,
re-initialising and closing the meter (tools/fnb58_read.py in a loop) made its
OUT endpoint time out, after which usbhid reset it and it stayed dead until
replugged (2026-11-08, twice).

Protocol as in tools/fnb58_read.py. On a write error the logger logs it and
exits instead of retrying, so it never hammers a wedged meter.
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
    return b"\x00" + bytes([0xAA, cmd]) + b"\x00" * 61 + bytes([crc])

def main():
    logpath = sys.argv[1]
    dev = sys.argv[2] if len(sys.argv) > 2 else find_dev()
    log = open(logpath, "a", buffering=1)
    fd = os.open(dev, os.O_RDWR)
    try:
        for c, k in ((0x81, 0x8E), (0x82, 0x96), (0x82, 0x96)):
            os.write(fd, pkt(c, k))
        next_keep = time.time()
        bucket_v, bucket_i = [], []
        bstart = time.time()
        while True:
            now = time.time()
            if now >= next_keep:
                os.write(fd, pkt(0x83, 0x9E))
                next_keep = now + 1
            r, _, _ = select.select([fd], [], [], 0.2)
            if r:
                d = os.read(fd, 64)
                if len(d) >= 62 and d[1] == 0x04:
                    for s in range(4):
                        o = 2 + 15 * s
                        bucket_v.append(int.from_bytes(d[o:o + 4], "little") / 1e5)
                        bucket_i.append(int.from_bytes(d[o + 4:o + 8], "little") / 1e5)
            if time.time() - bstart >= 1:
                if bucket_v:
                    v = sum(bucket_v) / len(bucket_v)
                    i = sum(bucket_i) / len(bucket_i) * 1000
                    log.write(f"{time.time():.1f},{v:.3f},{i:.1f},{v * i:.1f},{len(bucket_v)}\n")
                bucket_v, bucket_i = [], []
                bstart = time.time()
    except OSError as e:
        log.write(f"# {time.time():.1f} stopped: {e}\n")
        sys.exit(1)

if __name__ == "__main__":
    main()
