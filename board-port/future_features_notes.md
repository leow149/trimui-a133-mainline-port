# TrimUI Smart Pro — data pulled from live stock firmware (ADB root access)

Pulled while stock OS was booted (SD card swap), via `adb shell` + `devmem` + binary pulls.
Stock kernel build `#899`/`TRIMUIDEV@NUC` (Nov 28 2025), 4.9.191, sun50iw10/A133.
Source DT: `/sys/firmware/fdt` on live system → decompiled to `live.dts` (scratchpad).

## Buttons / gamepad input — SURPRISING FINDING: hybrid GPIO+ADC+UART, no single devicetree node

Three separate mechanisms feed into evdev/joystick devices — there is NO single "gamepad"
devicetree node, because the vendor split buttons across three different buses:

1. **`keyboard0`** (`compatible = "allwinner,keyboard_1350mv"`) — ADC-voltage-threshold keys,
   5 entries (`key0`..`key4`), pairs are `<voltage_mv keycode>`. Second value of each pair is a
   real Linux keycode: key1=0x73(KEY_VOLUMEUP), key2=0x72(KEY_VOLUMEDOWN), key3=0x1c(KEY_ENTER/START?),
   key4=0x66(KEY_HOME/menu). key0's pair (`0x1db 0x7372`) is odd — 0x7372 looks like a packed
   combo (bytes 0x73,0x72 = VOLUP+VOLDOWN together, i.e. a two-key-simultaneous shortcut), not a
   plain keycode. Likely covers volume +/-, menu/home, and maybe start.
2. **`aw_gpiokey`** (`compatible = "allwinner,aw_gpiokey"`) — single GPIO IRQ key, decodes to
   **PB0** (bank=1, pin=0 from `aw_gpiokey_int_port = <0x45 0x01 0x00 0x06 0x00 0x01 0x00>`).
   Likely a lid-close/wake or power-adjacent switch.
3. **UART-connected sub-MCU(s)** — the REAL D-pad-adjacent face/shoulder buttons and BOTH analog
   sticks come over two separate serial links, read by a closed-source vendor daemon:
   - `trimui_inputd` (`/usr/trimui/bin/trimui_inputd`, PID seen at runtime, unstripped ARM64 ELF,
     pulled to `scratchpad/trimui_inputd` for offline analysis) opens `/dev/ttyS3` and `/dev/ttyS4`
     (= `uart3`/`uart4`, MMIO `0x05000c00`/`0x05001000` per DTS) plus `/dev/uinput` and `/dev/disp`.
   - Symbol names recovered (binary not stripped): `dump_cmd_frame`, `pk_frame_to_axis_x`,
     `pk_frame_to_axis_y`, `pk_read_cal_config`, per-side state (`s_frame_l`/`s_frame_r`,
     `s_cal_l`/`s_cal_r`, `lastX`/`lastY` ×2, `s_rcv_buf_l`/`s_rcv_buf_r`) — strongly implies
     **two independent analog-stick modules**, each with its own MCU sending calibrated frames
     over its own UART (left stick → ttyS3, right stick → ttyS4, or vice versa), parsed into X/Y
     axes plus button bits.
   - Button name strings found in the binary: `TRIMUI KEY L`, `L2`, `A`, `B`, `X`, `Y`, `R`, `R2`
     — i.e. all four face buttons and both shoulder pairs arrive via this UART protocol, not GPIO.
   - `trimui_inputd` synthesizes the `TRIMUI Player1` virtual joystick (`/dev/uinput` → js0/event3);
     it has no USB VID/PID and doesn't show up in `lsusb`/USB device tree — confirmed virtual/uinput,
     not a real USB HID gamepad despite the input device's Microsoft-range vendor field artifact.
   - `keymon` (`/usr/trimui/bin/keymon`) is a separate consumer daemon that just reads
     `js0`/`event0`-`event3`, not a producer — not the hardware path.
   - Protocol itself (frame format, baud rate, calibration packet layout) has NOT been reverse
     engineered yet — next step if pursuing gamepad support is disassembling `trimui_inputd` (small,
     29KB, unstripped, function names recovered above) or logging raw bytes off ttyS3/ttyS4 with
     `trimui_inputd` killed and a `cat /dev/ttyS3 | xxd` capture during button presses.
   - D-pad specifically not confirmed on either path — may be part of the UART frame's button
     bitmask (unlabeled bits) rather than a separate `TRIMUI KEY` string, or may be ADC-based via
     `keyboard0`'s unexplained `key0` combo entry. Needs live packet capture to resolve.

