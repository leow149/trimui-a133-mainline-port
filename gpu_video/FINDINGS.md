# GPU + video engine: research (2026-11-01)

## Hardware (stock DT, stock rootfs from backups/emmc_chunks, extracted to gpu_video/stock/)
- GPU: PowerVR GE8300 (Series 8XE), BVNC 22.102.54.38, 0x01800000, SPI 97, clocks pll_gpu + gpu,
  power domain = PPU block at 0x07001000 (SPI 119, "allwinner,gpu-pd"; same address/shape as D1's
  sun20i-ppu), OPPs 228/400 MHz @0.95 V, 700 MHz @1.1 V.
- Cedar VE (H.264/H.265/MPEG decode, H.264/H.265/JPEG encode): 0x01c0e000, SPI 94, pll_ve + ve,
  behind the Allwinner IOMMU 0x030f0000 (master 2).
- Separate "google-vp9" decoder (Hantro G2) at 0x01c00000, SPI 90.
- Mainline has drivers for the H6/H616 versions of all of these: cedrus (sun50i-h6-video-engine),
  hantro (sun50i-h6-vpu-g2), sun50i-iommu (h6/h616), PPU (sun20i-ppu / h6-prcm-ppu). The A100 dtsi has
  none of the nodes yet.
- Reading VE_VERSION (0x01c0e0f0) with only VE_BGR gate+reset enabled HUNG THE BUS (device needed a
  power cycle). Don't poke VE registers by hand; let a driver bring up clocks incl. MBUS gating.

## Stock software
- GPU: Imagination Rogue DDK 1.19@6345021 (non-WS build): pvrsrvkm.ko (Dual MIT/GPL, 4.9.191),
  dc_sunxi.ko (display class on Allwinner disp2), libGLESv2/EGL/GLES_CM, libVK_IMG (Vulkan),
  libPVROCL (OpenCL), libpvrNULL_WSEGL (presents via the DDK display-class API). glibc 2.33.
- VE: CedarX (libvdecoder, libvencoder + h264/h265/jpeg, libVE, libMemAdapter), kernel cedar_ve built
  into the stock kernel.

## Route A: open stack (mainline powervr + Mesa Vulkan)
- Kernel: in-tree drivers/gpu/drm/imagination, firmware rogue_22.102.54.38_v1.fw published by
  Imagination (unofficial support, Mesa 25.3 timeframe). GE8300 is not in pvr_gpu_support_level ->
  needs powervr.exp_hw_support=1 (or adding the BVNC). MIPS firmware processor path exists.
  Needs: A100 dtsi gpu node + PPU power domain + clocks; binding compatible.
- Mesa (main, 2026-10-29): device_info/ge8300.h present with simple_internal_parameter_format_v1,
  but the driver only implements v2 (assert in rogue_hw_utils.h rogue_get_isp_samples..., plus
  pvr_arch_job_transfer.c / pvr_arch_cmd_buffer.c, ~8 sites). Vulkan only; GL would be Zink.
- pocketforge-os renders on the GE8300 on TSP with Linux 7.2 + powervr + that firmware using a private
  Mesa ("gpu-um-tsp": v1 parameter format + "2x backing mitigation"), not public
  (github.com/pocketforge-os/image issue 275, docs/A133-OPEN-GPU-SHIPPING-PLAN.md).

## Route B: vendor DDK on 7.2
- StarFive published the DDK 1.19@6345021 kernel source (exact same build as our blobs), GPL/MIT,
  with a 6.6 port: starfive-tech/linux JH7110_VisionFive2_6.6.y_devel, drivers/gpu/drm/img/img-rogue.
  It contains hwdefs for 22.102.54.38 (rgxcore_km_22.102.54.38.h, rgxconfig_km_22.V.54.38.h).
- Needed: port 6.6 -> 7.2, write services/system/rogue/sunxi_a133 platform layer (clocks, PPU,
  reset, OPP; RE from stock pvrsrvkm.ko), firmware rgx.fw/rgx.sh.22.102.54.38 from stock.
