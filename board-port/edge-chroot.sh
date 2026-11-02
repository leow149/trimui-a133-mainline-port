#!/bin/sh
# Alpine edge chroot with Mesa's PowerVR Vulkan driver and GStreamer, for GPU and video tests.
# Lives on the SD card's data partition (LABEL=TRIMUIDATA, ext4) so it survives reboots and
# doesn't take RAM (a 700 MB tmpfs chroot plus 256 MB CMA ran the 1 GB board out of memory).
# Reachable as /tmp/edge (bind mount). The first run needs network (/root/wifi-up.sh).
set -eu
D=/mnt/data
R=$D/edge
V=20260805
mkdir -p $D
mountpoint -q $D || mount -t ext4 "$(findfs LABEL=TRIMUIDATA)" $D
if [ ! -x $R/sbin/apk ]; then
	mkdir -p $R
	wget -q -O - https://dl-cdn.alpinelinux.org/alpine/edge/releases/aarch64/alpine-minirootfs-$V-aarch64.tar.gz | tar xz -C $R
fi
mkdir -p /tmp/edge
mountpoint -q /tmp/edge || mount --bind $R /tmp/edge
R=/tmp/edge
cp /etc/resolv.conf $R/etc/
# Wayland clients and compositors need POSIX shared memory.
mkdir -p /dev/shm
mountpoint -q /dev/shm || mount -t tmpfs -o mode=1777 tmpfs /dev/shm
for d in proc sys dev dev/pts; do mountpoint -q $R/$d || mount --bind /$d $R/$d; done
if [ ! -e $R/usr/bin/gst-launch-1.0 ]; then
	chroot $R /sbin/apk add -q mesa-vulkan-imagination vulkan-tools vulkan-loader \
		gstreamer-tools gst-plugins-base gst-plugins-good gst-plugins-bad gst-libav \
		libdrm-tests v4l-utils
fi
echo "chroot ready: chroot $R /usr/bin/env PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 vkcube --wsi display"
