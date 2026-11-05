#!/bin/sh
# CPU load of running processes over an interval: cpuload.sh <seconds> <name>...
# Prints per process (all threads) the share of one CPU core, from /proc/<pid>/stat utime+stime.
secs=$1; shift
hz=100
snap() { for n in "$@"; do for p in $(pidof "$n"); do echo "$n $p $(awk '{print $14 + $15}' /proc/$p/stat 2>/dev/null)"; done; done; }
a=$(snap "$@")
sleep "$secs"
b=$(snap "$@")
echo "$a" | while read -r n p t0; do
	[ -n "$p" ] || continue
	t1=$(echo "$b" | awk -v p="$p" '$2 == p {print $3}')
	[ -n "$t1" ] && echo "$n (pid $p): $(( (t1 - t0) * 100 / (hz * secs) ))% of one core"
done