- Catch: StarFive builds are the WS variant (DRM/GBM/Wayland, no display class); our stock blobs are
  non-WS and present through the display-class bridge (dc_server + DC bridge + a dc_* driver), which is
  NOT in StarFive's tree. Options: reconstruct DC server/bridge from the stock module (symbols present)
  + a small dc driver on top of our DRM/fbdev; or render offscreen with the blobs and copy (slow);
  bridge-table compatibility of libsrv_um vs a differently configured KM must be checked.
- Gives GLES 1/2/3, Vulkan, OpenCL on day one if it works; needs a glibc userland (Alpine is musl:
  glibc chroot/container or switch distro), and stays a closed blob stack.

## Video decode
- Cedrus + Hantro G2 on mainline via H6 compatibles, after adding A100 dtsi nodes (VE, G2, IOMMU),
  checking the VE version/quirks and the A100 CCU VE/IOMMU clocks. Userspace: V4L2 stateless
  (GStreamer v4l2codecs / FFmpeg v4l2-request). Moderate effort, mostly DT + quirks.

## 2026-11-01: open route, kernel side DONE (series 0046-0048)
- New driver drivers/pmdomain/sunxi/sun50i-a100-gpu-ppu.c: vendor PPU sequence (RE'd from stock
  gpu_pd_power_on/off: PSW_ON bits 0-7 1us apart, CLK_GATE, ISO0, switch groups 23,18,16,19..22,17,
  ISO1, RSTN, STATE=0, UPDATE, ACTIVE; off = reverse + PSW_OFF 0xff0000/0xff, STATE=2). The stock
  ppu_irq_handler (hardware-requested transitions via reg 0x28) is not implemented.
- a100 dtsi: gpu_ppu @0x07001000 (R_APB1_PPU clock+reset), gpu @0x01800000 SPI 97, clocks core=CLK_GPU
  sys=CLK_BUS_GPU, RST_BUS_GPU. compatible "allwinner,sun50i-a100-gpu","img,img-ge8300","img,img-rogue"
  (binding not written yet). GPU clock runs at pll-gpu default 432 MHz, no OPPs yet.
- Firmware embedded: CONFIG_EXTRA_FIRMWARE (powervr/rogue_22.102.54.38_v1.fw from
  gitlab.freedesktop.org/imagination/linux-firmware branch powervr, + xr829 files -> WiFi up at boot).
- cmdline powervr.exp_hw_support=1. FW v1.1 build 6976702 boots; FW trace (debugfs pvr_fw/trace_mask
  0x80007ffe) shows KCCB commands processed, idle handling.
- Mesa 26.2.6 (Alpine edge mesa-vulkan-imagination, in a tmpfs chroot /tmp/edge on the device):
  vulkaninfo: "PowerVR Rogue GE8300", Vulkan 1.2 (needs PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1).
  vkcube --wsi display: first geometry job -> GPU page fault, "Innocent lockup", context reset.
  Kernel WARN pvr_hwrt.c:257: hwrt_init_common_fw_structure only knows simple_parameter_format v2
  (2x2 tile-group aligned macrotiles). GE8300 = v1 (rgxconfig_km_22.V.54.38.h:
  SIMPLE_PARAMETER_FORMAT_VERSION 1, tile 16x16, 1 cluster, MIPS FW, SLC 64K).
- DDK 1.19 km takes TEMTILE1/2, MTileStride, ISPMerge*, ISPMtileSize from USERSPACE (rgxta3d.c
  RGXCreateHWRTData), so the v1 rule is in the closed stock UM blobs (libsrv_um / libGLESv2 /
  libVK_IMG) -> next: RE that from gpu_video/stock/rootfs, fix kernel pvr_hwrt.c + Mesa (v1 paths in
  rogue_hw_utils.h, pvr_arch_job_transfer.c, pvr_arch_cmd_buffer.c).

## 2026-11-01 evening: rendering works, but only the top-left quarter
- Mesa main (01bab93) cross-built for Alpine edge aarch64 (gpu_video/mesa-patches/build_mesa.sh: native
  stage for mesa_clc/vtn_bindgen2/pco_clc/pdsc_precomp with LLVM, cross stage with clang+lld+apk sysroot).
  Patch: v1 branch in pvr_arch_rt_mtile_info_init (one macrotile = all tiles), no 2x2 alignment in
  rogue_hw_utils.h for v1 (+ experiment hooks PVR_V1_RT_DOUBLE, PVR_PBE_CLIP_*, PVR_PBE_SIZE_INC).
