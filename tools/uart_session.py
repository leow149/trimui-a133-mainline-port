#!/usr/bin/env python3
"""Hold the TrimUI UART open once: log everything RX'd, send lines written to a FIFO.

Usage: uart_session.py PORT LOGFILE FIFO
Write a command with: printf 'cmd\n' > FIFO
A line "@@BAUD <rate>" is not sent: it switches the host side of the port to
that baud rate (the device side has to be switched first, e.g. with stty).
A line "@@DELAY <ms>" sets the pause after each sent line (default 50 ms, which
keeps the device from dropping input at 115200; bulk pushes at higher rates use less).
"""
import os
import sys
import threading
import time

import serial

port, logpath, fifo = sys.argv[1:4]
if not os.path.exists(fifo):
    os.mkfifo(fifo)

ser = serial.Serial(port, 115200, timeout=0.2)
log = open(logpath, "ab", buffering=0)


def reader():
    while True:
        data = ser.read(4096)
        if data:
            log.write(data)


threading.Thread(target=reader, daemon=True).start()

delay = 0.05
while True:
    # Reopening the FIFO for reading blocks until a writer appears; the serial port stays open.
    with open(fifo, "rb") as f:
        for line in f:
            if line.startswith(b"@@DELAY "):
                delay = int(line.split()[1]) / 1000
                log.write(b"\n>>> DELAY " + line.split()[1] + b"\n")
                continue
            if line.startswith(b"@@BAUD "):
                ser.flush()
                ser.baudrate = int(line.split()[1])
                log.write(b"\n>>> BAUD " + line.split()[1] + b"\n")
                continue
            ser.write(line.rstrip(b"\n") + b"\n")
            log.write(b"\n>>> TX: " + line.rstrip(b"\n") + b"\n")
            time.sleep(delay)
