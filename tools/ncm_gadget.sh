#!/bin/sh
# Runs ON THE TRIMUI: USB network gadget (CDC NCM) for throughput tests and IP
# access over the charging port. ncm_gadget.sh [stop]. No TAB characters.
G=/sys/kernel/config/usb_gadget/ncm
mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config
if [ "${1:-}" = stop ]; then
    [ -d $G ] || exit 0
    [ -s $G/UDC ] && echo "" > $G/UDC
    rm -f $G/configs/c.1/ncm.usb0
    for d in configs/c.1/strings/0x409 configs/c.1 functions/ncm.usb0 strings/0x409; do
        [ -d $G/$d ] && rmdir $G/$d
    done
    rmdir $G && echo "ncm_gadget: stopped"
    exit 0
fi
set -e
mkdir $G
echo 0x1d6b > $G/idVendor
echo 0x0104 > $G/idProduct
mkdir $G/strings/0x409
echo "TrimUI" > $G/strings/0x409/manufacturer
echo "TrimUI NCM gadget" > $G/strings/0x409/product
mkdir -p $G/configs/c.1/strings/0x409
echo "ncm" > $G/configs/c.1/strings/0x409/configuration
mkdir $G/functions/ncm.usb0
echo 02:54:53:50:00:01 > $G/functions/ncm.usb0/dev_addr
echo 02:54:53:50:00:02 > $G/functions/ncm.usb0/host_addr
ln -s $G/functions/ncm.usb0 $G/configs/c.1/
ls /sys/class/udc > $G/UDC
ip link set $(cat $G/functions/ncm.usb0/ifname) up
echo "ncm_gadget: $(cat $G/functions/ncm.usb0/ifname) up on $(cat $G/UDC)"
