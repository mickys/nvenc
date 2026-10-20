# NVIDIA Linux driver: HEVC / H.264 / AV1 quality settings (2026-10-20)

Third tracing session in the Debian VM that owns the RTX 4090 (driver 580.105.08, ffmpeg 7.1), same method as
`docs/traces/bframes-linux` (`trace-linux/nvtrace.so`: RM ioctls, pushed methods, buffers; no disassembly; the user's
decision 2026-10-20 "Tracing session for HEVC quality"). Per run: `log.*.gz`, `out.*`, dumps `dump/execN-710.bin` (setup),
`-70c` (RC data), `-718` (status), `-724` (RC process). Source: `testsrc2`, 1080p30 unless noted.

| run | encode |
|---|---|
| vp5 / vp6 / vp7 | hevc_nvenc `-preset p5 / p6 / p7 -tune hq -rc constqp -qp 28 -bf 0` |
| vp7k | the same as vp7 at 3840x2160 |
| vp7b | vp7 with `-bf 2` |
| vp7c | `-preset p7 -tune hq -rc cbr -b:v 6M -bf 0` |
| vcbr | hevc_nvenc `-preset p4 -rc cbr -b:v 6M -bf 0` |
| vsaq | vcbr + `-spatial_aq 1 -aq-strength 8` |
| vtaq | vcbr + `-temporal_aq 1 -rc-lookahead 8` |
| vla | vcbr + `-rc-lookahead 8` |
| vmp | vcbr + `-multipass fullres` |
| hp7 / hcbr / hsaq | h264_nvenc p7 hq constqp / p4 CBR / p4 CBR + spatial AQ |
| asaq | av1_nvenc p4 CBR + spatial AQ |

Findings (used in `src/enc.c`, T7.08):
- **p6 / p7 use 8x8 coding units** (`min_cu_size` 0; p4 / p5: 16x16), p5+ 4 references in the PPS, ME hint patterns 15 / 63,
  more ME winners, p7 four intra modes compared by SSD, skip evaluation for CU8 / CU64, `rdo_level` 1.
- **CBR rate control** (any preset): `hrd_type` 34, `rhopbi` 286 / 317 / 256, `Bmin` = buffer / 81920 (147 for 12 Mbit),
  `maxQPD` 6, `baseQPD` 3, the buffer twice the bitrate by default.
- **Spatial AQ** = `aqMode` 3 with the driver's map in the RC data at `act_stat_offset`: one signed byte per 32x32 CTB, rows
  256 bytes apart; values about log2(CTB luma variance + 1) - 8.6, clipped at -`aq-strength` (8: from -8 on flat CTBs to +4).
- **Lookahead / multipass** add an ME-only execute per frame (no reconstruction, no RC data) before the encode.
- The first-pass status dumps of `vmp` (2+ MB each) are left out except `exec1-718.bin`.
