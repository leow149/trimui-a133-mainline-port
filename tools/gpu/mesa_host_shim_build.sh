# Builds the PowerVR Vulkan driver and the noop DRM shim for the host (x86) from the
# Mesa worktree on bodenserver, inside the rocknix-build container. Used by vkstore_shim_run.sh.
set -e
TC=/mnt/ext1/rocknix/distribution/build.ROCKNIX-A133.aarch64/toolchain
export PATH=$TC/bin:$PATH
NF=/mnt/ext1/rocknix/distribution/build.ROCKNIX-A133.aarch64/build/mesa-01bab9302a6bf0a082c7ba8c49d74427cf10b0cf/.x86_64-linux-gnu/meson.conf
cd /mnt/ext1/rocknix/mesa-a133-wt
B=/mnt/ext1/rocknix/mesa-host-shim
if [ ! -f $B/build.ninja ]; then
  meson setup $B --native-file=$NF -Dbuildtype=debug -Dglvnd=disabled -Dgallium-drivers= -Dplatforms= -Dglx=disabled -Dvulkan-drivers=imagination -Dshared-llvm=disabled -Dtools=drm-shim -Dvideo-codecs= -Dbuild-tests=false -Dmesa-clc=enabled -Dprecomp-compiler=enabled -Dllvm=enabled > /mnt/ext1/rocknix/logs/shim_setup.log 2>&1 || { tail -20 /mnt/ext1/rocknix/logs/shim_setup.log; exit 1; }
fi
ninja -C $B src/imagination/vulkan/libvulkan_powervr_mesa.so src/imagination/drm-shim/libpowervr_noop_drm_shim.so > /mnt/ext1/rocknix/logs/shim_build.log 2>&1 || { grep -E "error|FAILED" /mnt/ext1/rocknix/logs/shim_build.log | head; exit 1; }
ls -la $B/src/imagination/vulkan/libvulkan_powervr_mesa.so $B/src/imagination/drm-shim/libpowervr_noop_drm_shim.so | cut -c30-
