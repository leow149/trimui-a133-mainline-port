cd /root/verify-20261107
G="chroot /tmp/edge /usr/bin/gst-launch-1.0 -q"
timeout 60 $G filesrc location=/root/t264.mp4 ! qtdemux ! h264parse ! v4l2slh264dec ! videoconvert ! video/x-raw,format=I420 ! checksumsink > h.txt 2>h.err; echo rc=$?
awk '{print $NF}' h.txt | paste - /dev/null | wc -l; head -c 300 h.err
