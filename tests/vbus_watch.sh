#!/bin/sh
# Log AXP717 boost-to-VBUS (0x13), power status (0x00) and the USB ID pin every 0.2 s, on change only, for $1 s.
N=$(( ${1:-120} * 5 )); L=/root/vbus_watch.log; prev=; i=0
mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug
echo "start $(cut -d' ' -f1 /proc/uptime)" > $L
while [ $i -lt $N ]; do
  s="$(/root/i2creg 0 34 13 | cut -d= -f2) $(/root/i2creg 0 34 00 | cut -d= -f2) id=$(grep usb0_id /sys/kernel/debug/gpio | grep -o ' lo \| hi ')"
  [ "$s" != "$prev" ] && echo "$(cut -d' ' -f1 /proc/uptime) 13=${s}" >> $L
  prev=$s; i=$((i+1)); sleep 0.2
done
echo end >> $L
