# XR829 Wi-Fi / Bluetooth coexistence (research, 2026-11-07)

Read-only research, nothing run on hardware. Problem: with `hci0` up and an LE scan running, Wi-Fi pings drop (5/6, or 0/10 with
`--duplicates`) and wlan0 loops auth/assoc until BT is rfkill-blocked. Once the xradio driver hung after the rfkill block.

## 1. How the vendor enables coex

There is NO host-side coex command on the BT side and no coex GPIO. The XR829 is one die; arbitration (EPTA = "enhanced PTA") runs
inside the Wi-Fi/BT firmware. The vendor driver only (a) tells the Wi-Fi firmware that BT exists via the SDD, (b) tags frames with a
priority, and (c) reacts to firmware BT_ACTIVE events by blocking TX and scans.

Evidence:
- **BT HCI side:** `rtk_hciattach` type `xradio` sends only reset, 0xFC18 (baud), 0xFC0A (bdaddr), reset
  (docs/bluetooth-xr829-protocol.md section 2); `strings rtk_hciattach` has no coex/pta/epta/wlan strings. Knulli `S32bluetooth`
  does only rfkill block/unblock + `rtk_hciattach -n -s 115200 ttyS1 xradio`. Nothing else touches Wi-Fi.
- **Vendor DT (A133 stock):** `wlan@0` (regon PL5, hostwake PL6), `bt@0` (bt_rst_n PL2), `btlpm@0` (bt_wake PL4, hostwake PL3).
  No coex/PTA pin. Same supplies (bldo1/aldo3) for both. Vendor kernel strings: only `sunxi-bt`, `sunxi-btlpm`, `xradio_bt_dbg`.
  The vendor wlan modules (`xradio_core/mac/wlan`) are not in the Knulli extract, so the exact vendor build is unavailable; the closest
  source is `wifi_xr829/upstream/YuzukiHD_Xradio-XR829` (Allwinner BSP derivative), used below.
- **SDD:** `sdd_xr829.bin` contains element `0xEB` (PTA config, 30 bytes) at offset 0x2c:
  `eb 1e 06 00 81 02 01 34 30 84 3f 28 a0 b2 89 5a 0d 00 ...`. Driver parses it (fwio.c ~l.126): `is_BT_Present = true`,
  listen interval = `(0x0281 >> 7) & 0x1f` = 5. Our SDD md5 `9122abf65c5f3045a4989b564007dc34` is identical to the vendor
  `vendor_data/firmware/sdd_xr829.bin`, so the SDD is not the difference.
- **Per-frame EPTA priority:** `WSM_EPTA_PRIORITY_*` (4 data, 5 mgmt/action/video, 6 voice, 7 EAPOL) placed in `wsm_tx.flags` bits 3:1
  (tx.c `xradio_tx_h_bt`, same as vendor).
- **BT_ACTIVE handling (the big difference), YuzukiHD `wlan/sta.c` ~l.1750-1805, `xradio.h` l.775-800, `wsm.h` l.394:**
  firmware sends `WSM_EVENT_BT_ACTIVE` (6) / `BT_INACTIVE` (5); eventData = `type<<16 | duration_ms`. For type 9
  (`BT_LINK_TPYE_INQUIRY`) the vendor sets `BT_active` bit1, remembers an end time (max `BT_MAX_BLOCK_TIME` 15000 ms), arms `BT_timer`;
  `xradio_is_bt_block()` then (1) stops TX dequeue in `wsm.c` ~l.3445 ("BT is busy, lock tx"), (2) rejects Wi-Fi scans with -EBUSY
  (`scan.c` l.210), (3) extends the TX-stuck timeout by 7.5 s (`queue.c` l.921), (4) wake-up of the bh on INACTIVE/timer expiry.
  Other link types only set bit0 (no blocking).
- Also vendor: `xradio_set_btcoexinfo()` on join (PS-Poll/NULL templates + override internal TX rate), and SUPPORT_EPTA debug MIBs
  `WSM_MIB_ID_EPTA_STAT` 0x1060 / `EPTA_STAT_CTRL` 0x1061 (read-only stats; vendor default ctrl 0x484).
  `WSM_MIB_ID_UPDATE_EPTA_CONFIG_DATA` 0x1011 is defined but never used by the vendor driver either.

## 2. Does our driver have it?

Mostly yes; the part missing is BT_ACTIVE/INACTIVE handling.
- Present: SDD 0xEB parse, `is_BT_Present`, listen-interval fixup (sta.c ~1444, tx.c assoc-req), EPTA tx priority (tx.c:1050),
  `xradio_set_btcoexinfo` (ap.c:627).
- Missing: `sta.c` ~l.1125 `case WSM_EVENT_BT_INACTIVE/ACTIVE: //STUB();` - events are dropped; no `BT_active` state, no TX block, no
  scan deferral, no queue-timeout extension, no `BT_timer`. Also no EPTA stats debugfs (0x1060), so we cannot see who wins.
- Nothing is "just not enabled": no module param or DT property gates it.

## 3. Ranked fixes (try on hardware)

1. **Lower the host LE scan duty cycle (no code, test first).** `hcitool lescan` / `btmgmt find` style tools use interval=window
   (0x0010/0x0010 = 100% duty), which starves any arbiter; with `--duplicates` it is continuous RX. Test with
   `btmgmt` / `hcitool cmd 08 0b` (LE Set Scan Parameters) interval 0x0060 (60 ms), window 0x0030 (30 ms), or via debugfs
   `echo 96 > /sys/kernel/debug/bluetooth/hci0/le_scan_interval; echo 48 > .../le_scan_window` before scanning (these default to 0x0060/0x0030
   in bluez/mgmt; bluetoothd also uses 0x0060/0x0030 style values). Test plan: ping gateway 30 x 0.2 s with scan using 100% vs 30% duty;
   expect loss to drop sharply. If 30% duty is clean, coex is working and the issue was the test tool.
