# USB-C data port: OTG role switching (2026-11-08)

The charging/data USB-C port (musb, `usb_otg`) now runs as `dr_mode = "otg"`.

- ID pin: PH8, `usb0_id_det-gpios = <&pio 7 8 GPIO_ACTIVE_HIGH>`. Stock DT has
  `usb_id_gpio = <&pio 7 8 mux0 pull0>`: plain input, no internal pull, so the
  board has an external pull-up. High = peripheral (ID floating), low = an OTG
  adapter grounds ID = host.
- VBUS presence: `usb0_vbus_power-supply = <&usb_power>` (AXP717 PRESENT property).
- No `usb0_vbus-supply`: the board does not drive VBUS in host mode (stock's
  `usb_regulator_io = "nocare"`, `usb_det_vbus_gpio = "axp_ctrl"` do not say how
  the AXP717 boost is switched). phy-sun4i-usb only enables a usb0 VBUS regulator
  when one is described, so this can never put 5 V on the port. Self-powered
  devices work through an OTG adapter; bus-powered ones need the VBUS-out work.
- No driver changes: phy-sun4i-usb and musb-sunxi already implement ID/VBUS
  scanning and the extcon notifier.

Evidence with the PC cable (tests/results-20261108/usb-otg/): PH8 = `in hi`,
AXP usb online/present = 1, UDC `configured`, iperf3 device->host 299 Mbit/s,
suspend/resume (usbsusp.sh 1) returns `configured`.

Physical test: see the commit/report; OTG adapter + self-powered device on the
port, watch `lsusb`/dmesg over Wi-Fi (`/root/wifi-up.sh`, 192.168.1.185).

## Host-mode findings (2026-11-08, linux branch `wip-otg-host`, NOT merged)

- **Host mode goes through EHCI0/OHCI0.** The A133 USB0 PHY is dual-route, and in host mode `sun4i_usb_phy0_reroute()` hands the port to `usb@5101000`/`usb@5101400`, not to the musb.
  - With `&ehci0`/`&ohci0` enabled and a **self-powered** USB-C hub, everything enumerated: hub 05e3:0610, a SanDisk Ultra Fit (sda, 33.6 MB/s read) and an AX88179A Ethernet adapter.
  - The musb DEVCTL is at sunxi offset 0x41 (word 0x40), not the standard 0x60.
- **VBUS out (AXP717 `boost` as usb0_vbus-supply): unresolved and unsafe as tried.**
  - A bus-powered hub dropped off when its own supply was removed, with boost "enabled". Either the boost does not reach the connector, or the USB-C CC side (source Rp) is missing.
  - EHCI0 calls phy_power_on() at probe, which would enable boost regardless of role. Gating it by ID (WIP) still left boost ENABLED after swapping back to the PC cable, with external VBUS present: a backfeed risk.
  - The following boot came up stuck in host role with PH8 high. The detect work's handling (early return on `phy0_init`, edge-only ID IRQ, polling) needs a proper look.
- **State on tsp-wip:** patch 0077 only. ID detection plus OTG dr_mode, no ehci0/ohci0, no boost, so host mode does not work yet and the port is safe.
- **Next:**
  1. Make role switching reliable (ID scan) and ensure VBUS can never be on while ID=device.
  2. Then enable ehci0/ohci0 for self-powered devices.
  3. Investigate VBUS out with a meter on the port and the AXP717 datasheet (boost output path / N_VBUSEN), and the board's CC circuit.