**Implication for mainline**: full gamepad support needs a new userspace or kernel driver
speaking the `trimui_inputd` UART protocol to `/dev/ttyS3`+`/dev/ttyS4` (mainline uart3/uart4 DT
nodes), NOT a simple `gpio-keys`/`adc-keys` devicetree addition. `keyboard0` and `aw_gpiokey` are
comparatively easy standard `adc-keys`/`gpio-keys` mainline ports for the handful of extra buttons
they cover (volume, menu, PB0 switch).

## GPU
- `pvrsrvkm` kernel module (PowerVR "GPU kernel services", 1.28MB), depends on `dc_sunxi`.
  Confirms this is the Imagination PowerVR blob path — matches standing plan to accept the
  vendor GPU blob rather than write a mainline driver. No new info beyond confirming module names.

## WiFi / Bluetooth (XR829, via `allwinner,sunxi-wlan`/`allwinner,sunxi-bt`)
- Modules: `xradio_core`, `xradio_mac`, `xradio_wlan` (WiFi); Bluetooth via generic
  `hci_uart`/H4/H5(three-wire) + XRadio low-power extension (`bluesleep`, `uart_index=1`).
- `wlan@0` node: `wlan_power1 = "axp2202-bldo1"`, `wlan_io_regulator = "axp2202-aldo3"`
  (voltage `0x325aa0` µV-ish encoded), `wlan_regon`/`wlan_hostwake` both on GPIO bank `0x0b`
  (PL, the always-on PMIC-adjacent GPIO bank) pins 5 and 6 respectively.
