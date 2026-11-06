# Bluetooth (XR829) bring-up — status

Status: **kernel + userspace implemented 2026-11-06; firmware download (BROM sync) not yet working.**
This is roadmap item **1.1**.

## What's done

- **Kernel** (patch `0062`): UART1 (PG6-9) enabled, `rfkill-gpio` for BT_RST_N (PL2),
  `btlpm` node for BT_WAKE/BT_HOSTWAKE (PL4/PL3), and the `xradio_btlpm` sleep/wake driver
  (provides `/proc/bluetooth/sleep/{lpm,btwake}`). `CONFIG_BT=y`, `BT_HCIUART=y`,
  `BT_HCIUART_H4=y`, `RFKILL_GPIO=y`, `XR_BT_LPM=y`. Verified on hardware: `/dev/ttyS1` up,
  rfkill + btlpm proc entries present, PL2/PL4 driven correctly.
- **Userspace**: bluez 5.54 (xradio branch) built in the edge chroot; `hciattach` + `hciconfig`
  with the `xr829` protocol. The firmware download is done in userspace (a "BROM" command
  protocol over UART1: 0x55 sync → SETUART → 1KB SEQWR chunks → SETPC jump → HCI reset/bdaddr).

## Open issue: the BROM sync never gets "OK"

`hciattach /dev/ttyS1 xr829` runs, power-cycles the BT via rfkill, asserts bt_wake, then sends
0x55 and reads `00 00` instead of `OK`, forever (even after the 500 ms power-cycle workaround).

Ruled out so far: UART1 pins (PG6=TX/PG7=RX/PG8=RTS/PG9=CTS, matches stock), BT power (bldo1
3.3 V on), reset polarity (GPIO_ACTIVE_HIGH: blocked→PL2 LOW, unblocked→PL2 HIGH, matching the
vendor `sunxi_bt_set_block`), bt_wake asserted (PL4 HIGH), 32k clock (same `CLK_OSC32K_FANOUT`
as WiFi, enabled by pwrseq), UART clock (24 MHz bus clock, identical to the working uart0/3/4).

Still to investigate: a scope/logic analyzer on the UART1 TX/RX lines (is the SoC actually
transmitting 0x55, and is the BT echoing anything), and whether the XR829 BT core needs a
power-on sequence the rfkill-gpio isn't reproducing.

## Wiring (fully known from stock)

From `vendor-notes/stock_firmware_notes.md` and the live DT:

- XR829 BT hangs off **UART1** (`uart@05000400`, PG6–PG9), H4/H5 (three-wire).
- `bt_rst_n` = PL2, `bt_wake` = PL4, `bt_hostwake` = PL3 (all on the R_PIO PL bank).
- 32k clock = RTC LOSC fanout.
- Firmware: `fw_xr829_bt.bin` (320 KB), already extracted in `vendor_data/firmware/`.
- Power rails: same as Wi-Fi (`axp2202-bldo1` = vcc-wifi, `aldo3` = vcc-wifi-io).

## What's missing

There is **no mainline XR829/XR819 Bluetooth driver**. The xradio Wi-Fi driver only has BT
*coexistence* hooks (`is_BT_Present`, `WSM_EVENT_BT_ACTIVE/INACTIVE`), not a BT stack. The stock
system runs `hci_uart` + a closed `bluesleep` (XRadio low-power) extension, with `fw_xr829_bt.bin`
loaded by a vendor helper.

So this is a **new driver** (or a `btqca`/`btmtk`-style serdev loader ported to XR829), not a DT +
config change. The unknown is the firmware-download protocol (vendor HCI commands / PSKEY-style
download) and the `bluesleep` wake-line handshake.

## Kernel config

The defconfig already has `CONFIG_BT=m`, `CONFIG_BT_HCIUART=m`, `CONFIG_BT_HCIUART_LL/BCM/QCA/MRVL=y`
— but the rootfs has no module auto-loading, so these would need `=y` or manual load. The actual BT
*controller* support (XR829) is what's missing.

## Ordering note

Because this needs a from-scratch firmware loader with a vendor protocol, it is **more work than
VP9** (which rides on the existing `hantro` G2 driver). VP9 (1.2) is the better first Phase 1 target;
Bluetooth should follow once the serdev/UART plumbing and firmware-download approach are nailed down
(possibly borrowing from the `btqca`/`btmtk` serdev drivers).
