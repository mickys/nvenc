# Tasks

The milestones of [design.md](design.md), split into small steps. Each step that is finished gets a short report in
[reports/](reports/) (`<id>.md`: what was done, what was found, what changed). Status: `todo`, `doing`, `done`,
`blocked`.

## M2: one H.264 IDR frame

| ID | Task | Done when | Status |
|---|---|---|---|
| T2.01 | Commit the standalone M2 helpers: `blocklinear.c` (block-linear tiling), `h264hdr.c` (SPS/PPS writer), `tools/h264check.m` (decode + PSNR), `.m` support in `build.sh` | committed | done |
| T2.02 | Find which method brings the MMU fault: run `encode1` with `NVENC_VAR=stop=1..4` | the first failing stage is known | done |
| T2.03 | Get the faulting GPU address (driver log / RM fault report) and print a buffer map (name, GPU VA, size) from `encode1` | the fault address matches a buffer, or is known to match none | done |
| T2.04 | Fix the fault (missing buffer, wrong size, alignment or address shift) | no MMU fault during `EXECUTE` | done |
| T2.05 | Clear the engine error: RC error 47 (`ROBUST_CHANNEL_NVENC0_ERROR`) with `error_status` 1 in the status buffer | the semaphore is released, no error notifier, `error_status` and `ucode_error_status` 0 | doing |
| T2.05a | Tools to see engine errors: RM's RC error log (`tools/rcerr`, root) decoded with `scripts/prbdecode.py` | the RC record of our run is readable | done |
| T2.05b | Bisect the methods: which buffer method turns a clean microcode error into RC 47 | the method is known | done |
| T2.05c | Bisect the picture setup contents (input surface config, recon config, ME/MD/quant controls, RC) with the engine encoding | a setup that encodes without error | done (via T2.05d) |
| T2.05d | Observe a working client (nullmoth's library) with an RM/IOKit interposer (`trace/nvtrace.c`), replay its picture setup with our buffers, measure the input layout | the replayed setup encodes and decodes ≥ 40 dB | done |
| T2.05e | Bisect our setup against the reference: which fields/methods are needed for our own setup to encode | `encode1` without `NVENC_REPLAY` encodes ≥ 40 dB | todo |
| T2.06 | Bitstream comes out: `total_bit_count` > 0, slice NAL(s) with the expected header in the bitstream buffer | `h264check` lists SPS, PPS, IDR slice | todo |
| T2.07 | Our SPS/PPS match the slice header the engine wrote (frame_num/POC bits, entropy, deblocking flag) | Apple's decoder accepts the stream | todo |
| T2.08 | Input layout right: block-linear NV12 as the engine reads it (flat-colour and gradient tests if the picture is scrambled) | PSNR ≥ 40 dB at QP 26 | done (layout measured in T2.05d; other sizes in T2.09) |
| T2.09 | Sizes: 256x128, 1920x1080 (cropped MB row), 3840x2160, 3840x2025 (odd height) | all decode at ≥ 40 dB | todo |
| T2.10 | Clean up `encode1` (keep only knobs that still matter), M2 notes in `design.md`, commit | committed, README status updated | todo |

## M3: P-frames, rate control, slices

| ID | Task | Done when | Status |
|---|---|---|---|
| T3.01 | Move the single-frame code into a library: `src/enc.c` (session: buffers, channel, picture setup) + `tools/encode1` on top of it | `encode1` gives the same result through `enc.c` | todo |
| T3.02 | Two reconstructed pictures, ping-pong: frame n writes one, frame n+1 references it (`SET_IN_REF_PIC*`) | a P-frame encodes without engine errors | todo |
| T3.03 | Frame numbering: `frame_num`, POC LSB, `l0` list, `pic_type`, `ref_pic_flag`, IDR only on frame 0 | a 2-frame IDR+P stream decodes | todo |
| T3.04 | Per-frame state buffers: history and colocated data carried from frame to frame | no drift over 60 frames | todo |
| T3.05 | `tools/encodeN`: N frames of moving synthetic content at constant QP; `h264check` decodes all frames, per-frame PSNR | 60 frames decode, PSNR ≥ 40 dB each | todo |
| T3.06 | CBR: RC structure + `IN_RCDATA`/`IO_RC_PROCESS` buffers; measure the real bitrate | within ±10 % of the target over 60 frames | todo |
| T3.07 | VBR (target + max bitrate) | average near target, peaks under max | todo |
| T3.08 | Low-latency settings: no B-frames, infinite GOP, IDR on request, small VBV (one frame) | per-frame size stays near bitrate/fps | todo |
| T3.09 | Multiple slices (fixed MB count per slice) | stream with N slices decodes | todo |
| T3.10 | M3 notes in `design.md`, commit | committed | todo |

## M4: VideoToolbox encoder plugin

| ID | Task | Done when | Status |
|---|---|---|---|
| T4.01 | Collect the public interface pieces: CMBaseObject + decoder SPI from WebKit, how the patched Metal plugin registers nullmoth's encoder factory | notes in `docs/vt-encoder.md` | todo |
| T4.02 | Stub encoder: a factory that returns an object whose every vtable slot logs its index and arguments and fails cleanly | the stub registers and VideoToolbox picks it for `vttest` | todo |
| T4.03 | Black-box probe: record the slot order and arguments for session create, property queries, encode, flush, invalidate | slot map in `docs/vt-encoder.md` | todo |
| T4.04 | Session start + supported properties dictionary (bitrate, real time, profile/level, keyframe interval, allow frame reordering = no) | VideoToolbox creates a session on our encoder | todo |
| T4.05 | Encode frame: CVPixelBuffer (NV12/BGRA) -> CPU tiling into the input surface -> `enc.c` | the engine encodes frames from VideoToolbox | todo |
| T4.06 | Output: CMSampleBuffer with avcC format description (our SPS/PPS), AVCC length prefixes, keyframe attachments | `vttest` decodes the output | todo |
| T4.07 | Flush/complete frames, invalidate, error paths | clean session teardown, no leaks over 1000 sessions | todo |
| T4.08 | Register through the Metal plugin instead of nullmoth's factory, per process (existing knob) | Parsec and ffmpeg `h264_videotoolbox` use it | todo |
| T4.09 | M4 notes, commit | committed | todo |

## M5: GPU-resident input, event-driven completion

| ID | Task | Done when | Status |
|---|---|---|---|
| T5.01 | Get a GPU address for an IOSurface's memory in our RM client (dup the driver's memory object, or import) | NVENC reads a frame from an IOSurface directly | todo |
| T5.02 | Copy-engine channel (`0xc7b5`): pitch-linear -> block-linear copy of luma and chroma | tiled surface matches `bl_tile_plane` byte for byte | todo |
| T5.03 | BGRA input: colour conversion on the GPU (Metal compute) or engine support, decide and implement | BGRA frames encode at ≥ 40 dB | todo |
| T5.04 | Cross-channel ordering: copy channel releases a semaphore, NVENC channel acquires it | no CPU wait between copy and encode | todo |
| T5.05 | Event-driven completion: RM event/notifier on semaphore release instead of spinning | encode thread sleeps while the engine works | todo |
| T5.06 | Measure Parsec 4K: time per frame inside our encoder | well under ~9 ms | todo |
| T5.07 | M5 notes, commit | committed | todo |

## M6: two sessions, HEVC

| ID | Task | Done when | Status |
|---|---|---|---|
| T6.01 | Session placement: NVENC0 and NVENC1, two sessions in one process | two streams encode at the same time | todo |
| T6.02 | HEVC VPS/SPS/PPS writer (`src/hevchdr.c`) | parsed by Apple's decoder | todo |
| T6.03 | HEVC picture setup from `nvenc_drv.h` (IDR only) | one HEVC IDR frame decodes ≥ 40 dB | todo |
| T6.04 | HEVC P-frames and RC (reuse M3) | 60-frame HEVC stream decodes | todo |
| T6.05 | HEVC through VideoToolbox | `vttest` HEVC works | todo |
| T6.06 | M6 notes, commit | committed | todo |
