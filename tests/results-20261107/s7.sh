cd /root/verify-20261107
echo "== audio"; /root/tinymix controls 2>&1 | head -30; /root/tinymix contents 2>&1 | grep -iE -A3 "jack|headphone" | head -20; cat /proc/asound/card0/pcm0p/info | head -3
echo "== led"; L=/sys/class/leds/rgb:indicator-0; cat $L/multi_intensity; echo "3 3 3" > $L/multi_intensity; echo 10 > $L/brightness; cat $L/brightness; echo 0 > $L/brightness; echo "0 0 0" > $L/multi_intensity; cat $L/brightness $L/multi_intensity
echo "== cpufreq under load"; for i in 1 2 3 4; do (timeout 4 sh -c 'while :; do :; done' &) ; done; sleep 2; cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq; cat /sys/class/thermal/thermal_zone0/temp; sleep 3; cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq
echo "== wifi"; timeout 60 sh /root/wifi-up.sh 2>&1 | tail -3; iw dev wlan0 link | head -5; ip -br a show wlan0; ping -c3 -W2 -I wlan0 $(ip route | awk '/default.*wlan0/{print $3}' | head -1) 2>&1 | tail -3
ip link set wlan0 down 2>&1; killall wpa_supplicant 2>&1; ip -br a
echo "== brightness readback"; cat /sys/class/backlight/backlight/actual_brightness
