#!/usr/bin/env python3
"""Push a file to the TrimUI over the UART console, chunked and md5-verified.

Works through tools/uart_session.py (which owns the serial port): commands go into
its FIFO, replies are read back from its log. The UART has no flow control and
drops characters when the device is busy, so: echo is turned off, the file goes
in small base64 chunks, and every chunk is checked by md5 and resent on mismatch.

Usage: uart_push.py LOCAL_FILE REMOTE_PATH FIFO LOG
"""
import base64
import hashlib
import os
import sys
import time

CHUNK = int(os.environ.get("PUSH_CHUNK", 16 * 1024))
LINE = int(os.environ.get("PUSH_LINE", 512))
RETRIES = 5

local, remote, fifo, log = sys.argv[1:5]
data = open(local, "rb").read()


def send(text):
    with open(fifo, "w") as f:
        f.write(text)


def wait_for(token, since, timeout=120):
    end = time.time() + timeout
    while time.time() < end:
        with open(log, "rb") as f:
            f.seek(since)
            tail = f.read().decode("latin-1").replace("\r", "")
        for line in tail.splitlines():
            # Replies land after the shell prompt ("~ # PUSHMD5 ..."); skip our own
            # command echo, which contains the token followed by "$(".
            i = line.find(token)
            if i >= 0 and "$(" not in line[i:]:
                return line[i:]
        time.sleep(0.3)
    return None


send("stty -echo\n")
time.sleep(1)
send(f"rm -f {remote}.part*\n")

chunks = [data[i:i + CHUNK] for i in range(0, len(data), CHUNK)]
for n, chunk in enumerate(chunks):
    want = hashlib.md5(chunk).hexdigest()
    b64 = base64.b64encode(chunk).decode()
    lines = "\n".join(b64[i:i + LINE] for i in range(0, len(b64), LINE))
    for attempt in range(RETRIES):
        since = os.path.getsize(log)
        part = f"{remote}.part{n:04d}"
        send(f"base64 -d > {part} <<'PUSH_EOF'\n{lines}\nPUSH_EOF\n"
             f"echo \"PUSHMD5 {n} $(md5sum < {part} | cut -c1-32)\"\n")
        got = wait_for(f"PUSHMD5 {n} ", since)
        if got and got.split()[2] == want:
            break
        print(f"chunk {n}: mismatch ({got}), retry {attempt + 1}", flush=True)
    else:
        sys.exit(f"chunk {n} failed after {RETRIES} attempts")
    print(f"chunk {n + 1}/{len(chunks)} ok", flush=True)

since = os.path.getsize(log)
send(f"cat {remote}.part* > {remote} && rm -f {remote}.part*; "
     f"echo \"PUSHDONE $(md5sum < {remote} | cut -c1-32)\"; stty echo\n")
got = wait_for("PUSHDONE ", since)
whole = hashlib.md5(data).hexdigest()
print(f"remote {got}  local {whole}")
sys.exit(0 if got and got.split()[1] == whole else 1)
