#!/bin/sh
R=/sys/kernel/debug/regmap/0-0034/registers
rd() { grep "^$1:" $R | cut -d' ' -f2; }
O3=$(rd e3); O1=$(rd e1)
trap '/root/i2creg 0 34 e1 $O1 >/dev/null; /root/i2creg 0 34 e3 $O3 >/dev/null; /root/i2creg 0 34 4c 04 >/dev/null' EXIT
for m in 10 13; do
  /root/i2creg 0 34 e3 $m >/dev/null
  for v in 0b 0d 02 04 0a 0c 09; do
    /root/i2creg 0 34 e1 $v >/dev/null; sleep 2
    a=$(rd e7); sleep 1; b=$(rd e7); sleep 1; c=$(rd e7)
    echo "e3=$m e1=$v: e7=$a,$b,$c irq4=$(rd 4c)"
  done
done
