#!/bin/bash
# Reboot the TrimUI N times over the UART session and report whether each reboot
# reaches the next kernel's init within 60 s. Stops at the first hang.
set -uo pipefail
cd "$(dirname "$0")/.."
FIFO=uart_logs/uart.fifo; LOG=uart_logs/ana1_polarity_test_20261030.log
N=${1:-5}; CMD=${2:-"sync; reboot"}
ok=0
for i in $(seq 1 "$N"); do
  start=$(stat -c %s "$LOG")
  printf '\n' > "$FIFO"; sleep 1; printf '%s\n' "$CMD" > "$FIFO"
  if timeout 60 bash -c "until tail -c +$start $LOG | grep -aq 'Run /sbin/init'; do sleep 1; done"; then
    ok=$((ok+1)); echo "reboot $i: ok"; sleep 8
  else
    echo "reboot $i: HUNG ($(tail -c +$start $LOG | tr -d '\r' | grep -a 'Restarting\|U-Boot SPL' | tail -1))"; break
  fi
done
echo "$ok/$i reboots ok"
