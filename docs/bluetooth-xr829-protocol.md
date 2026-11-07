# XR829 Bluetooth: firmware download / HCI init protocol (reverse engineered)

Status: research result, 2026-11-07. Nothing here has been run on hardware. Source of truth: the vendor
`rtk_hciattach` (type `xradio`) shipped in the Knulli A133 image, decompiled with Ghidra. No public source for
this protocol was found (web search for `hciattach_xradio`, `fw_xr829_bt.bin`, `xr_bt.conf` returned nothing).

## 1. Files

| File | Size | md5 | Where | In `linux/firmware`? |
|---|---|---|---|---|
| `fw_xr829_bt.bin` | 319920 | `0f2f017ffbc1bbba13b8590d9ef1d656` | Knulli `/lib/firmware/`, `vendor_data/firmware/` (identical) | NO (only in `vendor_data/firmware/`) |
| `boot_xr829.bin` / `fw_xr829.bin` / `sdd_xr829.bin` | 2184 / 175384 / 744 | WiFi only | | yes, not used by BT |

Only `fw_xr829_bt.bin` is needed for BT. It is a raw image loaded at address 0 and started with "set PC 0"
(first words `48 00 01 be 48 00 00 26 ...` look like a vector table). No header, no container: send the file as is.
Vendor path is hardcoded `/lib/firmware/fw_xr829_bt.bin`.

Vendor binaries (extracted to `/home/user/claude/trimui_a133_analysis/knulli_extract/rootfs/`):
`usr/bin/rtk_hciattach` (Knulli runs `rtk_hciattach -n -s 115200 ttyS1 xradio &` from `etc/init.d/S32bluetooth`
after `rfkill block bluetooth; sleep 0.3; rfkill unblock bluetooth`), `usr/bin/hciattach` (stock bluez, no xradio).
Functions: `xradio_init` 0x40a200, `userial_sync` 0x408ca0, `com_sync_baudrate` 0x4094a0,
`load_btfirmware` 0x409b00, `com_set_pc` 0x4097f0, `proc_baudrate`, `proc_bdaddr`.
Table entry `xradio`: init_speed 115200, speed 1500000, flags 0 (no flow control at start), proto 0 (H4).

## 2. Protocol, step by step (as executed by `xradio_init`)

Host UART: 8N1, raw, **no hardware flow control** during the BROM phase, 115200 baud.

1. Locate the rfkill with `type == bluetooth` (`/sys/class/rfkill/rfkillN/state`).
2. `echo 0 > state` (blocked, BT_RST_N low); usleep 20 ms; `echo 1 > state` (unblocked, reset released); usleep 20 ms.
   Vendor kernel `sunxi_bt_set_block` also enables bldo1 (3.3 V) and aldo3 (3.3 V IO) here; the 32 kHz comes from the RTC fanout.
3. `echo 1 > /proc/bluetooth/sleep/btwake` (BT_WAKE asserted) and `echo 0 > /proc/bluetooth/sleep/lpm` (LPM off); usleep 5 ms.
4. **ROM sync** (`userial_sync`): set fd O_NONBLOCK; loop: write one byte `0x55`, usleep 20 ms, `read(2)`;
   success when the 2 bytes are `"OK"` (or `"KO"`). Note the vendor prints the buffer even when read returned -1 (EAGAIN),
   so a printed `00 00` means NOTHING was received. After 16 failures: rfkill 0, 500 ms, rfkill 1, 20 ms, continue (endless).
5. **SET_UART** (cmd 0x10), arg = `0x03000000 | 1500000` (= 0x0316E360), 17-byte packet. Read 12-byte ack, check. Then switch host termios to
   1500000 and run `userial_sync` again (0x55 until "OK") at the new rate.
6. **Firmware**: loop over the file in 1024-byte chunks, offset starting 0: send SEQWR header packet (cmd 0x09, offset, length, data checksum);
   read 12-byte ack; write the raw chunk (no framing); read 12-byte ack. (If the ack flag bit0 is set one extra error byte follows.)
   The vendor only prints on errors and keeps going; a real tool should abort.
