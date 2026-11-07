# usage: sh hwonly.sh t320 ; hw-only VP9 decode + dmesg
n=$1
cd /root/verify-20261107
cp $n.webm /tmp/edge/root/
dmesg -c >/dev/null
timeout 40 chroot /tmp/edge /usr/bin/env PATH=/usr/bin:/bin gst-launch-1.0 -q filesrc location=/root/$n.webm ! matroskademux ! vp9parse ! v4l2slvp9dec ! videoconvert ! video/x-raw,format=I420 ! checksumsink > hw_$n.txt 2> hw_$n.err
echo "$n hw rc=$? frames=$(wc -l < hw_$n.txt)"
dmesg | grep -v "PM:" | grep -E "DBG|hantro|WARN" 
