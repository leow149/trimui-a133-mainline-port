# GPU and video decoding

How the PowerVR GPU and the Cedar video engine were brought up on mainline, what was wrong
on the way, and how it was verified. Patch numbers refer to `patches/linux/` (kernel),
`patches/mesa/`, `patches/u-boot/` and `patches/tf-a/`.

## Summary

| | |
|---|---|
| GPU | PowerVR GE8300 (BVNC 22.102.54.38) on the mainline `powervr` driver, at 696 MHz |
| Vulkan | Mesa's PowerVR driver with 5 patches; Vulkan 1.2 as reported by `vulkaninfo` |
| OpenGL | OpenGL 3.2 core/compat and OpenGL ES 3.1 through Zink (Alpine's Mesa 26.2) |
| Benchmark | vkmark 2025.01, headless, 1280x720: score 265 (208 before patch 0008) |
| Video decode | Cedrus: H.264 and H.265 bit-exact to FFmpeg's software decoders, MPEG-2 within IDCT rounding |
| 4K | 3840x2160 H.264 decodes bit-exact, 26-33 fps depending on the scene; IOMMU, no large CMA needed |

## Hardware

- **GPU**: PowerVR GE8300 (Series 8XE) at `0x01800000`, SPI 97, MIPS firmware processor, 16x16
  tiles, *simple internal parameter format v1*. Its power domain is a PPU block at `0x07001000`.
- **Video engine** (Cedar VE): `0x01c0e000`, SPI 94, IOMMU master 2. Per the A133 manual H.264
  and H.265 up to 4K@30, MPEG-2/VP8 and others up to 1080p@60.
- **VP9 decoder** (Hantro G2, "google-vp9" in the stock DT): `0x01c00000`, SPI 90, 720p@30 per the
  manual. Not enabled yet.
- **IOMMU** at `0x030f0000`, SPI 66: the same IP as on the H6/H616 (register layout identical),
  masters 0 = display engine, 2 = VE, 3 = CSI, 4 = ISP, 5 = G2D. Unlike the H6, the A133 CCU has
  only a bus clock gate for it, no reset line.

## GPU

### Kernel

- **Power domain** (`0046`): a small genpd driver for the A100/A133 GPU PPU, with the power-on/off
  sequence and delays of the vendor kernel. The GPU domain is switched off by runtime PM 50 ms
  after the last job.
- **Device tree** (`0047`, `0048`): GPU node with the `img,img-ge8300` compatible and the PPU as
  power domain. The GE8300 is not in the driver's list of supported cores yet, so it needs
  `powervr.exp_hw_support=1` on the kernel command line.
- **v1 parameter format** (`0049`): the HWRT setup only handled version 2 (2x2 tile groups) and
  page-faulted on the first geometry job. Version 1 uses a single macrotile covering the whole
  render target, as the vendor driver programs it.
- **GPU clock** (`0050`, `0051`): mainline left the GPU at the 432 MHz boot rate of its PLL. The
  stock firmware runs it at 700 MHz; the closest PLL rate is 696 MHz. The stock firmware does so
  with vdd-gpu-sys at 0.9 V; mainline keeps the rail at 1.0 V.
- **App exit with jobs in flight** (`0095`): closing the DRM file unmapped the VM while a job
  still ran, so an app exiting mid-frame page-faulted its fragment job and the firmware reset the GPU
  for every context. The queue kill now waits for the last submitted job.
- **Firmware**: `powervr/rogue_22.102.54.38_v1.fw` from Imagination's
  [linux-firmware fork](https://gitlab.freedesktop.org/imagination/linux-firmware/-/tree/powervr)
  (branch `powervr`), built into the kernel with `CONFIG_EXTRA_FIRMWARE`.

### Mesa

Mesa's PowerVR Vulkan driver knows the GE8300 but only implemented the v2 parameter format.
`patches/mesa/`:

| Patch | Problem | Fix |
|---|---|---|
| `0001` | Macrotile setup and ISP tile alignment asserted format v2 | v1: one macrotile, no 2x2 alignment |
| `0002` | Only the top-left quarter of every render target was rendered | The TA terminate clip is in tile units: 16 px on 16x16-tile cores, not 32 |
| `0003` | Nothing passed the depth test with D16/D24 depth buffers | On v1 cores the ISP compares the background depth as F32 whatever the format |
| `0004` | The display flipped buffers before the GPU had finished them (all PowerVR cores) | Signals of a submission without command buffers now wait for its waits |
| `0005` | High CPU use with dynamic rendering (Zink): every render recompiled and re-created its setup | Device caches for load-op and end-of-tile programs and for render targets |
| `0006` | 2D RGBA8 and 32-bit image stores | Done in software in the shader |
| `0007` | No linear filtering advertised for linear (strided) 2D images | Advertise it |
| `0008` | Low fill rate: no hidden surface removal for any app shader, null-descriptor branches, shader-emulated color write enable, zero-padded sample data | See [gpu-perf-vs-stock.md](gpu-perf-vs-stock.md); RetroArch under cage 29 -> 59 fps |
| `0009` | Transfer clip blits (odd-size uploads, depth/stencil copies, some blits/clears) crashed or hung on v1 cores; Xwayland and Qt Quick apps died | v1 fast-2D control stream and primitive block format, see [gpu-v1-transfer.md](gpu-v1-transfer.md) |

The quarter-coverage bug (`0002`) was found by dumping the region headers: the tiling engine
marked exactly ceil(w/32) x ceil(h/32) headers valid, independent of every other render setting.

`0005` in numbers, as share of one CPU core at 60 fps:

| | Before | After |
|---|---|---|
| `weston-simple-egl` on Zink | 71 % | 22 % |
| cage (Zink) compositing vkcube | 42 % | 14 % |
| vkcube on `VK_KHR_display` | 22 % | 12 % |

`patches/mesa/build_mesa.sh` cross-builds the driver for Alpine (musl) aarch64 in an
`alpine:edge` container: a native stage builds Mesa's shader build tools with LLVM, a cross
stage builds `libvulkan_powervr_mesa.so` with clang and an aarch64 sysroot, with the Wayland and
X11 window systems enabled (needed by Zink).

### Display glitches under GPU load: DRAM arbitration

With the GPU rendering, the panel showed flickering lines even for CPU-drawn content: the TCON
reported a frame-transfer underflow (`GINT0` bit 9) on about half the frames of vkcube. Since the
A133, DRAM port arbitration is done by a block called NSI (`0x03100000`, not in the manual),
which nothing programmed: all masters had the same priority. The stock firmware sets the display
engines to high priority. `patches/u-boot/0002` does the same in U-Boot's DRAM init (a port of
a pending upstream U-Boot series). Underflows during vkcube: 861 in 30 s before, 0 after.

### OpenGL and the Wayland session

OpenGL and OpenGL ES run on top of the Vulkan driver through Zink. GBM on the display-only card
can't pair it with the GPU, so the setup is a Wayland compositor: `rootfs/root/wayland-session.sh`
starts cage (wlroots) rendering on the GPU's render node, scanning out on the display engine, with
the output rotated to landscape. Vulkan apps use the driver's Wayland WSI directly.

### Measurements

- vkmark with patch 0008: score 265 (desktop 214, texture 370 fps). Before:
- vkmark 2025.01, `--winsys headless -s 1280x720`: score 208 (vertex 262, texture 242, shading
  108-187, blur 37, desktop 81, cube 357, clear 609 fps). No GPU errors; GPU 49 to 60 °C.
- `weston-simple-egl -f -b` (Zink, no vsync, in cage): about 240 fps at 65 % of a CPU core.

## Video decoding

- **VE SRAM** (`0056`): on the A133 the video engine gets its SRAM with SYS_CFG `0x03000000`
  bit 0 *cleared*, as on the H616 (the vendor driver does the same). The generic H6-style claim
  sets it instead; the bitstream unit then never became idle and the decoder hung.
- **Bounded wait** (`0052`): Cedrus' H.264 setup polled the bitstream unit without a timeout, so
  that hang was an unkillable kernel loop.
- **IOMMU** (`0057`-`0060`): the display engine and the video engine are behind the IOMMU.
  Decoder buffers come from normal memory: 4K worked with the default 64 MB CMA (more than 256 MB
  of CMA were needed without the IOMMU), and the display engine scans them out without a copy.

Verified with GStreamer's stateless V4L2 decoders against FFmpeg (`gst-libav`), comparing the
checksum of every decoded frame (`tests/video/ve_test.sh`):

| Clip | Result | Decode speed (VE) |
|---|---|---|
| 720p H.264 High, 150 frames | 150/150 identical | 231 fps |
| 720p H.265 Main, 150 frames | 150/150 identical | 300 fps |
| 720p MPEG-2, 150 frames | > 69 dB PSNR, max. difference 2 (IDCT rounding) | |
| 2160p H.264 High@5.1, synthetic, 90 frames | 90/90 identical | 30.9 fps |
| 2160p H.265 Main, synthetic, 90 frames | 90/90 identical | 48.9 fps |
| 2160p H.264, 60 s of *Big Buck Bunny* (7.5 Mbit/s, B-frames) | 1802/1802 identical | 26-33 fps |

Playback goes zero-copy to the display: `kmssink` imports the decoder's buffers and the display
engine's video plane scales them (3840x2160 to 720x405 on the portrait panel). The plane must be
put above the console's primary plane (`plane-properties="s,zpos=(int)3"`).

4K H.264 is at the limit of the hardware: the A133 manual specifies 4K@30 only with AFBC output and
the second scaler, which mainline Cedrus doesn't use.

## Tools

In `tools/gpu/`:

| Tool | Purpose |
|---|---|
| `vktest.c` | Offscreen Vulkan test: clear, triangles, depth formats, linear/optimal images, readback |
| `vkmulti.c` | Renders N frames pipelined into separate images and checks each |
| `fliptest.c` | KMS page-flip test with a moving bar, to see tearing and underflows |
| `fbgrab.c`, `colgrab.c` | Read the scan-out buffer (full frame, or one column at a high rate) |
| `uflow.c` | Counts TCON underflow events (polls `GINT0` every millisecond) |
| `regwatch.c` | Counts changes of registers, e.g. the display layer address to measure real frame rates |
| `regdump.c`, `regsample.c` | Register dumps and sampling for stock-vs-mainline comparisons |

The register dumps behind these comparisons are in `vendor-notes/gpu_regdump/`.

## Open points

- VP9: the Hantro G2 shares clock and reset with Cedrus, which pulses the reset; needs a shared
  reset in both drivers and its IOMMU master.
- Video encoding: there is no mainline encoder driver for this video engine.
- GPU frequency scaling (assessed 2026-11-07, not implemented):
  - `powervr` has no devfreq. vdd-gpu-sys is shared, so only the clock could drop, at a fixed voltage. That saves no energy per frame, and the GPU already powers off when idle.
  - A live clock change would need the firmware dance: force idle, change the clock, send `ROGUE_FWIF_KCCB_CMD_CORECLKSPEEDCHANGE`. The `core_clock_speed` in the runtime config is only set at probe.
  - The thermal case for it was checked with a 5 min soak (4 busy CPU cores + vkmark headless). With the thermal trips from patch 0067, the die levels at CPU 83-85 C / GPU 83 C through CPU throttling alone. Before 0067 it was 88/86 C and rising.
  - Caveat: these soaks ran with the back cover off and the stock SoC cooling removed (UART development setup). With a temporary heatsink, the same load reaches CPU 83 C / GPU 80 C at 5 min, still slowly rising. Re-measure with the stock cooling and back cover in place.
  - So a GPU cooling device is not needed for now. Revisit if a GPU-only load (CPU idle) heats past the 88 C GPU trip.
- `vkcube --wsi wayland` kept running after `kill cage`: on SIGTERM cage waits for its app without stopping it. Not a driver issue. Fixed in our launcher: `wayland-session.sh stop` ends the app, and cage then exits (2026-11-08).
