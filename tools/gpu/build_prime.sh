#!/bin/sh
# run inside alpine:edge container with /work = /mnt/ext1/trimui_build/mesa
set -eu
apk add -q clang lld >/dev/null
S=/work/sysroot
cd /work/vktest
clang --target=aarch64-alpine-linux-musl --sysroot=$S -fuse-ld=lld -O2 -Wall -o primeimport primeimport.c -lvulkan
ls -la primeimport
