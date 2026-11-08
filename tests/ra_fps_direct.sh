#!/bin/sh
# Like ra_fps.sh but without cage: Vulkan straight on KMS (VK_KHR_display).
CORE=$1; ROM=$2; N=${3:-600}
up() { cut -d' ' -f1 /proc/uptime; }
run() {
	printf "$1" > /tmp/edge/root/var.cfg
	cat /tmp/edge/root/.config/retroarch/trimui.cfg /tmp/edge/root/var.cfg > /tmp/edge/root/var2.cfg
	t0=$(up)
	chroot /tmp/edge /usr/bin/env PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 HOME=/root retroarch \
		--appendconfig=/root/var2.cfg --max-frames=$2 -L "$CORE" "$ROM" > /tmp/ra-fps.log 2>&1
	t1=$(up); echo "$t0 $t1" | awk '{print $2-$1}'
}
for v in "true alsa" "false null"; do
	set -- $v
	cfg="video_context_driver = \"khr_display\"\nvideo_rotation = \"3\"\nvideo_vsync = \"$1\"\naudio_driver = \"$2\"\n"
	s=$(run "$cfg" 1); t=$(run "$cfg" $N)
	echo "direct vsync=$1 audio=$2: $(echo "$t $s $N" | awk '{d=$1-$2; printf "%.1f fps (%.1f s, startup %.1f s)", $3/d, d, $2}')"
done
