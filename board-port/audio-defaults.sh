#!/bin/sh
# Quiet mixer levels at boot: the codec resets to full volume (DAC and
# headphone gain at 0 dB). About 28 dB below that; the speaker and the jack both
# hang off the headphone output, the line out is unused.
[ -e /dev/snd/controlC0 ] || exit 0
amixer -q set DAC 54
amixer -q set Headphone 4 on
amixer -q set "Line Out" 0 off
amixer -q set Speaker on
