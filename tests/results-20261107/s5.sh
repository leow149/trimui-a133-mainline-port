cd /root/verify-20261107
for a in 0x02001690 0x0200169c 0x01c00000; do echo "$a $(timeout 5 /root/devmem $a 2>&1)"; done
G="chroot /tmp/edge /usr/bin/gst-launch-1.0 -q"
timeout 60 $G filesrc location=/root/t264.mp4 ! qtdemux ! h264parse ! v4l2slh264dec ! videoconvert ! video/x-raw,format=I420 ! checksumsink > h2.txt 2>h2.err; echo rc=$?
awk '{print $NF}' h2.txt | paste - ../../tmp/hw.sum 2>/dev/null | head -0
awk '{print $NF}' h2.txt > h2.sum; awk '{print $NF}' /tmp/sw.txt > /dev/null
echo "h264 identical to earlier sw: $(paste h2.sum /tmp/hw.sum 2>/dev/null | awk '$1==$2' | wc -l)"
