# Stock AXP2202 (AXP717) battery current / capacity / voltage (static RE)

Source: vendor_data/kernel_re/vendor_kernel.elf (Linux 4.9 arm64), aarch64 objdump. All addresses are VAs.
Property numbers verified against the kernel's power_supply attribute table (stride 32 at 0xffffff8008bad258 = voltage_now(12)):
current_now=0x11, constant_charge_current=0x1d, capacity=0x2a, temp=0x2e.

## Key finding: POWER_SUPPLY_PROP_CURRENT_NOW is NOT implemented
`axp2202_bat_get_property` (0xffffff8008500308) has no case for 0x11 (CURRENT_NOW), 0x12 (CURRENT_AVG) or any discharge-current
path. Those fall to the default branch and return -EINVAL (-22). No ADC channel-enable (0xC0/0xC1) writes appear in
axp2202_bat_* functions or in axp2202_battery_probe (0xffffff8008501c4c). The only battery-current-like read is below.
(The strings "ibat", "axp803_get_ibat/get_disibat" belong to the older AXP803 driver, not AXP2202.)

## Current: case 0x1d = CONSTANT_CHARGE_CURRENT (at 0xffffff8008500aec)
- regmap_bulk_read(reg 0xCA, 2 bytes) -> b0 = reg 0xCA, b1 = reg 0xCB (BATT_CHRG_I_H/L).
- raw = ((b0 & 0x3F) << 8) | b1   (ubfiz w1,w0,#8,#6 ; orr)  -> 14 bits, unsigned, no sign bit, no offset.
- result = raw >> 1  (lsr #1), returned as-is (no *1000).
- Single read, no averaging, no data-select write beforehand, no charge/discharge distinction. 0xCA/CB is the
  battery CHARGE current ADC; the driver never reads a discharge-current ADC register.
  Scale by the stock code: reported value = raw/2 (raw LSB 1 mA would make this mA/2; unverified vs datasheet - LSB is
  unknown from the code alone; treat reported unit as whatever userspace expects, likely raw/2).

## Voltage: case 0x0c VOLTAGE_NOW (0xffffff8008500704)
- Reads reg 0xC4/0xC5 (2 bytes) three times, each = ((b0 & 0x3F)<<8)|b1 (14 bit), sorts 3 values -> median (averaging = median of 3).
- Valid only if 2000 <= v <= 8000 (sub 0x7d0 <= 0x1770), else 0. Result = v * 1000 (uV), i.e. 1 mV/LSB.

## Capacity: axp2202_get_soc (0xffffff8008500278), trimui_axp2202_get_bat_cap (0xffffff8008501424)
- Read reg 0x0C; if bit7 set -> re-read 0x0C and use (v & 0x7F); else read reg 0xA4. Clamp to 100.
  (0xA4 = gauge SOC %). Battery present = reg 0x00 bit3 (axp2202_get_bat_present 0xffffff8008500228).
- Status (case 0): reg 0x01 & 7 selects charge state (jump table), reg 0x00 bit5, reg 0xA4==100 -> full.
- Other: TEMP (0x2e, 0xffffff80085007f4): writes reg 0xCD bits[1:0]=0 (ADC data select), udelay, reads 0xCE/0xCF,
  ((hi&0x3F)<<8|lo)*500/1000 -> used with a table; time_to_empty reads 0xA6 (3 x 16-bit median),
  time_to_full 0xA8 (same); capacity_level 0x1b>>4 + 5. charge_full = design cap at drvdata+236,
  charge_counter = cap*soc/100. Only 0xCD select value 0 is ever written, so no select for current exists in this driver.

## Implication for mainline
Stock exposes no signed battery current at all. Charging current = ((reg0xCA&0x3F)<<8|reg0xCB) with stock halving; discharge
current must be read from a different ADC register (not used by stock; check AXP717 datasheet, likely 0xCC/0xCD
BATT_DISCHRG_I) and ADC channel enables 0xC0/0xC1 (not touched by the driver, so left at PMIC/boot defaults).

## Result (2026-11-08)
- Patch 0087 (kernel #57): AXP717 IIO current scale 0.5 mA/count. Laptop port now reads current_now ~620 mA (raw ~1240).
- Charging paused (ICC 0) while on the laptop: battery voltage 3.76 -> 3.64 V (battery supplying the system) but raw
  only 117. So the register is charge current only; a discharge current is not available from it.
- At 1.92 A on the 140 W charger the raw value alternated 2853/4276 between samples; 4276/2 = 2.14 A exceeds the
  setting, so the high level is likely an ADC artefact. Not investigated further.
