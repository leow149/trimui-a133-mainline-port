# USB-C host mode: VBUS output and CC research (2026-11-08)

Read-only research (no hardware touched). Sources: stock `live.dts`, the symbolized vendor kernel
(`vendor_data/kernel_re/vendor_symbolized.txt`), mainline `linux/`, and lkml / web search results.

## 1. How stock enables VBUS output in host mode

DT (`vendor_data/devicetree/live.dts`):

- The AXP2202 node (reg 0x34; AXP2202 is the register-compatible sibling of the AXP717) has `x-powers,drive-vbus-en`.
- It exposes a regulator `reg_drivevbus` (`axp2202-drivevbus`, enable ramp 1 ms, `drivevbusin-supply = vmid`).
- `ehci0` and `ohci0` both carry `drvvbus-supply = <&reg_drivevbus>`, and `hci-supply = <&reg_cldo3>` (USB 3.3 V).
- `usbc0` (the `sunxi-otg-manager`) has these properties:
  - `usb_port_type = 2` (OTG)
  - `usb_detect_type = 1` (ID + VBUS)
  - `usb_id_gpio = PH8` (input, no pull)
  - `usb_det_vbus_gpio = "axp_ctrl"` (VBUS presence from the PMIC power supply, not a GPIO)
  - `usb_regulator_io = "nocare"`
  - `det_vbus_supply = <&usb_power_supply>`
- `usb_power_supply` has `pmu_usb_typec_used = 0`. It also has `pmu_acin_usbid_drv = PH12` and `pmu_vbus_det_gpio = PH13`. The consuming node, `gpio_power_supply`, is `status = "disabled"`, so these are not used.
- There is **no Type-C controller node** (no fusb302, tcpc, husb or similar) anywhere in the live DT.
- The second USB-C port (`usbc1`/ehci1) uses a plain GPIO `regulator-fixed` (`usb1-vbus`, a PB-bank pin). Port 0 does NOT use a GPIO.

Vendor kernel code. The regulator ops are `regulator_{enable,disable,is_enabled}_regmap_axp2202_c_drivevbus`, at `0xffffff800834e658` and following:

| Op | Writes |
|---|---|
| enable | `regmap_update_bits(reg 0x13, mask 0x01, val 0x01)`, then `regmap_update_bits(reg 0x19, mask 0x10, val 0x10)` |
| disable | the same two registers cleared (0x13 first, then 0x19) |
| is_enabled | `(reg0x13 & 1) && (reg0x19 & 0x10)` |

Two registers are involved, not one:

- **Reg 0x19 bit 4** is the AXP717 `BOOST` enable (`AXP717_MODULE_EN_CONTROL_2` BIT(4)), which mainline also writes.
- **Reg 0x13 bit 0** is the additional bit. Mainline defines nothing at 0x13 and never writes it.

I could not find a public register description for 0x13 (the datasheet is not public). The stock idle dump has 0x13 = 0x00, 0x19 = 0x06 and 0x1e = 0x94. 0x1e = 0x94 means boost voltage field 9, i.e. 4550 + 9 x 64 = 5126 mV.

My hypothesis (unverified): 0x13 bit 0 is an OTG/discharge-mode bit. It switches the VBUS port from charger input to boost output, or connects the boost output to the VBUS pin and disconnects the input path. This fits the observed failure exactly: BOOST_EN alone was set, the converter nominally ran, but the output was not connected to the connector.

Call chain in stock:

- `sunxi_set_host_vbus` / `__sunxi_set_vbus` call `regulator_enable(drvvbus)`.
- They are invoked from the EHCI/OHCI host-start path, which is reached after `vbus_id_hw_scan` -> `hw_insmod_usb_host`, i.e. only once the ID scan says host.
- `__sunxi_set_vbus` additionally checks `usb_otg_id_status() == 1` (reads PH8 raw) before some paths.
- So VBUS is gated on role in software. Nothing in the vendor code shows a hardware interlock.

