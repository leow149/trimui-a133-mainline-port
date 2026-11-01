#!/bin/sh
# NSI (DRAM bus arbiter) port priorities, as the A133 BSP sets them. Without
# this every master is LOWEST and the display engine underruns (glitch lines)
# whenever the GPU loads memory. Interim until U-Boot SPL programs them.
# Ports: CPU GPU SD1 MSTG GMAC0 GMAC1 USB0 USB1 NDFC DMAC CE DE0 DE1 VE CSI ISP G2D EINK IOMMU CPUS
# entry = priority(0 lowest..3 highest):qos_sel(0 output, 1 input)
TABLE="0:1 0:1 0:0 0:0 0:0 0:0 0:0 0:0 0:0 0:0 0:0 2:1 2:1 0:1 2:1 2:1 0:1 0:0 3:1 0:0"
port=0
for e in $TABLE; do
	pri=${e%:*}; sel=${e#*:}
	b=$((0x03100000 + port * 0x200))
	/root/devmem $(printf 0x%x $((b + 0x10))) 0 >/dev/null
	/root/devmem $(printf 0x%x $((b + 0xc0))) 0 >/dev/null
	/root/devmem $(printf 0x%x $((b + 0x18))) $sel >/dev/null
	/root/devmem $(printf 0x%x $((b + 0x14))) $(printf 0x%x $(( (pri << 2) | pri ))) >/dev/null
	port=$((port + 1))
done
