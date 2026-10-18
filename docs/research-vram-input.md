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

## Numbers (2026-10-18, RTX 4090 in a KVM guest, 4K desktop, Parsec streaming)

### Per-frame costs of today's surface paths (`tools/surfcost.m`)

| path, 3840x2160 | wall | CPU | 1920x1080 wall |
|---|---|---|---|
| NV12 rendered by Metal into a **two-plane** IOSurface (plugin fallback: private image + CPU copy-out) | **5.94 ms** | **5.24 ms** | 1.41 ms |
| same planes as two **one-plane** IOSurfaces (VRAM import) | 0.24 ms | 0.25 ms | 0.66 ms |
| CPU lock of a GPU-rendered BGRA surface (page-off: copy engine to a staging buffer, then a CPU copy into the pages) | **7.59 ms** | **7.17 ms** | 1.97 ms |
| Metal convert + tile BGRA → NVENC-layout NV12, VRAM → VRAM (`vramtile`) | 0.063 ms GPU / 0.34 ms wall | ~0 | 0.029 ms GPU |
| CPU copy of a BGRA frame into VRAM through BAR1, `memcpy` | **22.8 ms** | 22.8 ms | 5.5 ms |
| same, 32-bit word loop (what NVAccel's display copy does) | **83.6 ms** | 83.6 ms | 21.1 ms |
| CPU read back through BAR1 | ~7 s | | ~1.5 s |

**BAR1 is effectively uncached in this VM.** RM maps it write-combined in the guest (caching type 2), but the host maps
the 4090's BAR1 (`0x2100000000-0x2200000000`) as **uncached-minus** (`/sys/kernel/debug/x86/pat_memtype_list` on edi,
VFIO's mapping), and guest write-combining doesn't take effect: 1.4 GB/s with `memcpy`, 0.37 GB/s with 32-bit stores.
On bare metal, write-combined BAR writes are typically several times faster, so the display-copy numbers above are a
property of this KVM setup as much as of the driver.

### Live: WindowServer and Parsec during a session (20 s windows)

| | static desktop | full-screen motion (CSS-animated layer in Safari) |
|---|---|---|
| Parsec frame rate (new desktop frames) | 20-30 fps | **~10 fps** |
| our encoder, entry → emit | 4.7 ms | 5.0 ms |
| WindowServer CPU (user / kernel) | 16 % / 3.5 % of a core | 23 % / **17 %** of a core |
| display transactions (`debug.nvaccel_iop_ok`) | 0.15 per s | **4.8 per s** |
| WindowServer GPU-mark waits that hit the 100 ms timeout | | **69 in 20 s** |
| parsecd CPU | 16 % | 10 % |
| screensharingd (a macOS Screen Sharing session open at the same time) | | 43 % |

A `sample` of WindowServer during the motion test: the plugin's `-[NVMTLTexture nvmtlSurfaceOut]` was on the stack for
~2860 samples in 8 s. Almost all of it (1343 samples under `nvmtlAdvancePart` alone) was **blocked in the kernel in
`NVRM_ESC_SURF_DIRTY`**, not copying (`memmove` at the top of the stack: 136 samples). The escape takes NVAccel's
accelerator family lock (`nvFamLock`, blocking), and the display transaction's CPU copy through BAR1 very likely runs
under that same lock. That last link is inferred from the code, not measured.

## What this means

1. **Capture in VRAM saves little in our encoder:** 0.87 ms CPU tiling → ~0.06 ms GPU + ~0.3 ms submit/wait. Parsec's
   encode latency would go 4.7 → ~4.1 ms (~3.9 ms with a GPU-side wait instead of a CPU wait).
2. **For the capture producer it's a 24x difference** when NV12 is rendered by Metal: 5.9 ms → 0.24 ms per 4K frame,
   if the plugin imported multi-plane surfaces into VRAM. In the live test, though, WindowServer's copy-outs were mostly
   blocked rather than copying, so the CPU copy isn't the main cost there. How ScreenCaptureKit produces Parsec's 420f
   frames (which process, which path) isn't pinned down yet.
3. **CPU readers of GPU-rendered BGRA surfaces** (screenshots, BGRA capture, VideoToolbox conversions) pay a 7.6 ms
   page-off per 4K frame today. Reading the VRAM copy on the GPU removes it.
4. **The biggest cost on this machine is the display copy path, not capture.** With zero-copy scan-out off (it makes
   capture black), every desktop update is copied by the CPU into the scanout through BAR1, word by word. Here that's
   ~35-85 ms of WindowServer kernel time per full 4K update, about 17 % of a core under motion. It holds the accelerator
   lock, the desktop runs at ~5-10 updates per second under motion, and Parsec can't deliver more frames than that.
   **Enabling zero-copy scan-out would remove that copy**, but capture then has to read the composite from VRAM rather
   than from the IOSurface's system pages: the same mechanism as above, on the producer side (WindowServer/SkyLight
   and the plugin), plus a page-off or GPU read where capture needs the pixels. That's driver work.
5. **Independent of the driver, write-combining for BAR1 in the VM** would make the existing copy path several times
   cheaper (KVM/VFIO maps the BAR uncached-minus on edi). Whether the host kernel/QEMU can map it write-combined needs
   research; it's a host-wide change on edi, so ask before touching it.

## Expected improvements (summary)

| change | who does it | gain (4K) | basis |
|---|---|---|---|
| encoder reads VRAM, GPU tiling | us (needs 2 below for NV12) | encode 4.7 → ~4.1 ms; ~0.9 ms CPU/frame saved | measured parts |
| plugin imports multi-plane IOSurfaces into VRAM | nullmoth | NV12 producer 5.9 → 0.24 ms per frame; removes CPU copy-outs | measured (synthetic) |
| consumers read VRAM instead of locking | us / apps | −7.6 ms per BGRA frame read | measured |
| zero-copy scan-out compatible with capture | nullmoth (+ the above) | removes ~35-85 ms CPU per desktop update here; desktop under motion ~10 fps → up to the GPU's compositing rate; WindowServer −~17 % of a core | measured components, end result estimated |
| write-combined BAR1 mapping in the VM | host (edi) | display copy several times cheaper without driver changes | measured uncached behaviour, gain estimated |

Not measured yet (would need the user's OK, since it affects the live desktop and the Parsec stream): desktop frame rate
and WindowServer CPU with zero-copy scan-out switched on (`sysctl debug.nvaccel_iop_flip=1`; capture goes black while
it's on).

**Risks and limits:**
- The VRAM copies count against the BAR1 budget (2 GB with a 4 GB BAR1).
- The share/dup path has no access check beyond IOSurface lookup. Fine for us, but nullmoth may want to restrict it.
- It ties our encoder to undocumented driver escapes (0xE2/0xE3, ABI version 2) that can change between releases.
  Check the version and fall back to the CPU path.