7. **SET_PC** (cmd 0x13) arg 0; read ack. Firmware starts.
8. usleep 5 ms. Host back to **115200**, enable CRTSCTS (hardware flow control ON from here), tcflush, usleep 10 ms.
9. HCI (H4, plain): `01 03 0c 00` (Reset), read event.
10. `01 18 fc 04 60 e3 16 00` (vendor cmd 0xFC18, LE u32 1500000 = change controller baud), read event, set host to 1500000, sleep 100 ms.
11. **Set BD_ADDR**: `01 0a fc 09 02 00 06 <bdaddr[6] little-endian>` (vendor cmd 0xFC0A, 13 bytes), read event.
    Address source order: `/sys/class/addr_mgt/addr_bt` (vendor-kernel-only; absent on mainline), then `/etc/bluetooth/xr_bt.conf`
    (text "aa bb cc dd ee ff", MSB first), else random (written back to `xr_bt.conf`). For mainline: generate once, persist, or derive from the SoC serial.
12. `01 03 0c 00` HCI Reset again, read event.
13. `ioctl(TIOCSETD, N_HCI=15)` then `ioctl(HCIUARTSETPROTO, HCI_UART_H4=0)`. hci0 appears. (Equivalent: close the port and run `btattach -B /dev/ttyS1 -P h4 -S 1500000`
    except that btattach opens with its own flow settings; keep CRTSCTS on.)

### BROM packet format (reverse engineered)

```
off  len  field
0    4    "BROM"
4    1    flags: bit0 = error (one extra error byte follows in a response), bit1 = response/ack, bit2 = checksum present. Requests use 0x04.
5    1    0
6    2    header checksum, big endian
8    4    payload length, big endian
12   n    payload: cmd byte, then cmd args as big-endian u32 (SEQWR: off u32, len u32, datacsum u16)
```
Header checksum: build the same packet with every integer in *little-endian* form and the checksum field zero, sum all 16-bit LE
words of the whole packet (a trailing odd byte is added as a byte), checksum = `~sum & 0xffff`; transmit it big endian. The SEQWR data
checksum is `~sum16` of the chunk computed the same way, sent big endian. A response is valid if flags bit1 set, length 0 and
(with bit2) the same word sum over the 12-byte header (checksum/length taken as logical values) is 0xFFFF. Worked values from the
decompiled formula: SET_PC(0) = `42524f4d04006052000000051300000000`; SET_UART(1.5M) = `42524f4d0400e96f00000005100316e360`.
I could not test the checksum byte order against a real BROM; if the first ack says checksum error, the likeliest fix is the byte order of the two checksum fields.

## 3. Reference implementation helper (userspace, Python)

```python
import struct
def csum_native(buf):            # buf = packet with ints in LE ("native") layout, csum field zero
    s = 0
    for i in range(0, len(buf) - 1, 2):
        s += buf[i] | (buf[i+1] << 8)
    if len(buf) & 1:
        s += buf[-1]
    return (~s) & 0xffff
def pkt(cmd, args=b""):          # args: list of (value, nbytes) ints
    payload = bytes([cmd]); native = bytes([cmd])
    for v, n in args:
        payload += v.to_bytes(n, "big"); native += v.to_bytes(n, "little")
    L = len(payload)
    nat = b"BROM" + bytes([4, 0, 0, 0]) + L.to_bytes(4, "little") + native
    c = csum_native(nat)
    return b"BROM" + bytes([4, 0]) + c.to_bytes(2, "big") + L.to_bytes(4, "big") + payload
def set_uart(baud): return pkt(0x10, [(0x03000000 | baud, 4)])
def set_pc(pc):     return pkt(0x13, [(pc, 4)])
def seqwr(off, data):           # header packet; then send `data` raw, then read a 2nd 12-byte ack
    return pkt(0x09, [(off, 4), (len(data), 4), (csum_native(data), 2)])
def check_resp(r):               # 12-byte ack
    assert r[:4] == b"BROM" and len(r) == 12
    flags = r[4]
    ln = int.from_bytes(r[8:12], "big")
    c = int.from_bytes(r[6:8], "big")
    s = sum(struct.unpack("<HH", r[0:4])) + (r[4] | r[5] << 8) + c + ln
    return (not flags & 1) and (flags & 2) and ln == 0 and ((not flags & 4) or (s & 0xffff) == 0xffff)
if __name__ == "__main__":
    print(set_uart(1500000).hex(), set_pc(0).hex(), seqwr(0, bytes(range(8))).hex())
```

