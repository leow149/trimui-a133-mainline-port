#!/bin/sh
# pmcap2.sh <name> <vkpass2 args...>: PM dump during hold + region headers at exit
N=$1; shift
chroot /tmp/edge /usr/bin/env PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 VK_ICD_FILENAMES=/root/v1_icd.json PVR_DUMP_RGN=/root/rgn_$N.bin /root/vkpass2 reps=1 hold=6 "$@" > /dev/null 2>&1 &
sleep 3
cat /sys/kernel/debug/dri/0/pvr_pm/dump > /mnt/data/pm2_$N.bin
wait
cp /tmp/edge/root/rgn_$N.bin /mnt/data/rgn_$N.bin
echo "$N pm=$(stat -c %s /mnt/data/pm2_$N.bin) rgn=$(stat -c %s /mnt/data/rgn_$N.bin)"
