# Bluetooth (XR829) bring-up — assessment

Status: investigated 2026-11-06, not implemented. This is roadmap item **1.1**.

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
