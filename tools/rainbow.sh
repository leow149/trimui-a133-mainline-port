#!/bin/sh
# Rotating rainbow on the TrimUI Smart Pro LEDs: a colour wheel around each stick
# ring (LEDs 1-11 left, 12-22 right) and the logo (LED 0) cycling through it.
# Usage: rainbow.sh [seconds] [brightness 0-255]; switches the LEDs off at the end.
SECS=${1:-60}
BRIGHT=${2:-20}
L=/sys/class/leds/rgb:indicator

# hue 0-359 -> "r g b"
rgb() {
	s=$(( $1 / 60 )); f=$(( ($1 % 60) * 255 / 60 )); g=$(( 255 - f ))
	case $s in
	0) echo "255 $f 0" ;;
	1) echo "$g 255 0" ;;
	2) echo "0 255 $f" ;;
	3) echo "0 $g 255" ;;
	4) echo "$f 0 255" ;;
	*) echo "255 0 $g" ;;
	esac
}

for i in $(seq 0 22); do echo "$BRIGHT" > $L-$i/brightness; done
end=$(( $(date +%s) + SECS ))
base=0
while [ "$(date +%s)" -lt "$end" ]; do
	for i in $(seq 1 11); do
		h=$(( (base + (i - 1) * 360 / 11) % 360 ))
		c=$(rgb $h)
		echo "$c" > $L-$i/multi_intensity
		echo "$c" > $L-$((i + 11))/multi_intensity
	done
	# The logo's channels are in "green red blue" order (see its multi_index).
	set -- $(rgb $base)
	echo "$2 $1 $3" > $L-0/multi_intensity
	base=$(( (base + 12) % 360 ))
done
for i in $(seq 0 22); do echo 0 > $L-$i/brightness; done
