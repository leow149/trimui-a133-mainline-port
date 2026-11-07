# VP9 decode bring-up (Hantro G2) — findings and plan

Status: investigated 2026-11-06, not yet implemented. This is roadmap item **1.2**.

## TL;DR

VP9 is the easiest of the remaining "hard gaps" because the mainline `hantro` (VeriSilicon) driver
**already supports the Allwinner G2 VP9 decoder** — it just isn't wired up for the A133. The work is
a DT node plus a shared-reset fix, not a new driver.

## What's already there

- `drivers/media/platform/verisilicon/hantro_drv.c` has
  `{ .compatible = "allwinner,sun50i-h6-vpu-g2", .data = &sunxi_vpu_variant }`.
- `sunxi_vpu_hw.c` defines `sunxi_vpu_variant` as a **VP9-only decoder** (`codec =
  HANTRO_VP9_DECODER`, ops `hantro_g2_vp9_dec_run/done`), clocks `{"mod","bus"}`, `legacy_regs=1`,
  `double_buffer=1`, `late_postproc=1`. `sunxi_vpu_hw_init()` sets the `mod` clock to 300 MHz.
- The A133's "google-vp9" (`live.dts` `vp9@01c00000`, `allwinner,sunxi-google-vp9`) is the **same
  Hantro G2 IP** the H6 uses — same address `0x01c00000`, same IRQ `GIC_SPI 90`.

## The one A133-specific complication: shared VE clock/reset

The H6 gives the G2 its own CCU domain (`CLK_BUS_VP9`, `CLK_VP9`, `RST_BUS_VP9`). The A133 does
**not** — verified against the A133 manual (CCU chapter) and `ccu-sun50i-a100.c`:

- Only one VE clock register exists: `VE_CLK_REG` @ `0x0690`.
- Only one VE reset/gating register exists: `VE_BGR_REG` @ `0x069C` (`VE_RST` bit 16, `VE_GATING`
  bit 0).
- There is no VP9 clock or reset register anywhere.

So the VP9 G2 shares the Cedar VE's clock (`CLK_VE`/`CLK_BUS_VE`) and reset (`RST_BUS_VE`). The
Cedar `cedrus` driver currently holds `RST_BUS_VE` **exclusively**
(`devm_reset_control_get(dev->dev, NULL)` in `cedrus_hw.c`), and `hantro` also uses an exclusive
array get (`devm_reset_control_array_get_optional_exclusive`). Two exclusive holders of one line →
the second probe fails `-EBUSY`.

## No SRAM needed

The manual lists SRAM C as borrowed by **VE (112K) and DE (20K) only** — VP9 is not mentioned. The
`hantro` driver has no SRAM handling either. So the A133 VP9 G2 is self-contained (internal SRAM),
same as the H6 G2. No `allwinner,sram` property.

## Implementation plan

### 1. DT node (`sun50i-a100.dtsi`, next to `video-codec@1c0e000`)

```dts
video-codec-g2@1c00000 {
    compatible = "allwinner,sun50i-h6-vpu-g2";
    reg = <0x01c00000 0x1000>;
    interrupts = <GIC_SPI 90 IRQ_TYPE_LEVEL_HIGH>;
    clocks = <&ccu CLK_BUS_VE>, <&ccu CLK_VE>;
    clock-names = "bus", "mod";
    resets = <&ccu RST_BUS_VE>;
    status = "disabled";
};
```

**No `iommus`** — the A133 IOMMU has no VP9 master (confirmed from the manual §3.11 table:
Master0=DE0, 1=Reserved, 2=VE, 3=CSI, 4=ISP, 5=G2D, 6=Reserved). The stock DT's `vp9` node also
omits it. The `docs/gpu-video.md` "needs its IOMMU master" note is wrong on this point.

Then `status = "okay";` in the board DT.

### 2. Shared reset (the real work)

The two drivers use **incompatible reset idioms**, which is why this isn't a one-line change:

