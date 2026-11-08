# RetroArch Vulkan black emulator frame on the PowerVR GE8300 (A133)

Status 2026-11-08: root cause found and fixed in a test build (not installed, not committed).

## Summary

The black picture is **not** a linear-texture problem. RetroArch 1.22.2's Vulkan driver never even
creates the streamed LINEAR texture on this driver, and linear sampling is fine. The cause is
`imageStore()` on storage images: **the TPU write path (`smp.wrt`) only writes texel (0,0)**; every
store at another coordinate is silently dropped. RetroArch converts RGB565 frames (what mgba, gambatte,
fceumm... deliver by default) with a compute shader (`rgb565_to_rgba8888.comp`) that `imageStore`s into
an RGBA8 storage image, so the sampled texture stays all zero and the frame is black. XRGB8888 content
(BGRA8888 path, e.g. mrboom) is uploaded with `vkCmdCopyBufferToImage` and displays correctly.

## What RetroArch really does (v1.22.2, `gfx/drivers/vulkan.c`)

* `vulkan_create_texture(VULKAN_TEXTURE_STREAMED)` first requires
  `linearTilingFeatures` to contain SAMPLED_IMAGE **and SAMPLED_IMAGE_FILTER_LINEAR**. The driver
  reports 0xcd81 for linear tiling (no FILTER_LINEAR, 0x1000), 0xdd81 for optimal. So it falls back to
  `VULKAN_TEXTURE_STAGING` (host buffer) plus a `VULKAN_TEXTURE_DYNAMIC` optimal image. The device has a
  single memory type (DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT), so no manual flush logic is involved.
* `VK_REMAP_TO_TEXFMT` (line 58) turns RGB565 into RGBA8 for every platform, so for RGB565 frames
  `vulkan_copy_staging_to_dynamic()` takes the `compute_upload` branch: SSBO (565 words) -> compute
  `rgb565_to_rgba8888` -> `imageStore` into a `STORAGE` R8G8B8A8 image -> GENERAL ->
  SHADER_READ_ONLY_OPTIMAL -> sampled by the filter chain.
* For XRGB8888 (`Using BGRA8888 format` in the log) it is a plain buffer-to-image copy.

Verified live on the device (cage + RetroArch, `video_driver=vulkan`, screenshots via `grim`):
mgba (RGB565) black; mrboom (BGRA8888) correct; mgba with GL (Zink) correct;
mgba with the patched driver correct. Vulkan validation layer on RetroArch shows only an
acquire-semaphore stage-mask nit, nothing that would explain black.

## Reproducer: `tools/gpu/vklinear.c`

Build (on bodenserver, same sysroot as `vktest`): `docker run --rm -v /mnt/ext1/trimui_build/mesa:/work alpine:edge sh /work/vktest/build_lin.sh`
(copy `vklinear.c lin.vert lin.frag *.comp build_lin.sh` to `/mnt/ext1/trimui_build/mesa/vktest/` first).
Run on the device: `chroot /tmp/edge env PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 /root/vklinear key=value ...`
(add `VK_ICD_FILENAMES=/root/pvrtest/icd.json` to test a modified driver).

Pattern is written by the CPU every frame (different per frame), sampled by a full-screen draw into an
RGBA8 target, compared exactly. Keys: `fmt=bgra|rgba|565 w= h= mode=linear|staging|compute rt=optimal|linear
layout=preinit|undef barrier=full|mem|none flush=0|1 frames= repeat= filter=nearest|linear cs=ra|const|ssbo|ids|pt|pt32|fix|dyn|atom|load
texfmt=rgba8|r32 cdbg=1 delay_ms= dummy=`. `mode=linear` is RetroArch's streamed-texture path (linear,
mapped, PREINITIALIZED -> GENERAL barrier exactly like `vulkan_transition_texture`), `mode=staging` the
buffer->optimal copy, `mode=compute` the RGB565 compute upload with RetroArch's own shader.

Use `rt=linear` (see "Other finding" below).

| test (240x160, 256x224, 320x240, 241x161, 3 frames) | original driver | patched |
|---|---|---|
| mode=linear fmt=bgra / 565 (streamed LINEAR texture, rewritten each frame) | PASS | PASS |
| mode=staging fmt=bgra / 565 (copy to optimal) | PASS | PASS |
| mode=compute fmt=565 cs=ra (what RetroArch does for RGB565) | **FAIL, all black** | PASS |
| cs=const / cs=pt / cs=fix / cs=ids (diagnostics) | only texel (0,0) written | correct |
| cs=atom (imageAtomicExchange) | PASS (software address path) | PASS |

