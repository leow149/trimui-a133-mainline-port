#!/bin/sh
R=/sys/kernel/debug/regmap/0-0034/registers
rd() { grep "^$1:" $R | cut -d' ' -f2; }
O3=$(rd e3); O1=$(rd e1)
trap '/root/i2creg 0 34 e1 $O1 >/dev/null; /root/i2creg 0 34 e3 $O3 >/dev/null' EXIT
/root/i2creg 0 34 e3 13 >/dev/null
for v in 08 01 0d 0b 0f 00; do
  /root/i2creg 0 34 e1 $v >/dev/null; sleep 3
  echo "e3=13 e1=$v: e7=$(rd e7) irq4=$(rd 4c) on=$(rd 00) vbusV=$(cat /sys/class/power_supply/axp20x-usb/voltage_now)"
done
