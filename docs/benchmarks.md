# Benchmarks

Every encoder and decoder of this project against what else a Mac has: Apple's VideoToolbox codecs (software on this
Mac), NullMoth's own NVIDIA decoders, and ffmpeg's software codecs (x264, x265, libaom, dav1d, libvpx, its own MPEG
decoders). The tables below are the current baseline (2026-10-20, nvenc `d19f650` + the T7.06 fix); raw results are in
`docs/bench/`. This is a living document: new measurements and findings go into the log right below, newest first, and the
tables are replaced when a full run is repeated (see "Updating this document" at the end).

## Findings log

Newest first. One entry per finding or measurement: date, what changed or was found, numbers, and the raw result file.

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
  code), looped: 300 frames at 1080p, 120 at 4K. Every encoder gets the same frames and the same target rate (6 Mbit/s at 1080p,
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
| ours H.264, real time (P only) | 328 | 5.01 | 25.95 | 44.9 |
| ours H.264 (2 B, quality) | 277 | 5.53 | 26.66 | 54.0 |
| ours HEVC, real time | 213 | 5.01 | 26.46 | 53.5 |
| ours HEVC (2 B) | 265 | 5.50 | 27.03 | 56.8 |
| ours HEVC Main 10 (2 B) | 228 | 5.50 | 26.97 | 57.4 |
| ours AV1, real time | 270 | 5.88 | 27.37 | 60.6 |
| ours AV1 (2 B, quality) | 285 | 6.11 | 28.09 | 65.2 |
| ours AV1 10-bit (2 B, quality) | 229 | 6.11 | 28.10 | 65.2 |
| Apple H.264 (software, VideoToolbox) | 50 | 6.27 | 25.74 | 47.0 |
| Apple HEVC (software, VideoToolbox) | 7 | 6.58 | 27.57 | 61.8 |
| x264 veryfast | 189 | 6.13 | 26.27 | 50.1 |
| x264 medium | 82 | 6.14 | 26.55 | 57.6 |
| x265 fast | 48 | 5.96 | 27.72 | 61.6 |
| x265 medium | 44 | 5.93 | 27.55 | 60.9 |
| libaom AV1 realtime, cpu-used 8 | 17 | 6.38 | 27.33 | 62.4 |

#### park_joy 1920x1080, 300 frames, target 6 Mbit/s

| encoder | fps | Mbit/s | PSNR-Y dB | VMAF |
|---|---:|---:|---:|---:|
| ours H.264, real time (P only) | 302 | 5.00 | 26.31 | 49.6 |
| ours H.264 (2 B, quality) | 290 | 5.50 | 26.92 | 58.0 |
| ours HEVC, real time | 227 | 5.00 | 26.73 | 56.4 |
| ours HEVC (2 B) | 256 | 5.43 | 27.26 | 59.2 |
| ours HEVC Main 10 (2 B) | 229 | 5.43 | 27.25 | 60.0 |
| ours AV1, real time | 269 | 5.82 | 27.58 | 63.8 |
| ours AV1 (2 B, quality) | 259 | 6.13 | 28.27 | 68.8 |
| ours AV1 10-bit (2 B, quality) | 240 | 6.13 | 28.26 | 68.8 |
| Apple H.264 (software, VideoToolbox) | 53 | 6.23 | 25.53 | 45.8 |
| Apple HEVC (software, VideoToolbox) | 6 | 6.42 | 27.52 | 62.8 |
| x264 veryfast | 174 | 6.16 | 26.93 | 57.6 |
| x264 medium | 79 | 6.18 | 26.83 | 63.4 |
| x265 fast | 39 | 6.03 | 27.98 | 65.7 |
| x265 medium | 34 | 6.04 | 27.92 | 65.8 |
| libaom AV1 realtime, cpu-used 8 | 21 | 6.12 | 27.33 | 64.6 |

#### crowd_run 3840x2160, 120 frames, target 20 Mbit/s

| encoder | fps | Mbit/s | PSNR-Y dB | VMAF |
|---|---:|---:|---:|---:|
| ours H.264, real time (P only) | 95 | 16.92 | 30.16 | 57.1 |
| ours H.264 (2 B, quality) | 77 | 18.68 | 30.37 | 61.1 |
| ours HEVC, real time | 71 | 17.08 | 30.93 | 66.9 |
| ours HEVC (2 B) | 85 | 17.86 | 30.89 | 65.0 |
| ours HEVC Main 10 (2 B) | 64 | 17.89 | 30.89 | 65.5 |
| ours AV1, real time | 74 | 20.08 | 31.97 | 73.2 |
| ours AV1 (2 B, quality) | 81 | 22.15 | 32.50 | 76.4 |
| ours AV1 10-bit (2 B, quality) | 71 | 22.13 | 32.51 | 76.5 |
| Apple H.264 (software, VideoToolbox) | 13 | 21.95 | 29.68 | 57.0 |
| Apple HEVC (software, VideoToolbox) | 2 | 25.18 | 32.25 | 74.2 |
| x264 veryfast | 40 | 25.64 | 28.10 | 46.3 |
| x264 medium | 15 | 26.89 | 29.05 | 55.2 |
| x265 fast | 13 | 19.30 | 31.76 | 70.9 |
| x265 medium | 11 | 19.33 | 31.74 | 70.9 |
| libaom AV1 realtime, cpu-used 8 | 9 | 23.54 | 31.76 | 72.6 |

#### park_joy 3840x2160, 120 frames, target 20 Mbit/s

| encoder | fps | Mbit/s | PSNR-Y dB | VMAF |
|---|---:|---:|---:|---:|
| ours H.264, real time (P only) | 89 | 16.86 | 30.18 | 57.3 |
| ours H.264 (2 B, quality) | 76 | 18.77 | 30.51 | 62.1 |
| ours HEVC, real time | 72 | 16.86 | 30.99 | 66.8 |
| ours HEVC (2 B) | 86 | 18.15 | 31.12 | 65.8 |
| ours HEVC Main 10 (2 B) | 64 | 18.10 | 31.11 | 65.9 |
| ours AV1, real time | 75 | 20.01 | 32.24 | 75.5 |
| ours AV1 (2 B, quality) | 82 | 21.85 | 32.69 | 78.2 |
| ours AV1 10-bit (2 B, quality) | 70 | 21.84 | 32.69 | 78.2 |
| Apple H.264 (software, VideoToolbox) | 14 | 22.29 | 29.47 | 54.7 |
| Apple HEVC (software, VideoToolbox) | 2 | 24.33 | 31.95 | 72.1 |
| x264 veryfast | 42 | 24.51 | 29.29 | 55.4 |
| x264 medium | 16 | 25.47 | 29.55 | 61.5 |
| x265 fast | 13 | 20.63 | 32.10 | 73.8 |
| x265 medium | 11 | 20.55 | 32.08 | 74.4 |
| libaom AV1 realtime, cpu-used 8 | 9 | 21.80 | 31.64 | 72.4 |

What this shows:
- **Speed:** our encoders run at 210-330 fps at 1080p and 64-95 fps at 4K end to end (the engine alone: 1.1-1.2 ms per 1080p frame,
  3.7-4.5 ms per 4K frame, see the latency table): 1.1-2.3x x264 veryfast and 5-50x x265, libaom and Apple's software encoders.
- **AV1 (not real time) is the best quality in every row:** VMAF 65-78, 3-6 points above x265 medium and libaom realtime, at
  6-16x their speed; at 4K it overshoots the 20 Mbit/s target by ~10 % (x265 stays within 4 %), at 1080p it is on target.
- **H.264** with B pictures and the quality controls lands between x264 veryfast and medium at 1080p and at or above x264 medium at
  4K, there with 25-30 % fewer bits (x264 overshoots the target).
- **HEVC** is behind x265 (5-8 VMAF) and undershoots the 4K target by ~10 %; it has no quality preset (NVIDIA's HEVC controls are
  the ones it already uses). B pictures help at 1080p (+3 VMAF at 10 % more bits), not at 4K. HEVC 10-bit costs nothing in
  quality and a little speed (the 16-bit input copy).
- Apple's VideoToolbox encoders run in software on this Mac (no Apple video hardware in a VM): 50 fps H.264, 6 fps HEVC at 1080p.

## Decode

#### Decode 1920x1080 (frames per second)

| codec | ours (NVDEC, VideoToolbox) | ours (NVDEC, engine time) | NullMoth's driver | Apple software (VideoToolbox) | ffmpeg software, 16 threads |
|---|---:|---:|---:|---:|---:|
| H.264 | 645 | - | 156 | 85 | 599 |
| HEVC | 969 | - | 827 | 351 | 378 |
| HEVC Main 10 | 642 | - | 584 | 287 | 331 |
| VP9 | 907 | - | - | - | 359 |
| AV1 | 939 | - | - | - | 95 |
| MPEG-2 | 695 | - | - | - | 1200 |
| MPEG-4 Part 2 | 694 | - | - | - | 1060 |
| VP8 | - | 1031 | - | - | 831 |

#### Decode 3840x2160 (frames per second)

| codec | ours (NVDEC, VideoToolbox) | ours (NVDEC, engine time) | NullMoth's driver | Apple software (VideoToolbox) | ffmpeg software, 16 threads |
|---|---:|---:|---:|---:|---:|
| H.264 | 185 | - | 38 | 24 | 154 |
| HEVC | 296 | - | 302 | 191 | 156 |
| HEVC Main 10 | 165 | - | 162 | 147 | 124 |
| VP9 | 293 | - | - | - | 161 |
| AV1 | 295 | - | - | - | 29 |
| MPEG-2 | 206 | - | - | - | 512 |
| MPEG-4 Part 2 | 25 | - | - | - | 358 |
| VP8 | - | 250 | - | - | 347 |

What this shows:
- **H.264:** 4-5x NullMoth's driver decoder and 7-8x Apple's software decoder; about ffmpeg's 16-thread speed at 1080p, faster at 4K.
- **HEVC 8 / 10-bit:** on par with NullMoth's decoder (same hardware); 1.1-3x Apple's software decoder.
- **VP9 and AV1:** only ours in hardware (VideoToolbox on an Intel Mac has neither); AV1 is 10x dav1d with 16 threads.
- **MPEG-2 / MPEG-4 / VP8:** ffmpeg's software decoders are faster on 16 threads (these codecs are cheap); ours mainly frees the
  CPU (and Apple's decoder refuses MPEG-4 ASP). Our VideoToolbox rate for them is bound by copying each picture out (~1 ms at 4K).
- **MPEG-4 Part 2 above 1920 pixels wide:** the engine still decodes correctly (55-57 dB against ffmpeg with the same IDCT) but
  flags every macroblock and takes ~1.3 us per macroblock (4K: 24 fps). Not understood yet; such streams are rare (ASP's highest
  level is 720x576).
- 10-bit decodes are measured with 10-bit output (`x420`); asking for 8-bit output adds a CPU conversion in VideoToolbox (about
  5x slower at 1080p).

## Real-time latency

#### Real-time encode latency (encodeFrame to encoded frame, libnvencvt statistics)

| codec | size | latency ms | engine ms |
|---|---|---:|---:|
| H.264 | 1920x1080 | 1.83 | 1.19 |
| HEVC | 1920x1080 | 1.87 | 1.23 |
| AV1 | 1920x1080 | 1.58 | 1.05 |
| H.264 | 3840x2160 | 5.82 | 4.35 |
| HEVC | 3840x2160 | 5.83 | 4.49 |
| AV1 | 3840x2160 | 4.82 | 3.70 |

From `VTCompressionSessionEncodeFrame` to the encoded frame in the app's callback, real time (2-frame VBV, P only); the engine
part is the hardware time. AV1 is the fastest of the three on Ada.

## 8K (7680x4320)

| codec | encode ms / frame (engine) | decode fps (VideoToolbox) |
|---|---:|---:|
| AV1 | 14.3 | 64 |
| HEVC | 16.1 | 68 |
| H.264 | 15.1 | 11 (NullMoth's: NVDEC decodes H.264 up to 4096 wide, so ours declines) |

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
