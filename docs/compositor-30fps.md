# Why cage (wlroots) composites at ~30 fps on the TrimUI Smart Pro

Status 2026-11-08: root cause found (GPU fill rate, not scheduling). No config-only fix for the
rotated-panel case; the fixes that exist are listed at the end. Nothing installed, nothing committed.

## Summary

* The frame period of 2 vblanks (33.8 ms) is **not** a firmware head-of-line stall, a scanout-buffer
  fence wait, or a buffer-count problem. The GE8300 simply needs **~18-20 ms of GPU time to run
  cage's composite pass** (one full-screen 720x1280 pass), which is longer than one 16.8 ms refresh.
  RetroArch's own pass adds 4-6 ms on top, so each cage frame lands after the second vblank.
* The GPU is slow per pixel: **a flat-colour full-screen 720x1280 draw costs ~4.0 ms, a textured one
  ~8.3 ms, a blended textured one ~10 ms** (about 230 / 110 Mpixel/s), independent of LINEAR vs
  OPTIMAL render target and of texture tiling. A cage composite (background fill + client texture,
  plus Zink's render pass) is therefore 14-18 ms.
* Without cage's composite RetroArch runs at full speed (no compositor GPU job at all: 59-60 fps).
  That happens when wlroots can **direct-scan-out** the client buffer.

## Evidence

### 1. The "2nd fragment job" is just the long render, it is not waiting for the vblank

Each cage frame submits 1 geometry + 2 fragment jobs (Mesa pvr: the first fragment job is a ~0.5 ms
marker/sync job, the second is the real render). Timeline of one period with RetroArch (mGBA) under
cage, `tools/gpu/trace_timeline.py` on a `gpu_trace.sh` capture with dma_fence events (times in ms,
relative to a vblank):

```
  0.00  VBLANK
  1.71  cage SUBMIT                       (wlroots renders right after the flip event)
  1.99  cage ->fw geom / frag(385) / frag(386)
  2.54  frag 385 signalled (marker job, 0.55 ms)
  9.55  retroarch SUBMIT (comp + geom + frag + frag)    <- RA queued behind cage's frag 386
 16.87  VBLANK
 22.73  frag 386 (cage) AND RA frag 377 signalled      <- 20.7 ms after cage's submit
 28.75  RA frag 378 signalled                          <- RA's own pass: 6.0 ms
 33.76  VBLANK
 35.72  cage SUBMIT (next frame)
```

The same happens with **no other GPU client** (`weston-simple-shm` under cage, one period):

```
  0.00 VBLANK    1.89 cage ->fw frag(432)       16.88 VBLANK       20.37 frag 432 signalled
```

Cage's job runs 1.9 -> 20.4 ms = 18.5 ms with the GPU otherwise idle. The firmware trace
(`/sys/kernel/debug/dri/0/pvr_fw/trace_mask`, groups main|mts|csw|pow|misc = 0x4616, restored to 0
afterwards) shows for every cage frame exactly: `Kick TA`, two ready-to-run commands, `Kick 3D`,
then 48.7 FW ticks (2.7 ticks/ms = 18 ms) later `3D finished`, with the GPU powered the whole time
and no UFO wait, no power-state change, no preemption in between. drm_sched dependencies of the
2nd fragment job: only `fence=0:0` and one long-signalled fence. So nothing waits on a vblank; the
alignment "finishes ~3.5-5.9 ms after the next vblank" is a coincidence of
(submit phase locked to vblank) + (18-20 ms of work). This refutes the head-of-line/out-fence
hypothesis. (The kworker CPU wait of ~19 ms per frame is the atomic commit waiting for this fence,
as expected.)

RetroArch's job in its queue is blocked only because the GPU is busy rendering cage's pass; RA's
own second fragment job needs 6 ms (4.3 ms when run alone on KMS).

### 2. Per-pass GPU cost, `tools/gpu/vkpass.c` (new)

Full-screen pass, 720x1280, wall time submit -> fence, median of 40 submits, driver unchanged:

| pass | ms |
|---|---|
| clear only | 1.35 (0.52 with loadOp DONT_CARE, i.e. ~0.5 fixed) |
| flat colour draw | 4.5 (draw itself ~4.0) |
| 2 / 4 flat draws | 8.6 / 16.1 |
| textured draw (nearest, 1:1) | 9.0 |
| textured, 90-degree rotated sampling | 9.0 |
| textured + blend (premultiplied over) | 10.8 |
| flat background + blended textured (what cage composites) | 14.6 |
| 360x640, 720x640 flat | 2.5 |
| 720x320, 720x160 flat | 1.4, 0.9 |

Render target LINEAR vs OPTIMAL and source texture LINEAR vs OPTIMAL: identical to 0.05 ms in every
combination (`rt=`, `src=`). So the "linear render targets are the problem" alternative is refuted;
cost is linear in pixels (core-bound: texture adds 4 ms, ALU/varying work dominates, memory
bandwidth is ~1 GB/s). The GPU runs at the nominal 696 MHz (clk_summary), that is ~3 cycles per
flat pixel and ~6 per textured pixel.

### 3. Other hypotheses

* **Swapchain buffer count** (wlroots 0.20, cage 0.3.1): debug log shows the output swapchain
  grows from 2 to 4 GBM LINEAR XR24 buffers on its own. Buffer count is not the limit.
* **Explicit sync / KMS fences**: the sun4i plane has no IN_FENCE_FD; wlroots uses implicit sync
  (the commit worker waits on the render fence). There is no out-fence dependency (see 1).
* **Atomic vs legacy**: `WLR_DRM_NO_ATOMIC=1` -> cage fails to start (awk div-by-zero, 0 frames).
  `WLR_DRM_NO_MODIFIERS=1`: 28.5 fps (same as baseline 28.5).
* **Idle client**: with `weston-simple-shm` cage renders once per 2 vblanks too (same 18.5 ms job).
* `WLR_RENDERER=pixman`: cage does not start with it here (instant exit).
* GPU power management: FW trace shows the GPU stays powered across the job; `power/control` is
  `auto` and was not changed.

### 4. Direct scan-out removes the problem

* sun4i-drm can import powervr dmabufs: `tools/gpu/primeimport.c` (Vulkan-exported 3.6 MB dmabuf ->
  `PRIME_FD_TO_HANDLE` on card1) returns OK. Cage's debug log lists no scanout failure.
* With cage started **without** the `wlr-randr --transform 90`, RetroArch (XR24, 720x1280 LINEAR
  client buffers, matching the output) gets **59.8 fps** (`ra_fps.sh`, 300 frames; the very
  first no-rotation variant printed 68 fps because frames are dropped) and the GPU trace shows
  **zero cage jobs**: only RetroArch's jobs (compute 0.9 ms, geometry 0.5 ms, fragment 4.3 ms
  max), period 16.5 ms, DRM plane-2 fb is the client's buffer. That is direct scan-out.
