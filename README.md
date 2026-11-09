# TrimUI Smart Pro: mainline Linux

Mainline Linux, U-Boot and Trusted Firmware for the **TrimUI Smart Pro** handheld (TG5020,
Allwinner **A133 Plus**). No vendor kernel or vendor boot loader: upstream code plus a series of
board and SoC patches, kept small and in upstream style so it can be submitted.

The device: 4x Cortex-A53 at up to 1.46 GHz, 1 GB RAM, a 720x1280 MIPI DSI panel mounted in
landscape, PowerVR GE8300 GPU, Allwinner Cedar video engine, AXP717 PMIC, XR829 Wi-Fi/Bluetooth,
two analog sticks, RGB LEDs, two USB-C ports.

## Status

Not submitted upstream yet.

| Area | Status |
|---|---|
| Boot | ✅ Mainline U-Boot + TF-A, blob-free boot chain, DRAM at the stock timings |
| Display | ✅ MIPI DSI panel (ER68576, detected by its panel ID; other IDs get the OTM1289A init), backlight, console rotated to landscape |
| GPU | ✅ PowerVR GE8300 at 696 MHz: Vulkan (Mesa, patched), OpenGL 3.2 / OpenGL ES 3.1 through Zink. vkmark 265; RetroArch (mgba, gambatte, fceumm) at full rate under the cage compositor in Vulkan and OpenGL mode. PPSSPP's Qt frontend doesn't start (it requires desktop OpenGL 4.6) |
| Video decoding | ✅ H.264 and H.265 (bit-exact, up to 4K), MPEG-2; behind the IOMMU, zero-copy to the display |
| Input | ✅ Buttons and both sticks (two UART microcontrollers) as one gamepad, power and volume keys, slider switch |
| Wi-Fi | ✅ XR829, 2.4 GHz, HT40 + short GI: ~35-39 Mbit/s receive, ~30-32 Mbit/s send (iperf3, 40 MHz AP); stable MAC address. Station, access point (hotspot, `rootfs/root/ap-up.sh`) and station + AP at the same time. WPA2 recommended: WPA3 (SAE) works but is slow (management frame protection in the firmware), see docs/wifi-throughput.md |
| Bluetooth | ✅ XR829 (H4 on UART1): firmware loaded at boot by a userspace uploader, classic and LE scanning work; stable BD address. Works alongside Wi-Fi at normal scan duty cycles (see docs/wifi-bt-coexistence.md) |
| Audio | ✅ Stereo speakers and headphones, jack detection mutes the speakers |
| Power | ✅ Battery level and charging (AXP717): 2.5 A input limit, 1.92 A charge current, battery current reading. No USB-PD or Type-C negotiation: the board has no PD chip and the PMIC's CC pins aren't connected (docs/axp717-typec-bc-re.md). CPU frequency scaling 408-1464 MHz, thermal throttling (CPU trips at 80, 88 and 95 °C; GPU at 88 °C, critical at 110 °C), suspend (s2idle) |
| USB | ✅ Data port: OTG by ID pin. Device mode with DMA (NCM network: ~310 Mbit/s to the host, ~155 from it, survives suspend); host mode with 5 V VBUS from the AXP717 (bus-powered hubs, sticks ~33 MB/s, Ethernet adapters), switched by the ID pin; no back-feed into a USB-C charger (docs/usb-otg.md). Second port: host only |
| Other | ✅ SD card, eMMC, RTC, RGB LEDs (23), UART console, reboot, hardware watchdog |
| Not working yet | ❌ video encoding. GPU frequency scaling isn't implemented (the GPU runs at a fixed 696 MHz). VP9 hardware decode is not possible: the A133 has no Hantro G2 (see docs/vp9-bringup.md); VP9 decodes in software. |

Suspend (s2idle) re-tested 2026-11-07 with the GPU, video engine and WiFi active (patches 0063-0064 fixed a resume hang in the XR829 driver and a watchdog reset). Wake by RTC alarm and by the power, volume up and volume down keys is verified. The USB gadget re-enumerates after resume (patch 0073) and the GPU survives suspend (patch 0076).

[`docs/gpu-video.md`](docs/gpu-video.md) describes the GPU and video bring-up in detail.

## Repository layout

