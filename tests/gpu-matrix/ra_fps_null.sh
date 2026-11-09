#!/bin/sh
# Measure RetroArch frame rate under cage: time N frames (minus a 1-frame run for startup).
# Usage: ra_fps.sh <core.so> <rom> [frames]   variants: vsync on/off x audio alsa/null
CORE=$1; ROM=$2; N=${3:-600}
up() { cut -d' ' -f1 /proc/uptime; }
run() { # $1 = extra cfg lines, $2 = frames
	printf "$1" > /tmp/edge/root/var.cfg
	cat /tmp/edge/root/.config/retroarch/trimui.cfg /tmp/edge/root/var.cfg > /tmp/edge/root/var2.cfg
	t0=$(up)
	/root/wayland-session.sh /usr/bin/env HOME=/root retroarch --appendconfig=/root/var2.cfg \
		--max-frames=$2 -L "$CORE" "$ROM" > /tmp/ra-fps.log 2>&1
	t1=$(up); echo "$t0 $t1" | awk '{print $2-$1}'
}
for v in "true null"; do
	set -- $v
	cfg="video_vsync = \"$1\"\naudio_driver = \"$2\"\n"
	s=$(run "$cfg" 1); t=$(run "$cfg" $N)
	echo "vsync=$1 audio=$2: $(echo "$t $s $N" | awk '{d=$1-$2; printf "%.1f fps (%.1f s for %d frames, startup %.1f s)", $3/d, d, $3, $2}')"
done