PMIC input detection in stock: `vbus_id_hw_scan` with `det_type == 2` ("axp_ctrl") calls `power_supply_get_property(axp usb, ONLINE)`. It is the same idea as mainline's `usb0_vbus_power-supply`, with the same blindness while boosting.

## 2. Does mainline's AXP717 `boost` do the same?

No. Mainline `AXP_DESC(AXP717, BOOST, ..., 0x1e, GENMASK(7,4), 0x19, BIT(4))` only sets reg 0x19 bit 4. It is missing reg 0x13 bit 0.

It also does not touch the input path or the current limit. `0x16`/`0x17` (input voltage and current limits) differ between the stock and mainline dumps but are charger-side.

Why AXP `present` reads 0 while boosting:

- `axp717_usb_power` reads `AXP717_ON_INDICATE` (0x00) bit 5, `VBUS_GOOD`. That is an input-side qualifier: VBUS above the VINDPM threshold while the charger input is enabled. When VBUS is an output (or the input path is not connected), it reads 0.
- That is correct in the boost-working case and is exactly the blindness the design must handle.
- The 16.38 V `voltage_now` is a saturated 14-bit ADC code (0x3fff = 16383 mV). The VBUS ADC channel is not valid, or is not measuring a connected rail, so it is not a real measurement. It is consistent with "boost is not actually on VBUS".
- The classic AXP803/813/221 `drivevbus` (`AXP20X_VBUS_IPSOUT_MGMT` bit) is different. It controls the N_VBUSEN pin as an OUTPUT to an EXTERNAL 5 V switch.
- phy-sun4i-usb handles that case with `poll_vbusen` (the AXP221/223 do not interrupt on VBUS change while driving N_VBUSEN) and by skipping its own VBUS when an external VBUS is already detected (`power_on`: "External vbus detected, not enabling our own vbus").
- That `power_on` check uses `data->vbus_det`, which is only valid after the first scan, and EHCI0 probes before it.
- The AXP717 integrated boost has no N_VBUSEN. The H700 RG35XX RFC (Philippe Simons, lkml 2502.2/09085) used boost -> GPIO fixed regulator (PI16) -> `usb0_vbus-supply`. He reported the same failure: HCI power_on enables VBUS unconditionally and never disables it, so with a host cable attached the 5 V is applied against another host. No fix was merged in what I found.

## 3. USB-C CC / ID handling

- There is no Type-C chip in the stock DT, and `pmu_usb_typec_used = 0`. So there is no I2C TCPC for the OS to talk to.
- PH8 (`usb_id_gpio`, no internal pull) is read as a plain GPIO ID. A USB-C receptacle has no ID pin, so PH8 must be driven by board hardware from CC. This is probably a CC-detect circuit, or a small Type-C "ID" chip with a GPIO output (fusb301/ptn5110-style parts, which can be GPIO-only and invisible in the DT).
- I cannot determine the circuit from software evidence. It is the key open item.
- CC pull resistors are most likely fixed Rd (5.1 k) with no Rp source capability, and no software control. That means:
  - A sink-only CC port is a spec violation when sourcing VBUS (no Rp advertised), but plain USB-A-female-to-C OTG adapters carry Rd and just wait for VBUS.
  - A powered hub works because it provides its own VBUS.
  - A bus-powered hub or stick needs the device-side VBUS and sees nothing more from CC.
- PH12 / PH13 / PH14 (`pmu_acin_usbid_drv`, `pmu_vbus_det_gpio`, `pmu_acin_det_gpio`) are listed in the vendor DT. They may be vestigial from the reference design, or may be real board nets. Check with a meter.

## 4. Recommended SAFE mainline design

Goal: 5 V on VBUS only when ID = host (PH8 low) AND no external VBUS, and removed within milliseconds when the cable role changes.

DT:

```
&usbphy {
    usb0_id_det-gpios = <&pio 7 8 GPIO_ACTIVE_HIGH>;   /* unchanged */
    usb0_vbus_power-supply = <&usb_power>;             /* unchanged */
    usb0_vbus-supply = <&reg_usb0_vbus>;               /* new: AXP717 OTG VBUS regulator */
};
```

