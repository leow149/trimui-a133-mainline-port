#!/bin/sh
# Cedrus check on the device: decode each clip with the VE and with the CPU and
# compare the per-frame checksums (I420). Needs the /tmp/edge chroot with
# gstreamer-tools gst-plugins-{base,good,bad} gst-libav, clips in /tmp/edge/root.
# Each decode runs under a timeout so a stuck VE can't keep the shell.
G="chroot /tmp/edge /usr/bin/gst-launch-1.0 -q"
run() { # name file demux+parse hwdec swdec
	timeout 60 $G filesrc location=/root/$2 ! $3 ! $4 ! videoconvert ! video/x-raw,format=I420 ! checksumsink > /tmp/hw.txt 2>/tmp/hw.err
	hw=$?
	timeout 120 $G filesrc location=/root/$2 ! $3 ! $5 ! videoconvert ! video/x-raw,format=I420 ! checksumsink > /tmp/sw.txt 2>/dev/null
	awk '{print $NF}' /tmp/hw.txt > /tmp/hw.sum; awk '{print $NF}' /tmp/sw.txt > /tmp/sw.sum
	same=$(paste /tmp/hw.sum /tmp/sw.sum | awk '$1 == $2' | wc -l)
	echo "$1: hw rc=$hw frames=$(wc -l < /tmp/hw.sum), sw frames=$(wc -l < /tmp/sw.sum), identical=$same"
	[ $hw -ne 0 ] && head -c 300 /tmp/hw.err
}
run H.264 t264.mp4 "qtdemux ! h264parse" v4l2slh264dec avdec_h264
run H.265 t265.mp4 "qtdemux ! h265parse" v4l2slh265dec avdec_h265
run MPEG-2 tmpeg2.ts "tsdemux ! mpegvideoparse" v4l2slmpeg2dec avdec_mpeg2video
dmesg | grep -i cedrus | tail -5
