# Live register dump — working stock system, 2026-08-23

Captured over ADB from the stock firmware (booted from eMMC, SD card removed), while the panel
was actively displaying normally. `/dev/mem` here only supports mmap()-based access (its read()
path returns EFAULT for MMIO/peripheral addresses) — `mmapread.c` in this directory is the
minimal static tool used to read it; pushed to `/data/local/tmp/mmapread` on the device.

## The decisive finding: interrupt number

Vendor's live devicetree (`devicetree/live.dts`) declares the combined `disp@06000000` node
(covering mixer0, TCON0, TCON1, DSI+DPHY in one block) with three interrupts:

    interrupts = <0x00 0x45 0x04  0x00 0x46 0x04  0x00 0x44 0x04>;   /* raw SPI 69, 70, 68 */

Live `/proc/interrupts` on stock shows exactly one active display interrupt, climbing at ~60Hz:

    369:  49072  0  0  0  wakeupgen  68 Level  dispaly

Raw SPI 68 — cross-validated against dma0 (raw SPI cell 0x2d=45 in the DT, shown as
`wakeupgen 45` with no offset in `/proc/interrupts`) to confirm vendor's `wakeupgen` numbering
is the raw SPI cell value directly, not GIC hwirq (+32).

**Our board DTS had `tcon_lcd0` on `GIC_SPI 101`** (hwirq 133, matching our own boot log's
"GICv2 133 Level 6511000.lcd-controller" — internally consistent, just pointed at the wrong
physical line). `sun4i_tcon_handler()` was very likely never firing all session, independent of
every register-content fix attempted — fixed to `GIC_SPI 68` in
`board-port/sun50i-a133-trimui-smartpro.dts`.

## Full register comparison

Compared against the same registers read live off our mainline build (TRIGGER_START stuck at
`0x10000007`, same boot as the "stuck at 0" decisive-test entry in `../CURRENT_STATE.md`).

