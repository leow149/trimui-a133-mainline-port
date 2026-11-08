#!/bin/sh
# vcc-pad suspend test: LEDs dim white, 10 s s2idle (RTC wake), then wait up to 40 s for a gamepad event.
L=/root/padsusp.log
EV=$(grep -l "TrimUI Smart Pro Gamepad" /sys/class/input/event*/device/name | head -1 | cut -d/ -f5)
for d in /sys/class/leds/rgb:indicator-*; do echo "40 40 40" > $d/multi_intensity; echo 255 > $d/brightness; done
echo "start $(cut -d' ' -f1 /proc/uptime) ev=$EV" > $L
sleep 5
echo 0 > /sys/class/rtc/rtc0/wakealarm; echo +10 > /sys/class/rtc/rtc0/wakealarm
sync; echo freeze > /sys/power/state
echo "resumed rc=$? $(cut -d' ' -f1 /proc/uptime)" >> $L
grep -A4 vcc-pad /sys/kernel/debug/regulator/regulator_summary >> $L
dmesg | grep -iE "pad|ledc|vled|serial" | tail -5 >> $L
if timeout 40 head -c 24 /dev/input/$EV > /dev/null; then echo "gamepad event OK $(cut -d' ' -f1 /proc/uptime)" >> $L; else echo "NO gamepad event in 40 s" >> $L; fi
