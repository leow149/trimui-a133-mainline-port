#!/bin/sh
# Bring up WiFi with /etc/wpa_supplicant/home.conf (bring-up helper).
# firmware is built into the kernel now; the driver comes up at boot

ip link set wlan0 up
pidof wpa_supplicant >/dev/null || \
	setsid wpa_supplicant -t -i wlan0 -c /etc/wpa_supplicant/home.conf > /tmp/wpa.log 2>&1 < /dev/null &
for i in $(seq 1 20); do
	iw dev wlan0 link | grep -q Connected && break
	sleep 1
done
udhcpc -i wlan0 -n -q -t 5
