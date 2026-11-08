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

## Host-mode findings (2026-11-08, earlier attempt on branch `wip-otg-host`; superseded by the RESOLVED section below)

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


## RESOLVED 2026-11-08: OTG host mode with VBUS out (patches 0079-0082)

Root causes and fixes:
1. **Stuck in host role (0079).** `usb_add_hcd()` calls `phy_set_mode(HOST_SS)`, then falls back to `HOST`, on the EHCI0/OHCI0 PHY (phy0). `sun4i_usb_phy_set_mode()` then switched phy0's dr_mode OTG -> host permanently and ignored the ID pin. HOST_SS is now a no-op for phy0.
2. **VBUS not tied to the role (0080).** The HCI's `phy_power_on()` at probe enabled VBUS whatever the role. Now phy0 VBUS is only on while ID = host and external VBUS is known absent. The detect work drops VBUS first thing when ID leaves host. `poll_vbusen` on the A100/A133 keeps polling while VBUS is on.
3. **No 5 V on the connector (0081).** The AXP717 boost needs reg 0x13 bit 0 (OTG output path) in addition to 0x19 bit 4, like the vendor AXP2202 drivevbus regulator. 0x13 was also added to the writeable regmap ranges.
4. **DT (0082).** &ehci0/&ohci0 enabled, `reg_boost` added as usb0_vbus-supply.

Hardware test (kernel #53):

| Step | ID (PH8) | boost | 0x13 | Result |
|---|---|---|---|---|
| PC cable | hi | off | 0x00 | gadget configured, 304 Mbit/s, suspend OK |
| hub + its charger | lo | on | 0x01 | hub, Ultra Fit, AX88179A enumerate |
| hub charger removed | lo | on | 0x01 | still enumerated, stick 32.7 MB/s, battery ~466 mA discharge, PMIC fault reg 0 |
| hub unplugged | hi | off | 0x00 | ISCR device, no VBUS |
| PC re-attached | hi | off | 0x00 | gadget configured again |

Notes:
- `/root/i2creg` reads (i2c-dev with a forced address, racing the kernel) occasionally return a stale byte (0xb4 on random registers). Re-read before trusting one value.
- The sunxi musb DEVCTL is at 0x41, not 0x60.
- Open: the AXP717 integrated Type-C CC block (0xe1 = 0x09, 0xe3 = 0x10) is left at its defaults; it was not needed.
- Verified 2026-11-08 (second hands-on round):
  - s2idle with a bus-powered hub attached: the hub, stick and AX88179A are reset (not disconnected) on resume, boost stays on, and the stick reads 32.4 MB/s afterwards.
  - Boot with the hub attached: host role, boost on, and devices enumerated by ~2.7 s.
  - Plain USB-A to USB-C OTG adapter with the stick, powered by the TrimUI: high-speed, 33.5 MB/s.
  - Each unplug: boost off, 0x13 = 0x00, device role. The PC re-attach gives the gadget.
