cd /root/verify-20261107
wc -l < h.txt
/root/devmem 2>&1 | head -2
for a in 0x01c00000 0x01c00004 0x01c00008 0x01c0000c 0x01c00040 0x02001690 0x0200169c; do echo "$a $(timeout 5 /root/devmem $a 32 2>&1)"; done
