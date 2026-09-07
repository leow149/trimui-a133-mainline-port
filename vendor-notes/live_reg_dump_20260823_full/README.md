# Full register + driver-state dump, 2026-08-23 (third stock boot)

Comprehensive capture requested explicitly to avoid needing yet another stock boot. Covers full
register ranges (not just the specific offsets checked in earlier passes) plus live driver-state
via `/sys/class/disp/disp/attr/sys` and `/attr/boot_para`, which turned out to be a genuinely
new and valuable source not used in earlier passes.

## Files
- `tcon0_full.txt` -- every register 0x000-0x1FC at TCON0 base (0x6511000), one per line
- `dsi_full.txt` -- every register 0x000-0x30C at DSI base (0x6504000)
- `dphy_full.txt` -- every register 0x000-0x11C at D-PHY base (0x6505000)
- `mixer0_full.txt` -- mixer0 GLOBAL+blend range 0x6100000-0x6101100
- `mixer0_channels_full.txt` -- mixer0 channels 0-3, 0x6102000-0x6106000
- `tcontop_full.txt` -- TCON-TOP mux, 0x6510000-0x651002C
- `attr_sys.txt` / `attr_boot_para.txt` -- disp2's own sysfs debug state, see below
- `dmesg.txt`, `proc_interrupts.txt`, `clk_rates.txt` -- software-side context
- `debugfs_disp_paths.txt`, `sysfs_disp_paths.txt` -- what's discoverable under
  `/sys/kernel/debug` and `/sys` for future reference (dispdbg exists but its `command`/`info`
  files didn't yield anything useful with a plain `help` write)

## Key findings

**`attr/sys` gives a direct live dump of vendor's own disp2 driver state** (no register
interpretation needed): `err:1  skip:58  irq:31486  vsync:0  vsync_skip:0`, three active
mgr0 layers (channels 1/2/3), 60fps confirmed. Both `err` and `skip` were re-checked ~90s apart
(two `attr/sys` reads) and **did not increase** -- both are one-time/transient events from early
boot settling, not ongoing conditions. `irq` climbed steadily between the two reads (~10700 over
the interval), confirming the interrupt fires continuously and matches `/proc/interrupts`'
`dispaly` line.

**`skip:58` corroborates the `disp_lcd_event_proc()` busy-skip counter traced earlier this
session** -- some transient busy did occur (consistent with vendor's own code tolerating it), but
only during startup, not persistently.

**Direct contradiction not yet resolved**: `TCON0_GINT0` reads `0x00000a00` -- zero bits set in
the 24-31 "enable" range that `SUN4I_TCON0_TRI_FINISH_ENABLE`/`TRI_COUNTER_ENABLE`/`VBLANK_ENABLE`
all occupy per mainline's (externally well-established, used successfully on many other mainline
Allwinner boards) header -- confirmed stable across 10 rapid consecutive reads AND this full dump.
Yet the interrupt fires continuously (irq counter climbing). Vendor's own `tcon_irq_query()`
(real source, `de_lcd.c`) requires `enable & flag & (1<<id)` to be nonzero for the handler to
treat any event as real -- meaning if GINT0's enable bits are genuinely all zero, vendor's own
`disp_lcd_event_proc()` ISR runs on every interrupt (hence the continuously-climbing raw IRQ
count) but its internal query logic should always see nothing pending, and would never actually
call `disp_al_lcd_tri_start()` for this panel. This is consistent with `TCON0_CPU_TRI3/4/5`
reading `0x00000000` (the TE/counter-retrigger config block that source only populates when
`lcd_fresh_mode==1`, which per the same source doesn't happen for a DSI **video**-mode panel).
**Not fully resolved**: what interrupt condition is actually asserting the physical GIC line
continuously if GINT0's own enable bits are off. Candidates not yet checked: whether GINT0's
enable/status half convention is genuinely reversed on this SoC generation vs. the older chips
mainline's header was written against (untested); whether the interrupt is actually sourced from
outside TCON0's GINT0 entirely (e.g. mixer/DE), though mainline's own mixer driver has no IRQ
handling at all to cross-check against.

**Confirms TCON0_CTL bit24 (`SUN4I_TCON0_CTL_IF_8080`) is set** (`0x81000000`) -- TCON0 genuinely
operates in CPU/8080-interface mode for this panel despite it being DSI video mode, matching
mainline's own dispatch (mainline never branches CPU-vs-HV mode on DSI video/command submode at
all -- it always uses CPU-interface wiring for any `sun6i_mipi_dsi`-driven panel). This part of
mainline's architecture was never in question; the earlier DSI video/command submode confusion
traced back to a cross-generation (H3-era) BSP source whose *policy* logic doesn't directly
apply to A133, even though its register-level facts (interrupt ID enum, bit positions) have
held up in every other case checked.

**`CPU_TRI0/1/2` match already-known values** (`block_space`=561, matching our own fix;
`start_delay`=14826, matching our own formula fix) with one new detail: `CPU_TRI1` reads
`0x002c04ff`, not just `0x000004ff` as earlier captures implied -- the upper 16 bits (`0x002c`)
are set to something mainline's `BLOCK_NUM`-only interpretation of this register doesn't account
for at all. Not yet identified; flagged for follow-up, not chased further this pass.

**`SUN4I_TCON_ECC_FIFO_REG` (0xfc) is nonzero** (`0x20004000`) on the live system -- per the real
vendor source this field is normally only set in the CPU-interface/DSI-command branches of
`tcon0_cfg()`, another data point consistent with A133 handling this differently than the H3-era
source describes, OR simply confirming ECC/FIFO settings apply broadly regardless of DSI submode.

Nothing in this pass points to a clean, confirmed root cause. It closes off several previously
open threads (BLOCK_SPACE, periodic retrigger, DSI_INT status bits, TRI_COUNTER-via-panel-flag,
the idle-reset step) as either already-matching or not applicable, and narrows the puzzle to: what
actually drives TCON0's continuously-firing interrupt on vendor's system if GINT0's own enable
bits are genuinely off, and why does the black screen persist even though DSI's own protocol-level
completion signals (INSTR_END, VIDEO_VBLK) already latch correctly on our own broken build.
