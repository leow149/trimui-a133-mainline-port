#!/bin/sh
# mx.sh <tag> <core.so> <rom> <video_driver> <frames>
# RetroArch under cage; env (VK_ICD_FILENAMES) passes through. Results in /root/mx/<tag>.*
T=$1; CORE=$2; ROM=$3; VD=$4; N=${5:-900}
O=/root/mx; mkdir -p $O /tmp/edge/root/mx
SSG=${SSGPU:-true}
printf 'video_driver = "%s"\nvideo_context_driver = "wayland"\naudio_driver = "null"\nvideo_gpu_screenshot = "%s"\nfps_show = "false"\n' "$VD" "$SSG" > /tmp/edge/root/mx.cfg
cat /tmp/edge/root/.config/retroarch/trimui.cfg /tmp/edge/root/mx.cfg > /tmp/edge/root/mx2.cfg
dmesg -c > /dev/null
/root/uflow 20 > $O/$T.uflow 2>&1 &
UF=$!
t0=$(cut -d' ' -f1 /proc/uptime)
/root/wayland-session.sh /usr/bin/env HOME=/root retroarch --appendconfig=/root/mx2.cfg --max-frames=$N \
  --max-frames-ss --max-frames-ss-path=/root/mx/$T.png -L "$CORE" ${ROM:+"$ROM"} > $O/$T.ralog 2>&1 &
RA=$!
sleep 14
timeout 10 chroot /tmp/edge /usr/bin/env XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-0 grim /root/mx/$T.grim.png 2>/dev/null
sh /root/gpu_trace.sh 3 $O/$T.trace > /dev/null
i=0; while kill -0 $RA 2>/dev/null && [ $i -lt 120 ]; do sleep 1; i=$((i+1)); done
t1=$(cut -d' ' -f1 /proc/uptime)
wait $UF 2>/dev/null
for p in $(pidof cage retroarch); do kill $p; done; sleep 1
vb=$(grep -c drm_vblank_event $O/$T.trace); cf=$(grep pvr_job_create $O/$T.trace | grep -c "cage.*job_type=FRAGMENT\|job_type=FRAGMENT.*cage")
echo "$T secs=$(echo $t0 $t1 | awk '{print $2-$1}') vblanks=$vb uflow=\"$(tail -1 $O/$T.uflow)\" ss=$([ -f /tmp/edge/root/mx/$T.png ] && echo yes || echo NO) kmsg=\"$(dmesg | grep -i -E 'pvr|fault|mmu|drm|tcon|dsi' | tr '\n' '|' | cut -c1-200)\""
