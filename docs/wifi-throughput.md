# XR829 Wi-Fi throughput (2026-11-07)

Setup: iperf3 to the wired laptop (192.168.1.132), 2.4 GHz ch1, 20 MHz, MCS7, -36 dBm.

## Bottleneck
TX was never aggregated. `xradio_is_ht()` tested `ht_oper.channel_type`, which is never
set, so `priv->htcap` stayed false and the "enable block ack" step after association was
skipped. Firmware BA policy read back (new debugfs `block_ack`): tx 0x00, rx 0xb1, and the
firmware A-MPDU counters showed `tx_ampdus 0`.

Evidence before the fix (ftrace + temporary counters in bh.c/wsm.c):
- CPU 95% idle, bh never saturated, SDIO write ~90 us per frame, hw_bufs_used avg 10 (TCP)
  or 31 (UDP flood: firmware-bound at ~1250 frames/s, not host-bound).
- TX confirms: every frame rate index 21 (MCS7), flags 0 (no aggregation), ~760 us of
  firmware time per frame; AP does not advertise short slot, so every lone frame pays
  long-slot contention (~220 us) plus preamble/SIFS/ACK.
- Ruled out as bottleneck: SDIO bus, bh loop/batching, IRQ latency (3 "missed interrupt"
  per minute at most), rate selection (MCS7), retries (~6-8%).

After the fix: firmware reports ~8 MPDUs/A-MPDU, TX 25-29 Mbit/s typical (best 32),
RX 27-30 Mbit/s (was 19-21). CPU: bh stays under ~10%.

## HT40 + short GI (2026-11-07, patches 0071-0072)
The router's 2.4 GHz radio went to 40 MHz (home-ssid and home-ssid-mlo on ch 1, secondary above).
- Caps: SUP_WIDTH_20_40 + SGI_20/40. Join PHY mode word carries width (bits 4-5 = 2) and
  primary position (bit 6; secondary above = 1) from `hw->conf.chandef`; the association
  mode must keep the join's width. Evidence: join 20/assoc 40 and join 40/assoc 20 both
  gave an associated link that passed no traffic (no DHCP lease); consistent words work.
- TX status: the confirm's six rate entries are now passed to mac80211 one-to-one with
  BW/SGI flags (before they were folded to 20 MHz indices, iw showed MCS7 65 Mbit/s).
- Result on home-ssid (40 MHz): TX 25-29 -> 29.5-31.8, RX 27-30 -> 35-38 Mbit/s; iw shows
  `MCS 7 40MHz short GI` 135-150 Mbit/s. Rates are no longer the limit (CPU idle,
  SDIO 50 MHz 4-bit); the ceiling is firmware/host per-frame cost.
- Narrowing: with `disable_ht40=1` in the network block (mac80211 joins at 40 then narrows
  to 20) the link works, 20 MHz MCS7, TX/RX 24-29 Mbit/s. A true 20 MHz-only AP joins
  with the old 20 MHz word (not testable here, router unchanged).
- WPA3 (home-ssid-mlo, SAE, MFP required): still slow with HT40 (TX 3.3-12, RX 10-12 Mbit/s,
  UDP 60M offered -> 77% loss at device), same as at 20 MHz. A-MPDU works (4 MPDUs/A-MPDU,
  MFP yes, rates MCS7 40MHz SGI, retries ~7%), power save off makes no difference. Ping
  latency avg 65 ms (max 347) vs 11 ms on home-ssid, sometimes multi-second TX stalls. IGTK
  rejected (`key->cipher unknown`, software BIP). Not caused by the HT work; open.
  Next: sniff home-ssid-mlo to see whether the loss is AP-side (virtual BSS) or key/PS related;
  compare with another client on the same SSID.

## Open leads
- About 1 in 10 TX runs stalls for a few seconds (0.00 Mbit/s intervals) or runs at 2-4
  Mbit/s; it also happened before the fix (baseline "2.7"). Not tied to idle time. Next
  step: sample `debugfs/xradio/txstate` and `ampdu_counters` (implicit_bar_fail) during
  a stall; try `ampdu_len` (MIB 0x000a) and `block_ack` toggling live.
