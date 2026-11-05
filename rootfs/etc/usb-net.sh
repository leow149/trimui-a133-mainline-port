#!/bin/sh
# sysinit: USB network on the charging port (NCM gadget, device 10.55.0.2/24,
# host side 10.55.0.1) plus an SSH server on it (dropbear, key login only).
# The laptop matches the gadget by its host MAC 02:54:53:50:00:02.
G=/sys/kernel/config/usb_gadget/usbnet
mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config
if [ ! -d $G ]; then
    mkdir $G
    echo 0x1d6b > $G/idVendor
    echo 0x0104 > $G/idProduct
    mkdir $G/strings/0x409
    echo "TrimUI" > $G/strings/0x409/manufacturer
    echo "TrimUI Smart Pro" > $G/strings/0x409/product
    mkdir -p $G/configs/c.1/strings/0x409
    echo "usbnet" > $G/configs/c.1/strings/0x409/configuration
    mkdir $G/functions/ncm.usb0
    echo 02:54:53:50:00:01 > $G/functions/ncm.usb0/dev_addr
    echo 02:54:53:50:00:02 > $G/functions/ncm.usb0/host_addr
    ln -s $G/functions/ncm.usb0 $G/configs/c.1/
    ls /sys/class/udc > $G/UDC
fi
IF=$(cat $G/functions/ncm.usb0/ifname)
ip addr add 10.55.0.2/24 dev $IF 2>/dev/null
ip link set $IF up
mkdir -p /etc/dropbear
pidof dropbear > /dev/null || dropbear -R -s -p 10.55.0.2:22
