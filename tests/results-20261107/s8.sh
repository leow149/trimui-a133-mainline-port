cd /root/verify-20261107
uname -a; ls; date
dmesg > dmesg_post_reboot.txt
dmesg | grep -iE "reset|wdt|watchdog|PM:|suspend|resume|warn|error|fail" | head -30
ls -la /sys/fs/pstore 2>&1 | head -5
cat /sys/class/rtc/rtc0/wakealarm; ls /tmp/edge/root | head -2
echo ---; ip -br addr 2>&1 | head -0; ls /dev/video* /dev/dri/*; cat /sys/class/drm/card1-DSI-1/status