2. **Implement BT_ACTIVE/INACTIVE in our driver (port from YuzukiHD).** Files: `xradio.h` (add `u16 BT_active; unsigned long BT_duration;
   struct timer_list BT_timer;` and the three inline helpers, plus `BT_LINK_TPYE_INQUIRY 9`, `BT_MAX_BLOCK_TIME 15000` in `wsm.h`),
   `main.c` (`timer_setup(&hw_priv->BT_timer, xradio_bt_timer, 0)` at init, `del_timer_sync` at unregister), `sta.c` (replace the two
   STUB cases with the vendor bodies; add `xradio_bt_timer()`), `wsm.c` TX dequeue loop (`if (xradio_is_bt_block(hw_priv)) break;` next to
   the `tx_lock` check; our tree has no `xradio_proc_wakeup`, use `xradio_bh_wakeup()` (bh.h:32)),
   `scan.c` (return -EBUSY when blocked, optional), `queue.c` stuck-timeout (+7.5 s while `BT_active`). Test plan: enable
   `XRADIO_DBG_NIY` printing, run LE scan and inquiry (`hcitool scan`), check dmesg shows BT_ACTIVE type/duration; confirm pings keep
   going and wlan0 does not deauth. Note this only blocks Wi-Fi TX during *inquiry* (type 9), so LE scan may not produce events at all;
   the log will tell whether the firmware reports LE scan with another type.
3. **Add EPTA diagnostics** (port `wsm_get_epta_statistics`, `struct xradio_epta_stat`, MIB 0x1060/0x1061, debugfs file) to see
   grants/aborts (`cnt_bt_abort`, `wl_req_info[].cnt_lose`, `medium_state`) while scanning. Pure read; tells us whether arbitration is
   active at all. If `num_active_rt_bt_links`/`nrt_bt_requests` stay 0 while scanning, the firmware does not see the BT core, which points
   to a BT-firmware-side issue (see 5).
4. **Wi-Fi power-save / stability while scanning:** keep the STA out of deep PS during BT scans (`iw dev wlan0 set power_save off`) and
   raise `listen_interval` is already 5 from the SDD. Cheap test to separate PS-poll starvation from outright TX loss.
5. **BT-core side:** no known vendor HCI coex command exists (0xFC18 is baud, 0xFC0A is bdaddr). Only if 1-4 fail,
   dump what vendor `rtk_hciattach` writes after the final reset with a UART sniffer on the stock device (PG6/PG7), to prove there are
   no further commands (static analysis says there are none).

## 4. Hang after BT rfkill block

- BT_RST_N (PL2, `rfkill-gpio` shutdown-gpios) and WL_REG_ON (PL5, mmc-pwrseq) are separate GPIOs; DT does not tie them together, and
  the supplies bldo1/aldo3 (WLAN power) are untouched by the BT rfkill. But the XR829 is a single die and the EPTA arbiter lives in
  the Wi-Fi firmware: hard-resetting the BT core while an EPTA grant/wait is outstanding (as happens in a scan storm) can leave
  the Wi-Fi firmware blocked waiting for BT, so WSM commands (`iw`) never complete and the kernel thread waits in the WSM command
  path. This is a hypothesis; it matches the vendor design (BT_active timer + extended timeouts of up to 15 s) but is not proven.
- The vendor always uses `rfkill block; sleep 0.3; rfkill unblock` only when Wi-Fi is idle at boot.
- Mitigations: stop scans (`hcitool`/`hciconfig hci0 down`) BEFORE rfkill block; add the BT_ACTIVE handling (fix 2) so a dead BT is
  cleared by `BT_timer`; check on the hang whether the stack shows `wsm_cmd` waiting (`cat /proc/<pid>/stack`) and the
  EPTA `medium_state` (fix 3).
- Related: `xr829_bt_up` first-run quirk (BROM sync missing on first run after boot) is possibly the same shared-die state.

## Hardware result (2026-11-07): coexistence works; the "broken Wi-Fi" was a 100% duty-cycle scan left running

Test: raw HCI LE scan (`tests/bt_coex_scan.sh`, passive) while pinging the gateway over wlan0.

| LE scan interval / window | Duty | Adv reports | Wi-Fi during scan | Wi-Fi after scan stops |
|---|---|---|---|---|
| 0x60 / 0x30 (BlueZ/kernel discovery default) | 50% | 449 | 10/10 | 6/6 |
| 0x10 / 0x10 (`hcitool lescan`) | 100% | 689 | 0/10 | 6/6, no reconnect |

- The firmware's EPTA arbiter works. A 100% duty scan simply leaves Wi-Fi no airtime.
- The earlier "Wi-Fi stays broken until BT is blocked" happened because `timeout` killed `hcitool lescan`, which never sent LE Set Scan Enable = 0. The chip kept scanning at 100% indefinitely.
- In practice, use normal BlueZ discovery (bluetoothd, 50% duty) and never leave a 100% scan running.
- Porting the vendor BT_ACTIVE / BT_INACTIVE handling (fix 2 above) is optional polish for heavy classic-BT use (inquiry), not needed for basic coexistence.
- Still open: the one xradio hang after a BT rfkill block, which happened while that runaway scan was active.
