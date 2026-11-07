#!/bin/sh
L=/root/verify-20261107/suspend.log
{
date; cat /sys/power/state /sys/power/mem_sleep 2>&1
dmesg > /root/verify-20261107/dmesg_pre_suspend.txt
sync
ls /sys/class/rtc/rtc0/wakealarm
if command -v rtcwake >/dev/null; then echo "using rtcwake"; rtcwake -m freeze -s 15; echo "rtcwake rc=$?"
else
 echo 0 > /sys/class/rtc/rtc0/wakealarm; echo +15 > /sys/class/rtc/rtc0/wakealarm; echo "alarm set: $(cat /sys/class/rtc/rtc0/wakealarm)"; echo freeze > /sys/power/state; echo "write rc=$?"
fi
date
} > $L 2>&1
