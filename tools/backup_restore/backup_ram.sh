#!/bin/bash
# Full physical RAM dump of the TrimUI Smart Pro, from the live stock system.
#
# System RAM is one contiguous region per /proc/iomem:
#     40000000-7fffffff : System RAM      (exactly 1024MB @ physical 0x40000000)
#       40080000-40a0ffff : Kernel code
#       40a80000-40c37fff : Kernel data
# No holes, so the whole range is safe to read -- no MMIO/reserved regions to skip.
# /dev/mem offsets ARE physical addresses, so skip=1024 (MB) == 0x40000000.
# CONFIG_STRICT_DEVMEM is not set on this kernel, so the full range is readable.
#
# CAVEAT, inherent and unavoidable here: the staging file lives in /tmp, which is
# tmpfs -- i.e. RAM. So the dump perturbs the very memory it captures, and the
# staging buffer's own pages appear in the image. There is no other transport off
# this device (adbd has no `exec:` service, so no streaming). Chunks are kept small
# (128MB) to bound that footprint. Treat this as a best-effort snapshot, not a
# forensically clean one.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${OUT:-$SCRIPT_DIR/ram_chunks}"
mkdir -p "$OUT"

CHUNK_MB=128
BASE_MB=1024           # 0x40000000
TOTAL_MB=1024          # 1GB of System RAM
CHUNKS=$(( TOTAL_MB / CHUNK_MB ))

echo "RAM dump: ${TOTAL_MB}MB from phys 0x40000000 in ${CHUNKS} chunks of ${CHUNK_MB}MB"
START=$(date +%s)

for (( i=0; i<CHUNKS; i++ )); do
    skip=$(( BASE_MB + i * CHUNK_MB ))
    f="$OUT/ram_$(printf '%02d' "$i").gz"

    if [ -s "$f" ]; then
        echo "[$((i+1))/$CHUNKS] already have $(basename "$f"), skipping"
        continue
    fi

    adb shell "dd if=/dev/mem bs=1M skip=$skip count=$CHUNK_MB 2>/dev/null | gzip -1 > /tmp/ram_chunk.gz" </dev/null

    if ! adb pull /tmp/ram_chunk.gz "$f" >/dev/null 2>&1; then
        echo "PULL FAILED at chunk $i -- aborting, re-run to resume"
        adb shell "rm -f /tmp/ram_chunk.gz" </dev/null
        exit 1
    fi
    adb shell "rm -f /tmp/ram_chunk.gz" </dev/null

    sz=$(stat -c%s "$f")
    el=$(( $(date +%s) - START ))
    printf '[%d/%d] phys 0x%08x -> %s %s bytes (%ss elapsed)\n' \
        "$((i+1))" "$CHUNKS" "$(( skip * 1024 * 1024 ))" "$(basename "$f")" "$sz" "$el"
done

echo "ALL RAM CHUNKS PULLED"
du -sh "$OUT"
