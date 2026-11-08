#!/bin/sh
# AXP717 Type-C CC probe: try 0xE3 low bits, read 0xE7 (volatile -> real I2C read via regmap debugfs).
# Always restores 0xE3 to its original value.
R=/sys/kernel/debug/regmap/0-0034/registers
mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug
rd() { grep "^$1:" $R | cut -d' ' -f2; }           # cached unless volatile
rdraw() { for i in 1 2 3 4 5; do /root/i2creg 0 34 $1; done | sed 's/.*= //' | sort | uniq -c | sort -rn | head -1 | awk '{print $2}'; }
ORIG=$(rd e3)
trap '/root/i2creg 0 34 e3 $ORIG >/dev/null' EXIT
state() {
  echo "$1: e3raw=$(rdraw e3) e7=$(rd e7) irq4=$(rd 4c 2>/dev/null) on=$(rd 00) vbusV=$(cat /sys/class/power_supply/axp20x-usb/voltage_now) bc=$(rd 05) bat_mA=$(( $(cat /sys/class/power_supply/axp20x-battery/current_now) / 1000 ))"
}
echo "orig e3=$ORIG e1=$(rd e1) e0=$(rd e0) e2=$(rd e2)"
state base
for v in 11 12 13; do
  /root/i2creg 0 34 e3 $v >/dev/null
  sleep 3
  state "e3=$v"
  sleep 2
  state "e3=$v+5s"
done
/root/i2creg 0 34 e3 $ORIG >/dev/null
sleep 3
state restored
