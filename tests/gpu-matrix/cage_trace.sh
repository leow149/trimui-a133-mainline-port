#!/bin/sh
# usage: cage_trace.sh <tag> <client...>; env (VK_ICD_FILENAMES etc.) is passed through
TAG=$1; shift
/root/wayland-session.sh "$@" > /tmp/ct_$TAG.log 2>&1 &
sleep 8
sh /root/gpu_trace.sh 3 /tmp/trace_$TAG.txt > /dev/null
/root/wayland-session.sh stop > /dev/null 2>&1
sleep 2
for p in $(pidof cage); do kill $p; done
echo traced $TAG
