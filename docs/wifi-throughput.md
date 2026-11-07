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
