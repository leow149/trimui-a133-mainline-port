# Compile-only test: builds vkstore.c (shader st.comp) and creates its compute pipeline on the
# host-built PowerVR driver over the noop DRM shim, as a GE8300 (PVR_SHIM_DEVICE_BVNC).
set -e
TC=/mnt/ext1/rocknix/distribution/build.ROCKNIX-A133.aarch64/toolchain
export PATH=$TC/bin:$PATH
cd /mnt/ext1/rocknix/vkst
glslangValidator -V --vn st_comp -o st_comp.h st.comp >/dev/null
VKINC=/mnt/ext1/rocknix/mesa-a133-wt/include
gcc -O0 -g -I$VKINC -o vkstore vkstore.c -ldl
S=/mnt/ext1/rocknix/mesa-host-shim/src/imagination
export LD_PRELOAD=$S/drm-shim/libpowervr_noop_drm_shim.so PVR_SHIM_DEVICE_BVNC=22.102.54.38 PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 MESA_SHADER_CACHE_DISABLE=true
./vkstore $S/vulkan/libvulkan_powervr_mesa.so 2>&1 | grep -v "not a conformant\|FINISHME" | tail -6
echo rc=${PIPESTATUS[0]}
