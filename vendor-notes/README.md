# vendor_data — artifacts pulled off the real device

Everything here came off the physical TrimUI Smart Pro. Most of it **cannot be regenerated
without booting stock firmware again** (SD card out, device on eMMC, ADB root). Treat as
irreplaceable. All files md5-verified after being moved out of the session scratchpad.

## firmware/ — the blobs the port actually needs
XR829 WiFi: `fw_xr829.bin`, `boot_xr829.bin`, `sdd_xr829.bin` (regulatory/cal), `etf_xr829.bin`
(factory test). XR829 Bluetooth: `fw_xr829_bt.bin`. PowerVR GPU: `rgx.fw.22.102.54.38` +
`rgx.sh.22.102.54.38` (shader binary).

Without these, WiFi/BT and GPU are dead even with correct drivers. They must ship in any final
rootfs under `/lib/firmware/`. No libre replacements exist for any of them.

## partitions/ — raw dumps from the onboard eMMC
- `bootloader.bin` (24MB, mmcblk0p1) — raw boot0/U-Boot image followed by a **FAT16 filesystem**
  (starts at byte 55808) holding `bootlogo.bmp` + `bat/*.bmp` battery animation + `magic.bin`.
  Proof the panel can be driven pre-Linux: U-Boot's own compiled-in display code
  (`sunxi_fb_pan_display`, `display_fb_request`, `allwinner,sun50i-disp`) lights it for the logo.
  A second ground-truth source for display bring-up if the Linux-side fix stalls.
  List it with `7z l bootloader.bin` (mtools rejects it: zero heads/sectors in the BPB).
- `bootlogo.bmp` — extracted from the above, 396x66 32bpp. Usable as a known-good test pattern.
- `env.bin` / `private.bin` (512KB each, p2/p7) — U-Boot environment and vendor private data.

Note these are **eMMC**-specific. Our own CFW boots from SD and does not replicate this layout.
Full verified eMMC image lives separately in `../backups/`.

## devicetree/ — the live, kernel-resolved DT from the running stock system
`live.dtb` pulled from `/sys/firmware/fdt`; `live.dts` is the decompile. More authoritative than
anything extracted from a firmware image, since it is exactly what the working kernel resolved.

Regenerate the .dts with:  `dtc -f -I dtb -O dts live.dtb > live.dts`
(`-f` is required — vendor uses indexed property names like `dram_para[00]` that dtc's strict
parser rejects.)

This is the source for: panel timings, DRAM parameters, WiFi/BT GPIO wiring, the LEDC RGB
controller, camera sensors, audio codec. See `../board-port/future_features_notes.md`. Also the
source for the real display interrupt number (`disp@06000000`'s third interrupt cell, raw SPI
68) — see `live_reg_dump_20260823.md`. **Correction (2026-09-07): this was a real, necessary fix,
not the bug's actual root cause** — the black screen is still unsolved as of the top of
`../CURRENT_STATE.md`; this fix just corrected one genuine bug (the wrong IRQ number) found along
the way.

## live_reg_dump_20260823_mixer.md — mixer/blend/channel/TCON-TOP live capture, 2026-08-23
Second stock-boot pass, covering the entire pixel path (mixer0, blend unit, channel/layer regs,
TCON-TOP routing mux) that the first pass didn't touch. Corrects an easy mistake: mixer0's real
base is `0x6100000`, not `0x6000000` (see the file for why). Conclusion: no bugs found anywhere
in this path — everything matches what mainline's driver already does. Combined with the first
pass's TCON0/DSI/DPHY results, essentially the entire static register configuration of the pixel
pipeline has now been checked against live hardware.

## live_reg_dump_20260823.md + mmapread.c — live register capture, 2026-08-23
Full TCON0/DSI/DPHY register dump pulled from the *working* stock system over ADB, compared
register-by-register against the same addresses read live off our (at-the-time broken) mainline
build. This is what found the wrong TCON0 interrupt number and disproved an earlier incorrect
`VIDEO_MODE` fix. `mmapread.c` is a minimal static mmap-based `/dev/mem` reader — needed because
this kernel's `/dev/mem` only supports mmap() access, not read(), for MMIO addresses (busybox has
no `devmem` applet either). Cross-compiled with the same `aarch64-linux-gnu-gcc` toolchain used
for kernel builds; push to `/data/local/tmp/mmapread` on the device, run as `mmapread <hex_addr>`.

## kernel_re/ — vendor kernel reverse engineering
- `vendor_kernel.elf` — the vendor 4.9.191 kernel converted to a symbolized ELF with
  `vmlinux-to-elf` (~34,900 symbols recovered, load base `0xffffff8008080000`). This is the
  expensive artifact; keep it.
- `vendor_symbolized.txt` — full `objdump -d` of the above. Regenerate any time with:
      aarch64-linux-gnu-objdump -d vendor_kernel.elf > vendor_symbolized.txt
- `tcon0_cfg_full.txt`, `tcon0_cfg_mode_auto_full.txt`, `dsi_cfg_full.txt` — extracted
  disassembly of the display-path functions the black-screen work is built on.
- `sunxi_udc_probe.txt` — USB device-controller probe disassembly (from the abandoned UDC
  reverse-engineering detour; ADB via stock firmware turned out to be the far better route).

Method and findings are written up in `../vendor_kernel_reverse_engineering.md`.

## logs/
`boot_stock.log` — full stock firmware boot over UART. Contains the vendor's own clock report
(`dclk(69000000)` requested -> `clk real: dclk(102000000)`), which is what proved the CPU-mode
divider scaling behind the display fix.

## trimui_inputd
The stock closed-source input daemon, **unstripped** (29KB, aarch64). Buttons and both analog
sticks arrive over UART (`/dev/ttyS3`+`/dev/ttyS4`) from a sub-MCU, not GPIO — recovered symbols
include `pk_frame_to_axis_x/y`, `dump_cmd_frame`, `pk_read_cal_config`, per-side calibration
state. Reimplementing this as an open serdev driver is the path to gamepad support.

## Deliberately NOT kept from the scratchpad
- `vendor_disasm.txt` (111MB) — unsymbolized disassembly, fully superseded by
  `vendor_symbolized.txt`.
- Our own test-boot UART logs (`boot_realfix.log` etc.) — historical debugging of hypotheses that
  are now resolved or disproven; the conclusions are captured in the writeups.
- `*.b64` transfer artifacts, `chunk_test.gz`, `venv/`, `p1check.bin` — throwaway.
