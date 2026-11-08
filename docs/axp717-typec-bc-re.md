# AXP2202 (AXP717) vendor USB/charger driver: Type-C, BC1.2, current limits (static RE)

Source: `vendor_data/kernel_re/vendor_kernel.elf` (aarch64 objdump; Ghidra/REA import timed out, so
everything below is from disassembly). Addresses are kernel VAs. Regs are I2C PMIC regs.

## Function map
| Function | Address |
|---|---|
| axp2202_set_iin_limit | ffffff80084ff2cc |
| axp2202_set_vindpm | ffffff80084ff320 |
| axp2202_usb_power_monitor | ffffff80084ff374 |
| axp2202_usb_set_property / get_property | ffffff80084ff3d8 / ffffff80084ff440 |
| axp2202_irq_handler_usb_in / usb_out | ffffff80084ff5ec / ffffff80084ff2b0 |
| axp2202_usb_set_current_fsm | ffffff80084ff650 |
| axp2202_usb_suspend / resume | ffffff80084ff690 / ffffff80084ff6dc |
| axp2202_usb_dt_parse | ffffff80084ff728 |
| axp2202_usb_probe | ffffff80084ff910 |
| axp2202_set_ichg | ffffff80084ffde8 |
| axp2202_bat_set_property | ffffff80085000c0 |
| axp2202_charger_ichg_set | ffffff800850134c |
| axp2202_bat_resume / suspend / shutdown | ffffff800850138c / ffffff80085013cc / ffffff8008501404 |
| axp2202_bat_dt_parse | ffffff80085014b8 |
| axp2202_battery_probe | ffffff8008501c4c |

## DT properties (usb, defaults from axp2202_usb_dt_parse)
| Prop | Default | Struct off (dt base +) |
|---|---|---|
| pmu_usbpc_vol | 4600 mV | +0xfc |
| pmu_usbpc_cur | 500 mA | +0x100 |
| pmu_usbad_vol | 4600 mV | +0x104 |
| pmu_usbad_cur | 1500 mA | +0x108 |
| pmu_usb_typec_used | 0 | +0x114 |
| pmu_bc12_en | 0 | +0x218 |
| pmu_cc_logic_en | 1 | +0x21c |
| pmu_boost_en | 0 | +0x220 (parsed, never used in probe) |
| pmu_boost_vol | 5126 mV | +0x224 |
| wakeup_usb_in / wakeup_usb_out | flags (property presence) | +0x1e0 / +0x1e4 |

## Registers
| Reg | Field | Use (evidence) |
|---|---|---|
| 0x00 | bit5 | ONLINE/PRESENT of USB (get_property props 3/4: `ubfx x1,x1,#5,#1`, ff440..ff494) |
| 0x0b | bit4 (0x10) | BC1.2 enable: set if pmu_bc12_en!=0, cleared otherwise (probe ffb34/ffc3c) |
| 0x0b | bit3 (0x08) | CC logic enable: set if pmu_cc_logic_en!=0 else cleared (probe ffb7c) |
| 0x16 | [3:0] | VINDPM: `(mV-3880)/80`, clamp 3880..5080 mV (set_vindpm ff320). Probe writes pmu_usbad_vol (4600 -> 9) |
| 0x17 | [5:0] | Input current limit IIN_LIM: `(mA-100)/50`, clamp 100..3250 mA (set_iin_limit ff2cc; readback `*50+100` at ff514) |
| 0x17 | bit7 | set when pmu_bc12_en==0, cleared when !=0 (probe ffb5c/ffc50). Likely "ignore BC1.2/CC, use register limit"; exact meaning not documented |
| 0x1e | [7:4] | boost voltage: `((pmu_boost_vol-4550)>>6)<<4` (5126 -> 9) (probe ffba0..ffbbc) |
| 0x1f | all | written 0x03 (probe ffbc8) |
| 0xe1 | bit2 | cleared when typec_used!=0; untouched when 0 (probe ffbe8) |
| 0xe3 | bit1 | set when typec_used!=0, cleared when 0 (ffc0c / ffc74) |
| 0xe3 | bit0 | set in both cases (ffc2c) |
| 0xc6/0xc7 | 14 bit | VBUS voltage readback (VOLTAGE_NOW, `(r6&0x3f)<<8|r7`, ff4e0..ff50c) |
| 0xe7 | [3:0],[2:0] | only read when typec_used!=0, for vendor power_supply prop 0x3c (ff588..ff5e8) |
| 0x62 | [5:0] | charge current ICC: `mA>>6` (64 mA/step), clamp 3072 mA (set_ichg ffde8; charger_ichg_set 1350c) |
| 0x61 | [3:0] | prechg, written 2 (probe 8501fc8) |
| 0x63 | [3:0] | terminate current (probe 8502014) |
| 0x64 | [2:0] | charge target voltage code, from pmu_init_chgvol (4200 default; 4100->0 ... 4400->4) |

