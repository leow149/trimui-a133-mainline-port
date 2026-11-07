# Bluetooth (XR829) bring-up — status

Status: **WORKING 2026-11-07 via userspace uploader `tools/bt/xr829_bt_up` (hci0 up, scans see devices).** Kernel driver still open.
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

## Working bring-up (2026-11-07)

No DT/kernel change was needed: WiFi had already powered bldo1/aldo3, osc32k-fanout (32 kHz) was running and
PL5 (WL_REG_ON) was high. The old "no reply" was timing: 0x55 spam started 5 ms after reset release. The chip's
ROM answers "OK" on the first 0x55 when the tool waits ~150 ms after releasing reset (rfkill 0 -> 1) before syncing.

Sequence (`tools/bt/xr829_bt_up -F 0 -a 02:00:00:aa:bb:01`, build with `tools/bt/build.sh`, installed as `/root/bt/xr829_bt_up`;
firmware in `/lib/firmware/xr829/fw_xr829_bt.bin`): rfkill 0/1 -> btwake=1, lpm=0 -> wait 150 ms -> 0x55 sync "OK" @115200 ->
SET_UART 1.5M (checksum byte order in the protocol doc is correct) -> sync @1.5M -> 313 SEQWR chunks (all acked) -> SET_PC 0 ->
**wait 300 ms** (fw boot; a reset sent after 15 ms is lost) -> firmware runs at **115200, no flow control** (CRTSCTS must stay off:
only PG6/PG7 matter, a floating CTS would stall TX) -> HCI reset OK -> 0xFC18 (1.5M) -> host 1.5M -> 0xFC0A bdaddr -> reset ->
TIOCSETD N_HCI + `HCIUARTSETPROTO` (argument **by value**, not pointer) -> hci0. The tool stays resident (forked child holds the fd).
BD address used: 02:00:00:aa:bb:01 (locally administered placeholder; the chip has no valid default).
Result: `hciconfig hci0 up` UP RUNNING, name XR829_BT, classic scan and LE scan see devices
(tests/results-20261107/bt/). Open: boot-time start script, persistent BD address, LPM, WiFi/BT coexistence, serdev driver.

## (Historical) Open issue: the BROM sync never gets "OK"

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

## Boot integration and status (2026-11-07)

- `/etc/bt-up.sh` (rootfs/etc, started from inittab) loads the firmware at boot, in the background, once wlan0 exists.
  - The BD address is the Wi-Fi MAC with the last byte + 1, for example 02:00:34:9a:30:3b. That's stable, because U-Boot derives the Wi-Fi MAC from the SoC ID.
  - hci0 is left DOWN. There is no bluetoothd yet; bring it up with `hciconfig hci0 up` (bluez is in the edge chroot).
  - The verbose log of the last attempt is in /tmp/bt-up.log.
- **First-run quirk:** the first `xr829_bt_up` run after boot never gets the BROM sync, even across 9 chip power cycles. These did not help:
  - a later start (15 s uptime),
  - raising BT_WAKE before the first reset,
  - priming ttyS1 with stty.
  The next run syncs on the first 0x55. The script therefore does a throw-away 5 s probe (`-p`) first. Cause still unknown; probably UART/tty state the first process leaves behind.
- **Wi-Fi/BT coexistence is broken (open):**

  | BT state | Wi-Fi to the gateway |
  |---|---|
  | hci0 down | 6/6 pings |
  | hci0 up, idle | 6/6 pings |
  | LE scan running | 5/6, or 0/10 with `--duplicates` |

  After a scan, Wi-Fi stays broken: wlan0 loops through auth/assoc retries until BT is switched off (rfkill block). Then a Wi-Fi reconnect works. Once, the xradio driver hung after the rfkill block (`iw` stuck in the kernel) and needed a reboot.
  Needs the vendor coexistence setup: Wi-Fi firmware coex parameters (SDD/wsm) or the BT vendor commands for PTA. Until then, don't use BT while relying on Wi-Fi.
