# Why the GPU was slower than on stock (2026-11-08)

Status: four driver-side causes found and fixed in Mesa, `patches/mesa/0008-pvr-pco-Fix-fragment-throughput-HSR-null-descriptors-CWE.patch`.
Not installed as the default driver yet (side by side, see "Deployment"). Stock baseline still
not measured (needs a stock boot; benchmark binaries are ready).

## Results (old driver -> 0008)

| Workload | old | 0008 |
|---|---|---|
| vkmark 2025.01 headless 1280x720, score | 207 | 267 |
| vkmark `desktop` scene | 81 fps | 216 fps |
| vkmark `texture` scene | 239 fps | 373 fps |
| RetroArch mGBA under rotated cage, Vulkan video driver | 29 fps | 58.5 fps |
| same, glcore (OpenGL via Zink) | 29 fps | 56.5 fps |
| cage composite, GPU time per frame (idle client) | 18.7 ms | 7.3 ms |
| cage fragment shaders, instruction groups | 62 / 83 | 28 / 19 |
| full-screen 720x1280 pass: flat | 4.6 ms | 2.0 ms |
| textured | 9.0 ms | 2.4 ms |
| background + blended textured | 14.6 ms | 6.0 ms |
| 8 overlapping opaque flat draws | 31.1 ms | 2.3 ms |

The panel refreshes at 59.4 Hz, so RetroArch under cage is now at (Vulkan) or near (glcore) full rate.
The RetroArch numbers are from `ra_fps.sh` (whole session incl. warm-up). A 3 s GPU trace in steady state
shows 175 frames per 178 vblanks.

Correctness: `vktest` and `vkmulti` identical ("all frames correct"); RetroArch screenshots (grim) in
Vulkan and glcore mode show the correct picture; no GPU faults in dmesg; vkmark ran clean.
Not done: CTS, other apps (PPSSPP, moonlight, vkcube visual), a real null-descriptor test.

## The four causes

1. **No hidden surface removal for any app shader.** PCO injected a sample-mask check (`discard_if`)
   into every non-internal fragment shader, and `pco_nir_pfo` always emitted ISP feedback
   (`discard_cond = is_helper_invocation`) -> every draw PUNCH_THROUGH. With
   `early_fragment_tests` it instead guarded the stores by reading back the tile buffer -> TRANSLUCENT.
   Either way no HSR, plus extra instructions. Fix: leave the check out when the multisample state is
   static, 1 sample, mask bit 0 set, no A2C, no sample shading; emit feedback only if the shader can
   demote/terminate. Same for alpha-to-one emulation when statically off. (Zink is covered: pvr lacks
   EDS3 PolygonMode/DepthClampEnable, so Zink bakes the multisample state.)
2. **Null-descriptor branches around every texture sample and UBO load.** Zink requires
   `nullDescriptor`; with it enabled PCO wrapped each access in `if (!is_null)` (test of 4 shared regs
   + if/else). Cost: cage composite 15.6 -> 10.8 ms by itself. Fix: null UBOs point at the
   zero page (16 KiB = maxUniformBufferRange) with size 0, null combined image samplers at a 1x1 zero
   texture with clamp-to-edge; no check for those loads/samples. Queries, fetches, shadow compares,
   SSBOs, storage/separate images keep it (for combined image samplers it tests a marker in sampler
   meta, `PCO_SAMPLER_META_NULL_IMAGE`, ex-RSVD0).
3. **VK_EXT_color_write_enable emulated in the shader.** Zink uses it (dynamic state), so every Zink
   fragment shader read back the tile buffer and selected per channel -> TRANSLUCENT, no HSR. Cost:
   15.6 -> 11.0 ms. Fix: extension no longer exposed (Zink works without it, uses it only for all-on/off).
4. **Zero padding of every sample instruction's data vector.** `smp` data is a vec16 padded with undef
   -> 12-14 zero moves per sample, per pixel. `pco_shrink_vecs` skipped vecs with multi-channel
   sources (the iterated UV); it now drops whole trailing sources. Textured pass 5.8 -> 2.4 ms.

Ruled out / noted on the way: register dumps stock vs mainline (only clock-gating auto vs on and
per-render ISP state differ); GPU clock is 696 MHz; render-target layout (linear vs optimal) irrelevant.
Experiment: hiding `nullDescriptor` entirely -> Zink refuses to start ("Zink requires the nullDescriptor
feature").

## Still open

- Per-pixel UBO loads: wlroots' `alpha` uniform is fetched with `ld` per pixel (4 groups + wait).
  The vendor driver preloads constants into shared registers per draw (PDS). Not fixed.
- Stock comparison: `tools/gpu/stock_build/vkpass` and `vkpass2` (vendor Vulkan on stock, glibc <= 2.17
  symbols). Run after a stock boot via `adb push` (`adb exec-out` doesn't work).

## Deployment / tools

- Build tree: bodenserver `/mnt/ext1/trimui_build/mesa`, persistent container `trimui-mesa`
  (alpine:edge): `docker exec trimui-mesa sh /work/build_mesa.sh`.
- On the device side by side: `/tmp/edge/root/pvr_new.so` via `VK_ICD_FILENAMES=/root/new_icd.json`
  (inside the edge chroot). Installed driver unchanged (md5 d5f96049...). Older test builds
  `pvr_hsr.so`/`pvr_nc.so`/`pvr_nd.so`/`pvr_exp.so` + json can be deleted.
- `tools/gpu/vkpass2.c` (+ `const.frag`, `const_ef.frag`, `flat_ef.frag`): vkpass with depth attachment
  (`depth= dcmp= dwrite=`) and `shader=const|const_ef|flat_ef`.
- Device scripts: `/root/cage_trace.sh <tag> <client>` (cage + 3 s GPU trace, parse with
  `tools/gpu/pvr_job_durations.py`), `/root/ra_fps_null.sh` / `ra_fps_gl.sh` (RetroArch fps, no audio,
  Vulkan / glcore), `/root/shot.sh <name> <video_driver>` (grim screenshot; grim installed in the chroot),
  `/root/vkm.sh` (vkmark).
- Shader dumps: `PCO_DEBUG_PRINT=fs,stats`.
