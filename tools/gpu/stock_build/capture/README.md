# PowerVR stock-driver capture (libpvrcap.so + vkxfer_stock)

Build (laptop, no device): `./build_capture.sh` (uses the stock rootfs, glibc <= 2.33). Outputs `libpvrcap.so`, `vkxfer_stock`.

Device-side (stock firmware), copy both files (e.g. to /root):

    LD_PRELOAD=/root/libpvrcap.so PVRCAP_DIR=/tmp/pvrcap VKXFER_NOFORK=1 ./vkxfer_stock rt_rgba8_3x1

- `VKXFER_NOFORK=1` runs only the first case matching the substring, in-process. Without it each case forks (the preload still works, but `ioctl.log` then mixes pids).
- After the final vkWaitForFences of the case, `vkxfer_stock` calls `pvrcap_dump_now(<test name>)` (via dlsym; no-op without the preload).
- `PVRCAP_DIR` defaults to /tmp/pvrcap. Delete it between runs (ioctl.log is opened O_APPEND).

## Outputs
- `<tag>_maps.txt`: `index cpu_addr length mmap_offset device bytes_readable`; `<tag>_map_<index>.bin`: contents of each recorded GPU mapping (faulting pages zero-filled).
- `ioctl.log`: only ioctls on fds whose /proc/self/fd link is /dev/dri/* or /dev/pvrsrvkm. Binary, little endian, records of a 56-byte header
  (7 x u64: magic "VRCAPv1", kind, seq, fd, a, b, len) followed by `len` bytes. Kinds:
  1 = arg struct before the call (a=req), 2 = arg struct after (a=req, b=return value or -errno),
  3/5 = level-1 buffer (up to 4096 B) behind a readable pointer found at struct offset b (a = pointer value; 3 before, 5 after),
  4/6 = level-2 buffer behind a pointer found inside the level-1 buffer (b = (L1 struct offset << 32) | offset in L1 buffer).
  The arg struct is _IOC_SIZE(req) bytes (cap 4096). Pointer detection: aligned u64 in [0x10000, 2^48) readable via write() to a pipe (EFAULT-safe). Caps per ioctl: 64 L1 and 256 L2 buffers.
- Decode: `python3 decode_log.py /tmp/pvrcap/ioctl.log --hex | less`. Match `seq` between in/out records.

Limits: mapping table 4096 entries; no locking against fork children; a munmap punching a hole in the middle of a mapping keeps only the head.
