# GE8300 transfer path: v1 parameter format (open, 2026-11-08)

## Problem

Mesa's PowerVR transfer queue has two 3D paths. `pvr_3d_copy_blit_core` (FastScale) works.
`pvr_3d_clip_blit` builds an ISP control stream and primitive blocks in software
(`pvr_isp_ctrl_stream` / `pvr_isp_primitive_block`) and only knows the non-simple format and the
simple internal parameter format **v2**. The GE8300 uses **v1**: the emitted block has a wrong
layout (5 dwords of non-simple compression words where the size function expects 4) -> assertion in
debug builds, malformed stream otherwise. Pre-existing, independent of patch 0008. Same in
Imagination's own Mesa branches (main, powervr-mesa-next, dev/devinfo, checked 2026-11-08).

Who hits it: Xwayland (glamor via Zink), moonlight-qt (Qt Quick via Zink) and any Vulkan/Zink user doing
- buffer<->image copies of some odd sizes into twiddled images (3x1, 159x1, 17x13; custom mapping
  "double stride"; 64x64, 300x7, 1000x3, 720x1280 are fine),
- depth/stencil aspect copies (D24S8, D32S8 -> DS merge),
- some scaled blits (64x64 -> 37x29) and small clears (3x1).

`tools/gpu/vkxfer.c`: 125-case transfer test, each case in a forked child. Old and 0008 driver:
80 pass, 45 crash (exactly the cases above).

Not affected: RetroArch (all tested cores), cage/wlroots, vkcube, weston clients, vkmark.

## Hardware ground truth (decoded so far)

Kernel debug patch (bodenserver linux-clean, kernel #59, NOT in patches/): debugfs
`/sys/kernel/debug/dri/0/pvr_pm/dump` = all free-list node pages, 64-byte header per node
("PMND", fl id, node idx, pages, size). Captures in scratch (`pm_*.bin`), tool `pmcap.sh`
(render with `vkpass2 hold=N`, cat the dump while the app holds).

TA-written v1 primitive block (64x64, full-screen triangle clipped to 80x80, 5 vertices, 3 triangles):

```
+00 qword format word   00003081 00050000
+08 ISP state words     002e0f00 (ISPA) 0e000000 (ISPB) 00000000 00000000
+18 index qword(s)      0e014a20 00005261   -> triangles (0,1,2) (1,0,3) (1,3,4), INDEX_DATA_WORDS_SIPF
+20 ISP vertices        5 x ISP_VERTEX_WORD_SIPF (x,y 13.4 + bias 2048, z)
+48 PDS state, 8 dw     00002900 00002700 01000411 00002940 00000000 00002880 00804003 00000000
+68 TSP data            per vertex 4 dw: W(1.0) u v pad
```

Format word (dw0/dw1), from 9 captures:
- bit 0: set when the primitive was clipped
- bits 6..10: primitive count - 1 (2 for 3 triangles, 4 for 5)
- bits 12..: TSP varying dwords per vertex (3 = W,u,v; 0 with no varyings)
- bit 5: set in a second block (meaning unknown)
- dw1 bits 16..18: ISP state words present (5 = ISPA + stencil words, 1 = ISPA only); the block's
  ISP state follows the format word and is padded to 8 bytes. ISPCTL is NOT in the block.
- ISPA (dw2): bit 21 dwritedisable, 17..19 dcmpmode, 8..15 line width -- same layout as TA_STATE_ISPA.

Control stream: NOT decoded. Candidates found in the PM pages are 64-bit entries
(`80000000_00000001`, `84000000_00000001`, `84000000_00010001`), not the generic IPF
PRIMITIVE_FORMAT/BASE words Mesa writes on v1. Needs: region header dump (UMD rgn_headers BO,
see the old `PVR_DUMP_RGN` hack in mesa_v1_10.patch) + physical addresses of the dumped PM pages
(PM page-table entries look like `47f4b001` = phys | valid) to follow region header -> control
stream -> primitive block.

Tried and failed (all GPU lockups, recovered by context reset): 8 primitive-block header variants
(ISPCTL+ISPA; +pad; v2 vertex format word; compression size / ISP compression words; HW-like
format qword + ISPA + pad with cs_isp_state_read/size 1/2, 0/0, 1/1, 1/4). Experimental code in
the bodenserver Mesa tree only (`PVR_V1_VARIANT`, `PVR_DBG_CLIP`), backup of the clean file:
`/mnt/ext1/trimui_build/mesa/pvr_arch_job_transfer.c.before_v1`.

## Alternatives to decoding the format

- Route the affected operations through the graphics pipeline (vk_meta-style copy/blit/clear
  shaders) on v1 instead of the clip-blit path. Bigger driver change, no format knowledge needed;
  depth/stencil copies need stencil export or a compute path.
