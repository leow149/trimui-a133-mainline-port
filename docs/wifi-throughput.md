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
