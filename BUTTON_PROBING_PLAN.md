# Finding the real button/joystick wiring — plan for when you have UART access

## Why this needs live hardware

Confirmed from the vendor DTB (`trimui_a133_analysis/dtb_86065152.dts`): the two
DT nodes that *could* describe buttons are both explicitly disabled:

```
aw_gpiokey {
	compatible = "allwinner,aw_gpiokey";
	aw_gpiokey_int_port = <0x45 0x01 0x00 0x06 0x00 0x01 0x00>;
	status = "disabled";
};
```

Only one GPIO is referenced there (a single int_port), status disabled — this
is not the full A/B/X/Y/D-pad/L/R/start/select layout. The only *enabled*
input-ish node is the 3-key ADC "keyboard_1350mv" (volume/fastboot-combo,
confirmed separately). Whatever reads the face buttons on real hardware is
either a hardcoded platform driver (GPIO table baked into vendor C code, not
DT) or something not visible in this particular DTB dump at all. No amount of
further static analysis is going to resolve this — it needs the device
actually running and being probed live.

## What you'll need

- A 3.3V UART-to-USB adapter (TX/RX/GND — do NOT connect the adapter's VCC/5V
  pin, the board has its own power).
- The TrimUI Smart Pro's UART pads — confirmed settings from the vendor kernel
  cmdline: `console=ttyS0,115200` — so once wired up, `115200 8n1` on a
  terminal program (`screen`, `minicom`, `picocom`) is the right config.
- I don't have the exact physical pad locations for this board — that's a
  teardown-photo/continuity-test job on the physical unit, not something
  extractable from the firmware image. Retro-handheld teardown communities
  (GBAtemp, r/RetroSubs, TrimUI Discord) often already have this documented
  for this exact device; worth checking before probing blind with a
  multimeter.

## Once you have a serial console (stock firmware, not our WIP build)

Boot the device normally on its stock/Knulli firmware with UART attached, and run:

```sh
# See what's actually bound to input
cat /proc/bus/input/devices
ls -la /sys/class/input/

# Watch raw GPIO edge changes while you press buttons — best single tool
# for this. Run it, then press each button one at a time, one per line
# of output.
evtest
# (it'll list available /dev/input/eventN devices — pick each in turn)

# If evtest doesn't show button events convincingly, fall back to pinctrl
# state, which shows every pin's current function/pull/level regardless of
# whether a driver claims it as an input device:
cat /sys/kernel/debug/pinctrl/*pinctrl*/pinmux-pins
cat /sys/kernel/debug/gpio

# dmesg from a cold boot will also show whatever driver DOES claim the
# buttons, even if it's not a clean gpio-keys binding:
dmesg | grep -iE "key|gpio|input|button"
```

The goal: for each of A/B/X/Y, D-pad (4 dirs), L1/L2/R1/R2, Start/Select,
joystick clicks — get either (a) a `BTN_*`/`KEY_*` event from `evtest` with a
device name, or (b) a specific GPIO bank+pin number that toggles when you
press it (from the pinctrl/gpio debugfs, cross-referenced against a press).

## What I'll do with that data

Once you have even a partial mapping (even just "button A = PA0, confirmed via GPIO
toggle"), send it over and I'll fill in the `gpio_keys_gamepad` node in
`board-port/sun50i-a133-trimui-smartpro.dts` — same pattern already used for
the Anbernic RG35XX 2024 reference (see that board's dts for the exact
structure: `compatible = "gpio-keys"`, one sub-node per button with
`gpios`/`linux,code`). That part of the file is a placeholder right now
specifically waiting on this.

## Secondary target: FEL-mode boot test

Once UART is wired up (useful for this anyway, to see console output),
the safe next step for the U-Boot build itself is testing over USB FEL mode —
doesn't touch eMMC, so a bad DRAM config just fails to boot rather than
risking anything:

```sh
# on your PC, with the device held in FEL mode (check TrimUI community docs
# for the exact button combo — usually a specific button held at power-on)
sunxi-fel -v spl u-boot-sunxi-with-spl.bin
sunxi-fel -v uboot u-boot-sunxi-with-spl.bin
```

(`sunxi-fel` is part of `sunxi-tools`, typically packaged for most distros —
`pacman -S sunxi-tools` on Arch.) If DRAM training in our placeholder config
is wildly wrong, this will simply hang or fail cleanly rather than doing
anything destructive — that's the whole point of testing via FEL first.
