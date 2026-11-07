#!/bin/sh
# Bring up WiFi with /etc/wpa_supplicant/home.conf (bring-up helper).
# firmware is built into the kernel now; the driver comes up at boot

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
udhcpc -i wlan0 -n -q -t 5
# Sync the clock once per connect and keep the RTC (which persists while the battery is
# connected) in step. On this LAN any NTP server works: the router redirects NTP to the Pi.
timeout 15 ntpd -n -q -p 192.168.1.1 >/dev/null 2>&1 && hwclock -w -u
