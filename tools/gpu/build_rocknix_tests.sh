#!/bin/bash
# Cross-builds the render-pass/HSR regression tests (vkrenderarea, vkscissor, vkrtt, vkhsr) for the
# ROCKNIX A133 image (glibc) with the ROCKNIX toolchain. Run inside the rocknix-build container on
# bodenserver with this directory copied to $1, e.g.
#   docker run --rm -i --user 1000:1000 -v /mnt/ext1/rocknix:/mnt/ext1/rocknix \
#       ghcr.io/rocknix/rocknix-build:latest bash -s /mnt/ext1/rocknix/vktests < build_rocknix_tests.sh
# On the device: PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 ./vkhsr  (prints a tile map of wrong pixels)
set -euo pipefail
T=/mnt/ext1/rocknix/distribution/build.ROCKNIX-A133.aarch64/toolchain/bin
cd "$1"
for s in ra_depth.vert:ra_depth_vert ra_pc.frag:ra_pc_frag ra_tex.frag:ra_tex_frag \
	 ra_green.frag:ra_green_frag hsr_discard.frag:hsr_discard_frag; do
	$T/glslangValidator -V --vn "${s#*:}" -o "${s#*:}.h" "${s%%:*}" >/dev/null
done
for t in vkrenderarea vkscissor vkrtt vkhsr; do
	[ -f $t.c ] && $T/aarch64-rocknix-linux-gnu-gcc -O2 -Wall -o $t $t.c -lvulkan
done
ls -la vkrenderarea vkscissor vkrtt vkhsr
