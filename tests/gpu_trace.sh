#!/bin/sh
# Record a GPU/display timeline: pvr jobs, drm_sched, vblanks, CPU fence waits.
# Usage: gpu_trace.sh [seconds] [out]   (run while the workload is going)
S=${1:-2}; O=${2:-/tmp/gpu_trace.txt}
T=/sys/kernel/tracing
mountpoint -q $T || mount -t tracefs none $T
echo 0 > $T/tracing_on; echo > $T/trace; echo 8192 > $T/buffer_size_kb
echo nop > $T/current_tracer
for e in pvr/enable gpu_scheduler/enable drm/drm_vblank_event/enable ${EXTRA_EVENTS:-} \
	dma_fence/dma_fence_wait_start/enable dma_fence/dma_fence_wait_end/enable; do
	echo 1 > $T/events/$e
done
echo 1 > $T/tracing_on; sleep $S; echo 0 > $T/tracing_on
cat $T/trace > $O
echo 0 > $T/events/enable
grep -vc "^#" $O
