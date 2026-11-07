cd /root/verify-20261107
sh /root/edge-chroot.sh 2>&1 | tail -3
ls -la /tmp/edge/root | head -30
ls /tmp/edge/usr/bin | grep -iE "gst|ffmpeg|v4l2|vulkaninfo|vkcube"
chroot /tmp/edge gst-inspect-1.0 2>/dev/null | grep -iE "v4l2sl|vp9|vpx"
chroot /tmp/edge ls /usr/lib/gstreamer-1.0 | grep -iE "vpx|matroska|libav|v4l2" 
chroot /tmp/edge ffmpeg -version 2>&1 | head -1
