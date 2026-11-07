#!/bin/sh
# Bring up XR829 Bluetooth as hci0: upload the firmware over ttyS1 and attach H4
# (tools/bt/xr829_bt_up, see docs/bluetooth-xr829-protocol.md). The BD address is the
# Wi-Fi MAC (stable, derived from the SoC ID by U-Boot) with the last byte + 1.
# The uploader leaves a resident child holding the tty; kill it to detach hci0.
# Runs in the background: the Wi-Fi driver loads its firmware into the same chip at
# boot, and a BT reset during that makes the BT boot ROM miss the sync. Wait for Wi-Fi,
# then retry with a timeout.
T=/root/bt/xr829_bt_up
[ -x "$T" ] || exit 0
pidof xr829_bt_up >/dev/null && exit 0
(
	for i in $(seq 1 30); do
		[ -e /sys/class/net/wlan0/address ] && break
		sleep 1
	done
	sleep 3
	MAC=$(cat /sys/class/net/wlan0/address 2>/dev/null)
	if [ -n "$MAC" ]; then
		ADDR=$(printf '%s:%02x' "${MAC%:*}" $(( (0x${MAC##*:} + 1) % 256 )))
	else
		ADDR=02:00:00:aa:bb:01
	fi
	# The first uploader run after boot never gets the BROM sync, even across
	# chip power cycles, later delays or BT_WAKE pre-set; the next run syncs at
	# once. Cause not found yet (2026-11-07). Do a short throw-away probe first.
	timeout 5 $T -p >/dev/null 2>&1
	for try in 1 2 3; do
		timeout 25 $T -F 0 -a "$ADDR" 2>/tmp/bt-up.log
		if [ -e /sys/class/bluetooth/hci0 ]; then
			# System D-Bus + bluetoothd (apk: dbus bluez). bluetoothd powers hci0
			# itself (AutoEnable), then bluetoothctl works for scanning/pairing.
			if [ -x /usr/lib/bluetooth/bluetoothd ]; then
				mkdir -p /run/dbus
				dbus-uuidgen --ensure
				# /run is on the persistent rootfs here, so clear a stale pid file
				# and socket from the previous boot or dbus-daemon refuses to start.
				pidof dbus-daemon >/dev/null || {
					rm -f /run/dbus/dbus.pid /run/dbus/system_bus_socket
					dbus-daemon --system
				}
				pidof bluetoothd >/dev/null || /usr/lib/bluetooth/bluetoothd
			fi
			exit 0
		fi
		sleep 2
	done
) >/dev/null 2>&1 </dev/null &
