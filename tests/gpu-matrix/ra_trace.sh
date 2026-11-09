#!/bin/sh
# RetroArch under cage, trace 3 s in steady state
printf 'video_vsync = "true"\naudio_driver = "null"\n' > /tmp/edge/root/var.cfg
cat /tmp/edge/root/.config/retroarch/trimui.cfg /tmp/edge/root/var.cfg > /tmp/edge/root/var2.cfg
/root/wayland-session.sh /usr/bin/env HOME=/root retroarch --appendconfig=/root/var2.cfg --max-frames=900 \
  -L /usr/lib/libretro/mgba_libretro.so "/root/roms/gba/Advance Wars (Europe) (En,Fr,De,Es).gba" > /tmp/ra_tr.log 2>&1 &
sleep 10
sh /root/gpu_trace.sh 3 /tmp/trace_ra.txt > /dev/null
wait
echo done
