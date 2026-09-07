#!/bin/bash
#
# Restore the stock eMMC image back onto the device.
#
# READ THIS FIRST.
#
# Restoring to eMMC is the one operation on this device with no safe fallback.
# An SD-card mistake just falls back to normal boot; an eMMC mistake takes out the
# boot chain with nothing left to boot from. Recovery then depends on FEL mode
# (BROM USB recovery), which lives in mask ROM and should survive anything written
# to eMMC -- but that has NOT been verified on this unit yet. Verify FEL recovery
# works before you ever need it.
#
# This script does NOT run unattended. It requires:
#   1. an explicit target argument, and
#   2. typing the confirmation phrase when prompted.
#
# Usage:
#   ./restore_emmc.sh check              # verify the backup is complete + intact, write nothing
#   ./restore_emmc.sh reassemble <path>  # build the full .img locally, write nothing to device
#   ./restore_emmc.sh restore-boot-only  # restore ONLY the raw boot bundle + p1..p5 (safer)
#   ./restore_emmc.sh restore-full       # restore the entire 7456MB (destructive, last resort)
#
# The device must be booted into something with adb root and a working /dev/mmcblk0
# (i.e. the stock firmware, or a recovery/CFW that provides both).

set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
CHUNKS="$DIR/emmc_chunks"
CHUNK_MB=256
TOTAL_MB=7456
NCHUNKS=30

die() { echo "ERROR: $*" >&2; exit 1; }

check_backup() {
    local n
    n=$(ls -1 "$CHUNKS"/emmc_*.gz 2>/dev/null | wc -l)
    [ "$n" -eq "$NCHUNKS" ] || die "expected $NCHUNKS chunks, found $n"
    gzip -t "$CHUNKS"/emmc_*.gz || die "one or more chunks are corrupt"
    echo "backup OK: $NCHUNKS chunks present and intact"
}

confirm() {
    local phrase="$1"
    echo
    echo "!!! This writes to the device's eMMC. There is no undo."
    echo "!!! Type exactly:  $phrase"
    read -r -p "> " got
    [ "$got" = "$phrase" ] || die "confirmation did not match -- nothing was written"
}

require_device() {
    adb shell "test -b /dev/mmcblk0 && echo ok" </dev/null 2>/dev/null | tr -d '\r' | grep -q ok \
        || die "device not reachable over adb, or /dev/mmcblk0 missing"
}

# Push one 256MB region back to the device. Staged through /tmp (tmpfs) because
# writing the staging file anywhere on /dev/mmcblk0 would corrupt the restore.
push_chunk() {
    local i="$1" skip=$(( $1 * CHUNK_MB ))
    local f="$CHUNKS/emmc_$(printf '%03d' "$i").gz"
    gzip -dc "$f" > /tmp/_restore_chunk.bin || die "decompress failed: $f"
    adb push /tmp/_restore_chunk.bin /tmp/_restore_chunk.bin >/dev/null || die "push failed at chunk $i"
    adb shell "dd if=/tmp/_restore_chunk.bin of=/dev/mmcblk0 bs=1M seek=$skip conv=notrunc 2>&1 | tail -1" </dev/null
    adb shell "rm -f /tmp/_restore_chunk.bin" </dev/null
    rm -f /tmp/_restore_chunk.bin
    echo "  restored region $i (offset ${skip}MB)"
}

case "${1:-}" in
check)
    check_backup
    ;;

reassemble)
    out="${2:-}"; [ -n "$out" ] || die "usage: $0 reassemble <output.img>"
    check_backup
    echo "writing ${TOTAL_MB}MB to $out ..."
    zcat "$CHUNKS"/emmc_*.gz > "$out" || die "reassembly failed"
    echo "done: $(du -h "$out" | cut -f1)"
    ;;

restore-boot-only)
    # Chunks 0-2 cover offset 0..768MB, which spans the raw boot bundle plus
    # p1(bootloader) p2(env) p3(env-redund) p4(boot) p5(rootfs) and the very start
    # of p6. That is everything needed to make the device boot stock again, without
    # touching the 6.7GB of user data beyond it.
    check_backup; require_device
    confirm "RESTORE BOOT"
    for i in 0 1 2; do push_chunk "$i"; done
    adb shell "sync" </dev/null
    echo "boot regions restored (offsets 0-768MB). Verify with: $0 check-device"
    ;;

restore-full)
    check_backup; require_device
    confirm "RESTORE EVERYTHING"
    for (( i=0; i<NCHUNKS; i++ )); do push_chunk "$i"; done
    adb shell "sync" </dev/null
    echo "full eMMC restored."
    ;;

check-device)
    # Re-verify the device against the backup without writing anything.
    require_device
    exec "$DIR/verify_emmc.sh"
    ;;

*)
    sed -n '2,30p' "$0"
    exit 1
    ;;
esac
