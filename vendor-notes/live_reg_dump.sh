#!/bin/sh
# Run on-device (stock, over adb shell) to dump specific 32-bit MMIO registers.
# Uses page-aligned dd (fast lseek) + hexdump, since this rootfs has no devmem applet.

read_reg() {
	name="$1"
	addr="$2"
	page=$((addr & ~0xfff))
	off=$((addr & 0xfff))
	skip=$((page / 4096))
	bytes=$(dd if=/dev/mem bs=4096 skip=$skip count=1 2>/dev/null | \
	        dd bs=1 skip=$off count=4 2>/dev/null | \
	        hexdump -e '1/1 "%02x "')
	b0=$(echo $bytes | cut -d' ' -f1)
	b1=$(echo $bytes | cut -d' ' -f2)
	b2=$(echo $bytes | cut -d' ' -f3)
	b3=$(echo $bytes | cut -d' ' -f4)
	echo "$name=0x$b3$b2$b1$b0"
}

echo "=== TCON0 ==="
read_reg TCON0_GINT0      0x6511004
read_reg TCON0_CTL        0x6511040
read_reg TCON0_CPU_IF     0x6511060
read_reg TCON0_CPU_TRI0   0x6511160
read_reg TCON0_CPU_TRI1   0x6511164
read_reg TCON0_CPU_TRI2   0x6511168

echo "=== DSI ==="
read_reg DSI_CTL          0x6504000
read_reg DSI_BASIC_CTL0   0x6504010
read_reg DSI_BASIC_CTL1   0x6504014
read_reg DSI_TRANS_START  0x6504060
read_reg DSI_TRANS_ZERO   0x6504078
read_reg DSI_TCON_DRQ     0x650407c
read_reg DSI_INST_FUNC0   0x6504020
read_reg DSI_INST_FUNC1   0x6504024
read_reg DSI_INST_FUNC2   0x6504028
read_reg DSI_INST_FUNC3   0x650402c
read_reg DSI_INST_FUNC4   0x6504030
read_reg DSI_INST_FUNC5   0x6504034
read_reg DSI_INST_FUNC6   0x6504038
read_reg DSI_INST_FUNC7   0x650403c
read_reg DSI_INST_LOOP_SEL 0x6504040
read_reg DSI_INST_LOOP_N0 0x6504044
read_reg DSI_INST_LOOP_N1 0x6504054
read_reg DSI_INST_JUMP_SEL 0x6504048
read_reg DSI_INST_JUMP_CFG0 0x650404c
read_reg DSI_PIXEL_PH     0x6504090
read_reg DSI_PIXEL_CTL0   0x6504080

echo "=== DPHY ==="
read_reg DPHY_GCTL        0x6505000
read_reg DPHY_TX_CTL      0x6505004
read_reg DPHY_ANA0        0x650504c
read_reg DPHY_ANA1        0x6505050
read_reg DPHY_ANA2        0x6505054
read_reg DPHY_ANA3        0x6505058
read_reg DPHY_ANA4        0x650505c
read_reg DPHY_PLL0        0x6505104
