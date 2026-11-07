#!/bin/sh
# Wake sources for s2idle. The power key (axp20x-pek) is enabled by its driver; the
# LRADC volume keys are only wakeup-capable (DT wakeup-source), so enable them here.
# Verified 2026-11-08: power, vol+ and vol- each wake the device from suspend.
for d in /sys/bus/platform/devices/*.lradc/power/wakeup; do
	[ -w "$d" ] && echo enabled > "$d"
done
exit 0
