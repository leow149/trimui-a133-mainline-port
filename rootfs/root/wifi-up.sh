#!/bin/sh
# Bring up WiFi with /etc/wpa_supplicant/home.conf (bring-up helper).
# The firmware is built into the kernel; the driver comes up at boot.

ip link set wlan0 up
# Restart wpa_supplicant if one is running with another config (e.g. a WPA3 test
# network), otherwise this would silently leave wlan0 on that network.
for p in $(pidof wpa_supplicant); do
	tr '\0' ' ' < /proc/$p/cmdline | grep -q /etc/wpa_supplicant/home.conf || { kill $p; sleep 1; }
done
pidof wpa_supplicant >/dev/null || \
	setsid wpa_supplicant -t -i wlan0 -c /etc/wpa_supplicant/home.conf > /tmp/wpa.log 2>&1 < /dev/null &
for i in $(seq 1 20); do
	iw dev wlan0 link | grep -q Connected && break
	sleep 1
done
# Keep udhcpc running (no -q) so it renews the lease and re-acquires one after a
# reconnect; a one-shot client left wlan0 without IPv4 once the lease ran out.
# -b: carry on in the background if there is no lease after the 5 tries.
P=/run/udhcpc.wlan0.pid
[ -f $P ] && kill $(cat $P) 2>/dev/null
udhcpc -i wlan0 -t 5 -b -p $P
# Sync the clock once per connect and keep the RTC (which persists while the battery is
# connected) in step. On this LAN any NTP server works: the router redirects NTP to the Pi.
timeout 15 ntpd -n -q -p 192.168.1.1 >/dev/null 2>&1 && hwclock -w -u
