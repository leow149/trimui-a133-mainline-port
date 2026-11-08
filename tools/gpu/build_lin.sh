#!/bin/sh
# run inside alpine:edge container with /work = /mnt/ext1/trimui_build/mesa (same as build.sh)
set -eu
apk add -q glslang clang lld >/dev/null
S=/work/sysroot
cd /work/vktest
glslangValidator -V --vn lin_vert -o lin_vert.h lin.vert >/dev/null
glslangValidator -V --vn lin_frag -o lin_frag.h lin.frag >/dev/null
glslangValidator -V --vn rgb565_comp -o rgb565_comp.h rgb565_to_rgba8888.comp >/dev/null
glslangValidator -V --vn cs_const -o cs_const.h cs_const.comp >/dev/null
glslangValidator -V --vn cs_ssbo -o cs_ssbo.h cs_ssbo.comp >/dev/null
glslangValidator -V --vn cs_ids -o cs_ids.h cs_ids.comp >/dev/null
glslangValidator -V --vn cs_pt -o cs_pt.h cs_pt.comp >/dev/null
glslangValidator -V --vn cs_pt32 -o cs_pt32.h cs_pt32.comp >/dev/null
glslangValidator -V --vn cs_fix -o cs_fix.h cs_fix.comp >/dev/null
glslangValidator -V --vn cs_dyn -o cs_dyn.h cs_dyn.comp >/dev/null
glslangValidator -V --vn cs_atom -o cs_atom.h cs_atom.comp >/dev/null
glslangValidator -V --vn cs_load -o cs_load.h cs_load.comp >/dev/null
clang --target=aarch64-alpine-linux-musl --sysroot=$S -fuse-ld=lld -O2 -Wall -o vklinear vklinear.c -lvulkan
ls -la vklinear