Narrowing (all with `cdbg=1`, raw view of the image memory): one store at (5,3) lands nowhere; all
invocations storing constants gives exactly one non-zero texel, (0,0); r32ui storage image behaves the
same (so not the unorm packing); writing the SSBO from the same invocations works for every
invocation (so dispatch/compute is fine). Atomics on the same image land in the right twiddled
positions. Only `smp.wrt` is affected. Setting `non_normalized_coords` on the point sampler used for
images (`PVR_BUFFER_TYPE_POINT_SAMPLER`, `pvr_arch_cmd_buffer.c` ~5963) did not change anything.
Pattern (stores with coordinate 0 work, everything else vanishes) looks like coordinates being treated
as normalized/out of range by the TPU write; the real hardware reason is not determined (GE8300 lacks
`tpu_extended_integer_lookup`, `tpu_image_state_v2`; upstream's smp.wrt path was written for BXS-4-64).

## Root cause location (Mesa `src/imagination/pco/pco_nir_tex.c`)

`lower_image()` lowers `image_deref_store` into `pco_emit_nir_smp()` with `write_data` (-> `nir_smp_write_pco`
-> `smp.wrt`, `pco_trans_nir.c` `trans_smp`), with float non-normalized coords (`nncoords`, since
`hw_int_support` is false without `PCO_DEBUG(INT_SMP)`). Image atomics in the same function already avoid
the TPU: twiddle in software (`usclib_twiddle2d`), add to the tex-state base address, global atomic.

## Fix (verified): software image stores

Same approach as the atomics: for 2D (optionally arrayed) stores to formats known at compile time, compute
the twiddled address and `nir_store_global_2x32` the texel, with a bounds check (the TPU dropped
out-of-bounds writes). `write_data` is one dword per channel (the TPU interleaves), so RGBA8 formats merge the
four low bytes into a dword; 32-bit-per-channel formats store their first N dwords as is. Everything else
(formatless, 16-bit/packed formats, 3D, MSAA, swizzled formats) still goes to the TPU, so no behaviour
change there. Storage images are optimal-tiled only (`pvr_formats.c` offers STORAGE_IMAGE only for
optimal), so the twiddled-only assumption holds (same one the atomics make).

Patch file: `tools/gpu/pvr-sw-image-store.patch` (against `pco_nir_tex.c` in
/mnt/ext1/trimui_build/mesa/mesa; the file is applied in that tree, uncommitted; original copy at
/mnt/ext1/trimui_build/mesa/pco_nir_tex.c.before_swstore). Inserted right before
`unsigned smp_desc = ia ? PCO_IA_SAMPLER : PCO_POINT_SAMPLER;` in `lower_image()`:

```diff
--- a/src/imagination/pco/pco_nir_tex.c
+++ b/src/imagination/pco/pco_nir_tex.c
@@ -1380,6 +1380,116 @@
       return true;
    }
 
+   /* Software image stores.
+    *
+    * The TPU write path (smp.wrt) only lands texel (0, 0) on GE8300: every
+    * store at a non-zero coordinate is silently dropped. Do the same thing the
+    * atomics above do instead: twiddle the coordinates in software and write
+    * the texel with a plain global store. Storage images are always twiddled
+    * (optimal tiling), and the format is known at compile time. write_data
+    * holds one dword per channel (the TPU does the final interleave), so for
+    * 8-bit channels the bytes are merged here. Only formats that are covered
+    * this way (RGBA8 and 32-bit-per-channel ones) take this path, anything
+    * else keeps using the TPU.
+    */
+   unsigned sw_texel_dwords = 0;
+   bool sw_merge_bytes = false;
+   if (write_data && !ia && format != PIPE_FORMAT_NONE && !msaa &&
+       image_dim == GLSL_SAMPLER_DIM_2D && util_format_is_plain(format)) {
+      const struct util_format_description *fdesc = util_format_description(format);
+      bool identity_swizzle = true;
+      for (unsigned c = 0; c < fdesc->nr_channels; c++)
+         identity_swizzle &= fdesc->swizzle[c] == PIPE_SWIZZLE_X + c;
+
+      if (identity_swizzle && fdesc->block.width == 1 &&
+          fdesc->block.height == 1 && fdesc->is_array) {
+         unsigned chan_bits = fdesc->channel[0].size;
+         if (chan_bits == 32) {
+            sw_texel_dwords = fdesc->nr_channels;
+         } else if (chan_bits == 8 && fdesc->nr_channels == 4) {
+            sw_texel_dwords = 1;
+            sw_merge_bytes = true;
+         }
+      }
+   }
+
+   if (sw_texel_dwords) {
+      nir_def *num_comps = nir_imm_int(b, 2);
+      nir_def *dim = nir_imm_int(b, image_dim);
+      nir_def *_is_array = nir_imm_bool(b, is_array);
+      nir_def *is_image = nir_imm_bool(b, true);
+      nir_def *size_comps = usclib_tex_state_size(b,
+                                                  tex_state,
+                                                  num_comps,
+                                                  dim,
+                                                  _is_array,
+                                                  is_image,
+                                                  lod);
+
+      nir_def *xy = nir_channels(b, coords, 0b11);
+      nir_def *in_bounds = nir_iand(b,
+                                    nir_ult(b, nir_channel(b, xy, 0), nir_channel(b, size_comps, 0)),
+                                    nir_ult(b, nir_channel(b, xy, 1), nir_channel(b, size_comps, 1)));
+
+      nir_def *twiddled_offset =
+         usclib_twiddle2d(b, xy, nir_channels(b, size_comps, 0b11));
+      twiddled_offset =
+         nir_imul_imm(b, twiddled_offset, util_format_get_blocksize(format));
+
+      if (is_array) {
+         nir_def *array_index = nir_channel(b, coords, 2);
+         nir_def *array_max = usclib_tex_state_array_max(b, tex_state);
+         array_index = nir_iclamp(b, array_index, nir_imm_int(b, 0), array_max);
+
+         nir_def *tex_meta = nir_load_tex_meta_pco(b,
+                                                   PCO_IMAGE_META_COUNT,
+                                                   elem,
+                                                   .desc_set = desc_set,
+                                                   .binding = binding);
+
+         nir_def *array_stride =
+            nir_channel(b, tex_meta, PCO_IMAGE_META_LAYER_SIZE);
+
+         twiddled_offset =
+            nir_iadd(b, twiddled_offset, nir_imul(b, array_index, array_stride));
+      }
+
+      nir_def *base_addr = usclib_tex_state_address(b, tex_state);
+      nir_def *addr = nir_uadd64_32(b,
+                                    nir_channel(b, base_addr, 0),
+                                    nir_channel(b, base_addr, 1),
+                                    twiddled_offset);
+      nir_def *addr_vec = nir_vec2(b, nir_channel(b, addr, 0), nir_channel(b, addr, 1));
+
+      nir_def *texel;
+      if (sw_merge_bytes) {
+         texel = nir_imm_int(b, 0);
+         for (unsigned c = 0; c < 4; c++) {
+            nir_def *byte = nir_iand_imm(b, nir_channel(b, write_data, c), 0xff);
+            texel = nir_ior(b, texel, nir_ishl_imm(b, byte, c * 8));
+         }
+      } else {
+         texel = nir_trim_vector(b, write_data, sw_texel_dwords);
+      }
+
+      nir_push_if(b, in_bounds);
+      {
+         nir_store_global_2x32(b,
+                               texel,
+                               addr_vec,
+                               .write_mask = BITFIELD_MASK(sw_texel_dwords),
+                               .access = nir_intrinsic_access(intr),
+                               .align_mul = 4,
+                               .align_offset = 0);
+      }
+      nir_pop_if(b, NULL);
+
+      data->common.uses.usclib = true;
+
+      nir_instr_remove(&intr->instr);
+      return true;
+   }
+
    unsigned smp_desc = ia ? PCO_IA_SAMPLER : PCO_POINT_SAMPLER;
    nir_def *smp_state = nir_load_smp_state_pco(b,
                                                ROGUE_NUM_TEXSTATE_DWORDS,
```

Verification (patched build md5 5163a2065096838fc55ba05662081ea5, loaded through `VK_ICD_FILENAMES`
from `/tmp/edge/root/pvrtest/`, installed driver a146262027a441de93cebb716d3b6803 untouched):
`vklinear mode=compute fmt=565` passes at 240x160, 256x224, 320x240 (linear filter), 241x161 with the
frame rewritten each of 3 frames, and RetroArch + mgba (RGB565, Vulkan) shows the Advance Wars map
(67 colours in the screenshot instead of pure black). Not tested: arrayed storage images, other formats
(unchanged path), CTS. The driver build still has the other local patches; only `pco_nir_tex.c` was
touched. The `pvr_arch_cmd_buffer.c` point-sampler experiment was reverted.

To install: copy `build/src/imagination/vulkan/libvulkan_powervr_mesa.so` to
`/tmp/edge/usr/lib/` (rebuild with `build_mesa.sh` from the current tree).

## Workarounds without a driver change

None on the RetroArch side for RGB565 (the remap is hard coded in 1.22.2). Cores that can output
XRGB8888 work today (use the core option or RetroArch's `video_...` pixel format if the core offers one).

## Other finding (separate bug, not the cause here)

Reading back an optimal render target with `vkCmdCopyImageToBuffer` in the same command buffer right
after the render pass returns the **previous** submission's picture (frame N shows frame N-1; resubmitting
the identical frame fixes it; reading a LINEAR render target directly is always right). So the
fragment-job -> transfer-job dependency (or cache invalidation before the transfer job's texture read)
is not honoured. This also hit the first version of the reproducer (hence `rt=linear`), and it will
affect RetroArch screenshots / readback, `vkcmdcopyimagetobuffer` based tests etc. Reproduce with
`vklinear mode=linear rt=optimal frames=3` (frame 1 and 2 differ, `repeat=3` makes later repeats pass).
Not investigated further.

## Files

* `tools/gpu/vklinear.c`, `lin.vert`, `lin.frag`, `rgb565_to_rgba8888.comp` (RetroArch's), `cs_*.comp` (diagnostics), `build_lin.sh`
* `tools/gpu/ra_shot.sh`: device-side helper that starts RetroArch in cage and screenshots with grim
  (needs `apk add grim` in the chroot; ENVX=... to pass env such as VK_ICD_FILENAMES)
* `tools/gpu/pvr-sw-image-store.patch`