- Kernel pvr_hwrt.c: v1 branch (one macrotile, tile_max = n-1) + DEBUG module params dbg_*_mul + DBGHWRT
  printk (remove before commit).
- vendor UM decoded (libsrv_um RGXAddRenderTarget -> bridge RGXCreateHWRTDataSet): for 1x:
  TEMTILE1/2 = tx<<18 / ty<<18, TEScreen=(tx-1)|(ty-1)<<12, ISPMtileSize=tx<<16|ty, ISPMerge*=0,
  MTileStride=tx*ty, PPPScreen=(w-1)|(h-1)<<16, RgnHeaderSize=tx*ty (5 B each), TPC n^2*8 (n=pow2 max).
  Ours now identical; registers sampled on GPU match (0xC08/C10/C18/F18, 0xC20/0xF28 = tile count).
- gpu_video/vktest: offscreen test (clear red + green triangle, linear or optimal, readback map).
  Triangle + clear are pixel-exact, NO faults, but only tiles x < ceil(tx/2), y < ceil(ty/2) are rendered
  (64->2, 256->8, 720->23, 1280->40 tiles). Linear image proves it is not the readback.
  vkCmdClearColorImage (transfer job, Mesa-built region headers) covers the FULL image.
- Ruled out (each changes nothing): HWRT te_screen/te_mtile/isp_mtile x2 (verified in registers),
  ppp_screen x2, rgn_header_size x4, whole RT dataset 2x (Mesa), PBE maxclip x2 (clip itself works, pixel
  units), PBE size_x/y +1, render area (coverage = min(render area, image/2)).
- regdump/ (static /dev/mem readers): full register dumps + 0xC00-0xFFF sampling on mainline (vkcube
  720x1280 1x) and stock (opengles_demo 1280x720 4xMSAA, ran over adb with SD removed). Differences in
  ISP_CTL upass_start (stock 0xf, ours 0), unnamed 0xF48 (stock f80007f8, ours fd0221f8), 0xC88
  (0x303 vs 0x202), 0xC28, 0xF30. Not yet compared at equal MSAA. pocketforge's "2x backing" workaround
  likely targets this same symptom.
- Next ideas: stock capture at 1x (MainUI renders 1x?) for a like-for-like register diff; RE the stock
  libGLESv2/libsrv_um 3D kick (frag regs: PBE words, ISP_CTL, bgobj) and compare field by field with Mesa.

## 2026-11-02: GPU works -- vkcube renders correctly (Mesa series in mesa-patches/0001-0004)
(Coordinates note: "top-left quarter" above means image coordinates; the panel is mounted rotated, so on
the screen it appeared top-right.)
- Quarter coverage = TA terminate clip (TA_STATE_TERMINATE0/1 in pvr_arch_framebuffer.c) hard-coded in
  32 px units; 16x16-tile cores read 16 px units. Found by dumping region headers (PVR_DUMP_RGN hook):
  TE marked exactly ceil(w/32)*ceil(h/32) headers valid, independent of every HWRT value.
- Depth tests against the cleared depth failed for D16/D24: ISP_BGOBJDEPTH must be F32 on v1 cores
  (stock writes 0x3f800000 too). D32F worked.
- Mesa queue bug (all cores): signals of a submission without command buffers ignored its waits ->
  WSI imported a stub fence (dma_buf bufinfo "fence:0:0") into the scan-out dma-buf.
- Verified: vktest (clear, triangle, 2 triangles depth, D16/D24/D32F, linear+optimal+copy), vkmulti
  (16 pipelined 720x1280 frames into separate images), scan-out buffers sampled per column during
  vkcube: 2400/2400 clean. Tools: vktest/vkmulti, frames/fbgrab.c, colgrab.c, fliptest/fliptest.c.
- Kernel: series 0049 "drm/imagination: Support the v1 simple internal parameter format" (clean).
- Mesa build: persistent image trimui-mesa-build:latest on bodenserver (CDN stalled once).

