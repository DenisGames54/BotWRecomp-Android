# Native Metal renderer — design (phase 1)

Goal: replace the vendored Cemu GPU layer (`runtime/cemu/Cafe/HW/Latte`: command processor,
register-state emulation, runtime R600→MSL decompiler, texture/buffer caches, Cemu's Metal
renderer) with a renderer written for this port: the game's GPU shaders translated to MSL ahead of
time, rendering going straight to Metal, the way DeadpoolRecomp drives Metal from D3D9.

This document is evidence-first. Everything in §2 was measured on this machine (M1 Pro, macOS 26)
with the tools in §8; statements that are reasoning or assumptions are marked as such.

---

## 0. Summary

* **BotW does not issue its frame through GX2 calls.** 100% of draws (18.1 M draws over 8,040
  frames; 14.8 M over 7,200 in a second run) reach the GPU inside display lists the game submits
  with `GX2DirectCallDisplayList`. In-game a frame is ~3,950 draws, ~5,120 nested
  `INDIRECT_BUFFER` calls and ~366 K PM4 dwords, while the game makes only a few hundred GX2 calls
  per frame (≈15 of them draws). The big per-frame lists are *retained*: recorded with GX2 on
  worker threads, re-executed every other frame, **patched in place by game code** (uniform
  constants, uniform-block addresses, viewports, instance counts) and re-recorded only ~9% of the
  time; 84% of the nested calls go to small lists whose contents never changed.
* Consequence: the PM4 bytes in guest memory are the de-facto ABI. An API-level translator
  (record GX2 calls, replay host commands) would go stale silently, because the game rewrites the
  recorded bytes without calling GX2. **Recommendation: a lean PM4 decoder designed for Metal
  (option B), plus metadata side tables from our own GX2 HLE where it is free (option C-style
  hints).** No Latte register machine, no runtime decompiler, no buffer/texture shadowing: the ~30
  packet types our GX2 emits are decoded straight into Metal state.
* Guest memory works as **one zero-copy `MTLBuffer`** (measured: 4 GB no-copy buffer over the
  guest `mmap`, `maxBufferLength` 8.88 GB, no memory committed by binding it). Uniform blocks and
  vertex data are read in place by the shaders; streamout can write in place.