## Q1. pmu_usb_typec_used != 0
Almost nothing. The ONLY effects (all in axp2202_usb_probe, once, at boot):
- reg 0xe1 bit2 cleared; reg 0xe3 bit1 set; reg 0xe3 bit0 set (when 0: only 0xe3 bit1 cleared, bit0 set).
- get_property prop 0x3b (59) returns 8 if typec_used else 4 (ff56c) - a type value for userspace.
- get_property prop 0x3c (60): returns 0 if typec_used==0; else reads reg 0xe7 and maps the low nibble:
  nibble in {5,6,9,12} -> 1; else if `(reg&7)-2 < 2` (reg&7 = 2 or 3) -> 2; else 0. This is a Type-C
  state/orientation style report (meaning of 1/2 not named in the binary).
- NO per-state input-current selection. No code writes Rp advertisement (default/1.5A/3A), no CC
  detection enable except the static reg 0x0b bit3 (pmu_cc_logic_en) and reg 0xe1/0xe3 bits above.
  Input limit is the same for Type-C or not (see Q2). The PMIC hardware handles CC/Rp/USB-C input
  limit autonomously; the driver just sets enable bits.

## Q2. PC vs adapter, current limit
- The driver does NOT read any BC1.2 result register (no read of reg 0x05 or any BC status anywhere
  in the usb driver). The strings "usb pc type"/"usb adapter type" are just log messages.
- pmu_bc12_en (default 0) only toggles reg 0x0b bit4 + reg 0x17 bit7 at probe. With bc12_en=0 BC1.2 is
  disabled in the PMIC, and bit7 of 0x17 is set.
- Behaviour is time-based (usb_in IRQ, ff5ec): `power_supply_changed`; if pmu_bc12_en==0:
  set_iin_limit(pmu_usbpc_cur=500 mA) immediately, clear manual flag, cancel work, queue
  axp2202_usb_set_current_fsm after 1250 jiffies (5 s @ HZ=250). The FSM (ff650) then, unless the
  flag (+0x308, set by a userspace write to INPUT_CURRENT_LIMIT, prop 0x23) is set,
  calls set_iin_limit(pmu_usbad_cur) (1500 default / 2500 per board DT).
  Probe also queues the FSM 5000 jiffies after boot (ffadc). So every source is treated as
  "500 mA, then after 5 s the adapter value", regardless of SDP/DCP. If pmu_bc12_en!=0 the driver skips
  this and trusts the PMIC's own BC1.2 -> IIN_LIM.
- Encoding: reg 0x17[5:0] = (mA - 100) / 50. 500 mA -> 8 (0x08); 2500 mA -> 48 (0x30); max 3250 -> 63.
- VINDPM reg 0x16[3:0] = (mV - 3880)/80 from pmu_usbad_vol (pc_vol is parsed but not applied).

