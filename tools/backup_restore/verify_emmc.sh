#!/bin/bash
# Verify the eMMC backup against the live device, region by region.
# gzip -t only proves the archives aren't corrupt; it says nothing about whether
# they match what's actually on the eMMC. This re-reads each 256MB region on the
# device, md5s it there, and compares against the md5 of the decompressed chunk.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIR="${DIR:-$SCRIPT_DIR/emmc_chunks}"
CHUNK_MB=256
TOTAL_MB=7456
CHUNKS=$(( (TOTAL_MB + CHUNK_MB - 1) / CHUNK_MB ))

fail=0
for (( i=0; i<CHUNKS; i++ )); do
    skip=$(( i * CHUNK_MB ))
    f="$DIR/emmc_$(printf '%03d' "$i").gz"

    dev=$(adb shell "dd if=/dev/mmcblk0 bs=1M skip=$skip count=$CHUNK_MB 2>/dev/null | md5sum" </dev/null | tr -d '\r' | awk '{print $1}')
    loc=$(gzip -dc "$f" | md5sum | awk '{print $1}')

    if [ "$dev" = "$loc" ]; then
        echo "[$((i+1))/$CHUNKS] offset ${skip}MB  OK    $dev"
    else
        echo "[$((i+1))/$CHUNKS] offset ${skip}MB  MISMATCH  device=$dev backup=$loc"
        fail=$((fail+1))
    fi
done

echo
if [ "$fail" -eq 0 ]; then
    echo "VERIFY PASSED: all $CHUNKS regions match the device byte-for-byte"
else
    echo "VERIFY FAILED: $fail/$CHUNKS regions differ"
    exit 1
fi
