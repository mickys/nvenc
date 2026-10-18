# Research: feeding NVENC from video memory (capture surfaces in VRAM)

Question (2026-10-18): T5.06 found a GPU tiling pass slower than the CPU (1.9 ms vs 0.87 ms at 4K) because the test
surface lived in system RAM. What would it take for capture frames to stay in VRAM, so NVENC reads them without a CPU
pass?

Sources: nullmoth's driver source (`vendor/nvidia-macos-driver`: `kexts/NVRM/accel/nvrm-accel.cpp`,
`kexts/NVRM/nvrm_surfshare*.{h,cpp}`, `kexts/NVRMFB/fb/nvrm-fb.cpp`, `plugin/NVMTLDevice.m`, `plugin/nvmtl_vk.c`,
`nvk/nvk-macos.patch`), runtime counters in the guest, and two probes (`tools/surfvram.m`, `tools/vramtile.m`).

## How IOSurfaces are stored on this driver today

- An IOSurface's own memory is always **pageable system RAM** (IOSurface.kext).
- **NVAccel keeps a VRAM copy** of every IOSurface the GPU uses: `NVVidMemory::allocPhysical` → NVRMFB `vramGrant`
  (NVKMS-KAPI `allocateMemory`, video memory, scanout type, pitch layout, mapped into the kernel through BAR1). It
  stays attached across page-offs ("sticky"). The runtime counters showed 6.8 GB allocated this way since boot
  (`debug.nvaccel_vm_alloc_bytes`); the name says "vm", but the memory is video memory.
- The **Metal plugin imports that VRAM** for `newTextureWithDescriptor:iosurface:plane:` (escape
  `NVRM_ESC_SURF_VRAM` 0xE2 → `NV04_SHARE` of the KAPI memory into the caller's RM client → `NV_ESC_RM_DUP_OBJECT`),
  draws into it, and marks it dirty (`NVRM_ESC_SURF_DIRTY` 0xE3).
- A **CPU lock pages it off**: `NVResource::pageoff` copies VRAM → the system-RAM pages with the copy engine.
- The plugin imports only **one-plane surfaces with a row pitch that's a multiple of 32**. Anything else (multi-plane NV12,
  odd pitches) goes through a private image, copied in and out by the CPU (`nvmtlSurfaceIn/Out`).
- Other CPU-mappable allocations (`NVK` "LOCAL+mapped", Shared MTLBuffers) are put in system RAM unless BAR1 covers all
  of VRAM (`nvrm_local_map_stays_vram`: needs `NVK_LOCAL_MAP_SYSMEM=0` **and** BAR1 ≥ VRAM; here 4 GB < 24 GB).
- BAR1 budget for the VRAM copies: half of BAR1 (2 GB with a 4 GB BAR1).
- Display: with flips off (`-nvrmnoflip`, our setting, since zero-copy scan-out makes capture black), WindowServer's final
  frame is paged off and then **copied by the CPU word by word into the scanout through BAR1**.

## What the probes measured (RTX 4090, Tahoe 26.7.1)

`tools/surfvram.m`: render a pattern into an IOSurface with Metal, then take its VRAM copy into **our own** RM client.

| surface | VRAM copy after the Metal render | our client maps it | CPU lock (page-off) |
|---|---|---|---|
| BGRA 1920x1080 | correct (0 of 384 samples wrong) | yes: share + dup **0.06-0.1 ms** | 2.3 ms |
| BGRA 3840x2160 | correct (0 of 1449) | yes | **7.8 ms** |
| NV12 (420f), two planes | **stale**: each plane has a VRAM allocation, but the pixels went through the plugin's private-image fallback | yes (both planes) | ~0 ms (already copied out by the CPU) |

`tools/vramtile.m`: the whole GPU-resident path, BGRA only. Metal converts a GPU-rendered BGRA surface into NV12 in
NVENC's layout, written into a second IOSurface. Our RM client takes that surface's VRAM, and NVENC encodes straight from
it (`Enc.extInput`).