Uploader driver sketch (pyserial; hold the tty open the whole time, never close between phases, see feedback_uart_port_cycling):
power-cycle via `/sys/class/rfkill/*/state`, write btwake=1/lpm=0, spam 0x55 every 20 ms until "OK", `pkt` exchanges as above,
switch baud with `termios`, upload, set_pc, then 115200+CRTSCTS HCI reset, 0xFC18, 0xFC0A, reset, `TIOCSETD`/`HCIUARTSETPROTO`
(`fcntl.ioctl(fd, 0x5423, 15)`, `fcntl.ioctl(fd, 0x400455c8, 0)`). Python's `fcntl`/`termios` have everything; no extra libs besides optional pyserial.

## 4. Recommended path: (a) userspace uploader first, (b) serdev driver later

(a) Userspace: ~150 lines of Python or ~300 lines of C (the existing bluez-5.54 `xr829` hciattach on the device is already this; fix its sync, do not rewrite).
Effort: 0.5 day to get hci0 if the chip answers; the protocol is now fully known, so the remaining work is hardware bring-up. Risk: low, no kernel rebuilds
and it matches the vendor design (the vendor does exactly this in userspace). Needs the tool running at every boot, before bluetoothd; fits an OpenRC/init script.

(b) Kernel: a `hci_uart` protocol / serdev driver `btxradio` (like `btqca`/`hci_bcm`): `setup()` would run steps 4 to 12 using `serdev_device_*`
(set_baudrate, write_buf, wait_until_sent, `serdev_device_set_flow_control`), request_firmware(`xr829/fw_xr829_bt.bin`) and `hci_set_bdaddr`/`HCI_QUIRK_USE_BDADDR_PROPERTY`
(local-bd-address from DT or nvmem), plus own power sequencing (vbat regulator, 32k clock, shutdown gpio, device-wake) in DT under `&uart1 { bluetooth { ... } }`.
Effort 2 to 4 days incl. DT binding. Pros: upstreamable shape, no btwake/lpm proc hacks, proper power management, loads modules automatically.
Cons: the BROM phase needs raw byte-level blocking reads with timeouts inside `setup`, and every experiment is a kernel build. The user said no upstreaming until polished,
so do (a) first to prove the hardware path, then port the verified sequence to (b).

## 5. Likely reason the current sync never gets "OK" (from comparing with the vendor flow)

Evidence from the DT/patch: in `sun50i-a133-trimui-smartpro.dts` `reg_bldo1` (vcc-wifi) is NOT always-on and `CLK_OSC32K_FANOUT` is only
enabled by the WiFi `mmc-pwrseq`, and neither is held by `rfkill-gpio`/`btlpm`. If WiFi (mmc1) is not active at test time, the chip has no power and/or no 32 kHz,
which gives exactly "no byte ever received". Checks/fixes, in order:
1. Add `clocks = <&rtc CLK_OSC32K_FANOUT>;` to the `rfkill-bt` node (rfkill-gpio supports one optional clock) and give bldo1/aldo3 a keeper
   (temporarily `regulator-always-on` on bldo1 to test; long term the serdev node's `vbat-supply`).
2. Measure whether the vendor timing matters: vendor starts 0x55 5 ms after releasing reset (20 ms after the rfkill write) and sends every 20 ms; a BROM sync
   window that ends after a short time would be missed by a tool that starts late. Start spamming immediately after unblock.
3. Treat `00 00` as "no data", not as data. Confirm RX activity with the chip in reset vs released (any glitch on RX = chip alive).
4. Make sure RTS (PG8) is not deasserted and CRTSCTS is off for the BROM phase; PG8/PG9 pinmux function = uart1 RTS/CTS.
5. PL2 level: vendor DT has `bt_rst_n` data=0 default; unblocked = high (already verified).

## 6. Open questions that need hardware

- Does the BROM answer at all at 115200 once 32k + bldo1 are guaranteed on (item 5.1)? What is the real sync window length?
- Checksum byte order (section 2) and the exact meaning of flag bits and of the 0x03 in the SET_UART arg (assumed UART mode/flow: 8N1 + something).
- Does the controller need CRTSCTS to be on before the 1.5 Mbaud switch, and does the XR829 BT UART tolerate 1.5 Mbaud on the A133 UART1 clock (24 MHz gives exactly 1.5M with divisor 1 at 16x).
- Which BD_ADDR to use (no `addr_mgt` on mainline): is the chip's default address valid when 0xFC0A is skipped; WiFi MAC derivation?
- Idle/LPM: what the controller expects on BT_WAKE/BT_HOSTWAKE after HCI is up (vendor sets lpm=0, so a first version can keep BT_WAKE high and skip LPM).
- Coexistence with the xradio WiFi driver's BT hooks (WSM_EVENT_BT_ACTIVE).
