#!/usr/bin/env python3
"""Boot /Image.test + /test.dtb once: catch U-Boot's autoboot over the UART session,
load the test files by hand and boot them. The next plain reboot uses the default files
again, so a kernel or DTB that does not come up costs one power cycle, not a card swap.
Run it, then reboot the device (or power it on)."""
import os, sys, time
fifo, log = "uart_logs/uart.fifo", "uart_logs/ana1_polarity_test_20261030.log"
def send(t):
    with open(fifo, "w") as f: f.write(t)
start = os.path.getsize(log)
end = time.time() + 300
while time.time() < end:
    with open(log, "rb") as f:
        f.seek(start); tail = f.read().decode("latin-1")
    if "Hit any key" in tail:
        for _ in range(10):
            send(" "); time.sleep(0.1)
        break
    time.sleep(0.05)
else:
    sys.exit("no U-Boot seen")
time.sleep(1)
for c in ["load mmc 0:1 ${kernel_addr_r} /Image.test",
          "load mmc 0:1 ${fdt_addr_r} /test.dtb",
          "setenv bootargs console=ttyS0,115200 root=PARTUUID=14c78a96-02 rootwait rw earlycon loglevel=4",
          "booti ${kernel_addr_r} - ${fdt_addr_r}"]:
    send(c + "\n"); time.sleep(4 if "Image" in c else 1.5)
print("test boot sent")
