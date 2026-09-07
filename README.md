# TrimUI Smart Pro — mainline Linux port

A from-scratch mainline Linux (and mainline U-Boot) bring-up for the **TrimUI Smart Pro**
handheld (model TG5020, Allwinner **A133 Plus** SoC), entirely blob-free at the bootloader
level. No vendor kernel, no vendor U-Boot, no Android — this boots pristine upstream Linux
and U-Boot with a small, documented set of board-support patches.

## Status

| Component | Status |
|---|---|
| U-Boot (mainline, blob-free boot chain) | ✅ Working |
| Boot to a shell over UART | ✅ Working |
| DRAM at real, correct timings | ✅ Working |
| Backlight / PWM | ✅ Working |
| Display (panel actually showing pixels) | ❌ **Open bug** — see below |
| GPU, Wi-Fi/BT, buttons, audio, USB | Not yet attempted |

**The one open blocker: black screen.** The panel attaches, the backlight turns on, but no
pixels ever reach it. Extensive investigation has narrowed this to a single register:
`TCON0_CPU_IF_REG`'s `TRIGGER_START` bit — a documented self-clearing one-shot that never
clears on this port, despite the entire rest of the pixel pipeline (every static register
value, the driver's control-flow/sequencing, and even measured real-time timing) confirmed
matching a genuinely working vendor system bit-for-bit.

- **[`COMMUNITY_HELP_REQUEST.md`](COMMUNITY_HELP_REQUEST.md)** — a technical summary of the
  specific open bug: what's confirmed working, what's been ruled out and how, and the
  specific open question. Written for anyone with deeper Allwinner/sunxi DSI-controller
  knowledge who might recognize the symptom. **This is the best starting point if you're
  here because someone linked you this repo about the display bug specifically.**

## Repo layout

```
patches/          Our actual code changes, as patches against a named upstream commit
  linux/             — DRM/KMS driver work: TCON, DSI, D-PHY, panel driver, PWM, DTS
  u-boot/             — board DTS + defconfig for mainline U-Boot
board-port/        Board bring-up scripts/configs used day-to-day (not raw patches)
tools/             Small standalone diagnostic tools written for this port
  devmem.c            — minimal statically-linked /dev/mem peek/poke tool
  drmtest.c           — raw-ioctl DRM legacy-modeset test tool (no libdrm dependency)
  backup_restore/     — eMMC/RAM backup & restore scripts (no dumps, scripts only)
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
git am /path/to/this/repo/patches/linux/0001-arm64-allwinner-add-trimui-smart-pro-a133-board-support.patch
cp /path/to/this/repo/patches/linux/trimui-a133-smartpro_defconfig .config
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig Image sun50i-a133-trimui-smartpro.dtb

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