## Q3. QC / D+/D- / PD
Confirmed: none. No D+/D- signalling, no QC, no USB-PD code or registers in the AXP2202 usb/battery
drivers; no strings for them. (pmu_boost_* is the OTG boost output, reg 0x1e, not fast charge.)

## Q4. Runtime vs suspend charge current
Registers: reg 0x62[5:0] = mA >> 6 (clamp 3072). Function axp2202_charger_ichg_set (1350c),
negative values clamp to 0.
| Hook | Address | Applies |
|---|---|---|
| battery_probe | 8501fb4 / 85022b0 | runtime (set_ichg(x19+252) then again with clamp) |
| bat_resume | 800850138c | pmu_runtime_chgcur (+252), re-enables bat IRQs |
| bat_suspend | 80085013cc | pmu_suspend_chgcur (+256), disables IRQs |
| bat_shutdown | 8501404 | pmu_shutdown_chgcur (+260) |
Defaults: runtime 500, suspend 1200, shutdown 1200 mA (board DT: 1000 / 1950). 1000 mA -> 15 (0x0f),
1950 mA -> 30 (0x1e, 1920 mA actual). Hooks are in the driver structs at ffffff8008bade80 (shutdown),
ffffff8008bade88/90 (suspend/resume). Also writable at runtime via power_supply prop 0x1d
(CONSTANT_CHARGE_CURRENT) -> axp2202_set_ichg (ff... 85000c0 -> ffde8).
Other: prop 0x2b writes reg 0x1b[7:4]; prop 0x0a writes reg 0x64[2:0] (charge voltage).

## Live readout on mainline (2026-11-08, kernel #54, via regmap debugfs; /root/i2creg reads race and are unusable here)
Wall charger through the FNB-58, battery 27 %, charging:
`05: 70` (BC1.2 result = 3, DCP), `0b: 1d` (bit3 CC logic on, bit4 BC1.2 on; stock 0x0d), `16: 01`,
`17: 1c` (input limit (28*50+100) = 1500 mA = sysfs input_current_limit), `e0: 21 e1: 09 e2: 07 e3: 10 e7: 00`.
With Type-C mode off (0xE3 bit1 = 0) 0xE7 reads 0, so it says nothing about the charger's Rp yet.

## CC experiments (2026-11-08, charger through FNB-58, no replug; scripts tests/cc_probe*.sh, all restore e1/e3)
- No public AXP717 datasheet; upstream commit 7a4c13c59222 only names 0xE1 TYPEC_CC_AA_EN, 0xE3 TYPEC_CC_MODE_CONTROL,
  0xE7 TYPEC_CC_STATUS (volatile), plus IRQs TYPEC_PLUGIN/REMOVE.
- 0xE3 low bits 01/10/11 alone: 0xE7 stays 00, no IRQ, VBUS and input current unchanged.
- 0xE1: with bit2 (0x0c/0x0d) or after such a step, 0xE7 becomes 0x40/0x41/0x44 and IRQ4 (0x4c) bit2 latches;
  bit1 alone/bit0+bit3 alone usually 00. Results depend on history (same e1/e3 gave 00 at first, 40 later; the low
  nibble alternated 44 -> 41 at fixed settings) => a state machine that updates on attach/toggle events.
  After restoring e1=09/e3=10, 0xE7 stayed 0x40 (was 00 before the experiments). Never disturbed VBUS.
- Next: decode with controlled outside changes (cable flip = orientation; PC port vs 5V/3A charger = Rp level).

## VBUS drop (more relevant for charge speed than negotiation)
- PMIC VBUS ADC: 4.972 V at ~60 mA vs meter 5.019 V; 4.10 V at 1.12 A vs meter 4.855 V
  => ~0.67 ohm between meter output and PMIC (cable + connectors). VINDPM (0x16 = 0x01) = 3.96 V; stock 4.0 V.
  Higher input current would push VBUS into VINDPM and throttle itself; a better cable matters more than Rp.

