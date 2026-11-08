#!/bin/sh
# run inside alpine:edge container with /work = /mnt/ext1/trimui_build/mesa (builds fbgrab for the device)
set -eu
apk add -q clang lld linux-headers libdrm-dev >/dev/null
S=/work/sysroot
cd /work/vktest
clang --target=aarch64-alpine-linux-musl --sysroot=$S -fuse-ld=lld -O2 -Wall -idirafter /usr/include -o fbgrab fbgrab.c
ls -la fbgrab
