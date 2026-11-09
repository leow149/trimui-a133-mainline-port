#!/bin/sh
# shot.sh <name> <video_driver>: RetroArch mGBA under cage, grim screenshot after 15 s; env passes through
n=$1
printf 'video_driver = "%s"\nvideo_context_driver = "wayland"\naudio_driver = "null"\n' "$2" > /tmp/edge/root/shot.cfg
cat /tmp/edge/root/.config/retroarch/trimui.cfg /tmp/edge/root/shot.cfg > /tmp/edge/root/shot2.cfg
/root/wayland-session.sh /usr/bin/env HOME=/root retroarch --appendconfig=/root/shot2.cfg \
  -L /usr/lib/libretro/mgba_libretro.so "/root/roms/gba/Advance Wars (Europe) (En,Fr,De,Es).gba" > /tmp/shot_$n.log 2>&1 &
sleep ${WAIT:-15}
chroot /tmp/edge /usr/bin/env XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-0 grim /root/g_$n.png
/root/wayland-session.sh stop > /dev/null 2>&1; sleep 3
for p in $(pidof cage retroarch); do kill $p; done
echo shot $n