Regulator: do NOT use the plain `boost` regulator for this. Either:

- (a) add a new AXP717 "otg/drivevbus" regulator whose enable op is (0x13 bit0 + 0x19 bit4) in that order, and whose disable clears 0x19 bit4 first, then 0x13 bit0. Fixed voltage from the boost (about 5.1 V, `0x1e` upper nibble = 9, as in stock). Mirror the stock sequence (the vendor kernel is the only evidence for these bits). Or
- (b) first verify with `i2cset` on the bench that setting 0x13 bit 0 together with the existing boost enable makes VBUS appear, then do (a).

Gating (primary invariant: VBUS regulator enabled only while role == host):

1. Do not enable VBUS in `sun4i_usb_phy_power_on()` for phy0 unless `data->id_det == 0 && !data->vbus_det` (after a scan has run; treat `-1` as "unknown, do not enable"). Count users without enabling.
2. In `sun4i_usb_phy0_id_vbus_det_scan()`:
   - On id_det -> 1 (device), disable the regulator immediately if `regulator_on`.
   - On id_det -> 0 with `users > 0` and no external VBUS, enable it.
   - Make the id_det -> 1 path run in the IRQ thread (or force `mod_delayed_work` with 0 delay). The debounce can stay for the host direction, but never for the removal direction.
3. Disable the regulator at probe, at shutdown/reboot, and in `power_off`. Also set `regulator-boot-on = false` (no `always-on`).
4. The hardware must also be checked. If the AXP717 itself blocks boost while an external VBUS above its threshold is present, that is a second interlock. This is unverified.

External VBUS detection while boosting:

- The PMIC `present` cannot be trusted while we boost. Do not rely on it.
- Use (a) ID (PH8, which comes from CC and changes when a PC cable replaces the OTG adapter) as the authority, and (b) the AXP IRQs (VBUS over-voltage and `AXP717_IRQ_BOOST_OVER_V`, plus VBUS fault bits in `AXP717_PMU_FAULT`), which should force the regulator off.
- Optionally add a DT-described external VBUS-detect GPIO if the meter reveals a usable net (PH13?).
- Before enabling, require `power_supply PRESENT == 0` read while the boost is still off. That read is valid at that point.

Driver change: yes, phy-sun4i-usb needs one (small). Today EHCI0 (and OHCI0) call `phy_power_on()` at probe and the regulator is switched on whenever the HCI is active, regardless of the ID line; the role switch never turns it off, and the first-scan `vbus_det` is not valid yet. The changes above fix that. The dual-route reroute itself needs no change.

Also fix the "stuck in host role with PH8 high" boot (early return on `phy0_init`, edge-only ID IRQ): add a post-init forced scan.

## 5. Open questions (meter / user)

1. Which circuit drives PH8 from CC, and what (if any) Rp/Rd is on CC1/CC2 of the data port? Measure CC1/CC2 vs GND with nothing, an OTG adapter, and a PC cable attached, and PH8 in each case. Identify any chip near the receptacle.
2. Is the AXP717 VBUS pin directly wired to the receptacle's VBUS, or through a switch or diode with a separate control (PH12/PH13)? Measure VBUS with boost on (both 0x19 bit4 only, and 0x19 bit4 + 0x13 bit0), no load, to confirm the 0x13 hypothesis.
3. Is there a backfeed path, i.e. does an external 5 V on VBUS damage a boosting AXP717? Do not test with real ports until the gating is in place. Use a current-limited bench supply first.
4. What is the real current limit of the boost (low nibble of 0x1e, stock 0x4)? What does a USB stick draw at the connector?
5. Does the user want an OTG adapter that holds ID low as the only supported scenario (today's behaviour)?

Sources: [lkml H700 OTG RFC](https://lkml.rescloud.iu.edu/2502.2/09085.html), [AXP717 boost regulator series](https://patchew.org/linux/20240816001824.6028-1-andre.przywara@arm.com), [AXP717 boost patch](https://lkml.rescloud.iu.edu/2404.2/03143.html).