- `cedrus` uses the *pulse* idiom: `reset_control_reset()` on resume/watchdog-timeout and
  `reset_control_assert()` on suspend (`cedrus_hw.c`).
- `hantro` uses the *deassert* idiom: `reset_control_deassert()` at probe and
  `reset_control_reset()` in `sunxi_vpu_reset` (decode-error recovery).

`reset_control_reset()` on a **shared** line is guarded: if `deassert_count != 0` (i.e. another
holder has it deasserted) it `WARN_ON`s and returns `-EINVAL` (`drivers/reset/core.c`). So naively
making both getters `*_shared` breaks `cedrus`' resume (hantro's probe deassert trips the WARN).

Correct shape (needs hardware verification):

- `cedrus`: `devm_reset_control_get` → `devm_reset_control_get_shared`; change resume's
  `reset_control_reset` → `reset_control_deassert` (equivalent, since suspend asserts first). The
  watchdog-timeout `reset_control_reset` stays but will only actually fire the first trigger.
- `hantro`: use a shared get for the sunxi variant (`devm_reset_control_array_get_shared`).

Because `deassert_count` is per-`struct reset_control` instance (not a global line refcount), the
framework relies on deassert/assert being idempotent register writes; the cross-driver correctness
here must be verified by decoding H.264/H.265 and VP9 back-to-back on hardware, not assumed.

### 3. Clock rate

`cedrus` sets `CLK_VE` (`mod`) to 320–600 MHz per its variant; `hantro` sets it to 300 MHz. Same
underlying clock, so the last `clk_set_rate()` wins. Only one decoder is active at a time, so this
is likely fine, but worth checking `clk_summary` after each.

### 4. IOMMU master (needs confirmation)

The H6 G2 uses `iommus = <&iommu 5>`, but `docs/gpu-video.md` records the A133 IOMMU masters as
"0 = display, 2 = VE, 3 = CSI, 4 = ISP, 5 = G2D" — no VP9 entry, and the stock DT's `vp9` node has
**no `iommus` property at all**. Two possibilities: (a) the A133 VP9 is not behind the IOMMU (drop the
property), or (b) it is, with a master number to determine from the A133 IOMMU chapter. **Confirm
from the manual's IOMMU section before finalising.** (The G2D entry at master 5 suggests the A133
numbering differs from the H6's.)

## Verification

Build, push, then:

```sh
# does hantro bind?
dmesg | grep -i hantro
ls /dev/video* /dev/media*
# decode a VP9 clip, stateless V4L2, compare vs FFmpeg software (same pattern as tests/video/ve_test.sh)
```

`CONFIG_VIDEO_HANTRO=m` is already in the defconfig, but the rootfs has no module auto-loading, so
either load it manually or flip it to `=y` in a follow-up if VP9 is wanted at boot.

## Result (2026-11-07): no G2 on the A133, patch 0061 reverted (0066)

Hardware testing showed the A133 has no Hantro G2:
- The whole 0x1c00000-0x1c0dfff window reads as zero, also the G2 version register and registers the driver has just written.
- That holds with VE_CLK/VE_BGR on and the block out of reset, with every VE_BGR bit set one by one, with all eight Cedrus VE_MODE values, and with MBUS_VE added as a clock.
- Cedrus at 0x1c0e000 responds normally at the same time.
- The manual's memory map has only the VE at 0x1c0e000, the CCU has no VP9 clock or gate, and VP9 is specified as 720p30 through the VE plus "video control firmware".
- The stock kernel has no VP9 driver; its vp9@1c00000 DT node is a leftover.

Every decode timed out in the hantro watchdog. The WARN in reset_control_reset came from that timeout pulsing the shared reset.

Patch 0066 reverts 0061: no G2 node, and Cedrus has its exclusive reset back. VP9 plays through the CPU decoder (avdec_vp9 / libvpx).

A firmware-assisted VE VP9 path would need Allwinner's userspace libraries (libcedarc and the VP9 firmware) reverse-engineered. That is not planned.

Debug logs: tests/results-20261107/vp9-debug/. Instrumentation: linux branch vp9-debug-wip.
