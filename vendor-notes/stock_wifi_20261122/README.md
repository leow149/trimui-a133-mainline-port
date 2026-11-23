# Stock Wi-Fi baseline (2026-11-22)

Stock TinaLinux 4.9.191, vendor driver XR_V02.16.93_HT40_01.40, same firmware as ours
(XR_C09.08.52.73_DBG_02.122), SDIO 50 MHz. Network: home AP 2.4 GHz ch1 HT40, -39 dBm,
stock joins as WPA2-PSK (no MFP; the SSID is WPA2/WPA3 transition, ROCKNIX/iwd picks SAE+MFP).

| Test (TX, device -> wired laptop) | Stock | ROCKNIX (SAE+MFP, same AP) |
|---|---|---|
| Paced 8 Mbit/s UDP, 60 s | flat, 1 small blip | dips every ~10 s (1-2 s slump + catch-up) |
| UDP max | 37-43 Mbit/s | ~14 Mbit/s |
| TCP | 36 Mbit/s | (older notes: ~14 on WPA3, 25-32 on WPA2) |

Stock under UDP load (debugfs_under_load.txt):
- A-MPDU: 242805 MPDUs in 47580 A-MPDUs = 5.1 per A-MPDU; set_ampdu_len 16; tala 0/0.
- bh_stat: tx_burst 184506 of tx_total 245038 (bursts several frames per bh loop).
- Queue 2: 86 pending (capacity 128). tx_delay mostly 1-10 ms.
- retry_ctrl short=15 long=15; ~17% frames retried, 0 failures.
- tpa_debug readable on stock (all zeros = TPA idle); our raw MIB read of 0x1041 was rejected.
