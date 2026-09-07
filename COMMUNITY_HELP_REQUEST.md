# Allwinner A133/A133P: DSI CPU-interface TRIGGER_START never self-clears — asking for a sanity check

Full repo, including the actual driver patches:
**https://github.com/leow149/trimui-a133-mainline-port**

## Board / SoC

TrimUI Smart Pro handheld (model TG5020), Allwinner **A133 Plus** (A100 binning, up to
2.0GHz), 1GiB LPDDR4, 720x1280 MIPI-DSI panel (OTM1289A/ER68576-family panel IC, runtime
ID-detected — the DTB's `lcd_driver_name` says `otm1289a` but the actual panel on this unit
identifies as `er68576`; same vendor panel driver module handles both).

Driver stack: mainline `drivers/gpu/drm/sun4i/sun4i_tcon.c` + `sun6i_mipi_dsi.c` +
`drivers/phy/allwinner/phy-sun6i-mipi-dphy.c`. TCON drives the panel via its **CPU/8080
interface** (command mode, `SUN4I_TCON0_CPU_IF_MODE_DSI`), not video mode — this is a
DSI-command-mode panel behind TCON's parallel CPU-interface path, not the more commonly
exercised DSI-video-mode path.

## The bug

`TCON0_CPU_IF_REG` (offset `0x60` in the TCON0 block), bit 1, `TRIGGER_START`. Per the A133
User Manual: a self-clearing one-shot — "write 1 to start a frame flush... software must
write 1 only when this flag is 0." **On our port, this bit never self-clears — not once, on
the very first attempt, under any configuration.** Register reads it stuck (e.g.
`0x10020007`) indefinitely after being set; the panel's backlight turns on but the screen
stays permanently black.

## Why this looks like a real driver/sequencing bug, not a broken board

We can dual-boot the exact same physical unit — same SoC, same PCB, same panel, same
FPC — into either vendor's stock Android/Tina kernel (from eMMC) or our mainline build
(from SD card). Vendor's stock system drives this exact silicon successfully. So the
hardware is proven capable; whatever's wrong is in our port.

## What's been ruled out, concretely (not just reasoned about)

- **Every static register value** in TCON0, DSI0, D-PHY, DE2/mixer0, and TCON-TOP matches
  vendor's live, working system bit-for-bit — checked via blind register dumps on stock
  Tina Linux, vendor's own live U-Boot console (`md.l`), and a symbol-resolved decompile of
  the actual vendor kernel binary. Three independent sources, all agree.
- **D-PHY lane state is confirmed healthy and identical to vendor's.** The D-PHY debug
  register documented at manual offset `0x10E0` (`direction` bit + per-lane LPTX state)
  reads `0x13755555` on *both* our stuck system and vendor's working one — every lane
  (clock + 4 data) sitting in `HS_ST` (encoding `101`), sustained. This rules out the
  "HS lanes never actually entered high-speed mode, dead electrically" failure mode that a
  closely related SoC generation (T113-S3/D1S, same TCON-TOP/DE2 IP family) is documented
  to have hit — that specific failure mode was confirmed via a scope on that board; ours
  reads identically to vendor's *good* state at this level.
- **Control-flow/sequencing was cross-referenced against a symbol-resolved decompile of
  the real vendor kernel** (not just Ghidra guesses — verified against strings/symbols in
  the actual binary), specifically `disp_al_lcd_tri_start()`, `disp_lcd_event_proc()`, and
  the real panel driver's `lcd_open_flow()`/`lcd_panel_init()`. Two real, byte-provable
  bugs were found and fixed this way — vendor's per-frame DSI retrigger uses a different
  `DSI_INST_JUMP_SEL` table (`0x63f07602`, a continuous streaming loop) than the one-time
  HS-clock-enable step (`0xf02`, a one-shot) uses, and our driver had these swapped; and
  vendor's real panel driver enables the DSI clock **before** sending any panel DCS init
  commands, not after (matching an upstream `sun6i_mipi_dsi.c` `FIXME` that flags this
  exact ordering as suspect but was never resolved). **Both fixes are real, verified
  correct, and built/flashed/tested on hardware — neither changed the symptom at all.**
- **OS-side timing was directly measured, not assumed.** Instrumented the D-PHY TX
  power-on path (`ktime_get()`, backed by the 24MHz architected timer) end-to-end. Every
  `udelay(1)` realizes 1.5-4.5x its nominal request (longer, never shorter); the whole
  D-PHY power-on sequence takes ~22-58us with the PLL getting ~11us of real settling time
  before the function that enables it returns. Nothing here looks starved. (Vendor's own
  disassembled code uses the identical nominal 1us delay with no lock-detect polling
  either, for what it's worth.)
- **A functional divergence *was* found, but it's downstream of the bug, not upstream of
  it**: `TCON0_GINT0_REG` bit 11 (`TRI_FINISH_INT_FLAG`) never asserts on our stuck system,
  but does assert (alongside bit 9, underflow) on vendor's system immediately after a
  manually-triggered, successfully-clearing `TRIGGER_START` pulse from a live U-Boot
  shell — even while `TRIGGER_START` reliably self-clears on vendor's hardware from a
  completely manual, Vsync-unsynchronized write. This says the *outcome* differs, not
  where the cause is.

## The open question

By elimination: static register configuration, driver control-flow/ordering, and
OS-measurable timing are all confirmed matching or otherwise ruled out. What's left is
either (a) something in the *actual electrical/HS-signaling behavior* during a live
transmission attempt — not visible to any register readback, since a status bit only
reports what the digital logic believes it commanded, not what happened on the wire — or
(b) a register/step we genuinely haven't found despite fairly exhaustive searching of the
A133 User Manual (chapter 6, TCON_LCD/DSI, read in full) and the decompiled vendor kernel.

## What we're asking

- **Has anyone actually gotten mainline's `sun4i_tcon.c` CPU/8080-interface DSI path
  (`SUN4I_TCON0_CPU_IF_MODE_DSI`, i.e. DSI command mode via TCON, not DSI video mode)
  working on *any* board?** The only in-tree users we're aware of (Pinephone/Pinetab on
  A64, TBS-A711 on A83T) apparently free-run continuously off `TRI_EN` alone with no
  per-frame software retrigger at all — on older TCON hardware that may simply not need
  one. A133's TCON generation empirically does (bare `TRI_EN`, the untouched mainline
  default, produces this exact black screen). If anyone has a *newer*-generation sunxi
  board with this actually working, a working reference would be extremely valuable.
- **Any known errata or undocumented step for A100/A133-family TCON+DSI CPU-interface
  mode** not in the public User Manual? Happy to share the full register-level comparison
  data if useful.
- **A sanity check on the methodology** — is there a register category, a clock-tree
  detail, or a sequencing step that this write-up's approach wouldn't have caught?

This is a condensed summary of a long-running investigation, not the whole story -- happy to
share more detail on any specific point on request.
