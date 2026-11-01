#!/bin/sh
# Alpine edge chroot in RAM (tmpfs) with Mesa's PowerVR Vulkan driver, for GPU tests.
# Needs network (/root/wifi-up.sh). Gone after a reboot.
set -eu
R=/tmp/edge
V=20260805
if [ ! -x $R/sbin/apk ]; then
	mkdir -p $R
	mountpoint -q $R || mount -t tmpfs -o size=700m tmpfs $R
	wget -q -O - https://dl-cdn.alpinelinux.org/alpine/edge/releases/aarch64/alpine-minirootfs-$V-aarch64.tar.gz | tar xz -C $R
fi
cp /etc/resolv.conf $R/etc/
for d in proc sys dev dev/pts; do mountpoint -q $R/$d || mount --bind /$d $R/$d; done
chroot $R /sbin/apk add -q mesa-vulkan-imagination vulkan-tools vulkan-loader
echo "chroot ready: chroot $R /usr/bin/env PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 vkcube --wsi display"