| | 1920x1080 | 3840x2160 |
|---|---|---|
| Metal convert + tile, VRAM → VRAM | **0.029 ms GPU** | **0.063 ms GPU** (0.34 ms wall incl. submit + wait) |
| bytes vs a CPU reference (same integer math) | identical | identical |
| NVENC from that VRAM | 1.16 ms, 49.6 dB | 4.17 ms, 49.4 dB |

For comparison, today's Parsec path at 4K: NV12 frames from ScreenCaptureKit in system RAM, our CPU tiling 0.87 ms +
engine 3.8 ms = 4.7 ms. Before that, WindowServer has already copied the frame on the CPU: a `sample` of WindowServer
during a Parsec session shows `-[NVMTLTexture nvmtlSurfaceOut/In]` → `memmove` (issue #93 measured WindowServer at
60-100 % of a core in that stack). The probes don't show how much of that is the capture frames.

## What it would take

**Already there (no driver change):**
- Any process that can look up an IOSurface ID can get the surface's VRAM into its own RM client (0xE2 + DUP), and
  NVENC can read it. No privilege needed.
- For one-plane surfaces rendered by the GPU (BGRA), the VRAM copy is current. A frame can go GPU → our Metal kernel →
  NVENC with no CPU pass, as `vramtile` shows.

**Driver changes (nullmoth's plugin / NVK), for Parsec's NV12 path:**
1. **Import multi-plane IOSurfaces into VRAM in the plugin.** The kernel already gives each plane its own VRAM allocation
   (`SURF_VRAM` takes a plane index); the plugin's `nvmtl_vk_surface_vram()` refuses anything but one plane with a
   32-aligned pitch and falls back to CPU copies. Supporting per-plane import (an R8 luma texture and an RG8 chroma texture
   on two VRAM allocations) would keep WindowServer's NV12 capture output in VRAM, and also remove WindowServer's CPU
   copies. It's the larger change: NVK images over imported per-plane memory, pitch rules, and the dirty/page-off
   bookkeeping per plane.
2. **A way for a consumer to know the VRAM copy is current** before reading it: a query escape (VRAM newest? pending GPU
   mark?) or a wait on WindowServer's completion mark (`nvSurfWaitReady` exists in the kernel, but only the display path
   uses it). Without it, an encoder can't tell a GPU-written surface from a CPU-written one, apart from trusting the
   producer.
3. Optional: zero-copy scan-out (`nvaccel_iop_flip`) breaks capture today because nothing pages off the scanned-out
   composite. If consumers read VRAM directly, capture and flips could coexist: flips also save WindowServer's CPU copy
   into the scanout.

**On our side (libnvencvt), once the driver keeps capture frames in VRAM:**
1. From the `CVPixelBuffer`, get the IOSurface ID; `SURF_VRAM` per plane + DUP + GPU map, cached per surface ID
   (ScreenCaptureKit cycles through a small pool), unmapped when the surface goes away.
2. Convert/tile on the GPU from VRAM into NVENC's layout: a Metal compute kernel (≈0.06 ms at 4K, as measured), writing an
   IOSurface whose VRAM our client maps. NVENC can't read pitch-linear input, and the copy engine can't produce NVENC's
   layout, so a shader pass stays necessary.
3. Order it: wait for the Metal command buffer (≈0.3 ms wall today), or better, have the NVENC channel wait on a semaphore
   the Metal work releases.
4. Never lock the source surface on the CPU, so no page-off happens.

**Expected gain at 4K:** our encoder 4.7 ms → ~4.1-4.2 ms (the 0.87 ms CPU tiling becomes ~0.06 ms GPU plus ~0.3 ms
submit/wait), plus whatever WindowServer spends copying the NV12 frame out on the CPU. The bigger win is system-wide: no
CPU copies of 12-33 MB frames in WindowServer or in the encoder, and BGRA consumers avoid a 7.8 ms page-off.

**Risks and limits:**
- The VRAM copies count against the BAR1 budget (2 GB with a 4 GB BAR1).
- The share/dup path has no access check beyond IOSurface lookup. Fine for us, but nullmoth may want to restrict it.
- It ties our encoder to undocumented driver escapes (0xE2/0xE3, ABI version 2) that can change between releases.
  Check the version and fall back to the CPU path.