- sunxi-mmc logs "data error, sending stop command" ~1 per minute under load, no xradio
  error follows (likely the silently retried control-register read in bh); check
  phase delays.
- `ba_tid_mask` 0xB1 ("HW limitation") unchanged; 0x3F untested.

Logs: tests/results-20261107/wifi/.

## WPA3 (SAE + MFP) performance: open (2026-11-07)

Measured on home-ssid, 40 MHz + SGI, same AP (UniFi U7 Pro XG):

| | WPA2-PSK | WPA3-SAE (MFP required) |
|---|---|---|
| TX / RX (iperf3) | ~31 / ~40 Mbit/s | ~14 / ~14 Mbit/s |
| Ping to the gateway | 11-17 ms | 70-150 ms, spikes to 460 ms, loss |

home-ssid-mlo (WPA3-only, MLO) behaves the same, so MLO is not the cause.

Attempted port of the vendor PMF support (linux branch `wip-xr829-pmf`, not merged):
- IGTK in firmware (key type 8) and the PROTECTED_MGMT_POLICY MIB.
- MMIE / multicast robust-mgmt TX.
- Ignoring unprotected deauths.

Results:
- **policy off, IGTK uploaded:** connects, no improvement.
- **protectedMgmtEnable=1:** connects, TX works, but AP-to-STA unicast data stops reaching the host after DHCP. This is the same with every policy-bit combination and with mac80211 ADDBA handling left on. The AP's protected ADDBA requests do decrypt; the AP then sends a BAR about every second.

Next lead: AP-initiated BA session / PN state in the firmware with PMF on. That needs an over-the-air capture (frame headers and BA/BAR timing are visible even when encrypted). Logs: tests/results-20261107/wifi/pmf-*.log.

Recommendation until then: use WPA2 (the home-ssid mixed-mode SSID) on this device.

## Hotspot / AP mode (2026-11-08)

`/root/ap-up.sh` (rootfs/root) runs an AP-only hotspot: SSID `trimui-ap`, WPA2, channel 1, DHCP via dnsmasq on 10.66.0.0/24, no NAT. The passphrase is generated once into /etc/hostapd/ap.pass. `ap-up.sh stop` reconnects the station.
- Verified: a phone associated, got a DHCP lease and answered pings (3/5, 9-93 ms, phone power save).
- xradio only accepts its two registered MACs (wlan0's and wlan0's + 1), so ap0 uses MAC + 1, and the BT address moved to MAC + 2.
- The AP comes up at 20 MHz even with `ht_capab=[HT40+]` (the hostapd HT40 setup reports "Could not configure driver mode").
- **Station + AP concurrently (2026-11-08, fixed):** root cause was `wsm_start()` always sending the 20 MHz PHY mode word. With wlan0 joined at HT40 (secondary above), starting the AP told the firmware to run 20 MHz, which retuned the shared radio away from the station's 40 MHz channel; wlan0 stayed associated but passed no traffic (8/8 ping loss on the old kernel, A/B in tests/results-20261108/wifi-concurrency/run7*). The start word now comes from `wsm_phy_mode_cur()` (the shared mac80211 chandef, the same word as the join). Result with ap0 up on home-ssid's channel: ping 0% loss, iperf3 TX ~32 / RX ~33-36 Mbit/s, hostapd `state=ENABLED`. `ap-up.sh start --keep-sta` follows the station's channel. hostapd's own OBSS scan falls back to a 20 MHz AP (a neighbour BSS on ch 6 overlaps the secondary channel); the AP at 20 MHz inside the station's 40 MHz channel context works. AP-only is unchanged.
## SDIO "data error, sending stop command" (2026-11-08)
About 4 per 120 s of iperf load at 50 MHz SDIO; they are retried transparently (no driver error, no throughput collapse). A 40 MHz cap (max-frequency on &mmc1) gave 1 per 120 s but cost ~10 % throughput, so it was reverted. Left as is.
