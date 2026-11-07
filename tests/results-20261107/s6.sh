cd /root/verify-20261107
echo "== vulkan"; timeout 60 chroot /tmp/edge env PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 vulkaninfo --summary 2>&1 | grep -E "deviceName|apiVersion|driverName|driverInfo|GPU[0-9]" | head -8
dmesg | grep -iE "powervr|pvr" | head -5
echo "== wifi conf"; ls -la /etc/wpa_supplicant/ 2>&1; cat /root/wifi-up.sh | head -20
echo "== wifi scan"; ip link set wlan0 up; timeout 5 iw dev wlan0 scan 2>&1 | grep -E "SSID|signal" | head -12 ; which iw wpa_supplicant wpa_cli
dmesg | grep -iE "xr829|xradio" | head -8
echo "== audio ctl"; /root/tinymix 2>&1 | head -25
echo "== usb host"; cat /sys/bus/usb/devices/1-0:1.0/../manufacturer /sys/bus/usb/devices/usb1/product 2>&1; ls /sys/bus/usb/devices
echo "== slider/input caps"; for i in 0 1 2 3 4; do echo "event$i: $(cat /sys/class/input/input$i/name) driver=$(basename $(readlink /sys/class/input/input$i/device/device/driver 2>/dev/null) 2>/dev/null) ev=$(cat /sys/class/input/input$i/capabilities/ev) sw=$(cat /sys/class/input/input$i/capabilities/sw) abs=$(cat /sys/class/input/input$i/capabilities/abs)"; done
echo "== jack state (read-only)"; ls /root/tinymix; 
echo "== gamepad serdev"; dmesg | grep -iE "gamepad|serdev|trimui" | head
echo "== wdt"; dmesg | grep -iE "wdt|watchdog"; ps | grep -iE "watchdog|wdt" | head -3
echo "== led test"; L=/sys/class/leds/rgb:indicator-0; ls $L; cat $L/brightness $L/max_brightness; ls $L
