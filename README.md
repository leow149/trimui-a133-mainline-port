# TrimUI Smart Pro — mainline Linux port

A from-scratch mainline Linux (and mainline U-Boot) bring-up for the **TrimUI Smart Pro**
handheld (model TG5020, Allwinner **A133 Plus** SoC), entirely blob-free at the bootloader
level. No vendor kernel, no vendor U-Boot, no Android — this boots pristine upstream Linux
and U-Boot with a small, documented set of board-support patches.

## Status

Work in progress, not yet submitted upstream in this form.

| Component | Status |
|---|---|
| U-Boot (mainline, blob-free boot chain) | ✅ Working |
| DRAM at real, correct timings | ✅ Working |
| Display (MIPI DSI panel, OTM1289A / ER68576) + backlight | ✅ Working, rotated to landscape |
| Buttons + analog sticks (two UART microcontrollers) | ✅ Working, one gamepad input device, sticks auto-centred |
| Power key, volume keys | ✅ Working |
| Battery level, charging (AXP717) | ✅ Working, stock charge current, battery thermistor on |
| CPU frequency scaling 408-1464 MHz, thermal throttling | ✅ Working |
| SD card, eMMC, UART console, reboot | ✅ Working |
| RTC (external 32.768 kHz crystal) | ✅ Working, keeps time across reboots |
| Audio: stereo speakers + headphone output | ✅ Working (playback), jack detection mutes the speakers |
| Stick RGB LEDs + logo LED (23 on one chain) | ✅ Working, `/sys/class/leds/rgb:indicator-N` (0 logo, 1-11 left, 12-22 right) |
| Slider switch | ✅ Working (input switch event, like stock) |
| Suspend (s2idle; firmware has no PSCI system suspend), wake on power key | ✅ Working, display and gamepad resume |
| USB-C (charging port) device mode | ✅ Working (configfs gadgets; tested with a serial gadget, 1 MB transfer verified) |
| USB-C second port (host) | ✅ Working (USB 2.0; hubs, storage, HID and USB Ethernet drivers built in) |
| Wi-Fi/BT (XR829), GPU (PowerVR GE8300), video decoding | ❌ No mainline driver |

The long-standing black-screen bug is solved. It had four independent causes, all needed:
SRAM C routed to the CPU (which shadows the whole display-engine register space), the
DE2 clock block missing from the device tree, the TCON interrupt pointing at the DSI's
line, and the panel reset on the wrong pin.
[`COMMUNITY_HELP_REQUEST.md`](COMMUNITY_HELP_REQUEST.md) is kept as a record of the
investigation; its open question is answered by the above.

## Repo layout

```
patches/linux/     Kernel patch series against a named upstream commit, one change per
                   patch (clk, drm/sun4i, panel, pwm, 8250_dw, gamepad, DT), + defconfig
patches/u-boot/    Board DTS + defconfig for mainline U-Boot
board-port/        Board bring-up scripts/configs used day-to-day (not raw patches)
  audio-defaults.sh   sysinit script: quiet mixer levels at boot (the codec resets to full volume).
                      Playback/mixer tools: aplay, amixer, speaker-test from Alpine's alsa-utils
                      (only those three + libasound and /usr/share/alsa are needed).
tools/             Small standalone tools written for this port
  padview/            framebuffer gamepad viewer (buttons, sticks, last events)
  devmem.c            minimal statically-linked /dev/mem peek/poke tool
  i2creg.c            I2C register peek/poke (used on the AXP717 PMIC)
  regsnap.c           nolibc AXP717 + pin-controller register dump, for stock-vs-mainline diffs
  drmtest.c           raw-ioctl DRM legacy-modeset test tool (no libdrm dependency)
  uart_session.py     holds the UART console open; commands go in through a FIFO
  uart_push.py        md5-verified file transfer over the UART console
  push_kernel.sh      installs a new kernel/DTB on the SD card over the UART, no card swap
  uboot_test_boot.py  boots a kernel/DTB installed with push_kernel.sh --test once
  reboot_loop.sh      reboot reliability test over the UART
  test_gadget.sh      minimal USB serial gadget, to test device mode
  backup_restore/     eMMC/RAM backup & restore scripts (no dumps, scripts only)
vendor-notes/      Our own recorded register comparisons/notes (not vendor source/binaries)
```

## What's *not* in this repo, and why

This repo intentionally does **not** vendor the full Linux/U-Boot/ARM-Trusted-Firmware/
BusyBox source trees, nor any binary pulled off the physical device (vendor kernel image,
eMMC/RAM dumps, firmware blobs, the vendor's own decompiled kernel source, the Allwinner
User Manual PDF). Reasons:

- **Size** — those trees are hundreds of MB to GB+ of almost entirely unmodified upstream
  code; a patch series is the normal, reviewable way to publish a board port.
- **Copyright** — vendor kernel binaries, firmware blobs, and a full decompilation of
  vendor's copyrighted kernel are not ours to redistribute. `vendor-notes/` includes only
  our own written observations/register-value notes (facts we measured), never vendor
  source or binaries.
- **Privacy** — raw eMMC/RAM dumps of a specific physical unit can contain device-specific
  identifiers; not included for the same reason the backup/restore *scripts* are provided
  instead of the backups themselves.

The decompiled vendor kernel cross-reference and full blind register dumps that informed
several of these patches were obtained via ADB/UART access to stock firmware plus Ghidra --
reproducible from your own unit, not included here for the reasons above.

## Building

This tracks upstream, so apply the patch on top of a clean checkout at the base commit:

```sh
# Linux — based on v7.2 area, commit 2626025102 (2026-08-21)
git clone https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
cd linux && git checkout 26260251022fbc2f248a3d747a9b2b961b18d2d8
git am /path/to/this/repo/patches/linux/*.patch
cp /path/to/this/repo/patches/linux/trimui-a133-smartpro_defconfig arch/arm64/configs/trimui_defconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- trimui_defconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image allwinner/sun50i-a133-trimui-smartpro.dtb

# U-Boot — commit 6073c36b2c (2026-08-20, v2026.10-rc area)
git clone https://source.denx.de/u-boot/u-boot.git
cd u-boot && git checkout 6073c36b2c8d39afe3ecc789b281667a3ddebc70
git am /path/to/this/repo/patches/u-boot/0001-sunxi-add-trimui-smart-pro-a133-board-support.patch
make trimui-smartpro_defconfig
make CROSS_COMPILE=aarch64-linux-gnu-
```

ARM Trusted Firmware and BusyBox are used untouched (mainline ATF at commit `6a164dd`,
BusyBox at commit `371fe9f` for the minimal test rootfs) — no patches needed there.

Then `board-port/assemble_image.sh` builds a flashable SD image (boot chain + FAT32 boot
partition + your rootfs of choice), and `board-port/update_sd_kernel.sh` swaps just the
kernel/DTB on an already-flashed card for fast iteration. See the script headers for exact
usage and the real, hard-won partition-layout/boot-chain notes (this board's BROM only
boots from a specific eGON offset, and boots SD over eMMC when both are present).

## License

The patches in `patches/` are GPL-2.0(-or-later), matching the Linux kernel and U-Boot they
modify. See [`LICENSE`](LICENSE). Our own original tools/scripts/docs in this repo are also
GPL-2.0-or-later unless noted otherwise.

## Credit

Built on mainline's existing `sun4i`/`sun6i` DRM driver family (originally written by
Maxime Ripard and the linux-sunxi community) and the existing `phy-sun6i-mipi-dphy`
D-PHY driver — this project extends and board-adapts that existing work, it doesn't start
display support from zero.