* The catch: wlroots only scans a client buffer out when the buffer transform equals the output
  transform. With the output rotated 90 (needed for the landscape UI) the client buffer would have to
  be pre-rotated **and** tagged with `wl_surface.set_buffer_transform`. RetroArch/Mesa WSI do
  neither: RA renders 1280x720 landscape buffers (`create_immed 1280x720`, XR24, destination
  1280x720; during start-up it first creates 720x1280 ones), cage must rotate them, so it composites.
  `video_rotation` has no effect in RA's Vulkan/Wayland path here (checked rotation 1 and 3: identical
  upright picture in the unrotated portrait output; the portrait picture also is not usable as is,
  the panel is mounted in portrait).
* Even without any rotation, cage's composite costs the same (weston-simple-shm under cage without
  transform: 3D job 18.6 ms, plus a 3.1 ms transfer for the shm upload). Rotation is not what makes
  it slow; the full-screen composite is.

Measured RetroArch mGBA (Advance Wars), `tests/ra_fps.sh`, 300 frames:

| setup | fps |
|---|---|
| cage, transform 90, WLR_RENDERER=gles2 (baseline) | 28.5 |
| cage, transform 90, WLR_DRM_NO_MODIFIERS=1 | 28.5 |
| cage, no transform (direct scan-out, portrait picture) | 59.8 |
| KMS direct (`khr_display`, earlier measurement) | 57.9 |

