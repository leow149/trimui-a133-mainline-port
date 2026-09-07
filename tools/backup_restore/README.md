# TrimUI Smart Pro — stock eMMC backup + RAM dump

Captured 2026-08-22 from the live stock firmware over ADB, with the SD card physically
removed (so the device booted its own onboard eMMC).

Stock kernel build `#899` / `TRIMUIDEV@NUC` (Nov 28 2025), Linux 4.9.191, A133 / sun50iw10.

## What's here

| Path | Contents |
|---|---|
| `emmc_chunks/emmc_000.gz` .. `emmc_029.gz` | Full eMMC image, 7456MB raw in 30x256MB gzipped chunks (264MB on disk) |
| `ram_chunks/ram_00.gz` .. `ram_07.gz` | Full physical RAM, 1024MB raw in 8x128MB gzipped chunks (82MB on disk) |
| `backup_emmc.sh` / `backup_ram.sh` | The capture scripts (resumable) |
| `verify_emmc.sh` | Re-reads the device and compares md5 per region |
| `restore_emmc.sh` | Reassembly + guarded restore (read the warnings in it) |

## Verification status

**eMMC: VERIFIED.** `verify_emmc.sh` re-read all 7456MB off the device, md5'd each 256MB region
on-device, and compared against the decompressed chunks: **all 30 regions matched byte-for-byte.**
This is a real restore-grade backup, not just an archive that happens to decompress.

**RAM: not verifiable**, and inherently imperfect — see the caveat below.

## Reassembling

Chunks are independently gzipped, so plain concatenation of the decompressed stream works:

    zcat emmc_chunks/emmc_*.gz > stock_emmc_full.img     # 7456MB
    zcat ram_chunks/ram_*.gz  > stock_ram_full.bin       # 1024MB

(`emmc_*.gz` / `ram_*.gz` glob in the right order because of the zero-padded numbering.)

## eMMC partition layout (for reference)

From the stock boot cmdline; block devices are plain `/dev/mmcblk0pN` on this system
(also symlinked under `/dev/by-name/`), **not** under `/dev/block/`.

| # | Name | Size | Purpose |
|---|---|---|---|
| — | (raw, pre-p1) | ~20.5MB | boot0 + TOC0 + U-Boot + ATF + SCP bundle (sectors 0-41983) |
| p1 | bootloader | 24MB | FAT16 boot resources: `bootlogo.bmp`, `bat/*.bmp`, `magic.bin` |
| p2 | env | 512KB | U-Boot environment |
| p3 | env-redund | 512KB | redundant U-Boot environment |
| p4 | boot | 24MB | Android boot.img (kernel + ramdisk + dtb) |
| p5 | rootfs | 560MB | read-only base rootfs (`root=` in cmdline) |
| p6 | rootfs_data | 2GB | overlay / persistent data |
| p7 | private | 512KB | vendor private data |
| p8 | recovery | 16MB | recovery image |
| p9 | pstore | 512KB | kernel panic/oops persistent storage |
| p10 | UDISK | 4.6GB | user / game storage |

Note the eMMC hardware boot areas (`mmcblk0boot0`/`boot1`) are **all zero** — this design does
not use the standard eMMC boot-partition mechanism. The real boot chain lives in the raw
unpartitioned space before p1, same eGON-header mechanism as SD boot.

## Caveats — read before relying on these

**The eMMC image is a snapshot of a live, mounted, running system.** `rootfs_data` and `UDISK`
were mounted read-write while being read, so those filesystems may be captured mid-write. The
image is byte-exact for what was on the media, but expect a possible fsck on those two after a
restore. The regions that matter for recovery — the raw boot bundle, `bootloader`, `boot`,
`rootfs`, `env` — are read-only or inert at runtime and are clean.

**The RAM dump is not forensically clean, and cannot be on this device.** The staging file lives
in `/tmp`, which is tmpfs — i.e. RAM. So the capture perturbs the very memory it records, and its
own buffer pages appear in the image. There is no streaming transport available: this device's
adbd is an ancient Allwinner BSP build with no `exec:` service, so `adb exec-out` fails instantly
with `error: closed` and everything has to go through a device-side temp file. Chunks were kept
at 128MB to bound the footprint. Treat it as a best-effort snapshot.

**Why chunks staged in `/tmp` and not `/mnt/UDISK`:** UDISK lives on `mmcblk0p10`, i.e. on the
very device being imaged. Staging there would have modified the backup's own source.

## Restoring

See `restore_emmc.sh`. It is deliberately **not** a one-shot script — restoring to eMMC is the
one operation on this device with no safe fallback (unlike SD boot, which just falls back to
normal boot on failure). Read it before running it.
