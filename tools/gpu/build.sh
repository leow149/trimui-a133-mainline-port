#!/bin/sh
# Cross-builds vktest and vkmulti for the device (Alpine, musl, aarch64). Runs in the
# alpine:edge container of patches/mesa/build_mesa.sh, whose aarch64 sysroot it reuses:
#   docker run --rm -v <mesa build dir>:/work -v <repo>/tools/gpu:/src -w /src \
#       alpine:edge sh build.sh
set -eu
apk add -q glslang clang lld >/dev/null
S=/work/sysroot
[ -f $S/usr/include/vulkan/vulkan.h ] || apk add -q --root $S --arch aarch64 --allow-untrusted --no-scripts vulkan-headers vulkan-loader-dev >/dev/null
cd "$(dirname "$0")"
glslangValidator -V --vn tri_vert -o tri_vert.h tri.vert >/dev/null
glslangValidator -V --vn tri_frag -o tri_frag.h tri.frag >/dev/null
clang --target=aarch64-alpine-linux-musl --sysroot=$S -fuse-ld=lld -O2 -Wall -o vktest vktest.c -lvulkan
clang --target=aarch64-alpine-linux-musl --sysroot=$S -fuse-ld=lld -O2 -Wall -o vkmulti vkmulti.c -lvulkan
ls -la vktest vkmulti