```
patches/
  linux/         Kernel series (94 patches) and defconfig
  u-boot/        Board support, DRAM arbiter priorities, Wi-Fi MAC alias (3 patches)
  tf-a/          Watchdog fix for system reset (1 patch)
  mesa/          PowerVR Vulkan driver fixes (9 patches) and build_mesa.sh
rootfs/          Files installed on the device
  etc/inittab           busybox init: mounts, watchdog, the scripts below, serial shell
  etc/usb-net.sh        USB network gadget (10.55.0.2) and SSH server at boot
  etc/audio-defaults.sh quiet mixer levels at boot
  root/wifi-up.sh       connects Wi-Fi with /etc/wpa_supplicant/home.conf
  root/edge-chroot.sh   Alpine edge chroot for the GPU/video userspace (Mesa, GStreamer)
  root/wayland-session.sh  runs a program in a cage Wayland session, landscape
  root/cpuload.sh       CPU load per process over an interval
image/           SD card image: assemble_image.sh, update_sd_kernel.sh, extlinux.conf
tools/           Development tools (see below), tools/gpu/ for GPU and display tests
tests/video/     Decoder test: ve_test.sh compares hardware and software decoding
tests/gpu-matrix/ GPU/display test matrix: RetroArch cores, Wayland/X11 clients, frame rates, underflows
docs/            gpu-video.md, gpu-perf-vs-stock.md (fragment throughput), gpu-v1-transfer.md (transfer
                 path on the v1 parameter format), USB OTG, Wi-Fi, Bluetooth, charging and VP9 notes;
                 history/ for older write-ups
vendor-notes/    Register dumps and notes from the stock firmware, for comparison
```

## Building

The patches apply with `git am` to the commits below; the result is the tree that was tested.
Cross compiler: `aarch64-linux-gnu-`.

### Linux

```sh
git clone https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
cd linux
git checkout 26260251022fbc2f248a3d747a9b2b961b18d2d8
git am /path/to/repo/patches/linux/*.patch
cp /path/to/repo/patches/linux/trimui-a133-smartpro_defconfig arch/arm64/configs/
```

The config builds the GPU and Wi-Fi firmware into the kernel, from `firmware/` in the kernel tree:

