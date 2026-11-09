# GPU / display test matrix (device side)

Scripts run on the TrimUI as root, with the edge chroot mounted (`/root/edge-chroot.sh`) and the scripts in
`/root/`. `VK_ICD_FILENAMES` (path inside the chroot) selects a driver build; unset = installed driver.
`/root/uflow` is `tools/gpu/uflow.c` built statically; `/root/gpu_trace.sh` is `tests/gpu_trace.sh`.

| Script | Purpose |
|---|---|
| `mx.sh <tag> <core.so> <rom> <vulkan\|glcore> <frames>` | RetroArch under cage: screenshot at a fixed frame, grim screenshot, 3 s GPU trace, TCON underflows, kernel messages |
| `mxall.sh` | 12 games x 5 cores x {vulkan, glcore} x {old, new driver} |
| `app.sh <tag> <wait> <program...>` | Same for any Wayland/X11 client (grim, trace, underflows, errors) |
| `axall.sh` | vkcube, weston clients, PPSSPP, moonlight with both drivers |
| `cage_trace.sh <tag> <client...>` | cage composite GPU time (parse with `tools/gpu/pvr_job_durations.py`) |
| `shot.sh <name> <video_driver>` | RetroArch grim screenshot |
| `ra_fps_null.sh`, `ra_fps_gl.sh` | RetroArch frame rate, no audio, Vulkan / glcore |
| `ra_trace.sh` | RetroArch under cage, 3 s GPU trace in steady state |
| `vkm.sh` | vkmark headless |

Laptop side: `mx_analyze.py <dir>` (fps per run from traces, underflows), `rawcmp.py <img dir>` (GPU frame vs
the core's raw frame, sampled at core pixel centres).

Results of the 2026-11-08 runs: `docs/gpu-perf-vs-stock.md`, `docs/gpu-v1-transfer.md`.
