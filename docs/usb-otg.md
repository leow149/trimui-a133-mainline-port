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
