cd /root/verify-20261107
dmesg > dmesg_after_vp9.txt; grep -n -iE "hantro|vpu|WARNING|reset_control|cedrus" dmesg_after_vp9.txt | head -20
sed -n 1,12p dmesg_after_vp9.txt
grep -E "ve|VE|bus_ve" /sys/kernel/debug/clk/clk_summary 2>&1 | head
