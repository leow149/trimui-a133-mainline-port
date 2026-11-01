#!/bin/bash
# Install build-artifacts/Image (and optionally the DTB) on the TrimUI's SD boot
# partition over the UART console, no card swap. Needs a running mainline shell on
# the device and tools/uart_session.py holding the port.
#
# 1. xz-compress the Image (cached by md5).
# 2. Switch both ends of the console to 1.5 Mbaud (A133 UART clock 24 MHz / 16 = 1.5 Mbaud
#    exactly; the FT232R adapter does it too).
# 3. Push Image.xz to /tmp with tools/uart_push.py (chunked, md5-verified, retried).
# 4. On the device: unxz, check md5, copy over /Image on the partition labelled TRIMUIBOOT
#    (never by /dev/mmcblkN: probe order flips between the SD card and the eMMC),
#    re-check md5, unmount.
# 5. Back to 115200 (U-Boot and the next kernel expect it).
#
# Usage: tools/push_kernel.sh [--dtb] [--test] [--reboot]
#   --test  install as /Image.test and /test.dtb next to the default files, for a
#           one-shot boot with tools/uboot_test_boot.py (the default boot is untouched)
set -euo pipefail
cd "$(dirname "$0")/.."
FIFO=uart_logs/uart.fifo
LOG=uart_logs/ana1_polarity_test_20261030.log
IMG=build-artifacts/Image
DTB=build-artifacts/sun50i-a133-trimui-smartpro.dtb
CARD_IMG=Image; CARD_DTB=sun50i-a133-trimui-smartpro.dtb; WITH_DTB=; TEST_MODE=
for a in "$@"; do
  case $a in
  --test) CARD_IMG=Image.test; CARD_DTB=test.dtb; TEST_MODE=1 ;;
  --dtb) WITH_DTB=1 ;;
  esac
done
FAST=1500000
send() { printf '%s\n' "$1" > "$FIFO"; }
# Wait for "TOKEN <value>" after the prompt, print the value.
ask() {
  local tok=$1 cmd=$2 start
  start=$(stat -c %s "$LOG")
  send "$cmd"
  for _ in $(seq 1 ${3:-120}); do
    v=$(tail -c +"$start" "$LOG" | tr -d '\r' | grep -ao "$tok [^ ]*" | grep -v '\$' | tail -1 | cut -d' ' -f2)
    [ -n "$v" ] && { echo "$v"; return 0; }
    sleep 1
  done
  echo "timeout waiting for $tok" >&2; return 1
}
want=$(md5sum < "$IMG" | cut -c1-32)
if [ ! -f "$IMG.xz" ] || [ "$(cat "$IMG.xz.src" 2>/dev/null)" != "$want" ]; then
  echo "compressing $IMG ($want)"
  xz -9 -T0 -k -f "$IMG"; echo "$want" > "$IMG.xz.src"
fi
echo "Image.xz $(stat -c %s "$IMG.xz") bytes"
restore() { send "@@DELAY 50"; send "stty 115200"; sleep 0.5; send "@@BAUD 115200"; }
trap restore EXIT
send ""; sleep 0.5
send "stty $FAST"; sleep 0.5; send "@@BAUD $FAST"; send "@@DELAY ${PUSH_DELAY_MS:-3}"; sleep 0.5; send ""
[ "$(ask FASTOK 'echo FASTOK $((6*7))' 20)" = 42 ] || { echo "no response at $FAST baud"; exit 1; }
echo "link up at $FAST baud"
t0=$(date +%s)
PUSH_CHUNK=65536 python3 -I tools/uart_push.py "$IMG.xz" /tmp/Image.xz "$FIFO" "$LOG"
[ -n "$WITH_DTB" ] && PUSH_CHUNK=65536 python3 -I tools/uart_push.py "$DTB" /tmp/new.dtb "$FIFO" "$LOG"
echo "transfer took $(( $(date +%s) - t0 )) s"
got=$(ask IMGMD5 'unxz -f /tmp/Image.xz && echo IMGMD5 $(md5sum < /tmp/Image | cut -c1-32)' 300)
[ "$got" = "$want" ] || { echo "decompressed md5 $got != $want"; exit 1; }
# The 64 MB boot partition fits only about one and a half kernels. A normal install
# drops a leftover test kernel first, then copies under a temporary name and renames
# it into place when there is room; otherwise it has to replace the old file in place.
# /tmp/Image is only deleted once the card copy verifies, so a failure can be retried
# without pushing again. --test refuses when the test kernel does not fit.
B=/mnt/boot
put="put() { f=\$1; t=\$2; rm -f \$t.new; \
need=\$(( \$(stat -c %s \$f) / 1024 + 64 )); free=\$(df -k $B | tail -1 | awk '{print \$4}'); \
if [ \$free -ge \$need ]; then cp \$f \$t.new && mv \$t.new \$t; \
elif [ -n \"$TEST_MODE\" ]; then echo NO_SPACE_FOR_TEST; return 1; \
else rm -f \$t && cp \$f \$t; fi; }"
cmd="$put; P=\$(findfs LABEL=TRIMUIBOOT) && mkdir -p $B && mount \$P $B"
[ "$CARD_IMG" = Image ] && cmd="$cmd && rm -f $B/Image.test $B/test.dtb"
cmd="$cmd && put /tmp/Image $B/$CARD_IMG"
[ -n "$WITH_DTB" ] && cmd="$cmd && put /tmp/new.dtb $B/$CARD_DTB"
cmd="$cmd && sync && echo CARDMD5 \$(md5sum < $B/$CARD_IMG | cut -c1-32) && rm -f /tmp/Image /tmp/Image.xz; umount $B"
got=$(ask CARDMD5 "$cmd" 300)
[ "$got" = "$want" ] || { echo "card md5 $got != $want"; exit 1; }
echo "kernel $want installed on the SD boot partition as /$CARD_IMG"
restore; trap - EXIT
for a in "$@"; do [ "$a" = --reboot ] && { sleep 1; send "sync; reboot"; echo "reboot sent"; }; done
exit 0
