#!/bin/sh
# usage: ra_shot.sh name core [content]   (device-side) ; env EXTRA appended to the retroarch command
n=$1; core=$2; shift 2
/root/retroarch.sh stop >/dev/null 2>&1; sleep 2
(/root/wayland-session.sh /usr/bin/env HOME=/root $ENVX retroarch --verbose --appendconfig /root/${CFG:-ra_vk.cfg} --log-file /root/ra_$n.log -L /usr/lib/libretro/$core "$@" >/root/ra_$n.out 2>&1 &)
sleep ${WAIT:-20}
chroot /tmp/edge /usr/bin/env XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-0 grim /root/g_$n.png
grep -i "Using RGB\|Using .* format\|texture\|Vulkan\]" /tmp/edge/root/ra_$n.log | head -12