## Plug-flip test (2026-11-08 ~11:57, A-to-C cable from the meter's USB-A out, UGREEN hub USB-A port; tests/cc_watch.sh)
- With detection armed (e3=13, e1=0d), 0xE7 alternates 0x41 <-> 0x44 every ~5 s, attached or not:
  while UNPLUGGED (VBUS 8 mV, on=0x18, bc=0x00) it still read 0x41, and after re-plugging in the flipped
  orientation it continued the same 41/44 alternation. IRQ4 never latched. Plug/unplug/orientation do not show up.
- Conclusion: the AXP717 CC logic runs, but its CC pins evidently do not see the receptacle's CC lines on this board
  (consistent with TrimUI's own pmu_usb_typec_used = 0, and with PH8 being driven by a separate CC/ID circuit,
  docs/usb-otg-vbus-research.md). Reading the charger's Rp level through the PMIC is not possible here.
- Unplug sequence otherwise normal: on 0x39 -> 0x18 (VBUS gone) -> 0x38/bc 0x10 -> 0x39/bc 0x70 (DCP) within ~5 s.
- The FNB-58 survived the unplug/replug with fnb58_logger.py holding it (supports the re-open theory of the crashes).

## Charge-rate test on a USB-C charger (2026-11-08 ~12:30, 140 W charger -> FNB-58 -> C-to-C -> TrimUI, kernel #55)
Meter values read off its display (its PC link times out in this setup, see below). Battery ~39 %, idle, back cover off.
| setting | meter | PMIC VBUS | est. into battery |
|---|---|---|---|
| ilim 1.5 A, ICC 0x0f (960 mA) | 5.17 V 1.14 A = 5.9 W | 4.92 V | ~1.0-1.1 A |
| ilim 2.5 A, ICC 0x1e (1920 mA) | 5.13 V 1.56 A = 8.0 W | 4.79 V | ~1.45-1.5 A |
- Path resistance meter->PMIC ~0.22 ohm (vs ~0.67 ohm with the A-to-C cable).
- At 1.92 A the battery current briefly read ~2x higher, then settled after ~20 s: probably PMIC die-temperature
  regulation (die ADC raw 650 = ~65 C). No PMU fault (0x08 = 0), 0x01 = 0xb2 (charging).
- BC1.2 result on this charger port: Unknown (no DCP signature). So "500 mA unless DCP" would cripple USB-C chargers;
  stock never used the BC result anyway (always pmu_usbad_cur 2.5 A ~5 s after plug-in).
- current_now is unreliable: raw in_current5 alternates 2853 / 4276 (ratio 1.5) between samples and reads ~2x the
  power-balance estimate. Driver item to investigate.
- FNB-58 PC link: with its measuring side on the charger and its data cable directly on the laptop, the first HID
  write times out every time (3/3), usbhid then resets it and it drops off. It worked for 45 min while both sides
  hung off the same charger-powered hub (shared ground). Don't open it in this setup.

## BC1.2 on PC ports (2026-11-08, kernel #56)
- Laptop USB port (USB-net working) and the UGREEN hub port both read BC1.2 result 3 (DCP), so patch 0085 picks 2.5 A
  there, like stock (which always went to 2.5 A). Likely cause: the TrimUI's own PHY/gadget is on D+/D- while the PMIC
  runs detection. Real DCPs short D+/D-, which can't coexist with a working data link.
- Effect on the laptop: VBUS at the PMIC 4.07 V (VINDPM 3.96 V limits the draw), no overcurrent on the laptop,
  gadget fine. So no harm, but "500 mA on a PC" does not happen.
- A reliable PC signal is the gadget being configured by a host (UDC state "configured"; musb calls
  usb_phy_set_power() with the configured bMaxPower).
- OTG host check after patch 0084: a USB stick on an OTG adapter got VBUS (0x13 = 0x01) and enumerated (EHCI, high speed).
- DECISION (user, 2026-11-08): keep it. PC ports also get 2.5 A like stock; VINDPM limits the draw to what the port
  gives. No 500 mA cap from the gadget state.
