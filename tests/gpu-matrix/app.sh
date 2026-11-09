#!/bin/sh
# app.sh <tag> <wait> <program> [args...]: run a Wayland client under cage, grim + 3 s GPU trace + underflows
T=$1; W=$2; shift 2
O=/root/ax; mkdir -p $O /tmp/edge/root/ax
dmesg -c > /dev/null
for p in $(pidof cage); do kill -9 $p; done; sleep 1
/root/wayland-session.sh /usr/bin/env HOME=/root QT_QPA_PLATFORM=wayland "$@" > $O/$T.log 2>&1 &
sleep $((W - 4))
/root/uflow 8 > $O/$T.uflow 2>&1 &
sleep 4
timeout 10 chroot /tmp/edge /usr/bin/env XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-0 grim /root/ax/$T.png 2>/dev/null
sh /root/gpu_trace.sh 3 $O/$T.trace > /dev/null
wait $! 2>/dev/null; sleep 2
/root/wayland-session.sh stop > /dev/null 2>&1; sleep 2
for p in $(pidof cage); do kill $p; done; sleep 1
for p in $(pidof PPSSPPQt moonlight vkcube weston-simple-egl es2gears_wayland weston-simple-vulkan weston-simple-dmabuf-egl); do kill -9 $p; done
echo "$T vblanks=$(grep -c drm_vblank_event $O/$T.trace) uflow=\"$(tail -1 $O/$T.uflow)\" shot=$([ -f /tmp/edge/root/ax/$T.png ] && echo y || echo n) kmsg=\"$(dmesg | grep -i -E 'pvr|fault|mmu|drm|tcon|dsi' | tr '\n' '|' | cut -c1-200)\" err=\"$(grep -i -E 'error|fail|assert|abort|segfault' $O/$T.log | grep -v -i 'EGL display ext\|GLES2 ext\|snd_seq\|MIDI' | head -2 | tr '\n' '|' | cut -c1-200)\""
