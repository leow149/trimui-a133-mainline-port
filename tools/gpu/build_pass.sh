#!/bin/sh
# run inside alpine:edge container with /work = /mnt/ext1/trimui_build/mesa (same as build_lin.sh)
set -eu
apk add -q glslang clang lld >/dev/null
S=/work/sysroot
cd /work/vktest
glslangValidator -V --vn lin_vert -o lin_vert.h lin.vert >/dev/null
glslangValidator -V --vn lin_frag -o lin_frag.h lin.frag >/dev/null
glslangValidator -V --vn flat_frag -o flat_frag.h flat.frag >/dev/null
glslangValidator -V --vn lin_rot_vert -o lin_rot_vert.h lin_rot.vert >/dev/null
clang --target=aarch64-alpine-linux-musl --sysroot=$S -fuse-ld=lld -O2 -Wall -o vkpass vkpass.c -lvulkan
ls -la vkpass
