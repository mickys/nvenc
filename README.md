# nvenc

A hardware H.264 encoder for macOS on NVIDIA GPUs before Blackwell (Turing, Ampere, Ada), running on
[nullmoth's NVIDIA driver](https://github.com/nullmoth/nvidia-macos-driver) and exposed to apps through VideoToolbox.
It drives the GPU's NVENC engine directly from user space and is written from public documentation only.

**Status: early.** M0 (RM client, capability probe) and M1 (a channel on NVENC0/1 that runs our commands: `tools/hello`)
work. Next: M2, one H.264 IDR frame. Milestones and design: [docs/design.md](docs/design.md).

Why: the driver's own encoder library is Blackwell-only and closed. With a layout fix it runs on an RTX 4090, but a
4K frame still costs ~9 ms in CPU copies, tiling and spin-waiting, the bitrate setting is ignored, and only one session
per process works ([measurements](https://github.com/mickys/kvm-tahoe-nvidia/blob/main/docs/nvenc-enable-plan.md)).

## Layout

| Path | What |
|---|---|
| `src/` | the encoder: `rm.c` (RM client, memory, GPU/CPU mappings), `channel.c` (GPFIFO channel on an engine, push buffer, doorbell) |
| `tools/` | test programs, each built against `src/`: `probe` (engines and classes), `hello` (M1: semaphores from host and NVENC) |
| `docs/design.md` | sources, design, milestones |
| `scripts/fetch-vendor.sh` | NVIDIA's open-gpu-kernel-modules (RM API, 610.57.04) and open-gpu-doc (NVENC docs) into `vendor/` |

## Build and run

Needs a macOS guest/host with nullmoth's driver and the Command Line Tools, reachable over ssh.

```sh
scripts/fetch-vendor.sh
./build.sh                 # NVENC_GUEST=<ssh host>, default "tahoe"; binaries land in the guest's ~/nvenc-dev
ssh tahoe ~/nvenc-dev/probe
```

## Ground rules

Public sources only: NVIDIA's open-gpu-doc and open-gpu-kernel-modules, nullmoth's open-source code, Apple's
published headers and SPI declarations, the H.264 standard. No disassembly or patching of closed binaries, including
nullmoth's codec libraries; where an interface isn't documented (VideoToolbox's encoder plugin vtable), it is worked
out black-box by observing calls into our own code.
