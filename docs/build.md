# Kernel build on bodenserver — optimized setup

The old approach (`kbuild*.sh`) was wasteful: every build ran `docker run --rm debian:bookworm` and
`apt-get update && apt-get install` the full cross toolchain from scratch, had **no ccache**, and
built a stale `src.tar` snapshot rather than the real tree.

## What changed (2026-11-06)

1. **Persistent container** `trimui-build` (`debian:bookworm`, gcc-aarch64-linux-gnu 12.2.0) —
   the toolchain + build deps are installed once, not per build.
2. **ccache** (20 GB at `/mnt/ext1/ccache`) wired in via `CC="ccache aarch64-linux-gnu-gcc"`.
3. **Source sync** from the local git tree with `rsync` (excluding `.git`/`*.o`/artifacts), instead
   of the `src.tar` snapshot.

## Usage

```sh
# one-time: container is already created (see /tmp/opencode/setup_build.sh)

# sync current source (from the laptop, in the linux/ tree):
rsync -az --exclude='.git/' --exclude='*.o' --exclude='*.a' --exclude='*.cmd' \
      --exclude='*.d' --exclude='arch/arm64/boot/Image' --exclude='*.dtb' \
      --exclude='.tmp_*' --exclude='Module.symvers' --exclude='modules.order' \
      -e "ssh -o BatchMode=yes" ./ user@bodenserver.home-ssid:/mnt/ext1/trimui_build/linux/

# build (16 threads, ccache):
ssh user@bodenserver.home-ssid 'docker exec -d trimui-build bash -c "
  cd /work/linux
  export CCACHE_DIR=/ccache
  make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- CC=\"ccache aarch64-linux-gnu-gcc\" -j16 \
       Image allwinner/sun50i-a133-trimui-smartpro.dtb > /work/build.log 2>&1"'

# watch:  ssh user@bodenserver.home-ssid 'tail -f /mnt/ext1/trimui_build/build.log'
```

Incremental rebuilds after a source change only recompile the touched objects (ccache serves the
rest), so they finish in ~1–2 min instead of ~15 min.

Config source of truth is `patches/linux/trimui-a133-smartpro_defconfig` (regenerate with
`make trimui-a133-smartpro_defconfig`). Firmware must live in `linux/firmware/{powervr,xr829}/`
before building (PowerVR blob from Imagination's linux-firmware fork, XR829 blobs from
`vendor_data/firmware/`).
