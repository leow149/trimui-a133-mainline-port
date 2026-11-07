mkdir -p /root/verify-20261107
echo "== dmesg warn/err"; dmesg | grep -iE "warn|error|fail|timeout|BUG|oops| -[0-9]+$|cannot|unable" | head -60
echo "== cmdline"; cat /proc/cmdline
echo "== blkid"; blkid; ls /dev/video* /dev/media* /dev/dri/* /dev/input/* /dev/rtc* /dev/watchdog* 2>&1
echo "== input"; for d in /sys/class/input/input*; do echo "$d: $(cat $d/name)"; done
echo "== power"; for p in /sys/class/power_supply/*; do echo "-- $p"; for f in type status capacity voltage_now current_now present online health temp; do [ -f $p/$f ] && echo "$f=$(cat $p/$f)"; done; done
echo "== rtc"; cat /sys/class/rtc/rtc*/name /sys/class/rtc/rtc*/since_epoch; hwclock -r 2>&1; date
echo "== cpufreq"; cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies
echo "== thermal"; for t in /sys/class/thermal/thermal_zone*; do echo "$(cat $t/type) $(cat $t/temp)"; done
echo "== backlight"; for b in /sys/class/backlight/*; do echo "$b $(cat $b/brightness)/$(cat $b/max_brightness)"; done
echo "== leds"; ls /sys/class/leds | wc -l; ls /sys/class/leds | head -30
echo "== drm"; for c in /sys/class/drm/card*-*; do echo "$c $(cat $c/status) $(cat $c/modes 2>/dev/null | head -2 | tr '\n' ' ')"; done
echo "== watchdog"; ls /sys/class/watchdog; cat /sys/class/watchdog/watchdog0/identity /sys/class/watchdog/watchdog0/state 2>&1
echo "== usb"; ls /sys/bus/usb/devices; cat /sys/class/udc/*/state
echo "== iommu"; ls /sys/class/iommu /sys/kernel/iommu_groups 2>&1| head; dmesg | grep -i iommu | head
echo "== alsa"; cat /proc/asound/cards; ls /dev/snd
echo "== lsmod"; lsmod
echo "== net"; ip -br a; ip link
echo "== bt"; ls /sys/class/bluetooth 2>&1; rfkill list 2>&1; dmesg | grep -iE "bluet|hci|btlpm|bt_|rfkill"
echo "== v4l"; for v in /sys/class/video4linux/*; do echo "$v $(cat $v/name)"; done
echo "== mounts edge"; mount | grep -E "edge|TRIMUIDATA"; ls /tmp/edge | head -3