## Remaining visible glitches = DISPLAY underrun under memory load (not GPU)
- fliptest (CPU-drawn moving bar, KMS flips, no GPU) is clean idle but glitches (vertical lines) while
  the GPU renders offscreen. TCON GINT0 bit 9 LCD_TRI_UNDERFLOW_FLAG fires only under GPU load
  (2/50 x 20 ms windows), LCD_DEBUG fifo underflow never seen.
- Ruled out with stock register reads (SD removed, adb): MBUS QoS (0x04810000 master cfg all zero on
  BOTH; 0x047fa000 all zero on both), DE clock 300 MHz both, DSI mod clock 150 MHz both, DRAM 792 both.
  Stock GPU runs 702 MHz (ours 432). CCU dumps: regdump/ccu_stock.txt, ccu_mainline.txt.
- Next: mainline drives the DSI panel in TCON CPU/trigger mode with "suspicious" block_space/start_delay
  timing (sun4i_tcon0_mode_set_cpu). Compare with the vendor's DSI/TCON setup (video vs command mode,
  TRI timing, DE/TCON FIFO thresholds) -- see earlier display RE in CURRENT_STATE.md.

## 2026-11-02 (night): display underrun SOLVED -- NSI (DRAM arbiter) port priorities
- A133 moved DRAM port arbitration out of MBUS into the "NSI" @0x03100000 (undocumented in the
  manual; stock DT node `nsi-controller@03100000`, BSP driver). Per port p: base+p*0x200,
  +0x10 mode, +0x14 pri (rd<<2|wr, 0..3), +0x18 qos sel, +0xc0 enable. Ports: CPU GPU SD1 MSTG
  GMAC0 GMAC1 USB0 USB1 NDFC DMAC CE DE0(11) DE1 VE CSI ISP G2D EINK IOMMU CPUS.
- Mainline boot left everything 0 (all LOWEST). BSP: DE0/DE1/CSI/ISP HIGH, IOMMU HIGHEST.
- Measured with gpu_video/display/uflow.c (polls TCON GINT0 bit 9 TRI underflow every 1 ms):
  vkcube 30 s: 861 underflows with DE LOWEST -> 0 with DE HIGH; 60 s vkcube after boot fix: 0.
  TCON TRI0/1/2, SAFE_PERIOD, CPU_IF, DSI HSA/VBLK packet stock values made NO difference.
- Fix: u-boot commit "sunxi: a133: dram: Set the NSI port priorities" (port of the upstream
  U-Boot series by Paul Kocialkowski, not yet in our base). Image built:
  build-artifacts/u-boot-sunxi-with-spl-nsi-20261102.bin (= 10-30 tree incl. uncommitted FEL hook
  + NSI). NOT flashed yet (needs user present). Interim: board-port/nsi-qos.sh as /etc/nsi-qos.sh
  sysinit (inittab backup /etc/inittab.bak-20261102). Gotcha: /root/devmem parses VALUES as hex.
- SD layout fact: our U-Boot lives at sector 16 (8 KB), not 256 as assemble_image.sh says.

## 2026-11-02 (night): GPU at 696 MHz + watchdog
- Stock DT: GPU OPPs 700/400/228 MHz but no supply -> vendor never scales voltage; stock DCDC2
  (vdd-gpu-sys) read 0.90 V with GPU at 702 MHz. Mainline: 1.00 V. Series 0050 (gpu clk
  CLK_SET_RATE_PARENT) + 0051 (board DT assigned-clock-rates 696 MHz on CLK_GPU; CLK_PLL_GPU is
  not in the dt-bindings header). Verified: 30x vkmulti all correct, 5 min vkcube 0 underflows,
  no GPU errors, GPU 55 C.
- Watchdog: CONFIG_SUNXI_WATCHDOG=y, PANIC_TIMEOUT=10, busybox `watchdog -T 16 -t 5` in inittab
  (board-port/inittab). Verified: SIGKILL of the daemon -> board reset after 16 s, boots normally.
- Boot partition (64 MB) cannot hold a test kernel next to the 36.8 MB Image; use
  TEST_KERNEL=/Image tools/uboot_test_boot.py for DTB-only one-shot tests.