## Root cause

GE8300 fill rate (~110-230 Mpix/s through the Mesa pvr driver) makes one 720x1280 composite pass
take ~18 ms, more than the 16.8 ms refresh. wlroots composites because the rotated output cannot
direct-scan-out an unrotated client buffer. With 2 vblanks per composite plus the client's own pass,
every fullscreen client is halved. It is not a firmware ordering bug, not a fence/vblank wait, not
buffer count and not linear render targets.

## Fixes / next steps (in order of payoff)

1. **Make direct scan-out possible with the rotation.** Client renders a landscape picture rotated
   90 degrees into a 720x1280 buffer and calls `wl_surface.set_buffer_transform(WL_OUTPUT_TRANSFORM_90)`
   (cage keeps `--transform 90`); wlroots then scans it out and cage costs no GPU time. For
   RetroArch this is a small patch in its Wayland+Vulkan path (pre-rotate with a rotation matrix
   in the final blit, set buffer transform); for Zink/GL apps the same via the EGL/Mesa Wayland WSI
   (Mesa wsi_wl does not expose pre-rotation, would need a patch). Alternative with identical
   effect: don't use cage for RetroArch, use the KMS-direct path (57.9 fps).
2. **Cheaper composite** if scan-out is not possible: the composite is background fill (4 ms) +
   texture (8-10 ms) + Zink overhead. Dropping the background fill (client covers the output, opaque
   XR24) and using a cheaper texture shader would put it near 9-10 ms, still too slow beside
   RetroArch's 4-6 ms pass. Needs wlroots/Zink work, not a flag.
3. **Raise the per-pixel throughput of the pvr driver.** 3 cycles per flat pixel and 6 per textured
   pixel on a core that should do about 1 pixel/clock suggests the USC/ISP configuration (tiles in
   flight is set up as Dusts=1, PartitionMask=0x3ff, USCTiles=10 in the firmware log), pixel output
   or shader code quality is leaving a lot on the table. Worth a look with `vkpass.c` as the
   benchmark (`w=64 h=64` vs `w=720 h=1280`, `shader=flat`). Doubling fill rate would make cage
   fit into one refresh.
4. A lower-cost interim for any compositor client: run the panel un-rotated (portrait) where
   the content is natively portrait; no use for the landscape console UI.

## Files

* `tools/gpu/vkpass.c`, `flat.frag`, `lin_rot.vert`, `build_pass.sh`: per-pass cost harness
  (`vkpass rt=linear|optimal src=linear|optimal|none shader=tex|flat rot= blend= under= draws= w= h= sw= sh=`).
  Build as `build_lin.sh` (docker alpine:edge on bodenserver, sysroot in /mnt/ext1/trimui_build/mesa).
* `tools/gpu/primeimport.c`, `build_prime.sh`: can card1 import a powervr dmabuf.
* `tools/gpu/fbgrab.c`, `build_fbgrab.sh`: build script for the existing plane grabber (does not work on
  GBM-allocated or imported buffers: MAP_DUMB EINVAL).
* `tools/gpu/pvr_job_durations.py`, `trace_timeline.py`: per-job submit->done durations and the
  merged timeline (jobs, dma_fence init/signal, vblanks) from a `tests/gpu_trace.sh` capture
  (`EXTRA_EVENTS="dma_fence/dma_fence_init/enable dma_fence/dma_fence_signaled/enable"`).
* `tests/wayland-session-noxform.sh`: `wayland-session.sh` without the `wlr-randr --transform 90` and
  with `$CAGE_OPTS` (e.g. `-D` for wlroots debug logging).

## Device state

Left as found: no cage/RetroArch running (seatd only), GPU `power/control=auto`, fw `trace_mask=0`,
installed driver md5 d5f96049... unchanged, temporary scripts/binaries/traces removed. `grim` was
installed into the edge chroot for screenshots and removed again; RetroArch rewrote its own
`retroarch.cfg`/core options on exit as it always does (video_rotation stays 0).
