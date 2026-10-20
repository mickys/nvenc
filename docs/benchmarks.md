# Benchmarks

Every encoder and decoder of this project against what else a Mac has: Apple's VideoToolbox codecs (software on this
Mac), NullMoth's own NVIDIA decoders, and ffmpeg's software codecs (x264, x265, libaom, dav1d, libvpx, its own MPEG
decoders). The tables below are from the full run of 2026-10-20 (nvenc `e29f761`: T7.06 to T7.08, 300 frames at 4K); raw results are in
`docs/bench/` (`2026-10-20-rerun.txt`; the first baseline: `2026-10-20-{enc,dec,lat-8k}.txt`). This is a living document: new measurements and findings go into the log right below, newest first, and the
tables are replaced when a full run is repeated (see "Updating this document" at the end).

## Findings log

Newest first. One entry per finding or measurement: date, what changed or was found, numbers, and the raw result file.

- **2026-10-20, rate control (T7.09):** NVIDIA's two-pass and lookahead traced and tried. Two-pass: -0.25..+0.65 VMAF at equal rate
  over 900 frames for 2x (H.264) / +7 % (HEVC) engine time, off by default (`NVENCVT_TWOPASS=1`). The real gain came from
  initializing the RC process buffer as the driver does (ours was zero, so the first IDR took ~37 frames' budget): +0.6-1.0 VMAF
  at equal rate over 900 frames and the rate on target (7.21 for 7.2 Mbit/s instead of 7.44); VBR, broken by T7.08 (it ran at
  its maximum), fixed. Lookahead: the engine ignores its hints; scene-cut IDRs lowered VMAF. Raw: `docs/bench/2026-10-20-rc-matrix.txt`.
- **2026-10-20, full rerun** (`docs/bench/2026-10-20-rerun.txt`, nvenc `e29f761`; tables above): after the frame-rate fix
  (T7.05), the HEVC p7 controls and NVIDIA's CBR fields (T7.08), 300 frames at 4K. **HEVC caught up with x265:** 1080p crowd_run /
  park_joy 62.1 / 65.1 VMAF (baseline 56.8 / 59.2 at 8-10 % under the target; x265 fast 61.6 / 65.7), 4K 72.2 / 72.7 (baseline
  65.0 / 65.8, 120 frames; x265 fast 72.1 / 74.1), with ~8 % more bits than x265. **H.264** with B pictures 59.7 / 63.1 at 1080p
  (baseline 54.0 / 58.0), at or above x264 medium. AV1 unchanged in quality, still the best row everywhere. With B pictures our rate is 2-9 % over the target on 6-second
  clips (real time within 1 %). Decoders, latency and 8K within a few % of the baseline.
- **2026-10-20, HEVC quality from NVIDIA's presets (T7.08, third Linux-driver trace):** quality mode now uses NVIDIA's p7 controls
  with 8x8 coding units, and CBR uses NVIDIA's rate control fields. crowd_run / park_joy 1080p, 6 Mbit/s CBR, 2 B pictures:
  57.1 -> 58.4 / 59.5 -> 61.4 VMAF (encodeN). Quality mode costs 5.6 ms per 1080p frame (22 ms at 4K); libnvencvt enables it
  only where it keeps up with the frame rate. Spatial AQ, now done exactly as the driver does it, still lowers VMAF here (off).
- **2026-10-20, HEVC quality (T7.07):** with the frame-rate fix, ours vs x265 fast at 1080p / 6 Mbit/s: crowd_run 61.2 VMAF at
  6.50 Mbit/s vs 61.6 at 5.96, park_joy 63.7 at 6.52 vs 65.7 at 6.03 (ours 2 B pictures). Mode-decision knobs, block sizes and
  the rate control's I-frame budget give nothing; spatial AQ through the engine's delta-QP map works but lowers VMAF on these
  clips. What is left is NVIDIA's p7 / lookahead settings (not traced yet).
- **2026-10-20, frame rate through ffmpeg (fixed, libnvencvt):** ffmpeg sets no `ExpectedFrameRate`, so a 50 fps stream got
  the per-frame bit budget of 60 fps: our real-time encodes in the baseline came out at 5.0 Mbit/s for a 6 Mbit/s target (the
  B-mode ones ~5.5). libnvencvt now takes the rate from the first frame's duration or the first two time stamps: 6.00 Mbit/s
  (real time) / 6.23-6.25 (2 B, one-second buffer) over 12 s. The baseline's H.264 / HEVC rows through ffmpeg are therefore at
  ~8-17 % below the target rate; the next full run fixes that.
- **2026-10-20, rate over longer runs:** the 4K "overshoot" (AV1 +10 %) and "undershoot" (HEVC -10 %) of the baseline are the
  120-frame clips (2.4 s against a one-second buffer). Over 20 s (`vtencfile`, crowd_run): all codecs within -2.5 / +2.6 % of
  the target at 1080p and 4K, real time or not. The next full run uses 300 frames at 4K too.
- **2026-10-20, baseline run** (`docs/bench/2026-10-20-{enc,dec,lat-8k}.txt`): every encoder and decoder on crowd_run / park_joy
  at 1080p and 4K, real-time latency, 8K. Summary: our AV1 is the best quality at every size (VMAF 3-6 above x265 medium) at
  6-16x x265's speed; H.264 between x264 veryfast and medium (1080p), at or above medium (4K); HEVC 5-8 VMAF behind x265.
  Decoders 4-5x NullMoth's for H.264, on par for HEVC, 10x dav1d for AV1.
- **2026-10-20, MPEG-4 Part 2 above 1920 wide:** pictures correct (55-57 dB) but every macroblock flagged and ~1.3 us per
  macroblock (4K: 24 fps). Buffer sizes are not the cause. Open (docs/tasks.md T14.02).
- **2026-10-20, uninjected ffmpeg HEVC was undecodable** (Apple's parameter sets with our slices; fixed in T7.06): found when the
  "Apple HEVC" row came out at our speed with broken files.
- **2026-10-20, VM stalls:** single encodes sometimes stall for seconds (seen as 4-10x slower runs at random); every encode now
  runs twice and the faster time counts.
- **2026-10-20, 10-bit decode output:** asking VideoToolbox for 8-bit output from a 10-bit stream adds a CPU conversion (about 5x
  slower at 1080p); the decode benchmark asks for `x420`.

## Setup

- **Machine:** macOS 26.7.1 (Tahoe) as a KVM guest on an AMD Ryzen 9 3950X (8 cores / 16 threads given to the guest, 32 GB),
  with a GeForce RTX 4090 passed through (NullMoth's driver 2.3.7, PCIe 4.0 x8). Software encoders and decoders run on those
  16 threads; ours run on NVENC / NVDEC with a little CPU for the frame copy.
- **Sources:** the first 59-60 frames of Xiph's `crowd_run` and `park_joy` (SVT test set; 1080p50 and 2160p50, both hard to
  code), looped to 300 frames. Every encoder gets the same frames and the same target rate (6 Mbit/s at 1080p,
  20 Mbit/s at 4K), with a 250-frame key interval.
- **Encoders:** ffmpeg 9.0.2 drives every one except our AV1 (ffmpeg has no VideoToolbox AV1 encoder: `tools/vtencfile`). "ours"
  is libnvencvt through VideoToolbox, as an app or `nvenc-run ffmpeg` gets it. "real time" = `RealTime` on (P pictures only, fast
  controls, the mode Parsec and streaming use); without it: 2 B pictures and NVIDIA's quality controls (recordings, exports).
  **fps** = frames / wall time of the whole command (reading the raw frames, encoding, writing the file), the better of two runs.
  Quality: luma PSNR and VMAF (libvmaf, default model) of the decoded file against the source frames.
- **Decoders:** files made by the software encoders at realistic rates (H.264 10 / 40 Mbit/s at 1080p / 4K, the others a bit
  less), 300 frames. Ours, NullMoth's and Apple's through VTDecompressionSession (`tools/vtdecode VTDECODE_SPEED`, frames released
  at once); ffmpeg's with 16 threads (wall time). VP8 has no VideoToolbox consumer: our CLI decoder's engine time.

## Encode

#### crowd_run 1920x1080, 300 frames, target 6 Mbit/s

| encoder | fps | Mbit/s | PSNR-Y dB | VMAF |
|---|---:|---:|---:|---:|
| ours H.264, real time (P only) | 308 | 6.01 | 26.61 | 51.0 |
| ours H.264 (2 B, quality) | 284 | 6.47 | 27.36 | 59.7 |
| ours HEVC, real time | 217 | 6.00 | 27.14 | 58.7 |
| ours HEVC (2 B) | 130 | 6.48 | 27.69 | 62.1 |
| ours HEVC Main 10 (2 B) | 128 | 6.47 | 27.67 | 63.0 |
| ours AV1, real time | 240 | 5.88 | 27.37 | 60.6 |
| ours AV1 (2 B, quality) | 258 | 6.11 | 28.09 | 65.2 |
| ours AV1 10-bit (2 B, quality) | 231 | 6.11 | 28.10 | 65.2 |
| Apple H.264 (software, VideoToolbox) | 50 | 6.27 | 25.74 | 47.0 |
| Apple HEVC (software, VideoToolbox) | 7 | 6.58 | 27.57 | 61.8 |
| x264 veryfast | 183 | 6.13 | 26.27 | 50.1 |
| x264 medium | 83 | 6.14 | 26.55 | 57.6 |
| x265 fast | 47 | 5.96 | 27.72 | 61.6 |
| x265 medium | 44 | 5.93 | 27.55 | 60.9 |
| libaom AV1 realtime, cpu-used 8 | 18 | 6.38 | 27.33 | 62.4 |

#### park_joy 1920x1080, 300 frames, target 6 Mbit/s

| encoder | fps | Mbit/s | PSNR-Y dB | VMAF |
|---|---:|---:|---:|---:|
| ours H.264, real time (P only) | 294 | 5.92 | 26.84 | 54.6 |
| ours H.264 (2 B, quality) | 282 | 6.40 | 27.46 | 63.1 |
| ours HEVC, real time | 205 | 5.91 | 27.22 | 60.5 |
| ours HEVC (2 B) | 129 | 6.50 | 27.91 | 65.1 |
| ours HEVC Main 10 (2 B) | 132 | 6.49 | 27.93 | 66.2 |
| ours AV1, real time | 259 | 5.82 | 27.58 | 63.8 |
| ours AV1 (2 B, quality) | 262 | 6.13 | 28.27 | 68.8 |
| ours AV1 10-bit (2 B, quality) | 236 | 6.13 | 28.26 | 68.8 |
| Apple H.264 (software, VideoToolbox) | 49 | 6.23 | 25.53 | 45.8 |
| Apple HEVC (software, VideoToolbox) | 6 | 6.42 | 27.52 | 62.8 |
| x264 veryfast | 181 | 6.16 | 26.93 | 57.6 |
| x264 medium | 79 | 6.18 | 26.83 | 63.4 |
| x265 fast | 38 | 6.03 | 27.98 | 65.7 |
| x265 medium | 34 | 6.04 | 27.92 | 65.8 |
| libaom AV1 realtime, cpu-used 8 | 22 | 6.12 | 27.33 | 64.6 |

#### crowd_run 3840x2160, 300 frames, target 20 Mbit/s

| encoder | fps | Mbit/s | PSNR-Y dB | VMAF |
|---|---:|---:|---:|---:|
| ours H.264, real time (P only) | 114 | 20.06 | 30.85 | 63.6 |
| ours H.264 (2 B, quality) | 88 | 21.79 | 31.44 | 69.5 |
| ours HEVC, real time | 98 | 20.07 | 31.54 | 71.4 |
| ours HEVC (2 B) | 125 | 21.49 | 31.89 | 72.2 |
| ours HEVC Main 10 (2 B) | 93 | 21.44 | 31.87 | 72.7 |
| ours AV1, real time | 69 | 19.91 | 31.94 | 73.1 |
| ours AV1 (2 B, quality) | 111 | 21.41 | 32.45 | 76.1 |
| ours AV1 10-bit (2 B, quality) | 93 | 21.43 | 32.47 | 76.2 |
| Apple H.264 (software, VideoToolbox) | 14 | 20.84 | 29.88 | 58.1 |
| Apple HEVC (software, VideoToolbox) | 3 | 21.34 | 31.73 | 70.4 |
| x264 veryfast | 49 | 20.59 | 29.20 | 52.6 |
| x264 medium | 19 | 20.96 | 29.77 | 59.7 |
| x265 fast | 18 | 19.87 | 31.92 | 72.1 |
| x265 medium | 10 | 19.90 | 31.91 | 72.2 |
| libaom AV1 realtime, cpu-used 8 | 10 | 20.56 | 31.61 | 71.6 |

#### park_joy 3840x2160, 300 frames, target 20 Mbit/s

| encoder | fps | Mbit/s | PSNR-Y dB | VMAF |
|---|---:|---:|---:|---:|
| ours H.264, real time (P only) | 77 | 19.88 | 30.83 | 62.9 |
| ours H.264 (2 B, quality) | 87 | 21.78 | 31.29 | 69.3 |
| ours HEVC, real time | 97 | 19.83 | 31.57 | 71.3 |
| ours HEVC (2 B) | 125 | 21.66 | 31.97 | 72.7 |
| ours HEVC Main 10 (2 B) | 91 | 21.68 | 31.96 | 73.1 |
| ours AV1, real time | 76 | 19.84 | 32.23 | 75.4 |
| ours AV1 (2 B, quality) | 105 | 21.17 | 32.63 | 77.8 |
| ours AV1 10-bit (2 B, quality) | 91 | 21.15 | 32.63 | 77.8 |
| Apple H.264 (software, VideoToolbox) | 16 | 20.85 | 29.54 | 54.8 |
| Apple HEVC (software, VideoToolbox) | 2 | 21.21 | 31.72 | 70.1 |
| x264 veryfast | 45 | 20.83 | 30.08 | 59.1 |
| x264 medium | 18 | 21.03 | 30.18 | 64.7 |
| x265 fast | 16 | 20.42 | 32.13 | 74.1 |
| x265 medium | 12 | 20.44 | 32.12 | 74.8 |
| libaom AV1 realtime, cpu-used 8 | 10 | 20.00 | 31.67 | 72.7 |

What this shows:
- **Speed:** our encoders run at 130-310 fps at 1080p and 70-125 fps at 4K end to end (the engine alone: 1.1-1.2 ms per 1080p
  frame, 3.7-4.5 ms per 4K frame in real time mode, see the latency table): 1.5-2.5x x264 veryfast at 4K and 3-12x x265,
  libaom and Apple's software encoders. HEVC with B pictures at 1080p (~130 fps) runs NVIDIA's p7 controls with 8x8 coding
  units (5.6 ms per frame); at 4K 50 fps libnvencvt leaves them off because they would not keep up (22 ms per frame).
- **AV1 (not real time) is the best quality in every row:** VMAF 65-78, 3-4 points above x265 medium and 3-5 above libaom
  realtime, at 10x their speed.
- **HEVC** with B pictures is now level with x265 on VMAF: crowd_run 62.1 vs x265 fast 61.6 (1080p) and 72.2 vs 72.1 (4K),
  park_joy 65.1 vs 65.7 and 72.7 vs 74.1, but with ~8 % more bits than x265 (see the rate note). Real time (P only) is on target
  and 1.5-5 VMAF lower. Main 10 adds 0.4-1.1 VMAF over 8-bit at the same rate.
- **H.264** with B pictures and the quality controls is at or above x264 medium at 1080p (59.7 / 63.1 vs 57.6 / 63.4) and far
  above it at 4K (69.4 vs 59.7 / 64.7), at 3.5x its speed (1080p) and 5x (4K).
- **Rate:** real-time encodes hit the target within 1 % (one-frame-period VBV). With B pictures (one-second VBV) ours come out 2-9 %
  above it on these 6-second clips (AV1 2-7 %, HEVC and H.264 7-9 %); over 20 s they stay within 3 % (findings log, "rate over
  longer runs"). x264 / x265 stay within 2-5 %.
- Apple's VideoToolbox encoders run in software on this Mac (no Apple video hardware in a VM): 50 fps H.264, 6-7 fps HEVC at 1080p.

## Decode

#### Decode 1920x1080 (frames per second)

| codec | ours (NVDEC, VideoToolbox) | ours (NVDEC, engine time) | NullMoth's driver | Apple software (VideoToolbox) | ffmpeg software, 16 threads |
|---|---:|---:|---:|---:|---:|
| H.264 | 670 | - | 159 | 85 | 649 |
| HEVC | 950 | - | 844 | 322 | 278 |
| HEVC Main 10 | 630 | - | 568 | 291 | 250 |
| VP9 | 990 | - | - | - | 343 |
| AV1 | 948 | - | - | - | 91 |
| MPEG-2 | 728 | - | - | - | 1107 |
| MPEG-4 Part 2 | 722 | - | - | - | 1027 |
| VP8 | - | 1031 | - | - | 843 |

#### Decode 3840x2160 (frames per second)

| codec | ours (NVDEC, VideoToolbox) | ours (NVDEC, engine time) | NullMoth's driver | Apple software (VideoToolbox) | ffmpeg software, 16 threads |
|---|---:|---:|---:|---:|---:|
| H.264 | 188 | - | 40 | 24 | 168 |
| HEVC | 309 | - | 291 | 190 | 156 |
| HEVC Main 10 | 173 | - | 167 | 148 | 123 |
| VP9 | 303 | - | - | - | 163 |
| AV1 | 307 | - | - | - | 28 |
| MPEG-2 | 207 | - | - | - | 505 |
| MPEG-4 Part 2 | 25 | - | - | - | 369 |
| VP8 | - | 251 | - | - | 361 |

What this shows:
- **H.264:** 4-5x NullMoth's driver decoder and 8x Apple's software decoder; about ffmpeg's 16-thread speed at 1080p, faster at 4K.
- **HEVC 8 / 10-bit:** on par with NullMoth's decoder (same hardware; ours 4-12 % faster in this run); 1.2-3x Apple's software
  decoder.
- **VP9 and AV1:** only ours in hardware (VideoToolbox on an Intel Mac has neither); AV1 is 10-11x dav1d with 16 threads.
- **MPEG-2 / MPEG-4 / VP8:** ffmpeg's software decoders are faster on 16 threads (these codecs are cheap); ours mainly frees the
  CPU (and Apple's decoder refuses MPEG-4 ASP). Our VideoToolbox rate for them is bound by copying each picture out (~1 ms at 4K).
- **MPEG-4 Part 2 above 1920 pixels wide:** the engine still decodes correctly (55-57 dB against ffmpeg with the same IDCT) but
  flags every macroblock and takes ~1.3 us per macroblock (4K: 25 fps). NVIDIA documents MPEG-4 Part 2 decoding up to
  2032x2032; such streams are rare (ASP's highest level is 720x576).
- 10-bit decodes are measured with 10-bit output (`x420`); asking for 8-bit output adds a CPU conversion in VideoToolbox (about
  5x slower at 1080p).

## Real-time latency

#### Real-time encode latency (encodeFrame to encoded frame, libnvencvt statistics)

| codec | size | latency ms | engine ms |
|---|---|---:|---:|
| H.264 | 1920x1080 | 1.85 | 1.19 |
| HEVC | 1920x1080 | 1.93 | 1.23 |
| AV1 | 1920x1080 | 1.72 | 1.05 |
| H.264 | 3840x2160 | 5.67 | 4.34 |
| HEVC | 3840x2160 | 5.84 | 4.50 |
| AV1 | 3840x2160 | 5.00 | 3.71 |

From `VTCompressionSessionEncodeFrame` to the encoded frame in the app's callback, real time (2-frame VBV, P only); the engine
part is the hardware time. AV1 is the fastest of the three on Ada.

## 8K (7680x4320)

| codec | encode ms / frame (engine) | decode fps (VideoToolbox) |
|---|---:|---:|
| AV1 | 14.3 | 64 |
| HEVC | 16.1 | 66 |
| H.264 | 15.3 | 11 (NullMoth's: NVDEC decodes H.264 up to 4096 wide, so ours declines) |

The 4K `crowd_run` frames scaled up; encoded with `tools/encodeN` (AV1 with 4 tiles), decoded through VideoToolbox.

## Examples

All of these run on macOS with the overlay installed (`libnvencvt.dylib` in the NVIDIA driver's bundle, `NVMTL_NVENCVT`
set). GUI apps get the encoders and decoders automatically once they create a Metal device; command-line tools go through
`nvenc-run`, which loads the library into the tool (`DYLD_INSERT_LIBRARIES`).

### Encode with ffmpeg

```sh
# H.264 High, 2 B pictures, 8 Mbit/s (not real time: NVIDIA's quality controls)
nvenc-run ffmpeg -i in.mov -c:v h264_videotoolbox -b:v 8M -bf 2 -g 250 out.mp4

# HEVC Main, same; -realtime 1 -bf 0 for streaming (P only, fast, ~2 ms per 1080p frame)
nvenc-run ffmpeg -i in.mov -c:v hevc_videotoolbox -b:v 6M -bf 2 -g 250 -tag:v hvc1 out.mp4

# HEVC Main 10, HDR10 (BT.2020 PQ). Tag the frames (setparams) if the source does not carry the colour itself
nvenc-run ffmpeg -i in.mov -vf setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc,format=p010le \
    -c:v hevc_videotoolbox -profile:v main10 -b:v 20M -bf 2 -tag:v hvc1 out.mp4

# capped VBR: average 15 Mbit/s, at most 30 (VideoToolbox DataRateLimits)
nvenc-run ffmpeg -i in.mov -c:v hevc_videotoolbox -b:v 15M -maxrate 30M -bufsize 30M -tag:v hvc1 out.mp4
```

Without `nvenc-run`, ffmpeg gets Apple's software encoders (they are always correct, just slower).

### Encode AV1

ffmpeg has no VideoToolbox AV1 encoder and AVAssetWriter no AV1 codec type, so AV1 goes through VTCompressionSession:

```swift
var session: VTCompressionSession?
VTCompressionSessionCreate(allocator: nil, width: 1920, height: 1080, codecType: kCMVideoCodecType_AV1,
                           encoderSpecification: nil, imageBufferAttributes: nil, compressedDataAllocator: nil,
                           outputCallback: nil, refcon: nil, compressionSessionOut: &session)
VTSessionSetProperty(session!, key: kVTCompressionPropertyKey_AverageBitRate, value: 6_000_000 as CFNumber)
VTSessionSetProperty(session!, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanFalse)   // B pictures + quality
// VTCompressionSessionEncodeFrame(...) per frame; samples are AV1 temporal units in display order (av1C in the format description)
```

The repository's `tools/vtencfile` does the same from a raw NV12 file and writes IVF:
`nvenc-run vtencfile av01 1920 1080 600 out.ivf /dev/null` with `VT_SRC=in.nv12 VT_BITRATE=6000000 VT_REORDER=1`.

### OBS Studio

Settings > Output > Recording (Advanced): Video Encoder "NVENC - H.264 / HEVC - (NVIDIA RTX 4090) - mickys/nvenc - <version>"
(OBS lists them as "Dynamically Registered ... Video Encoder" in some versions). Rate control CBR / VBR / CRF-like quality,
B-frames on for recordings (off for streaming keeps the latency at one frame).

### Decode

Nothing to set up: AVFoundation (QuickTime Player, Safari's media stack, AVAssetReader) and VTDecompressionSession pick the
NVDEC decoders for H.264, HEVC (8-12 bit, 4:2:0 / 4:4:4), VP9 (8-12 bit), AV1, MPEG-1/2 and MPEG-4 Part 2. That includes
`hev1` / `avc3` MP4 files (ffmpeg writes `hev1` for HEVC by default), which macOS cannot decode at all on its own.

```sh
nvenc-run ffmpeg -hwaccel videotoolbox -i in.mkv -f null -      # ffmpeg through VideoToolbox (our decoders)
```

## Updating this document

- **A new finding or a single measurement:** add a dated entry at the top of the findings log (what, numbers, how measured); put
  any raw output in `docs/bench/<date>-<topic>.txt`.
- **A full rerun** (new driver, new encoder version, other hardware): run `bench-all.sh` as below, save the output as
  `docs/bench/<date>-<part>.txt`, regenerate the tables with `scripts/bench-table.py` and replace the tables and the "What this
  shows" notes; keep the old raw files and say in the log entry what moved.
- Keep the setup section true for the run the tables come from (machine, driver, commits, sources, rates).

## Reproduce

In the guest, after `./build.sh nvencvt vtencfile vtdecode encodeN vp8decodeN`, with the sources as NV12 in `~/vt-streams/natural`
(`ffmpeg -i crowd_run_1080p50.y4m -frames:v 60 -pix_fmt nv12 -f rawvideo crowd60.nv12`, and so on):

```sh
bash scripts/guest/bench-all.sh enc dec lat 8k > results.txt    # about 1.5 hours
python3 scripts/bench-table.py results.txt                        # the tables above
python3 scripts/bench-table.py docs/bench/2026-10-20-*.txt        # the current tables from the stored results
```

`scripts/guest/bench-enc.sh` / `bench-dec.sh` run one source with chosen encoders / codecs.