## 2026-11-02 (night): video engine (Cedrus) -- probes, decode hung; fix prepared, untested
- DT (WIP, uncommitted, gpu_video/display/WIP-a100-dtsi-video-engine.patch): VE SRAM section +
  video-codec@1c0e000 (H6 fallback compatible, clocks bus-ve/ve/mbus-ve, RST_BUS_VE, SPI 94,
  no IOMMU; CMA raised to 64 MB). Kernel config: MEDIA/VIDEO_DEV built in, STAGING_MEDIA, CEDRUS.
- First try used the generic sun4i-a10-sram-c1 claim (writes SYS_CFG+0 = 0x7fffffff): cedrus
  registered /dev/video0, GStreamer v4l2slh264dec then spun forever in cedrus_skip_bits
  (VE_H264_STATUS VLD_BUSY never cleared; no VE irq ever). sysrq reset -> BL31 "PANIC at PC
  0x40006550" = sunxi_system_reset: the watchdog reset did not happen with the VE stuck, and the
  hardware watchdog did not fire either -> board needs a POWER CYCLE (as with the 11-01 VE hang).
- Vendor sunxi_cedar_probe (vendor_symbolized.txt @ffffff80084fa0f4) unconditionally does
  SYS_CFG+0x0 &= ~BIT(0) and SYS_CFG+0x4 &= ~BIT(24): on A133 bit 0 = 0 maps the VE SRAM to the VE
  (H616 semantics; mainline has "allwinner,sun50i-h616-ve-sram" for exactly that). DT now uses
  "allwinner,sun50i-a100-ve-sram", "allwinner,sun50i-h616-ve-sram". enable_cedar_hw_clk only does
  reset deassert + clk_enable -> nothing else A133-specific in the kernel driver.
- Series 0052: cedrus h264 skip-bits wait bounded with cedrus_wait_for() (was an unkillable spin).
- Test: gpu_video/video/ve_test.sh (+ clips) compares HW vs SW per-frame checksums with timeouts.
  ONLY test with the DTB as one-shot test.dtb (a stuck VE blocks even the reset).

## 2026-11-02 (night): stable WiFi MAC prepared (untested)
- Series 0053 (kernel DT alias ethernet0 = &wifi) + U-Boot commit e05abc51 (same alias as a path
  string in U-Boot's control DT): U-Boot derives ethaddr from the SID, fdt_fixup_ethernet sets
  local-mac-address, xradio already reads it (of_get_mac_address). Needs the new U-Boot.

## MORNING CHECKLIST (device needs a power cycle first)
1. Power cycle; default boot = Image aba222ed... NOT installed yet (SD has the earlier cedrus build,
   default DTB without VE, NSI script + watchdog in inittab).
2. Flash U-Boot (user present): build-artifacts/u-boot-sunxi-with-spl-nsi-mac-20261102.bin at SD
   sector 16 (8 KB) -- that is where the running one is (verified byte-identical to the 10-30 build;
   includes the uncommitted FEL-hook board.c like the running one). Then check NSI regs set without
   the script, ethaddr/local-mac-address present, remove /etc/nsi-qos.sh from inittab.
3. tools/push_kernel.sh (new Image), test-ve.dtb as /test.dtb, TEST_KERNEL=/Image
   tools/uboot_test_boot.py, then gpu_video/video/ve_test.sh.
- 2026-11-02 morning: user VISUALLY confirmed vkcube clean (no lines, no duplication) with the NSI boot script; 0 underflows/30 s.
- 2026-11-02 morning: U-Boot (NSI + wifi alias) FLASHED to SD sector 16 (the SD is /dev/mmcblk2 this
  boot -- always resolve via findfs LABEL=TRIMUIBOOT). Backup of the old boot area:
  backups/uboot_sd/sd_sector16_1MiB_before_nsi_flash_20261102.bin. Verified: NSI table set by
  U-Boot with the boot script disabled (script + inittab line now removed); WiFi MAC
  <SID-derived, locally administered> from local-mac-address, stable across reboots. Default DTB has NO VE node yet.
