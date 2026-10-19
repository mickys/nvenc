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
| T2.05 | Clear the engine error: RC error 47 (`ROBUST_CHANNEL_NVENC0_ERROR`) with `error_status` 1 in the status buffer | the semaphore is released, no error notifier, `error_status` and `ucode_error_status` 0 | done |
| T2.05a | Tools to see engine errors: RM's RC error log (`tools/rcerr`, root) decoded with `scripts/prbdecode.py` | the RC record of our run is readable | done |
| T2.05b | Bisect the methods: which buffer method turns a clean microcode error into RC 47 | the method is known | done |
| T2.05c | Bisect the picture setup contents (input surface config, recon config, ME/MD/quant controls, RC) with the engine encoding | a setup that encodes without error | done (via T2.05d) |
| T2.05d | Observe a working client (nullmoth's library) with an RM/IOKit interposer (`trace/nvtrace.c`), replay its picture setup with our buffers, measure the input layout | the replayed setup encodes and decodes ≥ 40 dB | done |
| T2.05e | Bisect our setup against the reference: which fields/methods are needed for our own setup to encode | `encode1` without `NVENC_REPLAY` encodes ≥ 40 dB | done |
| T2.06 | Bitstream comes out: `total_bit_count` > 0, slice NAL(s) with the expected header in the bitstream buffer | `h264check` lists SPS, PPS, IDR slice | done (T2.05d/e: SPS, PPS, IDR slice) |
| T2.07 | Our SPS/PPS match the slice header the engine wrote (frame_num/POC bits, entropy, deblocking flag) | Apple's decoder accepts the stream | done (Apple decodes; POC type 2 added) |
| T2.08 | Input layout right: block-linear NV12 as the engine reads it (flat-colour and gradient tests if the picture is scrambled) | PSNR ≥ 40 dB at QP 26 | done (layout measured in T2.05d; other sizes in T2.09) |
| T2.09 | Sizes: 256x128, 1920x1080 (cropped MB row), 3840x2160, 3840x2025 (odd height) | all decode at ≥ 40 dB | done |
| T2.10 | Clean up `encode1` (keep only knobs that still matter), M2 notes in `design.md`, commit | committed, README status updated | done |

## M3: P-frames, rate control, slices

| ID | Task | Done when | Status |
|---|---|---|---|
| T3.01 | Move the single-frame code into a library: `src/enc.c` (session: buffers, channel, picture setup) + `tools/encode1` on top of it | `encode1` gives the same result through `enc.c` | done |
| T3.02 | Two reconstructed pictures, ping-pong: frame n writes one, frame n+1 references it (`SET_IN_REF_PIC*`) | a P-frame encodes without engine errors | done |
| T3.03 | Frame numbering: `frame_num`, POC LSB, `l0` list, `pic_type`, `ref_pic_flag`, IDR only on frame 0 | a 2-frame IDR+P stream decodes | done |
| T3.04 | Per-frame state buffers: history and colocated data carried from frame to frame | no drift over 60 frames | done |
| T3.05 | `tools/encodeN`: N frames of moving synthetic content at constant QP; `h264check` decodes all frames, per-frame PSNR | 60 frames decode, PSNR ≥ 40 dB each | done |
| T3.06 | CBR: RC structure + `IN_RCDATA`/`IO_RC_PROCESS` buffers; measure the real bitrate | within ±10 % of the target over 60 frames | done |
| T3.07 | VBR (target + max bitrate) | average near target, peaks under max | done |
| T3.08 | Low-latency settings: no B-frames, infinite GOP, IDR on request, small VBV (one frame) | per-frame size stays near bitrate/fps | done |
| T3.09 | Multiple slices (fixed MB count per slice) | stream with N slices decodes | done |
| T3.10 | M3 notes in `design.md`, commit | committed | done |

## M4: VideoToolbox encoder plugin

| ID | Task | Done when | Status |
|---|---|---|---|
| T4.01 | Collect the public interface pieces: CMBaseObject + decoder SPI from WebKit, how the patched Metal plugin registers nullmoth's encoder factory | notes in `docs/vt-encoder.md` | done |
| T4.02 | Stub encoder: a factory that returns an object whose every vtable slot logs its index and arguments and fails cleanly | the stub registers and VideoToolbox picks it for `vttest` | done |
| T4.03 | Black-box probe: record the slot order and arguments for session create, property queries, encode, flush, invalidate | slot map in `docs/vt-encoder.md` | done |
| T4.04 | Session start + supported properties dictionary (bitrate, real time, profile/level, keyframe interval, allow frame reordering = no) | VideoToolbox creates a session on our encoder | done |
| T4.05 | Encode frame: CVPixelBuffer (NV12/BGRA) -> CPU tiling into the input surface -> `enc.c` | the engine encodes frames from VideoToolbox | done |
| T4.06 | Output: CMSampleBuffer with avcC format description (our SPS/PPS), AVCC length prefixes, keyframe attachments | `vttest` decodes the output | done |
| T4.07 | Flush/complete frames, invalidate, error paths | clean session teardown, no leaks over 1000 sessions | done |
| T4.08 | Register through the Metal plugin instead of nullmoth's factory, per process (existing knob) | Parsec and ffmpeg `h264_videotoolbox` use it | done (vttest through the plugin; Parsec waits for a parsecd restart) |
| T4.09 | M4 notes, commit | committed | done |

## M5: input path and completion (revised 2026-10-18 after T5.01)

Findings that changed the plan: NVENC on Ada reads only its own input layout (64x16 tiles of 16x4 sub-blocks, T2.05d);
`input_bl_mode` is ignored, and neither the classic GOB layout (what the copy engine writes) nor pitch-linear input
decodes correctly, with any block_height. A copy-engine pass from the IOSurface therefore can't feed NVENC directly; a
GPU path needs a compute pass (Metal, in the plugin's RM client) and cross-client memory sharing. The CPU path comes
first.

| ID | Task | Done when | Status |
|---|---|---|---|
| T5.01 | Measure the per-frame cost: CPU tiling vs engine (`tools/tilebench`) | numbers for 1080p and 4K | done |
| T5.02 | Fast tiling: SSE/AVX2 tile writes (64x16 tiles), optionally split over threads | 4K tiling ≤ 1 ms | done |
| T5.03 | Asynchronous encode: submit frame n and return; emit when the engine is done (output thread), so tiling of n+1 overlaps encoding of n | throughput limited by the engine, not CPU + engine | done |
| T5.04 | Event-driven completion: RM event / semaphore wake instead of spinning | encode thread sleeps while the engine works | done |
| T5.05 | Measure Parsec 4K: time per frame inside our encoder (needs a parsecd restart: user) | well under ~9 ms | done (Parsec L 4.7-4.8 ms, was ~10.5-12 ms with nullmoth's) |
| T5.06 | GPU swizzle (later): Metal compute pass from the IOSurface into NVENC's layout, memory shared with our RM client | no CPU pass over the frame | measured, not adopted (GPU 1.9 ms vs CPU 0.87 ms at 4K: surfaces are in system RAM) |
| T5.07 | M5 notes, commit | committed | done |

## M6: two sessions, HEVC

| ID | Task | Done when | Status |
|---|---|---|---|
| T6.01 | Session placement: NVENC0 and NVENC1, two sessions in one process | two streams encode at the same time | done |
| T6.02 | HEVC structures: `nvenc_h265_drv_pic_setup_s` (1024 B) and the H.265 ME/MD controls in `nvenc_drv.h`, compared with the H.264 ones we use | field map in the report | done |
| T6.03 | Observe nullmoth's `libnvenc_hevc` at runtime (option B: RM escapes, pushed methods, buffers): `tools/hevcref.m` registers its `NVEncHEVC_CreateInstance` in-process, one 256x128 IDR under `trace/nvtrace` | picture setup + method trace in `docs/traces/` | done |
| T6.04 | HEVC VPS/SPS/PPS writer (`src/hevchdr.c`) | Apple's `CMVideoFormatDescriptionCreateFromHEVCParameterSets` accepts them | done |
| T6.05 | One HEVC IDR from our own picture setup: codec switch in `enc.c` (application ID, codec type, H.265 setup), `encode1` HEVC mode at 256x128 | decodes ≥ 40 dB | done |
| T6.06 | HEVC IDR at 720p, 1080p, 3840x2160 (CTB alignment, recon layout) | all decode ≥ 40 dB | done |
| T6.07 | HEVC P-frames (1 reference, short-term RPS) | 60-frame stream decodes, no errors | done (1 reference) |
| T6.08 | HEVC rate control (CBR/VBR/low-latency, reuse M3) | bitrate within 10 % of target | done |
| T6.09 | HEVC through VideoToolbox: `hvc1` in `libnvencvt`; `vttest` HEVC; Parsec with HEVC | encoder used, decodes | done |
| T6.10 | M6 notes, commit, overlay update | committed | done |
| T6.11 | HEVC with 2-3 reference frames (T6.07 open point) | multi-reference streams decode | done |
| T6.12 | Ghosting on a hardware HEVC decoder (Parsec client, AMD Polaris) with 3 references | no ghost on that client | done: 2 spare DPB pictures |
| T6.13 | libnvencvt for every app: cheap GPU check, declare/refuse unsupported profiles; `tools/avwrite` | AVAssetWriter H.264/HEVC 360p-4K through our encoder | done |
| T6.14 | HEVC Main 10: 10-bit setup from nullmoth's (tracer fixed for 2.3.6), P010 and l10r input | Parsec 10-bit through our encoder | done: L 7.0-7.3 ms at 4K |

## M7: encoder quality (2026-10-18)

| Task | What | Done when | Status |
|---|---|---|---|
| T7.00 | Registration without a Metal device (CLI apps like ffmpeg get -12908): VT plug-in bundle or another hook | `ffmpeg -c:v hevc_videotoolbox` uses our encoder without injection | done another way: no supported registration exists (reports/T7.00-cli-and-extensions.md, T7.03-coremedia-codesign-summary.md); CLI tools use the overlay's `nvenc-run` wrapper (injection), released in v2.3.6-mickys.9 (names: reports/T7.00-names.md) |
| T7.01 | Full-range input (420f/xf20) signalled in the VUI (H.264 and HEVC) | Parsec's 420f stream decodes to the same levels as the source | done (ffmpeg, OBS) |
| T7.02 | H.264 High profile, CABAC, 8x8 transform (observe nullmoth's libnvenc_h264) | ~10 % less bitrate at equal PSNR; decodes | done: 4K 32 -> 49 dB at 20 Mbit/s |
| T7.03 | B-frames when RealTime is off (AllowFrameReordering): reordering, POC, DPB, output order | OBS/AVAssetWriter recordings with B-frames decode | open |
| T7.04 | HEVC 4:4:4 (Range Extensions) | a 4:4:4 stream decodes | moved to M13 (T13.01) |

## M8: H.264 hardware decoder (2026-10-19)

Why: nullmoth's driver registers its H.264 decoder (`libnvdec_h264_t2d` on cards before Blackwell) in every process that
uses Metal, and it decodes correctly, but on an RTX 4090 a 4K H.264 stream decodes at **32 fps** (Apple's software
decoder: 30 fps; their HEVC decoder: 175 fps at 4K). 4K60 H.264 playback is not possible, and command-line tools get no
hardware decoder at all (same registration limit as the encoder). Measured with `tools/vtdecode` (x264/x265 test streams,
hardware vs software output compared: bit-exact). Sources: NVIDIA's open-gpu-doc `classes/video/nvdec_drv.h`
(`nvdec_h264_pic_s`, DPB entries, status) and `clc9b0.h` (Ada NVDEC methods), the H.264 standard. Same ground rules as the
encoder (no disassembly or patching of closed binaries). **User, 2026-10-19: headers only for every decoder** - nullmoth's
decoder libraries are not observed at run time either (unlike the encoder's option B); buffer sizes and engine settings
are found by experiment.

| Task | What | Done when | Status |
|---|---|---|---|
| T8.00 | Measure the gap: hardware vs software H.264/HEVC decode, correctness and speed (`tools/vtdecode`) | numbers recorded | done: H.264 4K 32 fps (sw 30), 1080p 127 (sw 74); HEVC 4K 175 (sw 84); outputs bit-exact except one HEVC 1080p frame (44 dB) |
| T8.01 | A channel on NVDEC with the NVC9B0 object; host + engine semaphores (`tools/dechello`) | both semaphores land | done: NVDEC0 (engine 0x13; RM exposes no other NVDEC on this RTX 4090), both semaphores in 0.011 ms |
| T8.02 | H.264 bitstream reader: SPS, PPS (scaling lists, fall-back rules), slice header to dec_ref_pic_marking (`src/h264parse.c`), POC types 0-2 | parses x264 streams (High, CABAC, B-frames) and our encoder's | done (`tools/h264parsetest`): x264 default, Baseline (POC type 2), JVT scaling matrices, interlaced MBAFF, strict B-pyramid with 16 refs, 4 slices/picture: 0 parse errors, POC order = container timestamp order for every picture; MMCOs parsed (80 pictures). NVDEC parses slice data itself, so no reference-list construction is needed |
| T8.03 | One IDR picture: picture setup from `nvdec_h264_pic_s`, bitstream buffer, output surface, EXECUTE, status | the decoded luma matches the software decoder (PSNR 99 = identical) | done (`tools/decode1`): bit-exact (luma and chroma) for x264 IDRs at 256x128, 1080p, 2048x1152, 1440p, 3072x1728 and 4K, High/CABAC, Baseline/CAVLC, JVT scaling matrices, 8 slices; 1080p 1.15 ms, 4K 4.5 ms. Found by experiment: (1) `tileFormat` 1 / `gob_height` 0 writes the layout already measured for NVENC's input (64x16-byte tiles, `nvenc_in_offset`); (2) the end of the picture must be signalled in `eos[]` with `explicitEOSPresentFlag` (00 00 01 0b), else the last rows are never written and `error_status` is 0x20 (appending it to the bitstream does not help); (3) pictures wider than 1920 need `H264_SET_MBHIST_BUF_OFFSET` + `mbhist_buffer_size`, else the engine reads address 0 (MMU fault, Xid 31, RC record via `tools/rcerr`). Buffer sizes are still generous guesses (history 8 MB, MB history 8 MB, co-located 512 B/MB) |
| T8.04 | P/B pictures: DPB management (sliding window, MMCO), colocated buffers, reference surfaces | a 300-frame x264 stream with B-frames decodes bit-exact | done (`src/dec.c`, `tools/decodeN`): 11 streams bit-exact, every picture: x264 1080p I/B/P (300 frames), strict B-pyramid with 16 refs and MMCOs, temporal direct, 8 slices, Baseline, JVT matrices, open GOP, 4K, MBAFF, plus OpenH264, Apple's VideoToolbox encoder and our NVENC; 4K 4.65 ms/picture on the engine (215 fps), 1080p ~1.2 ms. Found: a reference must keep its `dpb[]` slot for its whole life (the co-located data names references by slot; moving entries broke temporal direct). Not yet: field pictures (PAFF), frame_num gaps (non-existing frames) |
| T8.05 | Output: decoded surface to NV12 (CVPixelBuffer planes) | 4K at ≥ 120 fps end to end | done: SIMD untile `nvenc_in_untile_plane` (inverse of the encoder's tiler) via `h264dec_read_nv12`, bit-exact; 4K 1.22 ms per picture (720p 0.21 ms), so 4.64 ms engine + 1.22 ms out = ~170 fps sequential (overlapping the copy with the next decode: ~215). NVDEC's display DMA (`enableTFOutput`, every `TilingFormat` 0-4) wrote nothing on Ada |
| T8.06 | `libnvencvt` decoder: VideoToolbox H.264 decoder plugin (`vt/nvencvt_dec.c`, interface from WebKit's `VTVideoDecoderSPI.h`), registered by `NVEncVT_Register` next to the encoders (so NullMoth's `NVMTL_NVENCVT` hook and `nvenc-run` bring it), rated 1100 above NullMoth's | AVFoundation and `vtdecode` use it; `nvenc-run ffmpeg -hwaccel videotoolbox` decodes in hardware | done: picked over NullMoth's, 0 errors, output bit-exact vs ffmpeg (VTDecompressionSession: 1080p 348 fps, 4K 106 fps; NullMoth's 127 / 32; Apple software 102 / 29); `ffmpeg -hwaccel videotoolbox` works with it (without it the hwaccel fails); AVAssetReader uses it; `UsingHardwareAcceleratedVideoDecoder` answers only for keys listed in copySupportedPropertyDictionary. Found: Apple's own software H.264 decoder intermittently mis-decodes 16 pixels in one frame of the x264 4K test (ours matches ffmpeg) |
| T8.07 | Why ffmpeg and AVAssetReader re-copy every decoded frame (~20 ms per 4K frame, a Metal transfer through NullMoth's plugin) | explained, avoided where the decoder can | done: (1) AVAssetReader: `alwaysCopiesSampleData` (default YES) makes MediaToolbox copy every frame for any decoder; its conformer calls our buffers conformant and has no attribute requirements (`tools/vtinternals/conformer-spy.c`); with NO, emit 20 -> 0.21 ms and a 4K frame costs 5.8 ms (~170 fps). (2) ffmpeg asks for OpenGL-compatible NV12, which CoreVideo cannot make on this system for any buffer (BGRA it can; `glcompat` test), so VideoToolbox transfers every frame whatever the decoder; `VTDecoderSessionCreatePixelBuffer(session, NULL, &pb)` (shape found by trial) gives an equally incompatible buffer. Faster transfers would need NullMoth's plugin to upload NV12 IOSurfaces on the GPU. Asynchronous decode (overlap) left for later |

## M9-M14: every codec the hardware has (user, 2026-10-19: "we want to add ALL of them")

RTX 4090 (Ada): two NVENC engines (0xc9b7), one NVDEC exposed by RM (0xc9b0), no NVJPG. Picture setups for every codec are
public: `nvdec_drv.h` (H.264, HEVC, VP9, AV1, VP8, MPEG-2, MPEG-4, VC-1) and `nvenc_drv.h` (H.264, HEVC, AV1). The
decoders share M8's pieces: the NVDEC channel and buffers, the output path to CVPixelBuffer (T8.05) and the VideoToolbox
decoder plugin `libnvdecvt` (T8.06), which gains one factory per codec. Order by value: what Macs with NVIDIA cards
lack most first. Each decoder is checked against a software decoder (ffmpeg: libdav1d, libvpx, its own H.264/HEVC/MPEG
decoders) frame by frame, with streams from more than one encoder.

| Milestone | Codec | Tasks (each: done when it decodes/encodes bit-exact or ≥ the stated quality) | Status |
|---|---|---|---|
| M9 | **AV1 decode** (8/10-bit, `av01`) | T9.01 OBU parser (sequence header, frame header, tile info, reference frame state) · T9.02 one key frame through `nvdec_av1_pic_s` · T9.03 inter frames (reference slots, segmentation, loop filter, CDEF, loop restoration, film grain parameters) · T9.04 VideoToolbox `av01` decoder (av1C) + ffmpeg `-hwaccel videotoolbox` · streams from libaom, SVT-AV1, rav1e | blocked (CDF layout, see below) |
| M10 | **VP9 decode** (8/10/12-bit, `vp09`) | T10.01 uncompressed + compressed header parser · T10.02 key frame through `nvdec_vp9_pic_s` · T10.03 inter frames, probability contexts (forward updates, backward adaptation counts), reference refresh, scaled references · T10.04 VideoToolbox `vp09` decoder · streams from libvpx | in progress (first; see below) |
| M11 | **HEVC decode, ours** (Main, Main 10, Main 12, 4:4:4, `hvc1`) | T11.01 VPS/SPS/PPS/slice header reader (beside `hevchdr.c`) · T11.02 IDR · T11.03 P/B with RPS · T11.04 10/12-bit and 4:4:4 · T11.05 VideoToolbox `hvc1` decoder rated above nullmoth's (theirs: one mismatched frame in the x265 1080p test) | planned |
| M12 | **AV1 encode** (8/10-bit) | T12.01 OBU writer (sequence and frame headers) · T12.02 one key frame through `nvenc_av1_drv_pic_setup_s` · T12.03 inter frames, references, rate control (CBR/VBR/low latency) · T12.04 10-bit · T12.05 VideoToolbox `av01` encoder | planned |
| M13 | **4:4:4 encode** + encoder probe | T13.01 HEVC Range Extensions 4:4:4 (was T7.04) · T13.02 H.264 High 4:4:4 Predictive · T13.03 probe whether Ada NVENC accepts the VP8 / VP9 application IDs (not in NVIDIA's Ada support matrix; drop if refused) | planned |
| M14 | **legacy decode** | T14.01 MPEG-2 (`mp2v`) and MPEG-1 · T14.02 MPEG-4 Part 2 (`mp4v`) · T14.03 VC-1 / WMV3 · T14.04 VP8 (`vp08`); VideoToolbox registration only where AVFoundation or ffmpeg can feed the codec | planned |

Not codecs, out of scope: the optical flow accelerator (OFA, 0xc9fa). No JPEG engine on this card.

**Order changed (2026-10-19): VP9 (M10) before AV1 (M9).** With headers only, AV1 is blocked: the engine reads its
symbol probabilities (CDFs) from a buffer whose layout no public header gives (`nvdec_drv.h` has the AV1 picture setup
but no CDF table structure), and NVDEC does not initialise or adapt them itself. VP9's probability tables and counters
are public (`nvdec_vp9EntropyProbs_t`, `nvdec_vp9EntropyCounts_t`), so VP9 goes first; AV1 waits for a layout hypothesis
to test (spec order, padded like VP9's tables).

## M10: VP9 decode

The driver parses both frame headers, keeps the four probability contexts, applies the compressed header's probability
updates and, after each frame, adapts the probabilities from the counts the engine wrote (backward adaptation); NVDEC
decodes the tiles. Source: VP9 Bitstream & Decoding Process Specification v0.7 (`vendor/specs/`, text via pdftotext),
`nvdec_drv.h`, `clc9b0.h`. Streams: `scripts/make-vp9-streams.sh OUTDIR` (libvpx through ffmpeg, with ffmpeg's reference
decodes).

| Task | What | Done when | Status |
|---|---|---|---|
| T10.00 | Default probability tables from the spec text | every table's count matches its declared dimensions | done: `scripts/spec-tables.py vp9 vp9-spec.txt src/vp9tables.h` (strips page furniture and comments, joins wrapped declarations, checks counts; scan tables come out as uint16_t) |
| T10.01 | Parser: uncompressed header, compressed header (boolean decoder, `diff_update_prob`, `decode_term_subexp`, `inv_remap_prob`, MV updates), superframes (Annex B), `setup_past_independence`, `reset_frame_context`, the four saved contexts, refresh_probs + adaptation (8.4) from NVDEC's counter layout | the header fields match ffmpeg's `trace_headers` on every frame of every test stream; each compressed header ends exactly on its zero padding | done (`src/vp9parse.c`, `tools/vp9parsetest`, `scripts/vp9-trace-compare.py`): 11 streams (alt-ref with hidden frames and superframes, 1080p alt-ref, tiles, tile rows, error resilient, frame-parallel, lossless, segmentation, 4K, 10-bit profile 2, 351x287), 0 differences in 10 820 compared fields, every compressed header padding all-zero. The adaptation is untested until T10.03 (needs the engine's counts). Assumed, to check there: NVDEC's coefficient counts are [ZERO, ONE, TWO+, no-more] with `countEobs` = more_coefs reads, its inter-mode counts are the tree's branch counts (ZEROMV, NEARESTMV, NEARMV/NEWMV); `ref_frame_sign_bias` is cleared by `setup_past_independence` also for error-resilient inter frames (7.2, as written) |
| T10.02 | One key frame through `nvdec_vp9_pic_s`: probability table (`nvdec_vp9EntropyProbs_t`), segment, filter, tile-size and counter buffers | bit-exact against ffmpeg | planned |
| T10.03 | Inter frames: reference slots, sign bias / compound, column MVs (previous frame), segmentation map read/write, backward adaptation from the counters, hidden frames, show_existing_frame, resolution changes / scaled references | all test streams bit-exact | planned |
| T10.04 | VideoToolbox `vp09` decoder in `libnvencvt` (vpcC), 10-bit output | AVFoundation / ffmpeg `-hwaccel videotoolbox` decode VP9 with it | planned |
