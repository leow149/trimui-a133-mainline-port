#!/bin/sh
# Arm AXP717 CC detection (e3=13, e1=0d), log e7/irq4/VBUS every 0.5 s for $1 s (default 120), then restore.
R=/sys/kernel/debug/regmap/0-0034/registers
mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug
rd() { grep "^$1:" $R | cut -d' ' -f2; }
O3=$(rd e3); O1=$(rd e1); N=$(( ${1:-120} * 2 )); L=/root/cc_watch.log
trap '/root/i2creg 0 34 e1 $O1 >/dev/null; /root/i2creg 0 34 e3 $O3 >/dev/null; echo "restored e1=$O1 e3=$O3" >> $L' EXIT
/root/i2creg 0 34 4c 04 >/dev/null
/root/i2creg 0 34 e3 13 >/dev/null; /root/i2creg 0 34 e1 0d >/dev/null
echo "armed $(date +%T) e1=$O1 e3=$O3" > $L
prev=
i=0; while [ $i -lt $N ]; do
  s="e7=$(rd e7) irq4=$(rd 4c) on=$(rd 00) bc=$(rd 05) vbus=$(cat /sys/class/power_supply/axp20x-usb/voltage_now)"
  k=${s% vbus=*}
  [ "$k" != "$prev" ] && echo "$(cut -d' ' -f1 /proc/uptime) $s" >> $L
  prev=$k; i=$((i+1)); sleep 0.5
done