* Shaders: our own R600 decoder + MSL emitter (`tools/shaders`, not Cemu's decompiler) translates
  **1,308 / 1,308** vertex+pixel shader entries from the two transferable caches, and **1,308 /
  1,308** compile with the Metal compiler. 95.5% of those programs exist verbatim in the game's
  files, so the full set can be enumerated offline (~4,900 programs in the material/terrain
  archives alone).
* **Phase-1 native slice works beyond the target:** with `--renderer native` (Cemu's GPU layer
  not started at all) the native decoder + run-time-translated shaders + Metal render the boot
  screens and the **complete title menu** correctly, and reach gameplay with the **HUD, title card
  and button prompts** correct; the 3D world is mostly missing (known gaps: depth-as-color copies,
  streamout, occlusion queries, render-scale). Unoptimized in-game rate ~22 fps at ~3,670
  draws/frame; 597 shaders were translated and compiled at run time with 0 failures.
* Not used by BotW (measured scenes): geometry shaders, MSAA, compute dispatches, `IMMD` draws.
  Used: streamout (~9 draws/frame), occlusion queries + conditional rendering, QUADS (~50
  draws/frame), POINTS, big-endian 16/32-bit indices, 2D-tiled surfaces, BC1–BC5,
  cube/array/3D textures, depth compare, context-state shadowing, depth buffers sampled as color
  textures, transient render-target memory reused with different formats.

---

## 1. Where the Cemu layer sits today

```
game (recompiled PPC) ──GX2 calls──▶ runtime/src/gx2 (HLE, writes PM4 into guest memory)
                                         │ latte::submit(addr, ndw)
                                         ▼
                      runtime/cemu_shims/latte_glue.mm ──TCL ring──▶ Cemu LatteCommandProcessor
                                                                     register file, shader decompiler,
                                                                     texture/buffer caches, MetalRenderer
```

The seam is `runtime/src/latte/latte.h` (`submit`, `retired`, flips, vsync, event sink). The
native path implements the same contract (`runtime/src/gpu_native/backend.cpp`); a build-time
option puts a run-time selector in front of it without editing the Cemu glue (§6).

---

## 2. How BotW drives the GPU (measured)

Conditions: `build-native` (same game code as `build/`), Cemu backend rendering, PM4 statistics
tap on every submission (`--gpu-stats`), headless, 60 fps mode, title → "Continue" → start of the
Shrine of Resurrection. Runs of 110–170 s; in-game ~50 fps, ~3,950 draws/frame.

### 2.1 Submission structure

| measurement (8,040-frame run, title + in-game) | value |
|---|---|
| draws total / max per frame | 18,121,384 / 5,347 |
| draws in GX2 command-pool buffers (top level) | **0 (0.0%)** |
| draws inside display lists | **100%**, all under `GX2DirectCallDisplayList` submissions |
| submissions: pool buffers / direct-called DLs | 36,731 / 443,169 (≈55 direct DLs per frame) |
| PM4 dwords from pool buffers / from DLs | 1.15 M / 1,724.8 M |
| nested `INDIRECT_BUFFER` packets (GX2CallDisplayList) | 23.1 M (≈5,120 per in-game frame), max nesting depth 1 |

Per in-game frame (steady state): ~3,950 draws, ~5,120 DL calls, ~366 K dwords, ~110
render-target switches, ~413 distinct pipeline-state keys.

What the display lists are:

* Two ~124 K-dword lists (`0x39001860`/`0x38de1860`, double-buffered) carry ~3,250 draws each
  and are submitted every other frame. Against its previous submission each was unchanged 40% of
  the time, changed only in payload words 51% of the time (identical packet layout; ~9 K changed
  dwords over 1,071 calls) and re-recorded (different layout) ~9% of the time.
* Changed payload words, by packet (all direct DLs): `SET_ALU_CONST` 125,801 (uniform register
  values) ≫ attribute-buffer sizes 7,924, VS/PS uniform-block addresses 5,376/5,318,
  `NUM_INSTANCES` 3,959, base instance 3,508, viewport `PA_CL_VPORT_*` 2,833 each, …; plus 44,868
  re-recordings.
* Nested lists (INDIRECT_BUFFER targets): 4,141 distinct; 2,753 never changed; **84% of the
  18.9 M nested calls go to lists that never changed** (recorded at load: one draw + its state).
* GX2 call trace of a 3-vsync in-game window (≈2 game frames, `--trace-frames`): 892 GX2 calls
  in total — 37 `GX2BeginDisplayListEx`/`EndDisplayList` pairs on `WorkerMgr/Worker1|2`, 216
  `GX2SetBlendControlReg`, 100 `GX2CallDisplayList`, 77 uniform-block sets, 5
  `GX2DrawIndexedEx`, **0 `GX2DrawIndexedEx2`** — while the stream carried ~7,900 draws.
* Writers: a write-watch on a DL page caught `GX2SetBlendControlReg`, `GX2SetColorBuffer` and
  `GX2DrawIndexedEx` writing it (recording). The in-place payload changes happen with no GX2 call
  that could produce them, so they come from game code writing into recorded PM4 — inferred, not
  caught in the act (the watch tool only traps the first write to a page).
* Static scan of `U-King.rpx`: 1,671 direct GX2 call sites to 129 functions, no GX2 function
  address taken, **no PM4 headers synthesized in game code** (no `lis 0xC0nn`+`ori 0xXX00` nor
  `li`+`oris` pairs) and no PM4-looking words in data sections. Packet headers always come from
  our GX2 HLE; the game only rewrites payloads.

### 2.2 Packets that reach the GPU

All from our GX2 HLE: `SET_CONTEXT_REG` 93 M, `SET_RESOURCE` 63 M, `SET_CTL_CONST` 29 M,
`INDIRECT_BUFFER_PRIV` 19.6 M, `SET_CONFIG_REG` 16.6 M, `DRAW_INDEX_2` 15.3 M,
`INDEX_TYPE`/`NUM_INSTANCES` 15.3 M, `SET_SAMPLER` 11.5 M, `SET_ALU_CONST` 5 M (30 M dwords),
`SURFACE_SYNC` 2.9 M (~390/frame), `SET_ALL_CONTEXTS` 1.2 M, `MEM_WRITE` 277 K (query init),
`STRMOUT_BUFFER_UPDATE` 191 K, `SET_PREDICATION` 69 K, `DRAW_INDEX_AUTO` 59 K, occlusion-query
HLE packets 35 K each, `CONTEXT_CONTROL` 75 K (always `0x80000077`: register shadowing on),
`LOAD_*` 55 K each inside DLs (`GX2SetContextState`), `WAIT_REG_MEM` 10 K (GPU fences),
`HLE_CLEAR_COLOR_DEPTH_STENCIL` 4.5 K, `HLE_COPY_SURFACE_NEW` 92, scan-out copies 2/frame, swap
packets 1/frame. Unknown/bad packets 0, type-0 packets 0, `DRAW_INDEX_IMMD` 0, `EVENT_WRITE` 0.

### 2.3 Draws

| | |
|---|---|
| primitive types (draws) | TRIANGLES 14.88 M, **QUADS 364 K**, POINTS 60 K, TRIANGLE_STRIP 41 K, LINE_STRIP 7 K |
| index types | `U16_BE` 14.6 M, `U32_BE` 0.74 M (all big-endian) |
| instanced draws | 119–143 K (~0.8%) |
| streamout enabled | 48–68 K draws (~9/frame in-game) |
| predicated (conditional render) | 33–41 K (~9/frame), occlusion queries ~5/frame |
| geometry shader / MSAA / compute dispatch | **0 / 0 / 0** |
| distinct render-target configurations | 430–452; RT switches ~85 per frame average, ~110 in-game |
| pipeline-state keys (programs + output formats + blend + MSAA + prim class) | 826–869 distinct per run, ~413 per in-game frame |
| distinct programs executed (content hash) | fetch 139, vertex 208–211, pixel 270–273 |

### 2.4 Surfaces and textures

* Color targets (draws): RGBA8 9.1 M, R11G11B10F 8.9 M, RG8 7.7 M, RGBA8-sRGB 7.7 M, R16F 1.1 M,
  R32F 1.0 M, RGBA16F 352 K, RGB10A2 317 K, R8 67 K, RGBA32F 59 K, RGBA16 42 K, R16 23 K,
  RGBA16UI 63. Tile modes: 2D_TILED_THIN1 99.3%, then LINEAR_ALIGNED, 1D_TILED_THIN1, 1D/2D THICK.
* Depth: D32F 12.8 M draws, D16 4.2 M; 2D_TILED_THIN1.
* Register layout quirks of our GX2 HLE (inherited from Cemu): `CB_COLORn_BASE` holds the full
  address (not `>>8`, swizzle in bits 8–10); `GX2SetDepthBuffer` leaves `DB_DEPTH_BASE` 0 and
  puts the address in `DB_HTILE_DATA_BASE`; GX2 leaves stale `CB_COLORn_*` registers with a full
  `CB_TARGET_MASK` — only targets enabled in `CB_SHADER_MASK` are real.
* Textures (1,767 distinct by address): 2D 1,695, 2D-array 52, cube 14, 3D 6; tile modes
  2D_TILED_THIN1 1,109, LINEAR_ALIGNED 390, 1D_TILED_THIN1 263, THICK 5; formats BC1 (+sRGB) 964,
  BC4 153, BC5 48, BC3 (+sRGB) 49+, RGBA8 168, R16 174, RGBA16 124, R8 24, 5_6_5 and RT formats.
  Component selection (`DST_SEL` in resource word 4) is used heavily (single-channel BC4/R8 fonts
  and masks); ignoring it renders text as solid blocks — confirmed in the slice.
* **Reinterpretation, measured on one in-game frame** (`tools/shaders/analysis/frame_aliasing.py`
  over a `--gpu-dump-frame` log): 4,965 samples of targets written earlier in the frame in the same
  format; depth buffers sampled as color textures (D32F→R32F 1,528 samples, D16→R16 216); color
  targets read in another format (R32F→R16F 1,308, R16F→R16 260, R16F→RG11B10F 200 — some may be
  transient-address reuse rather than true reinterpretation); 11 render-target addresses used with
  2–4 different formats/sizes within the frame (transient memory pool).
* Copies: `HLE_COPY_SURFACE_NEW` only 92 per run, all `TM_LINEAR_SPECIAL` → tiled (texture
  uploads). Clears ~0.5/frame. Scan-out: TV 1280×720 RGB10A2 and DRC 854×480 — sharing one
  address with other targets during the frame.

### 2.5 Shaders

* Inventory: the two transferable caches hold 1,307 + 889 entries → **735 VS entries (382
  distinct programs), 573 PS entries (458 distinct), 0 GS**. VS entries outnumber programs because
  Cemu keys VS variants on the fetch shader and register state.
* Provenance (byte search in the game files): 1,064 entries are verbatim in
  `Pack/Bootup_Graphics.pack` (`Shader/uking_mat.product.sbfsha` 715, `uking_sys` 74, agl
  `agl_technique_pfx/common/technique/lght.sharcb` 155, KSys `uking_pass_shader.sharcb` 56, gsys
  `common.sharcb` 18, terrain/flower/grass/tree/water archives 46); 185 more in `Bootup.pack`
  (Effect 94, System 34, Layout 20) and `TitleBG.pack` (Terrain 34, Effect 3); 59 not found
  (40 share their first 48 bytes with an archived program: variants).
* Size of the full set (heuristic: one `EXPORT_DONE`+`END_OF_PROGRAM` per program): uking_mat
  ~3,544, terrain ~507, flower ~436, grass ~145, water ~88, sys ~56, grudge ~51, route ~20,
  tree ~14 → **~4,860 programs**, plus the agl/gsys/KSys sharcb sets, effect (ptcl) shaders and
  layout shaders.
* Features (of the 1,308 translations): predicate-stack control flow 130 VS / 116 PS, loops 3,
  `KILL` 132 PS, depth-compare sampling 8 VS / 97 PS, cube maps + `CUBE` 105 PS, 3D 79, arrays 182,
  gather 45, depth export 7, uniform registers (`cfile`) 62 VS / 89 PS, uniform blocks 658 VS /
  370 PS, address-register relative addressing 17, VFETCH from buffers in the VS body 125,
  streamout 1.

### 2.6 Today's cost (Cemu path, for reference)

`[gpu]` log in-game: 49–51.6 fps at ~3,950 draws/frame; main-thread waits ≈ 11 ms/s on
`GX2DrawDone`, ≈ 48 ms/s on events. Where the rest of the frame time goes (Cemu GPU thread vs.
game CPU) was not profiled in this phase.

---

## 3. Options

### A. Translate at the GX2 API level

Record GX2 calls into host command lists while a display list is recorded; replay them on
`Call/DirectCallDisplayList`. **Rejected on the evidence:** the game rewrites recorded PM4
payloads in place without calling GX2 (§2.1) and exposes the raw lists (`GX2GetCurrentDisplayList`,
`GX2PatchDisplayList` patches shader addresses at byte offsets, `GX2CopyDisplayList` copies raw
words). A host-side recording would have to mirror and re-diff the guest bytes anyway — option B
with extra steps. Recording also runs on several cores at once.

### B. Lean PM4 decoder designed for Metal

Decode the packets our GX2 writes directly into typed state and Metal work. Cost estimate
(reasoned): ~366 K dwords/frame ≈ 22 M dwords/s at 60 fps; a switch decoder at 1–2 ns/dword is
20–45 ms/s of one core. The phase-1 decoder confirms decoding is not where time goes (§6.1); state
resolution and resource validation are.

### C. Hybrid

B, plus metadata our GX2 HLE has for free: `GX2SetPixelShader`/`VertexShader` know the
`GX2*Shader` struct behind a program address (register block, sampler/uniform tables),
`GX2InitTextureRegs` knows the full surface description (mip offsets, AA, swizzle),
`GX2SetColorBuffer` the full color buffer. Side tables keyed by guest address remove guesswork;
the PM4 stays authoritative.

**Recommendation: B with C's side tables.** It is the only option consistent with what the game
does to its display lists, keeps the GX2 HLE unchanged, and lets every subsystem below be native.
"Not emulating the command processor" holds where it matters: the packet stream is parsed once
into Metal-ready state; there is no Latte register machine behind it.

---

## 4. Recommended architecture

```
GX2 HLE (unchanged) ─PM4─▶ gpu_native::Decoder (GPU thread)
                              │ register file, DL walking (re-decode every frame: DLs are
                              │ patched in place), shadow-area writes for context states
                              ├─▶ state resolver ─▶ PSO cache, depth/stencil + sampler caches
                              ├─▶ surface cache (address-keyed MTLTextures, views, detile jobs)
                              ├─▶ shader library (AOT metallib / pre-translated MSL; JIT fallback)
                              └─▶ Metal encoder: guest-memory MTLBuffer bound with offsets for
                                   uniform blocks, attributes, streamout
```

### 4.1 Guest memory is a Metal buffer

`g_mem` is one anonymous `mmap` (MAP_NORESERVE, first 64 KB PROT_NONE). Wrap
`[g_mem+64K, g_mem+64K+4 GB)` with `newBufferWithBytesNoCopy` (measured: works; a kernel reading
guest offsets 0x10000000/0xF4000000 returns the right bytes; footprint stays 8.5 MB). The phase-1
renderer does exactly this. Consequences:

* Uniform blocks are bound in place (`setBuffer:guest offset:addr-0x10000`); GX2 uniform blocks are
  already little-endian (games `GX2EndianSwap` them; Cemu uploads them raw).
* Vertex attributes are fetched in the vertex function with the byte swaps the fetch shader
  specifies. No vertex buffer cache.
* Streamout writes straight into guest memory (phase 3), as on hardware.
* CPU/GPU coherency is the Wii U's own contract: the game double-buffers and fences
  (`GX2SetGPUFence`, timestamps); the backend retires timestamps from Metal completion handlers.

### 4.2 Decoder

`runtime/src/gpu_native/pm4_decoder.cpp` (exists): register file, context-state shadowing
(`CONTEXT_CONTROL 0x80000077` + `LOAD_*` → writes mirrored to the shadow area), `INDIRECT_BUFFER`
recursion, fences, `MEM_WRITE`, timestamps, predication, HLE packets → typed renderer calls. No DL
decode cache: re-decoding is cheap and DLs are patched in place without notice. If profiling
disagrees, cache per (DL address, content hash) — a 124 K-dword memcmp is ~20 µs.

### 4.3 Surfaces, tiling, formats, aliasing

* **One address-keyed surface cache** for textures and targets; per surface: host texture, write
  sequence (which surface at an address was written last), content hash for CPU data.
* **Detiling on the GPU**: compute kernels for 1D/2D thin/thick reading the guest buffer and
  writing the texture (AddrLib address math; Cemu's port is in `runtime/src/latte/addrlib` +
  `runtime/cemu/.../LatteAddrLib`). Phase 1 detiles on the CPU through the vendored
  `gx2SurfaceCopySoftware`. BC1–BC5 stay compressed (sampled natively on Apple GPUs).
* **Formats**: table from GX2 surface format (hw format | int/signed/sRGB/float bits) to
  `MTLPixelFormat`, `DST_SEL` → `MTLTextureSwizzleChannels` views (5_6_5/1_5_5_5 get an R↔B swap).
* **Aliasing** (measured, §2.4): same-class reinterpretation via `newTextureViewWithPixelFormat`
  (textures created with `PixelFormatView`; Metal *aborts* on incompatible views, so the renderer
  checks compatibility first); depth→color (D32F→R32F, D16→R16) and cross-class through a compute
  copy; transient address reuse means the cache must pick the *last written* surface at an
  address, never just "a" surface there.
* **Invalidation**: CPU writes reach the GPU through DCFlush (our coreinit already calls
  `LatteBufferCache_notifyDCFlush` for DCFlushRange/memcpy/OSBlockMove), `GX2Invalidate` and
  `SURFACE_SYNC` (~390/frame). Phase 1 uses a sampled content hash, once per frame per texture.

### 4.4 Pipelines and caches

* PSO key = VS/PS translation + color formats/write masks/blend (`CB_BLENDn_CONTROL`,
  `CB_COLOR_CONTROL`, masks) + depth format + sample count + primitive class. Measured ~870 keys
  per run (~413 per frame); the slice created 892 in-game with 0 failures.
* Depth/stencil and samplers cached by register words (border colors beyond Metal's three need a
  shader path; rare).
* `MTLBinaryArchive` populated as pipelines are created (Deadpool's `shader_cache.mm` model).

### 4.5 Shaders (detail in §5)

* ABI: buffer(0) = per-draw `StageInfo` (native texture sizes, render-scale factors, attribute
  strides, alpha-test reference, window→clip transform); `cfile` and `ubN`/`abN` assigned
  compactly per shader and listed in the manifest; texture/sampler slot N = GX2 slot N; VS
  outputs/PS inputs named by Latte semantic id (`[[user(sNN)]]`).
* Vertex fetch lives in the VS (from the fetch shader: format, offset, endian swap, divisor);
  strides and addresses are run-time values.
* Index buffers are big-endian: converted per draw into a ring (phase 1: CPU, 0.84 s per 5 s
  in-game → cache static ones by address+hash, or convert on the GPU before the pass). QUADS
  (~50/frame) become lists in the same step; FANs/LOOPs likewise; RECTS (unseen) would need VS-side
  expansion.
* Geometry shaders: none measured; if one appears, mesh shaders (Cemu's approach) or compute.

### 4.6 Render scale / upscaling

Contract: **the guest always sees native sizes**; the host allocates some targets larger.
* Which surfaces scale: reuse the existing rule set (`runtime/cemu_shims/botw_texture_rules.inc`,
  generated by `tools/gen_texture_rules.py` from the BotW graphic pack), keyed by size/format.
* Shader-visible corrections come from `StageInfo`, never from recompiling shaders:
  `frag_coord_scale` (pixel position reads), `tex_scale` (texel fetches / unnormalized coords),
  `tex_size` (native sizes for `GET_TEXTURE_RESINFO`). Viewport/scissor are scaled on the host
  when the bound target is scaled. These fields already exist in the phase-1 ABI.
* Output: scan-out copy into the drawable with the existing bicubic upscale, or MetalFX spatial.

### 4.7 Synchronization and timing

Retire a submission's timestamp from its Metal command buffer's completion handler (done in
phase 1). Vsync at 59.94 Hz with BotW's flip accounting (Cemu `LatteTiming_signalVsync`'s BotW
branch: one flip executed per vsync while requests are pending) — done. Occlusion queries: Metal
visibility results written back in Cemu's query layout (LE u64 begin/end at +0/+8, clear the
'OCPU' marker); phase 1 reports "visible". Conditional rendering reads results at decode time.

---

## 5. Ahead-of-time shaders

### 5.1 Enumerating everything

1. **Shader archives** (`Pack/Bootup_Graphics.pack`: `Shader/uking_{mat,terrain,terrain_water,
   terrain_route,sys,tree,flower,grass,grudge_slime_mold}.product.sbfsha`; BFSHA v4.5). Each
   shading model stores every compiled variation, i.e. the complete static-option space. Parse
   BFSHA to get programs *with* their GX2 register blocks.
2. **agl/gsys/KSys system shaders** (`System/Agl/agl_resource.*.ssarc`: `agl_technique{,_pfx,
   _shdw,_lght,_lght_lpp}.sharcb`, `agl_common.sharcb`; gsys `common.sharcb`; KSys
   `uking_pass_shader.sharcb`).
3. **Effects** (`Bootup.pack:Effect`, `TitleBG.pack:Effect`, `content/Effect/*.sesetlist` EFTB)
   and **layout** shaders (`Bootup.pack:Layout`).
4. **Fetch shaders**: shipped ones next to the models' vertex layouts; others from
   `GX2InitFetchShaderEx` (8 call sites). Their identity is the attribute layout, fully described
   by the bfres vertex buffers — enumerable offline.
5. **The transferable cache** (what has actually been used) is the regression/priority set;
   run-time misses get logged and appended.

### 5.2 State-dependent variants

Cemu keys shaders on extra state (`LatteSHRC_CalcVSAuxHash/PSAuxHash` and base-hash mixing in
`LatteShader.cpp`): VS — fetch shader, PS input table, GS, `PA_CL_VTE_CNTL`, RECTS/POINTS,
streamout, DX clip space, (Metal) strides, rasterization enable, integer textures; PS —
`CB_SHADER_MASK`, alpha test, texture dims, target data types, depth attachment, textures that are
bound targets. Native handling:

| state | native handling |
|---|---|
| fetch layout (formats, offsets, endian) | baked per (VS, fetch shader) pair — both enumerable; strides/addresses are uniforms |
| semantic tables (SQ_VTX_SEMANTIC, SPI_VS_OUT_ID, SPI_PS_INPUT_CNTL) | baked: same `GX2*Shader` struct as the program |
| alpha test function | function constant (reference is a uniform) |
| texture dimension / integer / depth compare per slot | function constants selecting declared texture types (`[[function_constant]]` on arguments) |
| render-target data type (float/int/uint) | function constant per target (63 draws use an integer target; phase 1 bakes it) |
| viewport-transform disable, GL vs DX clip space | function constants |
| render scale | never in the shader — `StageInfo` (§4.6) |

Specializing an AOT-compiled function with `MTLFunctionConstantValues` is an AIR-level pass, far
cheaper than compiling MSL; resulting pipelines go into the binary archive. Phase 1 bakes these
from the register snapshot and lists them per translation (`variant_regs` in `manifest.tsv`; the
run-time cache keys on the same `(register, value)` pairs).

### 5.3 Packaging

`.metal` → `xcrun metal -c` → `.air` → `xcrun metallib` → `botw_shaders.metallib` shipped with the
app (`compile_all.py` does this when the toolchain is present). **This machine has no offline
Metal toolchain** (`xcrun metal` → "missing Metal Toolchain; use xcodebuild -downloadComponent
MetalToolchain"); I did not install it, so phase 1 validates with the Metal framework compiler
(`mtlcheck`, same front end). Fallback without the toolchain: ship the MSL, compile it at first
start in the background (Deadpool's model), keep the binary archive.

---

## 6. What phase 1 built

* `runtime/src/gpu_native/` — compiled only with `-DBOTW_NATIVE_GPU=ON`. Every source is wrapped in
  `#if BOTW_NATIVE_GPU`, so the default and iOS builds (which glob `runtime/src`) compile them to
  empty objects.
  * `gpu_native.cmake` + `latte_switch.mm.in`: the option. The generated switch compiles
    `runtime/cemu_shims/latte_glue.mm` with the 20 functions the native backend overrides renamed
    to `latte::latte_cemu_*` and defines the real names as a run-time selector (Cemu by default,
    `--renderer native` / `BOTW_RENDERER=native`). Functions the glue gains later keep Cemu's
    implementation automatically (this already absorbed `set_display_mode` and
    `set_output_filter`). No edit to the glue or `main.cpp`; arguments are read with
    `_NSGetArgv`.
  * `pm4_stats.cpp`: the measurement tap (`--gpu-stats N`, `--gpu-dump-frame N`) behind §2.
  * `backend.cpp` (queue, timestamps, vsync/flip, events, liveness/timing log),
    `pm4_decoder.{h,cpp}`, `renderer.h`, `renderer_metal.mm` (guest-memory buffer, surface cache,
    CPU detiling, format/swizzle views, run-time translation via `tools/shaders`, PSO/depth/sampler
    caches, clears, scan-out, PNG capture).
* `tools/shaders/`: `latte2msl` (inventory reader + own R600 decoder + MSL emitter; also linked into
  the native runtime), `mtlcheck`, `compile_all.py`, `analysis/` (static scan, SARC/Yaz0 reader,
  frame aliasing).

Shader pipeline (both caches): translated VS 735/735, PS 573/573, GS 0/0; compiled 1,308/1,308
(MSL 3.1, Metal framework compiler, 18.8 s cold at -j4). Compilation proves well-formedness; the
slice below is the first semantic check, phase 5 adds differential tests.

### 6.1 Native runtime slice (`--renderer native`, Cemu's GPU layer never started)

* Boot screens and the **full title menu** (art, logo, Continue/New Game/Options/amiibo/DLC, version
  text) render correctly: 0 skipped draws, 37 shaders, ~115 pipelines, 75 surfaces.
  Evidence: `build-native/reports/native_title_menu_strip.png`.
* After "Continue", gameplay: **HUD hearts, "Shrine of Resurrection" title card, Move/Controls/
  Camera prompts correct**; the 3D world is mostly black with fragments
  (`build-native/reports/native_ingame_frame4800.png`). Known causes: depth buffers sampled as
  color are skipped (~32 draws/frame), streamout not emitted, occlusion queries forced visible, no
  render-scale handling, stencil not wired, scissor/viewport edge cases.
* 597 shaders translated + compiled at run time with **0 failures**; 892 pipelines, 0 failures;
  1,890 surfaces.
* Speed (unoptimized, GPU thread): in-game ~22 stream fps at ~3,670 draws/frame. Per 5 s: draw
  encoding 3.5 s, of which texture validation 0.84 s and index conversion 0.87 s; fence waits 0.
  Before the once-per-frame texture validation, hashing guest textures on every bind took 95% of
  the thread (≈3 fps) — the first thing a real dirty-tracking scheme removes.
* Bugs the slice flushed out (all fixed): `DST_SEL` swizzles (fonts as blocks, single-channel
  textures red), scan-out picking a same-address DRC surface, integer render targets needing
  integer outputs, an incompatible texture view (RGBA16Uint→BC4) aborting the process, stale
  target registers attaching a 64×3 surface next to the 1280×720 TV target.

---

## 7. Plan

| phase | work | effort (focused) |
|---|---|---|
| 2 | Perf foundation: DCFlush/`SURFACE_SYNC` dirty tracking (drop per-frame hashing), static index-buffer cache + GPU QUADS conversion, binding fast path (precomputed binding tables instead of string compares), GPU detiling kernels; on-screen presentation (CAMetalLayer, display modes) | 1–2 weeks |
| 3 | Full 3D: depth→color and cross-class copy pass, stencil, streamout into guest memory, Metal visibility queries + predication, render-scale rules port (`StageInfo` fields exist), border colors | 2–3 weeks |
| 4 | Correctness tooling: per-pass capture diff against the Cemu path on scripted routes; translator differential tests (run our MSL and Cemu's on identical inputs) | 2 weeks |
| 5 | AOT at scale: BFSHA/SHARCB/EFTB parsers → full program inventory, function-constant variants, metallib build step (needs the Metal toolchain), miss logging, binary archive | 2–3 weeks |
| 6 | Make native the default once a test route matches; then drop the Cemu layer from the default build; iOS/A-series check | 1 week |

Risks: in-place DL patching assumption (verify the writer with a better watch tool), aliasing
corner cases, predication latency, translator semantics (mitigated by phase 4).

---

## 8. Reproducing

```sh
cmake -G Ninja -S . -B build-native -DBOTW_NATIVE_GPU=ON && nice -n 10 ninja -C build-native -j4 BotWRecomp
export BOTW_USER_ROOT="$HOME/Library/Application Support/BotWRecomp-test-native"   # copy of the test data
S="1500:key:a,2400:key:a,3300:key:a,3600:key:a,3900:key:a,4200:key:a"
# measurements (Cemu renders): -> $BOTW_USER_ROOT/gpu_stats.txt, gpu_frame_4600.txt
build-native/BotWRecomp --game ../game --headless --mute --seconds 150 --gpu-stats 300 --gpu-dump-frame 4600 --script "$S"
uv run python -I tools/shaders/analysis/frame_aliasing.py "$BOTW_USER_ROOT/gpu_frame_4600.txt"
uv run python -I tools/shaders/analysis/gx2_static_scan.py ../game/code/U-King.rpx
# native renderer: -> $BOTW_USER_ROOT/captures-native/frame_N.png
build-native/BotWRecomp --game ../game --headless --mute --renderer native --seconds 120 --capture 600 --script "$S"
# offline shader pipeline
cmake -G Ninja -S tools/shaders -B build-native/shaders-tool && ninja -C build-native/shaders-tool
build-native/shaders-tool/latte2msl --cemu runtime/cemu --out build-native/shaders <copies of *_mtlshaders.bin>
uv run python -I tools/shaders/compile_all.py build-native/shaders
```