- `bt@0` node: same power rails (`axp2202-bldo1`/`aldo3`), `bt_rst_n` on PL pin 2.
- `btlpm@0` (low-power mode / wake lines): `uart_index=1` (→ **uart1**, MMIO `0x05000400`,
  matches XRadio-over-UART Bluetooth being on the SoC's uart1), `bt_wake`=PL pin 4,
  `bt_hostwake`=PL pin 3.
- This is a real, complete power/GPIO wiring recipe for a mainline `sunxi-wlan`+`hci_uart`
  (or a from-scratch xradio driver) port whenever WiFi/BT work is picked up. No mainline XR829
  driver exists upstream per earlier session notes — this would be a from-scratch or
  out-of-tree-port effort either way.

## Camera
- Newly discovered feature (not previously on the roadmap). Full V4L2/ISP capture pipeline present:
  `vind0` (video-in) → `csi@0`/`csi@1` → `mipi@0`/`mipi@1` → `isp@0`/`isp@1` → `scaler@0-3` →
  `actuator@0` (lens AF) / `flash@0`. Kernel modules: `ov5648_mipi`, `vin_v4l2`, `vin_io`,
  `videobuf2_dma_contig`.
- Two sensors defined in DT, **both `status = "disabled"`** (present in silicon/DT but not
  enabled by default — device likely has no populated camera module, or it's a shared design
  with a camera-equipped sibling SKU):
  - `sensor0`: `ov5648_mipi`, rear-facing-style entry (no explicit "front"/"back" but paired
    against sensor1 which is marked `"front"`), i2c addr not captured this pass.
  - `sensor1`: `gc030a_mipi`, `sensor1_pos = "front"`, i2c addr `0x42`, `sensor1_twi_cci_id=2`,
    reset GPIO `0x45 0x04 0x07 ...` (bank=4/PE, pin=7 → PE7), pwdn GPIO PE6.
  - Both share regulators off `axp2202` (`iovdd`/`avdd`/`dvdd` via aldo3/aldo2-ish supplies,
    exact supply phandles `0x4f`/`0x4e`/`0x55`).
- Given `status = "disabled"` on both and no physical camera confirmed on this handheld model,
  this is almost certainly SHARED SoC/DT baseline with another Allwinner-based product, not an
  actual TrimUI Smart Pro feature — **deprioritize unless a camera module is physically found**.

## Audio
- Internal codec: `codec@0x05096000`, `compatible = "allwinner,sunxi-internal-codec"`,
  `status = "okay"` — this is the primary onboard analog audio path (headphone jack, speaker),
  standard `sun8i-codec`-family in mainline terms (would need register/quirk cross-check, not
  done this pass).
- External amp: `acm8625` (I2S/TAS-style smart amp?), `status = "disabled"`, powered from
  `axp2202-aldo2`. Disabled like the camera — likely unused on this SKU or reserved for a
  variant with a bigger speaker.
- Sound card graph nodes present for multiple paths: `sndcodec`, `sndspdif`, `snddmic`,
  `snddaudio0-3` — far more audio routing than this handheld visibly needs; likely generic
  platform baseline, only `sndcodec` (headphone/speaker via internal codec) is realistically
  relevant.

## Other input devices confirmed (not gamepad-related)
- `axp2202-pek`: PMIC power-button, standard.
- `audiocodec sunxi Audio Jack`: headphone insert/remove detection, standard `extcon`-style jack.

## RGB LEDs (addressable, WS2812-style) — NEW, previously missed entirely
- `ledc@0x05018000`, `compatible = "allwinner,sunxi-leds"` — Allwinner's dedicated "LEDC"
  peripheral: bit-bangs a single-wire addressable-RGB protocol (WS2812/NeoPixel-family) out on
  **one pin, PE5** (`ledc_pins_a`, muxsel 5), driving `led_count = 0x17` = **23 individually
  addressable RGB LEDs**, `output_mode = "RGB"`. IRQ 35 (`GIC_SPI 35`), clocked from `clk_ledc`
  + `clk_cpuapb`.
- Full bit timing captured from DT (all in ns): `t0h=400 t0l=850 t1h=800 t1l=450`
  `reset=84 wait_time0=84 wait_time1=84 wait_data_time=600000`. These map directly onto a
  WS2812B-ish protocol and are exactly what a mainline `leds-sunxi`/`ledc` driver (does not exist
  upstream yet — would need writing from scratch, likely closest existing analog is
  `drivers/leds/leds-ws2812b.c`-style bit-bang or a dedicated Allwinner LEDC port) would need.
- Confirmed live: 23 sysfs `led` class devices exist (`sunxi_led0r/g/b` .. `sunxi_led22r/g/b`),
  all currently at brightness 0 (off) on stock, `max_brightness=255` each.
- Real hardware feature, not shared-SoC baseline noise (`status = "okay"`, unlike the disabled
  camera/amp nodes above) — worth prioritizing once display is solved, it's a self-contained,
  well-specified port (single GPIO, real timing params in hand already).

## Firmware blobs — pulled to `scratchpad/firmware/`
Everything referenced by `/lib/firmware` on stock, copied for safekeeping (originals only exist
on the stock rootfs, not in our own build):
- `fw_xr829.bin` (175KB), `boot_xr829.bin` (2.2KB), `sdd_xr829.bin` (744B, regulatory/cal data),
  `etf_xr829.bin` (77.8KB, factory test firmware) — XRadio XR829 **WiFi** firmware set.
- `fw_xr829_bt.bin` (320KB) — XRadio XR829 **Bluetooth** firmware.
- `rgx.fw.22.102.54.38` (118.8KB) + `rgx.sh.22.102.54.38` (404KB) — Imagination PowerVR **GPU**
  firmware + shader header, versioned `22.102.54.38` (matches whatever Rogue-series GPU is in
  this A133 SoC). Required for `pvrsrvkm` to do anything.
- These are the complete "blobs we absolutely need" set: without them, WiFi/BT and GPU are both
  non-functional even with correct drivers/DT — they must ship in any final rootfs under
  `/lib/firmware/`.

## Bootloader / partition layout — pulled to scratchpad

**IMPORTANT CAVEAT: this entire section is eMMC-specific, not SD-card-specific.** The SD card
was physically removed for this whole stock-OS data-pull session (per the user), so every
`mmcblk0`/`/dev/mmcblk0pN` reference below is the **onboard eMMC** (`&mmc2` in our own board DTS),
not the SD card our own CFW boots and runs from (`&mmc0`). Our port's own partitioning is
unrelated to this table — we don't need to replicate it. Likewise, `bootloader.bin` (dumped below)
is the **vendor's own eMMC-resident U-Boot fork** (Allwinner BSP/SDK-based), which may or may not
be the same U-Boot our SD-card CFW runs — its working pre-kernel display code proves the *panel/
TCON hardware* can be driven outside Linux, but isn't necessarily directly reusable code unless
we're running that exact vendor U-Boot ourselves. The register/DT-level findings elsewhere in this
document (TCON, DSI, LEDC, WiFi/BT GPIO wiring, camera, audio codec) are genuine board-level
hardware facts and are NOT medium-dependent — those apply the same whether Linux is booted from
SD or eMMC, since they describe fixed SoC-to-board wiring, not storage-layout.

Full partition table recovered from the stock boot `cmdline` (also matches `/proc/partitions`) —
this describes the **eMMC's own layout only**:

| partition | device | purpose |
|---|---|---|
| bootloader | mmcblk0p1 (24MB) | U-Boot/boot0 + FAT16 boot-resource area (see below) |
| env | mmcblk0p2 (512KB) | U-Boot environment |
| env-redund | mmcblk0p3 (512KB) | redundant U-Boot environment |
| boot | mmcblk0p4 (24MB) | kernel/initramfs |
| rootfs | mmcblk0p5 (560MB) | root — this is `root=` in cmdline |
| rootfs_data | mmcblk0p6 (2GB) | overlay/persistent data |
| private | mmcblk0p7 (512KB) | vendor private data |
| recovery | mmcblk0p8 (16MB) | recovery image |
| pstore | mmcblk0p9 (512KB) | kernel panic/oops persistent storage (`pstore_blk`) |
| UDISK | mmcblk0p10 (4.6GB) | user/game storage |

Block devices are plain `/dev/mmcblk0pN` (also symlinked at `/dev/by-name/<label>`), **not**
under `/dev/block/` on this system.

**`bootloader` partition (mmcblk0p1) dumped whole (`scratchpad/bootloader.bin`, 24MB) — turned
out to be a raw boot0/U-Boot image followed by a FAT16 filesystem** (FAT header starts at byte
55808), containing:
- `bootlogo.bmp` (396×66, 32bpp) — small boot-logo banner U-Boot itself draws pre-kernel.
- `bat/bat0.bmp`..`bat5.bmp`, `bat/low_pwr.bmp`, `bat/battery_charge.bmp` — battery-level and
  charging-animation frames, also drawn by U-Boot before Linux loads.
- `magic.bin` (512B) — likely a boot-animation control/selector flag.

**This is a second, independent confirmation that the display hardware CAN be driven outside
Linux entirely** — U-Boot's own compiled-in display code (strings recovered: `sunxi_fb_pan_display`,
`display_fb_request`/`display_fb_release`, `allwinner,sun50i-disp`) successfully lights the panel
enough to show a logo/battery icon before the kernel ever runs. Not yet disassembled/cross-checked
against the TCON hypothesis (CPU-mode + periodic retrigger) — could serve as a second ground-truth
source if the current Linux-side fix doesn't pan out, or as a source for the raw bootlogo image
itself as a known-good test pattern. Also dumped `env.bin`/`private.bin` (both 512KB) for
completeness, not yet inspected.

## Not yet investigated (lower priority / no obvious signal found)
- SD card / TF socket devicetree wiring (working already via U-Boot/mainline SD boot — not urgent).
- Battery/charging (`axp2202_parameter` battery-model curve was pulled incidentally, present
  above `codec@0x05096000` grep context — full `battery-model.parameter` blob captured in
  `live.dts` around line 6624 if ever needed for charging-curve tuning).
- RTC, thermal, exact I2C addresses for PMIC/other peripherals beyond what's listed above.
