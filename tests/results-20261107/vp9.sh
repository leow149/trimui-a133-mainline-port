cd /root/verify-20261107
cp t320.webm t720.webm /tmp/edge/root/
G="chroot /tmp/edge /usr/bin/gst-launch-1.0 -q"
for n in t320 t720; do
 dmesg -c >/dev/null
 timeout 60 $G filesrc location=/root/$n.webm ! matroskademux ! vp9parse ! v4l2slvp9dec ! videoconvert ! video/x-raw,format=I420 ! checksumsink > hw_$n.txt 2> hw_$n.err; echo "$n hw rc=$?"
 timeout 90 $G filesrc location=/root/$n.webm ! matroskademux ! vp9parse ! avdec_vp9 ! videoconvert ! video/x-raw,format=I420 ! checksumsink > sw_$n.txt 2>/dev/null; echo "$n sw rc=$?"
 awk '{print $NF}' hw_$n.txt > hw_$n.sum; awk '{print $NF}' sw_$n.txt > sw_$n.sum
 echo "$n frames hw=$(wc -l <hw_$n.sum) sw=$(wc -l <sw_$n.sum) identical=$(paste hw_$n.sum sw_$n.sum | awk '$1==$2'|wc -l)"
 head -c 400 hw_$n.err
 dmesg | tail -15 > dmesg_$n.txt; cat dmesg_$n.txt
done
