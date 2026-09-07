#!/bin/bash
# Assemble a flashable eMMC/SD image for the TrimUI Smart Pro mainline port.
#
# Layout (512-byte sectors):
#   sector 0        MBR
#   sector 256      u-boot-sunxi-with-spl.bin (boot0+SPL+U-Boot+ATF, eGON header)
#                   — 128KB "high" eGON location, NOT the 8KB low one. Confirmed
#                   via real hardware + the vendor's own Knulli image: this
#                   board's BROM only reliably boots from the high location.
#   sector 2048     partition 1: boot, FAT32, 64MiB  (Image, dtb, /extlinux/extlinux.conf)
#   sector 133120   partition 2: rootfs, ext4, 256MiB (buildroot output, must match
#                                                        BR2_TARGET_ROOTFS_EXT2_SIZE)
#   sector 657408   end of image
#
# Confirmed working on real hardware: SPL+BL31+U-Boot all boot cleanly from
# the SD card via this bootloader offset (mmc0 enabled in the board DTS too,
# needed for U-Boot to read the boot partition below).
#
# NOTHING in this script touches real hardware by itself — it only builds a
# local .img file; writing it to a card/device is a separate, explicit step.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ART="${ART:-$ROOT_DIR/build-artifacts}"
BOARD="${BOARD:-$ROOT_DIR/board-port}"
OUT="${OUT:-$ROOT_DIR/image}"
mkdir -p "$OUT"

UBOOT="$ART/u-boot-sunxi-with-spl.bin"
KERNEL="$ART/Image"
DTB="$ART/sun50i-a133-trimui-smartpro.dtb"
ROOTFS_SRC="$1"   # path to buildroot's rootfs.ext2 / rootfs.ext4, passed in once it exists

IMG="$OUT/trimui-mainline.img"
BOOT_IMG="$OUT/boot.fat32.img"

BOOT_START_SECTOR=2048
BOOT_SIZE_MIB=64
BOOT_SIZE_SECTORS=$((BOOT_SIZE_MIB * 1024 * 1024 / 512))
ROOTFS_START_SECTOR=$((BOOT_START_SECTOR + BOOT_SIZE_SECTORS))
ROOTFS_SIZE_MIB=256
ROOTFS_SIZE_SECTORS=$((ROOTFS_SIZE_MIB * 1024 * 1024 / 512))
TOTAL_SECTORS=$((ROOTFS_START_SECTOR + ROOTFS_SIZE_SECTORS))

echo "== sanity checks =="
for f in "$UBOOT" "$KERNEL" "$DTB"; do
	[ -f "$f" ] || { echo "missing: $f"; exit 1; }
done
UBOOT_SIZE=$(stat -c%s "$UBOOT")
[ "$UBOOT_SIZE" -lt $((BOOT_START_SECTOR * 512 - 256 * 512)) ] || {
	echo "u-boot-sunxi-with-spl.bin ($UBOOT_SIZE bytes) too big to fit before sector $BOOT_START_SECTOR"; exit 1;
}
echo "u-boot-sunxi-with-spl.bin: $UBOOT_SIZE bytes (fits before partition 1, good)"

echo "== creating blank image ($TOTAL_SECTORS sectors) =="
rm -f "$IMG"
truncate -s $((TOTAL_SECTORS * 512)) "$IMG"

echo "== writing MBR partition table =="
# id= pins the disk signature so root=PARTUUID=14c78a96-02 in extlinux.conf
# stays valid across re-assembles (sfdisk otherwise randomizes it each run --
# a real, hit gotcha: SD/eMMC mmcblkN enumeration order isn't guaranteed
# stable across boots on this board, so root= needs to name a partition by
# UUID, not a raw device path, and that UUID needs to actually stay constant).
sfdisk "$IMG" <<-EOF
	label: dos
	label-id: 0x14c78a96
	unit: sectors
	${IMG}1 : start=${BOOT_START_SECTOR}, size=${BOOT_SIZE_SECTORS}, type=c, bootable
	${IMG}2 : start=${ROOTFS_START_SECTOR}, size=${ROOTFS_SIZE_SECTORS}, type=83
EOF

echo "== writing boot0/SPL/U-Boot/ATF at sector 256 (128KB, high eGON location) =="
dd if="$UBOOT" of="$IMG" bs=512 seek=256 conv=notrunc status=none

echo "== building FAT32 boot partition content =="
rm -f "$BOOT_IMG"
truncate -s $((BOOT_SIZE_SECTORS * 512)) "$BOOT_IMG"
mkfs.vfat -n TRIMUIBOOT "$BOOT_IMG" > /dev/null
mcopy -i "$BOOT_IMG" "$KERNEL" ::Image
mcopy -i "$BOOT_IMG" "$DTB" ::sun50i-a133-trimui-smartpro.dtb
mmd -i "$BOOT_IMG" ::extlinux
mcopy -i "$BOOT_IMG" "$BOARD/extlinux.conf" ::extlinux/extlinux.conf
echo "== writing boot partition into image =="
dd if="$BOOT_IMG" of="$IMG" bs=512 seek=$BOOT_START_SECTOR conv=notrunc status=none

if [ -n "$ROOTFS_SRC" ] && [ -f "$ROOTFS_SRC" ]; then
	echo "== writing rootfs partition into image =="
	ROOTFS_SIZE=$(stat -c%s "$ROOTFS_SRC")
	MAX_SIZE=$((ROOTFS_SIZE_SECTORS * 512))
	[ "$ROOTFS_SIZE" -le "$MAX_SIZE" ] || { echo "rootfs image ($ROOTFS_SIZE bytes) bigger than partition 2 ($MAX_SIZE bytes)"; exit 1; }
	dd if="$ROOTFS_SRC" of="$IMG" bs=1M seek=$((ROOTFS_START_SECTOR * 512 / 1024 / 1024)) conv=notrunc status=none
	echo "done: $IMG ($(stat -c%s "$IMG") bytes), rootfs included"
else
	echo "no rootfs image supplied yet — image has boot chain + boot partition only"
	echo "rerun: $0 <path-to-rootfs.ext2> once buildroot finishes"
fi
