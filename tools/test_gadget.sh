#!/bin/sh
# Runs ON THE TRIMUI: minimal USB gadget (one ACM serial function, no storage) to
# test enumeration. test_gadget.sh [stop]. No TAB characters (UART paste).
G=/sys/kernel/config/usb_gadget/test
mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config
if [ "${1:-}" = stop ]; then
    [ -d $G ] || exit 0
    [ -s $G/UDC ] && echo "" > $G/UDC
    rm -f $G/configs/c.1/acm.0
    for d in configs/c.1/strings/0x409 configs/c.1 functions/acm.0 strings/0x409; do
        [ -d $G/$d ] && rmdir $G/$d
    done
    rmdir $G && echo "test_gadget: stopped"
    exit 0
fi
set -e
mkdir $G
echo 0x1d6b > $G/idVendor
echo 0x0104 > $G/idProduct
mkdir $G/strings/0x409
echo "TrimUI" > $G/strings/0x409/manufacturer
echo "TrimUI test gadget" > $G/strings/0x409/product
mkdir -p $G/configs/c.1/strings/0x409
echo "acm" > $G/configs/c.1/strings/0x409/configuration
mkdir $G/functions/acm.0
ln -s $G/functions/acm.0 $G/configs/c.1/
ls /sys/class/udc > $G/UDC
echo "test_gadget: bound to $(cat $G/UDC)"
