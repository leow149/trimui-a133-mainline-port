#!/bin/sh
# Wi-Fi ping during a raw HCI LE scan. Usage on the device: sh -s INTERVAL WINDOW < tests/bt_coex_scan.sh
# (hex, 0.625 ms units, for example 60 30 = 50% duty, 10 10 = 100% duty). Needs the edge chroot and hci0.
H="chroot /tmp/edge /usr/bin/env PATH=/usr/bin:/bin"
INT=${1:-60}; WIN=${2:-30}
$H hciconfig hci0 up
echo "before:"; ping -c 5 -W 2 -I wlan0 192.168.1.1 | grep transmitted
( timeout 14 $H btmon > /tmp/btmon.txt 2>&1 & )
$H hcitool cmd 0x08 0x000B 00 $INT 00 $WIN 00 00 00 | tail -1
$H hcitool cmd 0x08 0x000C 01 00 | tail -1
echo "during scan (interval 0x$INT window 0x$WIN):"; ping -c 10 -W 2 -I wlan0 192.168.1.1 | grep transmitted
$H hcitool cmd 0x08 0x000C 00 00 | tail -1
sleep 3
echo "after:"; ping -c 6 -W 2 -I wlan0 192.168.1.1 | grep transmitted
echo "adv reports: $(grep -c 'LE Advertising Report' /tmp/btmon.txt)"
echo "auth events total: $(dmesg | grep -c -E 'wlan0: (deauth|authenticate)')"
