#!/bin/bash
# Swap just the kernel (+dtb) on an already-working TrimUI SD card.
#
# The card's boot partition already carries a working extlinux setup and the
# rootfs already has devmem/drmtest baked in, so a full re-image is unnecessary
# and would only risk losing those. This replaces Image + dtb in place.
#
# No sudo: uses udisksctl, which mounts removable media via polkit and mounts
# FAT owned by the invoking user, so plain cp works.
#
# Usage: ./update_sd_kernel.sh /dev/sdX1    (the BOOT PARTITION, not the disk)

set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ART="${ART:-$SCRIPT_DIR/../build-artifacts}"
PART="${1:-}"

[ -n "$PART" ] || { echo "usage: $0 /dev/sdX1   (boot partition)"; exit 1; }
[ -b "$PART" ] || { echo "ERROR: $PART is not a block device"; exit 1; }

# Refuse anything that isn't removable, and refuse the system disk outright.
disk=$(lsblk -no PKNAME "$PART")
rm_flag=$(lsblk -dno RM "/dev/$disk")
root_disk=$(lsblk -no PKNAME "$(findmnt -no SOURCE /)" 2>/dev/null || true)
[ "$disk" != "$root_disk" ] || { echo "ERROR: $PART is on the system disk. Refusing."; exit 1; }
[ "$rm_flag" = "1" ] || { echo "ERROR: /dev/$disk is not removable. Refusing."; exit 1; }

echo "Target partition: $PART  (on removable disk /dev/$disk)"
lsblk -o NAME,SIZE,TYPE,FSTYPE,LABEL "/dev/$disk"
echo

# Mount via udisks (no sudo). If already mounted, reuse the existing mountpoint.
MNT=$(lsblk -no MOUNTPOINT "$PART")
UNMOUNT_AFTER=0
if [ -z "$MNT" ]; then
    out=$(udisksctl mount -b "$PART")
    echo "$out"
    MNT=$(printf '%s' "$out" | sed -n 's/.* at \(.*\)$/\1/p' | sed 's/\.$//')
    UNMOUNT_AFTER=1
fi
[ -n "$MNT" ] && [ -d "$MNT" ] || { echo "ERROR: could not determine mountpoint"; exit 1; }
echo "Mounted at: $MNT"

[ -f "$MNT/Image" ] || {
    echo "ERROR: no Image at $MNT -- this does not look like the boot partition."
    [ "$UNMOUNT_AFTER" = "1" ] && udisksctl unmount -b "$PART" || true
    exit 1
}

echo "--- before ---"; ls -la "$MNT"

# Rollback copy goes on the HOST, never on the card: the boot partition is 64MB
# and the kernel is ~61.5MB, so a second copy does not fit -- attempting it fills
# the filesystem and leaves a truncated file behind.
cp "$MNT/Image" "$ART/Image.prev-$(date +%Y%m%d-%H%M%S)"
echo "rollback copy saved to host: $ART/Image.prev-*"

cp "$ART/Image" "$MNT/Image"
[ -f "$ART/sun50i-a133-trimui-smartpro.dtb" ] && cp "$ART/sun50i-a133-trimui-smartpro.dtb" "$MNT/" || true
sync

echo "--- after ---"; ls -la "$MNT"

if [ "$UNMOUNT_AFTER" = "1" ]; then
    udisksctl unmount -b "$PART"
    echo "unmounted -- safe to remove"
fi
echo
echo "Kernel updated. Insert into the device and power on with UART attached."