- `firmware/powervr/rogue_22.102.54.38_v1.fw` from Imagination's
  [linux-firmware fork](https://gitlab.freedesktop.org/imagination/linux-firmware/-/tree/powervr),
  branch `powervr`.
- `firmware/xr829/boot_xr829.bin`, `fw_xr829.bin` and `sdd_xr829.bin` from `/lib/firmware` of
  the stock firmware. They aren't redistributable, so take them from your own device.

```sh
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- trimui-a133-smartpro_defconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image allwinner/sun50i-a133-trimui-smartpro.dtb
```

The GE8300 isn't on the `powervr` driver's list of supported cores yet: boot with
`powervr.exp_hw_support=1` (`image/extlinux.conf` does).

### Trusted Firmware and U-Boot

```sh
git clone https://github.com/ARM-software/arm-trusted-firmware.git
cd arm-trusted-firmware
git checkout 6a164dd
git am /path/to/repo/patches/tf-a/*.patch
make CROSS_COMPILE=aarch64-linux-gnu- PLAT=sun50i_a133 bl31

git clone https://source.denx.de/u-boot/u-boot.git
cd u-boot
git checkout 6073c36b2c8d39afe3ecc789b281667a3ddebc70
git am /path/to/repo/patches/u-boot/*.patch
make trimui-smartpro_defconfig
make CROSS_COMPILE=aarch64-linux-gnu- \
     BL31=/path/to/arm-trusted-firmware/build/sun50i_a133/release/bl31.bin
```

The result is `u-boot-sunxi-with-spl.bin`.

### Mesa (Vulkan driver)

The PowerVR driver is cross-built for Alpine Linux (musl) in an `alpine:edge` container:

```sh
mkdir mesa-build && cd mesa-build
git clone https://gitlab.freedesktop.org/mesa/mesa.git
git -C mesa checkout 01bab9302a6bf0a082c7ba8c49d74427cf10b0cf
git -C mesa am /path/to/repo/patches/mesa/*.patch
cp /path/to/repo/patches/mesa/build_mesa.sh .
docker run --rm -v "$PWD":/work -w /work alpine:edge sh build_mesa.sh
```

The driver ends up in `build/src/imagination/vulkan/libvulkan_powervr_mesa.so`. It replaces the
one of Alpine's `mesa-vulkan-imagination` package in the chroot on the device (see Userspace).

## Installing

`image/assemble_image.sh` builds an SD card image from the U-Boot binary, kernel, device tree and
a root file system: U-Boot at 128 KiB, a 64 MiB FAT32 boot partition with `Image`, the DTB and
`extlinux/extlinux.conf`, then the root partition. The card used during development has U-Boot
at 8 KiB instead; the boot ROM boots from both. With an SD card in the slot, the device boots
from it rather than from the eMMC.

To update an existing card, `image/update_sd_kernel.sh` replaces kernel and DTB on its boot
partition in a card reader, and `tools/push_kernel.sh` installs them on the running device over
USB.

## Userspace

The root file system is a minimal Alpine Linux 3.22 with busybox init, plus the files in
`rootfs/`. The USB-C charging port comes up as a USB network adapter at boot: the device is
`10.55.0.2`, reachable over SSH (dropbear, key login).

The GPU and video userspace (current Mesa, GStreamer) runs in an Alpine edge chroot that
`rootfs/root/edge-chroot.sh` creates on an ext4 partition labelled `TRIMUIDATA` on the SD card
and mounts at `/tmp/edge`. Examples:

```sh
# Vulkan, straight to the display
chroot /tmp/edge env PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 vkcube --wsi display

# OpenGL ES through Zink, in a Wayland session in landscape
/root/wayland-session.sh weston-simple-egl -f

# Hardware video decoding, played on the display
chroot /tmp/edge gst-launch-1.0 filesrc location=video.mp4 ! qtdemux ! h264parse \
    ! v4l2slh264dec ! queue ! kmssink driver-name=sun4i-drm plane-id=35 can-scale=true \
      plane-properties="s,zpos=(int)3"
```

Mesa refuses the GE8300 without `PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1`, as it isn't a
conformant Vulkan implementation yet.

## Tools

| Tool | Purpose |
|---|---|
| `tools/push_kernel.sh` | Installs kernel/DTB on the device's SD card over the USB network (UART as fallback) |
| `tools/uboot_test_boot.py` | Boots a test kernel/DTB once from U-Boot over the UART; the next reboot uses the default again |
| `tools/uart_session.py`, `uart_push.py` | Holds the UART console open behind a FIFO; md5-verified transfers over it |
| `tools/reboot_loop.sh` | Reboot reliability test |
| `tools/padview/` | Framebuffer gamepad viewer |
| `tools/rainbow.sh` | Colour cycle on the stick and logo LEDs |
| `tools/devmem.c`, `i2creg.c`, `regsnap.c` | Register access: MMIO, I2C (the PMIC), PMIC and pin controller snapshots |
| `tools/drmtest.c` | DRM modeset test without libdrm |
| `tools/ncm_gadget.sh`, `test_gadget.sh` | USB gadget setups for device-mode tests |
| `tools/backup_restore/` | Backup of the stock system's eMMC and RAM, eMMC restore (scripts only) |
| `tools/gpu/` | GPU and display tests, see [`docs/gpu-video.md`](docs/gpu-video.md) |

## What's not in this repo

No source trees, and nothing copyrighted by the vendor: no firmware blobs, kernel images, eMMC or
RAM dumps, or decompiled vendor code, and not the Allwinner manual. The source trees are almost
entirely unmodified upstream code, for which a patch series is the reviewable form. The vendor's
files aren't ours to redistribute, and dumps of a physical unit contain device-specific data.
`vendor-notes/` has only our own measurements. The vendor files can be pulled from your own
device: `tools/backup_restore/` for the eMMC, the stock firmware for the firmware files.

## History

The display stayed black for a long time. It had several independent causes, all of which had to
be fixed: SRAM C mapped to the CPU (which hides the whole display engine register space), the
display engine clocks missing from the device tree, the TCON interrupt pointing at the DSI's,
the panel reset on the wrong pin and, finally, the wrong panel controller: this unit's panel is an
ER68576, not the OTM1289A named in the vendor device tree.
[`docs/history/community-help-request.md`](docs/history/community-help-request.md) is the
write-up from that time.

## License

The patches are under the license of the project they modify: GPL-2.0 for Linux and U-Boot,
BSD-3-Clause for TF-A, MIT for Mesa. Everything else in this repo is GPL-2.0-or-later
([`LICENSE`](LICENSE)) unless noted otherwise.

## Credits

This builds on the work of the linux-sunxi community, in particular the `sun4i` DRM driver
family, the `sunxi-ng` clock drivers and Cedrus, on Imagination's open-source `powervr` kernel
driver and Mesa PowerVR driver, on fifteenhex's [xradio](https://github.com/fifteenhex/xradio) driver for the XR819 (imported in
`0042` and extended for the XR829), and on Paul Kocialkowski's U-Boot series for the A133 NSI arbiter,
which `patches/u-boot/0002` ports.