| Register | Mainline (broken) | Vendor (working) | Notes |
|---|---|---|---|
| TCON0_GINT0 (0x6511004) | 0xc8000200 | 0x00000a00 | Vendor: TRI_FINISH_ENABLE (bit27) **and** VBLANK_ENABLE both 0 — stable across 5 rapid samples. Vendor does not use TCON's own interrupt enables for the per-frame drive at all. |
| TCON0_CTL (0x6511040) | 0x810001e0 | 0x81000000 | |
| TCON0_CPU_IF (0x6511060) | 0x10000007 (**TRIGGER_START stuck**) | 0x10000005 (clear — self-clears normally) | |
| TCON0_CPU_TRI0 (0x6511160) | 0x022f02cf | 0x023002cf | BLOCK_SPACE off by 1 (560 vs 561) — minor, not yet investigated further |
| TCON0_CPU_TRI1 (0x6511164) | 0x000004ff | 0x000004ff | exact match |
| TCON0_CPU_TRI2 (0x6511168) | 0x1ae8000a | 0x39ea000a | **START_DELAY very different: 6888 vs 14826.** TRANS_START_SET matches (10). Not yet investigated — candidate for follow-up if IRQ+VIDEO_MODE fixes aren't sufficient alone. |
| TCON0_GCTL (0x6511000) | not captured | 0x80000000 | |
| TCON0_DCLK (0x6511044) | not captured | 0xf0000004 | DCLK_OUT_EN all set, matches our existing fix |
| DSI_CTL (0x6504000) | 0x01010001 | 0x01010001 | exact match |
| **DSI_INT (0x6504004)** | not captured (mainline never uses this register at all) | 0x00020004 | **Enable field bit1 set, status field bit2 set. DSI has its own interrupt controller mainline's driver never touches.** Not wired into our fix (TCON's SPI 68 fix may be sufficient alone, since vendor's combined disp node suggests one shared physical line) — flag for follow-up if SPI 68 alone isn't enough. |
| DSI_BASIC_CTL0 (0x6504010) | 0x00030001 (INST_ST=1) | 0x00030001 (INST_ST=1) | **Exact match — INST_ST=1 is normal/red-herring, same as BLOCK_CURRENT_NUM historically.** Do not chase this. |
| DSI_BASIC_CTL1 (0x6504014) | 0x00000000 (our now-reverted "fix") | 0x000050a7 | **VIDEO_MODE=1, VIDEO_PRECISION=1, VIDEO_FILL=1, VIDEO_ST_DELAY=1290.** Confirms the VIDEO_MODE-clearing fix was wrong; reverted in `sun6i_mipi_dsi.c`. |
| DSI_BASIC_CTL (0x650400c, VIDEO_BURST etc.) | not captured | 0x00000000 | Confirms non-burst path is correct (matches existing mainline code, no change needed) |
| DSI_TRANS_START (0x6504060) | 0x0000000a | 0x0000000a | exact match |
| DSI_TRANS_ZERO (0x6504078) | not captured | 0x00000000 | matches mainline's write of 0 |
| DSI_TCON_DRQ (0x650407c) | not captured | 0x10000027 | DRQ_ENABLE_MODE set, plus a nonzero DRQ_SET value (0x27=39) — mainline's non-burst "magic" branch computes this from hsync porch; not directly cross-checked against 39 |
| DSI_INST_FUNC0-3 (0x6504020-2c) | not captured | 0xf, 0x10000001, 0x20000010, 0x2000000f | not cross-checked against mainline's sun6i_dsi_inst_init() computed values in detail |
| DSI_INST_LOOP_SEL (0x6504040) | not captured | 0x30000002 | |
| DSI_INST_JUMP_SEL (0x6504048) | not captured | 0x63f07006 | |
| DSI_INST_JUMP_CFG0 (0x650404c) | not captured | 0x00560001 | |
| DSI_PIXEL_PH (0x6504090) | not captured | 0x1308703e | |
| DSI_PIXEL_CTL0 (0x6504080) | not captured | 0x00010008 | |
| DSI_SYNC_HSS/HSE/VSS/VSE, BLK_* | not captured | all nonzero, populated | Confirms mainline's sun6i_dsi_setup_timings() non-burst sync/blanking-packet path is the right one in principle (matches vendor using it too) |
| DSI_BURST_LINE/DRQ (0x65040f0/f4) | not captured | 0x00000000 | zero, consistent with non-burst mode |
| DPHY_GCTL (0x6505000) | 0x00000031 | 0x00000031 | exact match |
| DPHY_TX_CTL (0x6505004) | 0x10000000 | 0x10000000 | exact match |
| DPHY_ANA0 (0x650504c) | 0x00000044 | 0x00000044 | exact match |
| DPHY_ANA1 (0x6505050) | not captured | 0x80000020 | |
| DPHY_ANA2 (0x6505054) | not captured | 0x0f000012 | |
| DPHY_ANA3 (0x6505058) | not captured | 0xff040000 | |
| DPHY_ANA4 (0x650505c) | 0x844635ee | 0x844635ee | exact match |
| DPHY_PLL0 (0x6505104) | 0x00f78a82 | 0x00f78a82 | **exact match** — confirms mainline's PLL N/M/P computation (div=8, n=138, P=7) is bit-for-bit correct, matches vendor's own reported `pll(414000000)` |
| DPHY_TX_TIME0-4 (0x6505010-20) | not captured | 0x0a06000e, 0x0a033207, 0x0000001e, 0x00000000, 0x00000303 | not cross-checked against mainline's phy driver computed values |

## Clock rates (debugfs, `/sys/kernel/debug/clk/<name>/clk_rate`)

| Clock | Vendor (stock) | Mainline (live) | Notes |
|---|---|---|---|
| `mipi_host` (DSI "mod" clock) | 150000000 | 150000000 | exact match — mainline's A100 DSI variant never explicitly sets this rate, but it's already correct via clock-tree defaults |
| `tcon_lcd0` (TCON0 channel clock, pre-internal-divider) | 408000000 | not separately re-checked here, but 408MHz÷4=102MHz matches the already-implemented `SUN6I_DSI_TCON_DIV=4` dclk fix and the vendor boot log's `dclk real 102000000` |

## What's still unverified / candidates for follow-up

If the SPI 68 + VIDEO_MODE fixes together aren't sufficient:
1. **TCON0_CPU_TRI2 START_DELAY** (6888 mainline vs 14826 vendor) — over 2x different, formula not yet derived from vendor disassembly (`tcon_get_start_delay` in vendor_symbolized.txt, not yet read).
2. **DSI's own interrupt** (`DSI_INT_REG` @ offset 0x004, enable bit1 set on vendor, completely unused by mainline's driver) — vendor's combined disp DT node suggests this may be routed through the *same* SPI 68 line already, in which case fixing TCON0's number alone may be enough, but this is unconfirmed.
3. TCON0_CPU_TRI0 BLOCK_SPACE off-by-one (560 vs 561).
4. DSI_TCON_DRQ's DRQ_SET value (39) not cross-checked against mainline's computed value for our panel's hsync porch.
