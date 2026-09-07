#!/bin/bash
# Full eMMC backup of the TrimUI Smart Pro, pulled over ADB from the live stock system.
#
# Why chunked-with-a-device-side-temp-file instead of streaming:
# this device's adbd is an ancient Allwinner BSP build with no `exec:` service, so
# `adb exec-out` dies instantly with "error: closed", and `adb shell` is a PTY that
# corrupts binary. So: dd a chunk on-device -> gzip -> /tmp -> adb pull -> rm -> repeat.
#
# Chunks go to /tmp (tmpfs, RAM-backed) NOT /mnt/UDISK, because UDISK lives on
# mmcblk0p10 -- writing there would modify the very device being imaged.
#
# Resumable: re-running skips chunks already pulled.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${OUT:-$SCRIPT_DIR/emmc_chunks}"
mkdir -p "$OUT"

CHUNK_MB=256           # 256MB raw; worst-case-incompressible still fits tmpfs (486MB free)
TOTAL_MB=7456          # 7634944 KB from /proc/partitions
CHUNKS=$(( (TOTAL_MB + CHUNK_MB - 1) / CHUNK_MB ))

echo "eMMC backup: ${TOTAL_MB}MB in ${CHUNKS} chunks of ${CHUNK_MB}MB"
START=$(date +%s)

for (( i=0; i<CHUNKS; i++ )); do
    skip=$(( i * CHUNK_MB ))
    f="$OUT/emmc_$(printf '%03d' "$i").gz"

    if [ -s "$f" ]; then
        echo "[$((i+1))/$CHUNKS] already have $(basename "$f"), skipping"
        continue
    fi

    # dd on device, compress in place. gzip -1: the A53 is the bottleneck, and the
    # large empty regions (UDISK 4.5G/9.3M used, rootfs_data 1.9G/4.3M used) collapse
    # to near-nothing even at -1.
    adb shell "dd if=/dev/mmcblk0 bs=1M skip=$skip count=$CHUNK_MB 2>/dev/null | gzip -1 > /tmp/emmc_chunk.gz" </dev/null

    if ! adb pull /tmp/emmc_chunk.gz "$f" >/dev/null 2>&1; then
        echo "PULL FAILED at chunk $i -- aborting, re-run to resume"
        adb shell "rm -f /tmp/emmc_chunk.gz" </dev/null
        exit 1
    fi
    adb shell "rm -f /tmp/emmc_chunk.gz" </dev/null

    sz=$(stat -c%s "$f")
    el=$(( $(date +%s) - START ))
    echo "[$((i+1))/$CHUNKS] offset ${skip}MB -> $(basename "$f") ${sz} bytes (${el}s elapsed)"
done

echo "ALL CHUNKS PULLED"
du -sh "$OUT"
