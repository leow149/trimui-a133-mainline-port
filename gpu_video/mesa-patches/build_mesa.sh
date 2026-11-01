#!/bin/sh
# Cross-build Mesa's PowerVR Vulkan driver for Alpine edge aarch64 (musl).
# Runs inside an alpine:edge x86_64 container; /work is the bind-mounted build dir.
# Stage 1 builds the shader build tools (mesa_clc, vtn_bindgen2, pco_clc) natively
# with LLVM; stage 2 cross-compiles the driver using them, without LLVM.
set -eu
cd /work
apk add -q build-base clang lld llvm llvm-dev clang-dev libclc-dev spirv-llvm-translator-dev \
	spirv-tools-dev glslang meson ninja py3-mako py3-yaml py3-packaging py3-ply \
	flex bison pkgconf bash git python3 libdrm-dev zlib-dev zstd-dev expat-dev \
	llvm-static clang-static zlib-static zstd-static libxml2-dev elfutils-dev \
	linux-headers >/dev/null
if [ ! -f sysroot/.done ]; then
	rm -rf sysroot; mkdir -p sysroot/etc/apk
	cp /etc/apk/repositories sysroot/etc/apk/
	apk add -q --root sysroot --arch aarch64 --initdb --allow-untrusted --no-scripts \
		musl-dev gcc g++ libstdc++-dev libdrm-dev zlib-dev zstd-dev expat-dev \
		elfutils-dev linux-headers >/dev/null
	touch sysroot/.done
fi

# Stage 1: native tools
if [ ! -f native/build.ninja ]; then
	meson setup native mesa --buildtype=release --prefix=/work/tools \
		-Dvulkan-drivers=imagination -Dgallium-drivers= -Dplatforms= \
		-Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false \
		-Dgles1=disabled -Dgles2=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
		-Dmesa-clc=enabled -Dinstall-mesa-clc=true \
		-Dprecomp-compiler=enabled -Dinstall-precomp-compiler=true -Dbuild-tests=false
fi
ninja -C native src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 \
	src/imagination/pco/uscgen/pco_clc src/imagination/pdsc/pdsc_precomp
mkdir -p tools/bin
cp native/src/compiler/clc/mesa_clc native/src/compiler/spirv/vtn_bindgen2 \
	native/src/imagination/pco/uscgen/pco_clc native/src/imagination/pdsc/pdsc_precomp tools/bin/
export PATH=/work/tools/bin:$PATH

# Stage 2: cross build of the driver
S=/work/sysroot
cat > cross.txt <<EOT
[binaries]
c = ['clang', '--target=aarch64-alpine-linux-musl', '--sysroot=$S']
cpp = ['clang++', '--target=aarch64-alpine-linux-musl', '--sysroot=$S']
ar = 'llvm-ar'
strip = 'llvm-strip'
pkg-config = 'pkg-config'
[properties]
sys_root = '$S'
pkg_config_libdir = '$S/usr/lib/pkgconfig:$S/usr/share/pkgconfig'
[built-in options]
c_link_args = ['-fuse-ld=lld']
cpp_link_args = ['-fuse-ld=lld']
[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'cortex-a53'
endian = 'little'
EOT
if [ ! -f build/build.ninja ]; then
	meson setup build mesa --cross-file cross.txt --buildtype=debugoptimized \
		-Dvulkan-drivers=imagination -Dgallium-drivers= -Dplatforms= \
		-Dglx=disabled -Degl=disabled -Dgbm=disabled -Dllvm=disabled \
		-Dopengl=false -Dgles1=disabled -Dgles2=disabled \
		-Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=enabled \
		-Dmesa-clc=system -Dprecomp-compiler=system -Dbuild-tests=false
fi
ninja -C build src/imagination/vulkan/libvulkan_powervr_mesa.so
ls -la build/src/imagination/vulkan/libvulkan_powervr_mesa.so
