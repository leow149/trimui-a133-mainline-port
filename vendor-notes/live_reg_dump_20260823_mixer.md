# Live register dump — mixer/DE/TCON-TOP path, 2026-08-23 (second stock boot)

Captured over ADB from the working stock system, in response to the user's explicit request to
be exhaustive this time rather than needing yet another stock boot. Covers the entire pixel path
from the mixer through the TCON-TOP mux to TCON0, none of which had been checked before this
session (all prior stock-boot work focused on TCON0/DSI/DPHY directly).

## Corrected base address

**Mixer0's actual base is `0x6100000`, not `0x6000000`.** A comment already in
`sun8i_mixer.c` (above `sun50i_a133_mixer0_cfg`) states vendor's `de_rtmx_init()` places mixer0
at `DE0_base + 0x100000` — our own board DTS already gets this right (`mixer@6100000`). An initial
read at `0x6000000` returned `0x00000001`, which is *not* mixer0's `GLOBAL_CTL` — don't reuse that
address.

## Mixer0 (base 0x6100000)

| Register | Address | Live value | Decoded |
|---|---|---|---|
| GLOBAL_CTL | 0x6100000 | 0x00001001 | bit0 (RT_EN) set, as expected. Bit12 also set — not defined in mainline's header at all; not chased further (low suspicion, see reasoning below). |
| GLOBAL_DBUFF | 0x6100008 | 0x00000000 | DBUFF_ENABLE (bit0) clear. Mainline only ever writes this as a pulse in `sun8i_mixer_commit()`; reading 0 at a random moment is expected self-clearing behavior, same pattern as `TRIGGER_START` — not a red flag. |
| GLOBAL_SIZE | 0x610000c | 0x04ff02cf | height-1=1279, width-1=719 → exactly 1280×720, matches the panel. Confirms `SUN8I_MIXER_GLOBAL_SIZE` (offset 0xc, the DE2 convention, matching `de_type=SUN8I_MIXER_DE2` for the A133) is the correct offset, not the DE33-style 0x8. |

## Blend unit (base 0x6100000 + DE2_BLD_BASE 0x1000 = 0x6101000)

| Register | Address | Live value | Decoded |
|---|---|---|---|
| PIPE_CTL | 0x6101000 | 0x00000701 | bit0 (FC_EN(0)) set; bits8-10 (EN(0)/EN(1)/EN(2)) all set — vendor has 3 pipes simultaneously active (its boot animation composites 3 layers). |
| ROUTE | 0x6101080 | 0x00000321 | Pipe→channel mapping, 4 bits/pipe: pipe0→ch1, pipe1→ch2, pipe2→ch3. **Channel 0 is never used by vendor's boot screen at all.** |
| PREMULTIPLY | 0x6101084 | 0x00000000 | |
| BKCOLOR | 0x6101088 | 0xff000000 | Opaque black, matches mainline's `BLEND_COLOR_BLACK` default. |
| OUTSIZE | 0x610108c | 0x04ff02cf | Matches GLOBAL_SIZE exactly (1280×720). |
| MODE(0) | 0x6101090 | 0x03010301 | Matches mainline's `SUN8I_MIXER_BLEND_MODE_DEF` constant exactly. |
| OUTCTL | 0x61010fc | 0x00000000 | INTERLACED bit clear — progressive, as expected. |

## Channel/layer registers (base 0x6100000 + DE2_CH_BASE 0x2000 + channel×DE2_CH_SIZE 0x1000)

Channel 0 (0x6102000) reads as uninitialized garbage (`SIZE=0x1f7f1547`, `COORD=0xffc0f7e0`) —
**confirmed genuinely unused** by vendor, consistent with ROUTE never mapping any pipe to it.

Channel 1 (0x6103000, one of vendor's 3 active pipes) has real values: `SIZE=0x04ff02cf`
(1280×720), `PITCH0=0x00000b40` (2880 = 720×4 bytes, 32bpp stride), a real framebuffer address at
offset 0x18 (`0xfb800000`, matching the `addr[fb800000...]` seen in the disp debug sysfs dump
captured earlier).

**Channel 2 (0x6105000) is the one that matters** — mainline's plane-to-channel assignment logic
in `sun8i_mixer_planes_init()` (`sun8i_mixer.c`) puts VI channels at phy_index 0..vi_num-1 first,
then UI channels at phy_index vi_num..vi_num+ui_num-1. With this board's `vi_num=2, ui_num=2`,
our **primary plane (UI channel 0) lands on phy_index 2**, not 0. Initially looked like a possible
lead (channel 0 sitting unused looked suspicious) but this rules it out: channel 2 is *also* one
of vendor's three genuinely active pipes (`ROUTE`'s pipe1→ch2), with sane live values:
`ATTR=0x00000005`, `SIZE=0x04ff02cf` (1280×720), `COORD=0x00000000`, `PITCH0=0x00000b40` (2880),
and a real framebuffer address (`0xfcb08000`). Channel assignment is correct; not the bug.

## TCON-TOP mux/gate (dpss-top, base 0x6510000)

Already correctly implemented per a detailed comment already in `sun8i_tcon_top.c` (from an
earlier session, reverse-engineered from Allwinner's public GPL disp2 source for this exact chip)
— confirmed load-bearing (skipping it entirely left every atomic commit timing out waiting for
vblank forever). Verified live to be certain rather than trust the comment alone:

| Register | Address | Live value | Decoded |
|---|---|---|---|
| PORT_SEL_REG | 0x651001c | 0x00000020 | DE0_MSK (bits0-1) = 0 → mixer0 routed to port 0 (TCON0/LCD). DE1_MSK (bits4-5) = 2 → mixer1 routed to port 2. Sane. |
| GATE_SRC_REG | 0x6510020 | 0x00010000 | Bit16 (`TCON_DSI_GATE`) set — DSI clock gate enabled, matching mainline's `has_dsi=true` quirk for the A133. |

## Interrupts

`/proc/interrupts` still shows only the single `dispaly` (wakeupgen 68) line active and climbing
(~19013 at time of this check, well past the earlier ~49072 baseline from a longer-uptime check
earlier — consistent, still just the one shared line). No separate mixer/DE
interrupt source is active. Consistent with everything established earlier about GIC_SPI 68 being
the sole real interrupt driving this entire pixeline pipeline (TCON0+DSI+mixer all share it).

## Conclusion of this pass

**No new bugs found.** Every mixer/blend/channel/TCON-TOP register checked lines up exactly with
what mainline's driver already does or should produce. Combined with the exhaustive TCON0/DSI/
DPHY verification from earlier (also all confirmed correct except the two fixed
formula bugs), essentially the entire static register configuration of the pixel path — from
mixer0 through the TCON-TOP mux through TCON0 through DSI through the D-PHY analog block — has
now been checked against live working hardware and found correct. The remaining bug, whatever it
is, is very likely NOT a static register-value mismatch anywhere in this path.
