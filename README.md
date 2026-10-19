# nvenc

A hardware H.264 encoder for macOS on NVIDIA GPUs before Blackwell (Turing, Ampere, Ada), running on
[nullmoth's NVIDIA driver](https://github.com/nullmoth/nvidia-macos-driver) and exposed to apps through VideoToolbox.
It drives the GPU's NVENC engine directly from user space and is written from public documentation only.

**Status: early.** M0 (RM client, capability probe), M1 (a channel on NVENC0/1 that runs our commands: `tools/hello`)
M2 (one H.264 IDR frame from our own picture setup, 256x128 to 4K: `tools/encode1`) and M3 (`src/enc.c`: P-frames
with up to 3 references, CBR/VBR, low-latency one-frame VBV, slices: `tools/encodeN`) work. M4 (`libnvencvt.dylib`: a VideoToolbox H.264 and (M6) HEVC
encoder, loaded by the Metal plugin with `NVMTL_NVENCVT=<process list|1>` in nvrm610.conf (patch in kvm-tahoe-nvidia `guest/nvenc`) or injected with `DYLD_INSERT_LIBRARIES`) works.
Next: M5, GPU-resident input. Milestones and design: [docs/design.md](docs/design.md); step-by-step tasks with reports:
[docs/tasks.md](docs/tasks.md).

Why: the driver's own encoder library is Blackwell-only and closed. With a layout fix it runs on an RTX 4090, but a
4K frame still costs ~9 ms in CPU copies, tiling and spin-waiting, the bitrate setting is ignored, and only one session
per process works ([measurements](https://github.com/mickys/kvm-tahoe-nvidia/blob/main/docs/nvenc-enable-plan.md)).

## Using it

Releases ship inside the [mickys overlay](https://github.com/mickys/nvidia-macos-driver/releases) for nullmoth's driver
(RTX 40 / Ada; H.264 High, HEVC Main and Main 10).

- **Apps** (Parsec, OBS, AVFoundation apps): the Metal plugin loads `libnvencvt.dylib` into every process that uses
  Metal, and VideoToolbox lists "NVENC - H.264 - (NVIDIA <card>) - mickys/nvenc - <version>" and its HEVC twin.
- **Command-line tools** (ffmpeg, HandBrakeCLI, avconvert) never use Metal, so they see the encoder only through the
  overlay's wrapper: `nvenc-run ffmpeg -i in.mov -c:v hevc_videotoolbox -b:v 20M out.mp4` (it sets
  `DYLD_INSERT_LIBRARIES` for that one tool; the library registers itself when injected). macOS has no supported way for a
  third party to register a video encoder for every process: VideoToolbox loads only Apple-signed encoder plug-ins, and
  MediaExtension has decoder extensions but no encoder ones ([docs/reports/T7.00-cli-and-extensions.md](docs/reports/T7.00-cli-and-extensions.md)).
  The wrapper relies on the boot settings nullmoth's driver needs (relaxed SIP, `amfi_get_out_of_my_way`) for hardened
  and Apple binaries; unsigned builds (evermeet.cx, Homebrew) take it anyway.

## Layout

| Path | What |
|---|---|
| `src/` | the encoder: `rm.c` (RM client, memory, GPU/CPU mappings), `channel.c` (GPFIFO channel on an engine, push buffer, doorbell, MMU fault info), `blocklinear.c` (NVENC input layout), `h264hdr.c` (SPS/PPS), `enc.c` (the encode session: IDR/P, references, rate control, slices) |
| `tools/` | test programs, each built against `src/`: `probe` (engines and classes), `hello` (M1: semaphores from host and NVENC), `encode1` (M2: one IDR frame), `encodeN` (M3: N frames, rate control via environment), `h264check` (decode with Apple's decoder, PSNR), `rcerr` (RM's RC error log, root), `vtrandom` (one random frame through VideoToolbox), `vtprobe` (stub VT encoder that logs every call), `vtstress` / `encloop` (many sessions, leak checks) |
| `vt/` | `nvencvt.c`: the VideoToolbox encoder (`libnvencvt.dylib`); interface in `docs/vt-encoder.md` |
| `trace/` | `nvtrace.c`: a `DYLD_INSERT_LIBRARIES` interposer that records a process's RM escapes, NVENC channel methods and buffers |
| `scripts/` | `prbdecode.py` (RM protobuf records), `trace2blob.py`, `mixsetup.py` (setup bisection) |
| `docs/design.md` | sources, design, milestones, lessons per milestone |
| `docs/tasks.md`, `docs/reports/` | the milestones split into tasks; one report per finished task |
| `docs/traces/` | captures of a working NVENC client (picture setups, logs) |
| `scripts/fetch-vendor.sh` | NVIDIA's open-gpu-kernel-modules (RM API, 610.57.04) and open-gpu-doc (NVENC docs) into `vendor/` |

## Build and run

Needs a macOS guest/host with nullmoth's driver and the Command Line Tools, reachable over ssh.

```sh
scripts/fetch-vendor.sh
./build.sh                 # NVENC_GUEST=<ssh host>, default "tahoe"; binaries land in the guest's ~/nvenc-dev
ssh tahoe ~/nvenc-dev/probe
ssh tahoe 'cd ~/nvenc-dev && ./encode1 1920 1080 26 /tmp/o.h264 /tmp/o.nv12 && ./h264check 1920 1080 /tmp/o.h264 /tmp/o.nv12'
```

## Ground rules

Public sources only: NVIDIA's open-gpu-doc and open-gpu-kernel-modules, nullmoth's open-source code, Apple's
published headers and SPI declarations, the H.264 standard. No disassembly or patching of closed binaries, including
nullmoth's codec libraries; where an interface isn't documented (VideoToolbox's encoder plugin vtable), it is worked
out black-box by observing calls into our own code.

## License

MIT (see `LICENSE`). The headers in `vendor/` (NVIDIA open-gpu-kernel-modules, open-gpu-doc, WebKit SPI) keep their own licences.
