// av1dec.c: see av1dec.h. Section numbers refer to the AV1 specification.
#include "av1dec.h"
#include "blocklinear.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "class/clc56f.h"
#include "class/cl2080_notification.h"
#include "clc9b0.h"
#include "nvdec_drv.h"

static NvU32 hi(NvU64 a) { return (NvU32)(a >> 8); }
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int alloc(Rm *rm, RmMem *m, size_t size)
{
    if (rm_mem_alloc(rm, m, (size + 0xfff) & ~(size_t)0xfff, 0x1000, true, true, true)) return -1;
    memset(m->cpu, 0, m->size);
    return 0;
}

// The default CDF tables, one per quantizer context (init_coeff_cdfs picks by base_q_idx). Layout hypothesis files for
// now: $AV1_CDF_DIR/cdf_q0..3.bin (default ~/av1-cdf).
static int load_cdf_tables(Av1Dec *d)
{
    const char *dir = getenv("AV1_CDF_DIR"); char path[512];
    for (int q = 0; q < 4; q++) {
        if (dir) snprintf(path, sizeof path, "%s/cdf_q%d.bin", dir, q);
        else snprintf(path, sizeof path, "%s/av1-cdf/cdf_q%d.bin", getenv("HOME") ? getenv("HOME") : ".", q);
        FILE *f = fopen(path, "rb"); if (!f) { fprintf(stderr, "av1dec: %s missing\n", path); return -1; }
        fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
        d->cdf_tab[q] = malloc((size_t)n);
        if (!d->cdf_tab[q] || fread(d->cdf_tab[q], 1, (size_t)n, f) != (size_t)n) { fclose(f); return -1; }
        fclose(f); d->cdf_size = (size_t)n;
    }
    return 0;
}

int av1dec_open(Av1Dec *d, Rm *rm, unsigned maxW, unsigned maxH, int bit_depth)
{
    memset(d, 0, sizeof *d);
    d->rm = rm; d->maxW = (maxW + 127) & ~127u; d->maxH = (maxH + 127) & ~127u;
    d->bit_depth = bit_depth; d->bps = bit_depth > 8 ? 2 : 1;
    d->pitch = (size_t)d->maxW * (size_t)d->bps;
    for (int i = 0; i < 8; i++) d->ref[i] = -1;
    if (load_cdf_tables(d)) return -1;
    NV_NVDEC_ALLOCATION_PARAMETERS dp = { .size = sizeof dp, .prohibitMultipleInstances = 0, .engineInstance = 0 };
    if (chan_create(rm, &d->chan, NV2080_ENGINE_TYPE_NVDEC0, NVC9B0_VIDEO_DECODER, &dp)) { fprintf(stderr, "av1dec: channel failed\n"); return -1; }
    // filter buffer: the six neighbour areas nvdec_drv.h sizes, one after the other
    const size_t W = d->maxW, H = d->maxH;
    d->flt_top = 0;
    d->flt_vert = d->flt_top + AV1_DBLK_TOP_BUF_SIZE(W);
    d->flt_cdef = d->flt_vert + AV1_DBLK_LEFT_BUF_SIZE(H);
    d->flt_lr = d->flt_cdef + AV1_CDEF_LEFT_BUF_SIZE(H);
    d->flt_hus = d->flt_lr + AV1_LR_LEFT_BUF_SIZE(H);
    d->flt_fgs = d->flt_hus + AV1_HUS_LEFT_BUF_SIZE(H);
    const size_t flt = d->flt_fgs + AV1_FGS_LEFT_BUF_SIZE(H);
    const size_t bsCap = W * H * 2 > (4u << 20) ? W * H * 2 : (4u << 20);
    struct { RmMem *m; size_t size; } b[] = {
        { &d->setup, 0x2000 }, { &d->bs, bsCap }, { &d->cdf_def, d->cdf_size + 4096 }, { &d->cdf_out, d->cdf_size + 4096 },
        { &d->tileinfo, AV1_TILEINFO_BUF_SIZE }, { &d->tilestream, AV1_TILESTREAMINFO_BUF_SIZE }, { &d->filter, flt },
        { &d->intra, AV1_INTRA_TOP_BUF_SIZE * (size_t)d->bps }, { &d->gm, AV1_GLOBAL_MOTION_BUF_SIZE }, { &d->fg, AV1_FGS_BUF_SIZE },
        { &d->status, 0x1000 }, { &d->sem, 0x1000 } };
    for (size_t i = 0; i < sizeof b / sizeof b[0]; i++) if (alloc(rm, b[i].m, b[i].size)) { fprintf(stderr, "av1dec: alloc failed\n"); return -1; }
    for (int i = 0; i < 8; i++) if (alloc(rm, &d->cdf[i], d->cdf_size + 4096)) return -1;
    const size_t luma = nvenc_in_plane_size(d->pitch, d->maxH) + 65536, chroma = nvenc_in_plane_size(d->pitch, d->maxH / 2) + 65536;
    for (int i = 0; i < AV1_SURFACES; i++)
        if (alloc(rm, &d->luma[i], luma) || alloc(rm, &d->chroma[i], chroma) ||
            alloc(rm, &d->tmv[i], AV1_TEMPORAL_MV_BUF_SIZE(W, H)) || alloc(rm, &d->seg[i], AV1_SEGMENT_ID_BUF_SIZE(W, H))) {
            fprintf(stderr, "av1dec: surface alloc failed\n"); return -1;
        }
    chan_mthd(&d->chan, 0, NVC56F_SET_OBJECT, 1, (NvU32)NVC9B0_VIDEO_DECODER);
    return 0;
}

void av1dec_close(Av1Dec *d)
{
    chan_destroy(&d->chan);
    RmMem *m[] = { &d->setup, &d->bs, &d->cdf_def, &d->cdf_out, &d->tileinfo, &d->tilestream, &d->filter, &d->intra, &d->gm, &d->fg, &d->status, &d->sem };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) rm_mem_free(d->rm, m[i]);
    for (int i = 0; i < 8; i++) rm_mem_free(d->rm, &d->cdf[i]);
    for (int i = 0; i < AV1_SURFACES; i++) { rm_mem_free(d->rm, &d->luma[i]); rm_mem_free(d->rm, &d->chroma[i]); rm_mem_free(d->rm, &d->tmv[i]); rm_mem_free(d->rm, &d->seg[i]); }
    for (int q = 0; q < 4; q++) free(d->cdf_tab[q]);
}

void av1dec_release(Av1Dec *d, int surf) { if (surf >= 0 && surf < AV1_SURFACES) d->busy[surf] = false; }

void av1dec_read_nv12(Av1Dec *d, int surf, uint8_t *y, size_t ystride, uint8_t *uv, size_t uvstride, unsigned w, unsigned h)
{
    const size_t b = (size_t)d->bps;
    nvenc_in_untile_plane(y, ystride, d->luma[surf].cpu, w * b, h, d->pitch);
    nvenc_in_untile_plane(uv, uvstride, d->chroma[surf].cpu, ((w + 1) & ~1u) * b, (h + 1) / 2, d->pitch);
}

static unsigned sample(const uint8_t *p, size_t x, size_t y, size_t pitch, int bps, int shift)
{
    if (bps == 1) return p[nvenc_in_offset(x, y, pitch)];
    const size_t o = nvenc_in_offset(2 * x, y, pitch);
    return (unsigned)(p[o] | p[o + 1] << 8) >> shift;
}

void av1dec_read_i420(Av1Dec *d, int surf, uint8_t *dst, unsigned w, unsigned h)
{
    const uint8_t *L = d->luma[surf].cpu, *C = d->chroma[surf].cpu;
    const unsigned cw = (w + 1) / 2, ch = (h + 1) / 2;
    const int b = d->bps, sh = b == 2 ? 16 - d->bit_depth : 0;
    #define PUT(i, v) do { if (b == 1) dst[i] = (uint8_t)(v); else { dst[2 * (i)] = (uint8_t)(v); dst[2 * (i) + 1] = (uint8_t)((v) >> 8); } } while (0)
    const size_t U = (size_t)w * h, V = U + (size_t)cw * ch;
    for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x++) PUT((size_t)y * w + x, sample(L, x, y, d->pitch, b, sh));
    for (unsigned y = 0; y < ch; y++) for (unsigned x = 0; x < cw; x++) {
        PUT(U + (size_t)y * cw + x, sample(C, 2 * x, y, d->pitch, b, sh));
        PUT(V + (size_t)y * cw + x, sample(C, 2 * x + 1, y, d->pitch, b, sh));
    }
    #undef PUT
}

static int q_ctx(int q) { return q <= 20 ? 0 : q <= 60 ? 1 : q <= 120 ? 2 : 3; }

// superres upscaling parameters of one plane (7.16)
static void superres_steps(int fw, int uw, int sub, unsigned short *step, unsigned short *init)
{
    const int dw = (fw + sub) >> sub, upw = (uw + sub) >> sub;
    const int s = ((dw << 14) + (upw / 2)) / upw;
    const int err = upw * s - (dw << 14);
    int x = (-((upw - dw) << 13) + upw / 2) / upw + (1 << 7) - err / 2;
    *step = (unsigned short)s; *init = (unsigned short)(x & ((1 << 14) - 1));
}

static int decode_frame(Av1Dec *d)
{
    Av1Frame *f = &d->f; const Av1Seq *s = &d->st.seq;
    if ((unsigned)f->upscaled_width > d->maxW || (unsigned)f->frame_height > d->maxH) { fprintf(stderr, "av1dec: frame larger than the session\n"); return -1; }
    int surf = -1;
    for (int i = 0; i < AV1_SURFACES && surf < 0; i++) {
        bool used = d->busy[i];
        for (int k = 0; k < 8; k++) if (d->ref[k] == i) used = true;
        if (!used) surf = i;
    }
    if (surf < 0) { fprintf(stderr, "av1dec: no free surface\n"); return -1; }

    // CDF tables to read: the defaults for this quantizer, or those saved with the primary reference frame
    const bool fresh = f->primary_ref_frame == AV1_PRIMARY_REF_NONE;
    if (fresh) memcpy(d->cdf_def.cpu, d->cdf_tab[q_ctx(f->base_q_idx)], d->cdf_size);
    RmMem *cdf_in = fresh ? &d->cdf_def : &d->cdf[f->ref_frame_idx[f->primary_ref_frame]];
    memset(d->cdf_out.cpu, 0, d->cdf_size);
    // tiles
    AV1TileInfo *ti = d->tileinfo.cpu; AV1TileStreamInfo *ts = d->tilestream.cpu;
    const int sbShift = s->use_128x128_superblock ? 5 : 4;
    for (int t = 0; t < d->ntiles; t++) {
        const int r = d->tile[t].row, c = d->tile[t].col, k = r * f->tile_cols + c;
        memset(&ti[k], 0, sizeof ti[k]); memset(&ts[k], 0, sizeof ts[k]);
        ti[k].width_in_sb = (unsigned char)((f->mi_col_starts[c + 1] - f->mi_col_starts[c] + (1 << sbShift) - 1) >> sbShift);
        ti[k].height_in_sb = (unsigned char)((f->mi_row_starts[r + 1] - f->mi_row_starts[r] + (1 << sbShift) - 1) >> sbShift);
        ts[k].tile_start = (unsigned)d->tile[t].start; ts[k].tile_end = (unsigned)d->tile[t].end;
        if (getenv("AV1_TILEINFO_OLD")) {   // experiment: the 16-byte tile info with the byte range inside
            AV1TileInfo_OLD *o = (AV1TileInfo_OLD *)d->tileinfo.cpu + k; memset(o, 0, sizeof *o);
            o->width_in_sb = ti[k].width_in_sb; o->height_in_sb = ti[k].height_in_sb;
            const unsigned s0 = ts[k].tile_start, e0 = ts[k].tile_end;
            o->tile_start_b0 = (unsigned char)s0; o->tile_start_b1 = (unsigned char)(s0 >> 8); o->tile_start_b2 = (unsigned char)(s0 >> 16); o->tile_start_b3 = (unsigned char)(s0 >> 24);
            o->tile_end_b0 = (unsigned char)e0; o->tile_end_b1 = (unsigned char)(e0 >> 8); o->tile_end_b2 = (unsigned char)(e0 >> 16); o->tile_end_b3 = (unsigned char)(e0 >> 24);
        }
        if (getenv("AV1_TILE_END_INCL")) ts[k].tile_end--;   // experiment
    }

    nvdec_av1_pic_s *ps = d->setup.cpu; memset(ps, 0, sizeof *ps);
    if (getenv("AV1_BSFILL")) {   // experiment: replace the bitstream (0, 255, or r = random)
        const char *e = getenv("AV1_BSFILL"); uint8_t *b = d->bs.cpu;
        for (size_t i = 0; i < d->bsN; i++) b[i] = e[0] == 'r' ? (uint8_t)(rand() >> 7) : (uint8_t)atoi(e);
    }
    ps->stream_len = (unsigned)d->bsN;
    ps->reserved12 = (unsigned)d->skip; ps->sw_skip_start_length = (unsigned)d->skip;
    if (getenv("AV1_STREAMLEN")) ps->stream_len = (unsigned)atoi(getenv("AV1_STREAMLEN"));   // experiment
    ps->use_128x128_superblock = (unsigned)s->use_128x128_superblock; ps->chroma_format = 1; ps->bit_depth = (unsigned)s->bit_depth;
    ps->enable_filter_intra = (unsigned)s->enable_filter_intra; ps->enable_intra_edge_filter = (unsigned)s->enable_intra_edge_filter;
    ps->enable_interintra_compound = (unsigned)s->enable_interintra_compound; ps->enable_masked_compound = (unsigned)s->enable_masked_compound;
    ps->enable_dual_filter = (unsigned)s->enable_dual_filter; ps->reserved10 = (unsigned)s->enable_order_hint;   // "0 - disable order hint"
    ps->enable_jnt_comp = (unsigned)s->enable_jnt_comp; ps->enable_cdef = (unsigned)s->enable_cdef;
    ps->enable_fgs = (unsigned)(s->film_grain_params_present && f->fg.apply_grain);
    ps->frame_type = (unsigned)f->frame_type; ps->show_frame = (unsigned)f->show_frame; ps->disable_cdf_update = (unsigned)f->disable_cdf_update;
    ps->allow_screen_content_tools = (unsigned)f->allow_screen_content_tools; ps->cur_frame_force_integer_mv = (unsigned)f->force_integer_mv;
    ps->scale_denom_minus9 = f->use_superres ? (unsigned)(f->superres_denom - 9) : 0;
    ps->allow_intrabc = (unsigned)f->allow_intrabc; ps->allow_high_precision_mv = (unsigned)f->allow_high_precision_mv;
    ps->interp_filter = (unsigned)f->interpolation_filter; ps->switchable_motion_mode = (unsigned)f->is_motion_mode_switchable;
    ps->use_ref_frame_mvs = (unsigned)f->use_ref_frame_mvs; ps->refresh_frame_context = (unsigned)!f->disable_frame_end_update_cdf;
    ps->delta_q_present_flag = (unsigned)f->delta_q_present; ps->delta_q_res = (unsigned)f->delta_q_res;
    ps->delta_lf_present_flag = (unsigned)f->delta_lf_present; ps->delta_lf_res = (unsigned)f->delta_lf_res; ps->delta_lf_multi = (unsigned)f->delta_lf_multi;
    ps->coded_lossless = (unsigned)f->coded_lossless; ps->tile_enabled = (unsigned)(f->tile_cols * f->tile_rows > 1);
    ps->superres_is_scaled = (unsigned)f->use_superres;
    ps->tile_cols = (unsigned)f->tile_cols; ps->tile_rows = (unsigned)f->tile_rows; ps->context_update_tile_id = (unsigned)f->context_update_tile_id;
    ps->cdef_damping_minus_3 = (unsigned)(f->cdef_damping - 3); ps->cdef_bits = (unsigned)f->cdef_bits;
    static const unsigned txmode[3] = { 0, 3, 4 };   // "0:ONLY4x4, 3:LARGEST, 4:SELECT"
    ps->frame_tx_mode = txmode[f->tx_mode]; ps->frame_reference_mode = f->reference_select ? 2 : 0;
    ps->skip_mode_flag = (unsigned)f->skip_mode_present;
    ps->skip_ref0 = (unsigned)f->skip_mode_frame[0]; ps->skip_ref1 = (unsigned)f->skip_mode_frame[1];
    ps->allow_warp = (unsigned)f->allow_warped_motion; ps->reduced_tx_set_used = (unsigned)f->reduced_tx_set;
    ps->superres_upscaled_width = (unsigned short)f->upscaled_width;
    if (f->use_superres) {
        superres_steps(f->frame_width, f->upscaled_width, 0, &ps->superres_luma_step, &ps->superres_init_luma_subpel_x);
        superres_steps(f->frame_width, f->upscaled_width, s->subsampling_x, &ps->superres_chroma_step, &ps->superres_init_chroma_subpel_x);
    }
    ps->base_qindex = (unsigned char)f->base_q_idx; ps->y_dc_delta_q = (char)f->delta_q_y_dc;
    ps->u_dc_delta_q = (char)f->delta_q_u_dc; ps->v_dc_delta_q = (char)f->delta_q_v_dc; ps->u_ac_delta_q = (char)f->delta_q_u_ac; ps->v_ac_delta_q = (char)f->delta_q_v_ac;
    ps->qm_y = (unsigned char)(f->using_qmatrix ? f->qm_y : 15); ps->qm_u = (unsigned char)(f->using_qmatrix ? f->qm_u : 15); ps->qm_v = (unsigned char)(f->using_qmatrix ? f->qm_v : 15);
    for (int i = 0; i < 8; i++) {
        ps->cdef_y_pri_strength |= (unsigned)f->cdef_y_pri[i] << (4 * i); ps->cdef_uv_pri_strength |= (unsigned)f->cdef_uv_pri[i] << (4 * i);
        ps->cdef_y_sec_strength |= (unsigned)(f->cdef_y_sec[i] == 4 ? 3 : f->cdef_y_sec[i]) << (2 * i);
        ps->cdef_uv_sec_strength |= (unsigned)(f->cdef_uv_sec[i] == 4 ? 3 : f->cdef_uv_sec[i]) << (2 * i);
    }
    ps->segment_enabled = (unsigned char)f->seg_enabled; ps->segment_update_map = (unsigned char)f->seg_update_map;
    ps->segment_temporal_update = (unsigned char)f->seg_temporal_update;
    for (int i = 0; i < 8; i++) for (int j = 0; j < 8; j++) ps->segment_feature_data[i][j] = (short)(f->seg_feature_enabled[i][j] ? f->seg_feature_data[i][j] : 0);
    ps->last_active_segid = (unsigned char)f->last_active_seg_id; ps->segid_preskip = (unsigned char)f->seg_id_pre_skip;
    ps->filter_level[0] = (unsigned char)f->lf_level[0]; ps->filter_level[1] = (unsigned char)f->lf_level[1];
    ps->filter_level_u = (unsigned char)f->lf_level[2]; ps->filter_level_v = (unsigned char)f->lf_level[3];
    ps->lf_sharpness_level = (unsigned char)f->lf_sharpness;
    for (int i = 0; i < 8; i++) ps->lf_ref_deltas[i] = (char)(f->lf_delta_enabled ? f->lf_ref_deltas[i] : 0);
    for (int i = 0; i < 2; i++) ps->lf_mode_deltas[i] = (char)(f->lf_delta_enabled ? f->lf_mode_deltas[i] : 0);
    static const int lr_size_code[5] = { 0, 0, 0, 0, 0 }; (void)lr_size_code;
    for (int p = 0; p < 3; p++) {
        ps->lr_type |= (unsigned char)(f->lr_type[p] << (2 * p));
        const int sz = f->lr_size[p], code = sz >= 256 ? 3 : sz >= 128 ? 2 : sz >= 64 ? 1 : 0;
        ps->lr_unit_size |= (unsigned char)(code << (2 * p));
    }
    ps->current_frame.width = (unsigned short)f->upscaled_width; ps->current_frame.height = (unsigned short)f->frame_height;
    ps->current_frame.stride[0] = ps->current_frame.stride[1] = (unsigned short)(d->pitch / 16);
    ps->current_frame.frame_buffer_idx = (unsigned)surf;
    for (int i = 0; i < 7; i++) {
        ref_frame_struct_s *rf = &ps->ref_frame[i];
        const int slot = f->frame_is_intra ? -1 : f->ref_frame_idx[i], rs = slot >= 0 ? d->ref[slot] : -1;
        const unsigned rw = rs >= 0 ? d->w[rs] : (unsigned)f->upscaled_width, rh = rs >= 0 ? d->h[rs] : (unsigned)f->frame_height;
        rf->info.width = (unsigned short)rw; rf->info.height = (unsigned short)rh;
        rf->info.stride[0] = rf->info.stride[1] = (unsigned short)(d->pitch / 16);
        rf->info.frame_buffer_idx = (unsigned)(rs >= 0 ? rs : surf);
        rf->sf.x_scale_fp = (short)((((int)rw << 14) + f->frame_width / 2) / f->frame_width);
        rf->sf.y_scale_fp = (short)((((int)rh << 14) + f->frame_height / 2) / f->frame_height);
        rf->sign_bias = (unsigned char)f->ref_frame_sign_bias[i + 1];
        rf->wmtype = (unsigned char)f->gm_type[i + 1];
    }
    ps->tileformat = 1; ps->gob_height = 0;
    {   // experiments (T9.02): other readings of ambiguous setup fields; AV1_POKE=offset:value,... writes setup bytes
        const char *e;
        if (getenv("AV1_BD0")) ps->bit_depth = (unsigned)(s->bit_depth - 8);
        if ((e = getenv("AV1_STRIDE"))) {
            unsigned short v = !strcmp(e, "bytes") ? (unsigned short)d->pitch : (unsigned short)(d->pitch / (size_t)d->bps);
            ps->current_frame.stride[0] = ps->current_frame.stride[1] = v;
            for (int i = 0; i < 7; i++) ps->ref_frame[i].info.stride[0] = ps->ref_frame[i].info.stride[1] = v;
        }
        if (getenv("AV1_CHROMA0")) ps->chroma_format = 0;
        if ((e = getenv("AV1_POKE"))) {
            char buf[256]; snprintf(buf, sizeof buf, "%s", e);
            for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
                unsigned o = 0, v = 0; if (sscanf(tok, "%i:%i", &o, &v) == 2 && o < sizeof *ps) ((uint8_t *)ps)[o] = (uint8_t)v;
            }
        }
    }
    ps->Av1FltTopOffset = (unsigned)(d->flt_top >> 8); ps->Av1FltVertOffset = (unsigned)(d->flt_vert >> 8);
    ps->Av1CdefVertOffset = (unsigned)(d->flt_cdef >> 8); ps->Av1LrVertOffset = (unsigned)(d->flt_lr >> 8);
    ps->Av1HusVertOffset = (unsigned)(d->flt_hus >> 8); ps->Av1FgsVertOffset = (unsigned)(d->flt_fgs >> 8);

    if (getenv("AV1_DROP")) {   // experiment: AV1_DROP=offset:bytes removes bytes from the setup (an older struct layout)
        unsigned o = 0, nb = 0;
        if (sscanf(getenv("AV1_DROP"), "%i:%i", &o, &nb) == 2 && o + nb < sizeof *ps) {
            memmove((uint8_t *)ps + o, (uint8_t *)ps + o + nb, sizeof *ps - o - nb); memset((uint8_t *)ps + sizeof *ps - nb, 0, nb);
        }
    }
    {   // global motion: per reference, the six warp parameters (32 bytes each, 7 references; layout assumed)
        int32_t *g = d->gm.cpu; memset(g, 0, 7 * 32);
        for (int i = 0; i < 7; i++) for (int k = 0; k < 6; k++) g[i * 8 + k] = f->gm_params[i + 1][k] ? f->gm_params[i + 1][k] : (k % 3 == 2 ? 1 << 16 : 0);
    }
    Chan *c = &d->chan;
    chan_rewind(c);
    // experiment (T9.02): AV1_NULL=n points the n-th buffer method below at address 0, to see whether the engine reads it
    const int nullIdx = getenv("AV1_NULL") ? atoi(getenv("AV1_NULL")) : -1; int midx = 0;
    #define BUF(m, a) do { chan_mthd(c, 0, (m), 1, midx++ == nullIdx ? 0u : hi(a)); } while (0)
    // clc9b0.h names no AV1 application ID; AVD (0x12) is taken as the AV1 decoder (AV1_APP_ID overrides, experiment)
    chan_mthd(c, 0, NVC9B0_SET_APPLICATION_ID, 1, getenv("AV1_APP_ID") ? (NvU32)strtoul(getenv("AV1_APP_ID"), NULL, 0) : (NvU32)NVC9B0_SET_APPLICATION_ID_ID_AVD);
    chan_mthd(c, 0, NVC9B0_SET_CONTROL_PARAMS, 1, DRF_NUM(C9B0, _SET_CONTROL_PARAMS, _CODEC_TYPE, NVDEC_CODEC_AV1) |
              DRF_NUM(C9B0, _SET_CONTROL_PARAMS, _GPTIMER_ON, 1));
    BUF(NVC9B0_SET_DRV_PIC_SETUP_OFFSET, d->setup.gpu);
    BUF(NVC9B0_SET_IN_BUF_BASE_OFFSET, d->bs.gpu);
    chan_mthd(c, 0, NVC9B0_SET_PICTURE_INDEX, 1, (NvU32)surf);
    BUF(NVC9B0_SET_NVDEC_STATUS_OFFSET, d->status.gpu);
    BUF(NVC9B0_SET_TILE_SIZE_BUF_OFFSET, d->tileinfo.gpu);
    BUF(NVC9B0_SET_FILTER_BUFFER_OFFSET, d->filter.gpu);
    BUF(NVC9B0_SET_INTRA_TOP_BUF_OFFSET, d->intra.gpu);
    BUF(NVC9B0_AV1_SET_PROB_TAB_READ_BUF_OFFSET, cdf_in->gpu);
    BUF(NVC9B0_AV1_SET_PROB_TAB_WRITE_BUF_OFFSET, d->cdf_out.gpu);
    BUF(NVC9B0_AV1_SET_SEGMENT_READ_BUF_OFFSET, d->seg[surf].gpu);     // inter frames: the primary reference's
    BUF(NVC9B0_AV1_SET_SEGMENT_WRITE_BUF_OFFSET, d->seg[surf].gpu);
    BUF(NVC9B0_AV1_SET_COL_MVWRITE_BUF_OFFSET, d->tmv[surf].gpu);
    for (int i = 0; i < 3; i++) BUF(NVC9B0_AV1_SET_COL_MV0_READ_BUF_OFFSET + 4 * (NvU32)i, d->tmv[surf].gpu);
    BUF(NVC9B0_AV1_SET_GLOBAL_MODEL_BUF_OFFSET, d->gm.gpu);
    BUF(NVC9B0_AV1_SET_FILM_GRAIN_BUF_OFFSET, d->fg.gpu);
    BUF(NVC9B0_AV1_SET_TILE_STREAM_INFO_BUF_OFFSET, d->tilestream.gpu);
    {   // experiment (T9.02): which other buffers the engine reads; AV1_EXTRA = bit mask, 4 MB of a spare buffer each
        static RmMem extra; const char *e = getenv("AV1_EXTRA"); const unsigned m = e ? (unsigned)strtoul(e, NULL, 0) : 0;
        static const NvU32 mt[10] = { NVC9B0_SET_PIC_SCRATCH_BUF_OFFSET, NVC9B0_SET_COLOC_DATA_OFFSET, NVC9B0_SET_HISTORY_OFFSET,
                                      NVC9B0_SET_HISTOGRAM_OFFSET, NVC9B0_SET_SLICE_OFFSETS_BUF_OFFSET, NVC9B0_SET_EXTERNAL_MVBUFFER_OFFSET,
                                      NVC9B0_AV1_SET_HINT_DUMP_BUF_OFFSET, NVC9B0_AV1_SET_SUB_STREAM_ENTRY_BUF_OFFSET,
                                      NVC9B0_SET_CRC_STRUCT_OFFSET, NVC9B0_SET_INTRA_TOP_BUF_OFFSET };
        if (m && !extra.size && alloc(d->rm, &extra, 10u << 22)) return -1;
        for (int i = 0; i < 10; i++) if (m & (1u << i)) chan_mthd(c, 0, mt[i], 1, hi(extra.gpu + ((NvU64)i << 22)));
    }
    for (int i = 0; i < AV1_SURFACES; i++) {
        if (i == surf) { BUF(NVC9B0_SET_PICTURE_LUMA_OFFSET0 + 4 * (NvU32)i, d->luma[i].gpu); BUF(NVC9B0_SET_PICTURE_CHROMA_OFFSET0 + 4 * (NvU32)i, d->chroma[i].gpu); }
        else { chan_mthd(c, 0, NVC9B0_SET_PICTURE_LUMA_OFFSET0 + 4 * (NvU32)i, 1, hi(d->luma[i].gpu)); chan_mthd(c, 0, NVC9B0_SET_PICTURE_CHROMA_OFFSET0 + 4 * (NvU32)i, 1, hi(d->chroma[i].gpu)); }
    }
    #undef BUF
    chan_mthd(c, 0, NVC9B0_EXECUTE, 1, 0u);
    const NvU32 val = ++d->semval; volatile NvU32 *sv = d->sem.cpu;
    chan_mthd(c, 0, NVC9B0_SEMAPHORE_A, 3, (NvU32)(d->sem.gpu >> 32), (NvU32)d->sem.gpu, val);
    chan_mthd(c, 0, NVC9B0_SEMAPHORE_D, 1, DRF_DEF(C9B0, _SEMAPHORE_D, _STRUCTURE_SIZE, _ONE) |
              DRF_DEF(C9B0, _SEMAPHORE_D, _OPERATION, _RELEASE) | DRF_DEF(C9B0, _SEMAPHORE_D, _PAYLOAD_SIZE, _32BIT));
    const double t0 = now_ms();
    chan_submit(c);
    while (sv[0] != val && now_ms() - t0 < 1000) { }
    d->engine_ms = now_ms() - t0;
    if (sv[0] != val) { fprintf(stderr, "av1dec: no completion (error notifier 0x%x)\n", chan_error(c)); return -1; }
    const nvdec_status_s *st = d->status.cpu;
    d->err_status = st->error_status; d->ok_blocks = st->mbs_correctly_decoded;
    if (d->verbose)
        printf("  av1dec: surf %2d type %d%s %dx%d q %d tiles %d -> status 0x%x, ok %u, err %u, %.2f ms\n", surf, f->frame_type,
               f->show_frame ? "" : " (hidden)", f->upscaled_width, f->frame_height, f->base_q_idx, d->ntiles, st->error_status,
               st->mbs_correctly_decoded, st->mbs_in_error, d->engine_ms);
    if (getenv("AV1_DIAG")) {   // raw status words, how much of the CDF output table the engine wrote, the output plane
        const NvU32 *w = d->status.cpu; printf("    status words:"); for (int i = 0; i < 16; i++) printf(" %08x", w[i]); printf("\n");
        const uint16_t *co = d->cdf_out.cpu, *ci = cdf_in->cpu; size_t nz = 0, ch = 0, last = 0;
        for (size_t i = 0; i < d->cdf_size / 2; i++) { if (co[i]) { nz++; last = i; } if (co[i] != ci[i]) ch++; }
        printf("    cdf out: %zu nonzero words (last %zu), %zu differ from the input, of %zu\n", nz, last, ch, d->cdf_size / 2);
        const uint8_t *L = d->luma[surf].cpu; size_t lnz = 0; for (size_t i = 0; i < d->luma[surf].size; i++) lnz += L[i] != 0;
        printf("    luma surface: %zu nonzero bytes\n", lnz);
        struct { const char *n; RmMem *m; } bb[] = { { "filter", &d->filter }, { "intra", &d->intra }, { "tmv", &d->tmv[surf] },
            { "seg", &d->seg[surf] }, { "gm", &d->gm }, { "fg", &d->fg }, { "chroma", &d->chroma[surf] } };
        for (size_t k = 0; k < sizeof bb / sizeof bb[0]; k++) {
            const uint8_t *q = bb[k].m->cpu; size_t nz = 0, first = (size_t)-1, lastb = 0;
            for (size_t i = 0; i < bb[k].m->size; i++) if (q[i]) { nz++; if (first == (size_t)-1) first = i; lastb = i; }
            printf("    %-7s %8zu bytes: %zu nonzero (%zu..%zu)\n", bb[k].n, bb[k].m->size, nz, nz ? first : 0, lastb);
        }
    }
    // reference update (7.20): the surface and the CDFs to save (written by the engine unless frame-end update is off)
    for (int i = 0; i < 8; i++) if (f->refresh_frame_flags & (1 << i)) {
        d->ref[i] = surf;
        memcpy(d->cdf[i].cpu, f->disable_frame_end_update_cdf ? cdf_in->cpu : d->cdf_out.cpu, d->cdf_size);
    }
    d->w[surf] = (unsigned)f->upscaled_width; d->h[surf] = (unsigned)f->frame_height;
    av1_end_frame(&d->st, f);
    if (!f->show_frame) return 0;
    d->busy[surf] = true; d->cur_surf = surf; d->cur_w = f->upscaled_width; d->cur_h = f->frame_height;
    return 1;
}

int av1dec_tu(Av1Dec *d, const uint8_t *buf, size_t n)
{
    int shown = 0;
    for (size_t pos = 0; pos < n;) {
        Av1Obu o; if (av1_obu(buf + pos, n - pos, &o)) { fprintf(stderr, "av1dec: bad OBU\n"); return -1; }
        const uint8_t *pl = buf + pos + o.header_len;
        d->st.temporal_id = o.temporal_id; d->st.spatial_id = o.spatial_id;
        const uint8_t *tg = NULL; size_t tgn = 0;
        if (o.type == AV1_OBU_SEQUENCE_HEADER) {
            if (av1_parse_sequence_header(&d->st.seq, pl, o.payload_len)) return -1;
        } else if (o.type == AV1_OBU_FRAME_HEADER || o.type == AV1_OBU_FRAME) {
            if (d->have_hdr && o.type == AV1_OBU_FRAME_HEADER) { pos += o.header_len + o.payload_len; continue; }   // a copy
            size_t hb;
            if (av1_parse_frame_header(&d->st, pl, o.payload_len, &d->f, &hb)) { fprintf(stderr, "av1dec: frame header error\n"); return -1; }
            if (d->f.show_existing_frame) {
                const int sfc = d->ref[d->f.frame_to_show_map_idx];
                if (sfc < 0) return -1;
                if (d->f.frame_type == AV1_KEY_FRAME)   // a shown key frame refreshes every slot (7.21, 7.20)
                    for (int i = 0; i < 8; i++) { if (i != d->f.frame_to_show_map_idx) memcpy(d->cdf[i].cpu, d->cdf[d->f.frame_to_show_map_idx].cpu, d->cdf_size); d->ref[i] = sfc; }
                av1_end_frame(&d->st, &d->f);
                if (shown) av1dec_release(d, d->cur_surf);
                d->busy[sfc] = true; d->cur_surf = sfc; d->cur_w = (int)d->w[sfc]; d->cur_h = (int)d->h[sfc]; shown = 1;
            } else {
                d->have_hdr = true; d->bsN = 0; d->ntiles = 0; d->skip = 0;
                if (o.type == AV1_OBU_FRAME) { tg = pl + hb; tgn = o.payload_len - hb; }
            }
        } else if (o.type == AV1_OBU_TILE_GROUP && d->have_hdr) { tg = pl; tgn = o.payload_len; }
        if (tg) {
            // experiment (T9.02): AV1_BS_MODE 1 = the frame OBU payload from its header on, 2 = the whole temporal unit,
            // with the tile offsets relative to that and the skip fields set to where the tile data starts
            const int bsMode = getenv("AV1_BS_MODE") ? atoi(getenv("AV1_BS_MODE")) : 0;
            if (bsMode && d->bsN == 0) {
                const uint8_t *from = bsMode == 2 ? buf : pl;
                const size_t len = bsMode == 2 ? n : o.payload_len;
                memcpy(d->bs.cpu, from, len); d->skip = (size_t)(tg - from); d->bsN = len;
            }
            static Av1Tile tl[AV1_MAX_TILES]; int start; bool last;
            const int nt = av1_parse_tile_group(&d->f, tg, tgn, tl, AV1_MAX_TILES, &start, &last);
            if (nt < 0) { fprintf(stderr, "av1dec: tile group error\n"); return -1; }
            for (int t = 0; t < nt && bsMode; t++) {
                d->tile[d->ntiles].start = d->skip + tl[t].offset; d->tile[d->ntiles].end = d->skip + tl[t].offset + tl[t].size;
                d->tile[d->ntiles].row = tl[t].row; d->tile[d->ntiles].col = tl[t].col; d->ntiles++;
            }
            for (int t = 0; t < nt && !bsMode; t++) {   // each tile's data, back to back in the bitstream buffer
                if (d->bsN + tl[t].size + 64 > d->bs.size || d->ntiles >= AV1_MAX_TILES) return -1;
                memcpy((uint8_t *)d->bs.cpu + d->bsN, tg + tl[t].offset, tl[t].size);
                d->tile[d->ntiles].start = d->bsN; d->tile[d->ntiles].end = d->bsN + tl[t].size;
                d->tile[d->ntiles].row = tl[t].row; d->tile[d->ntiles].col = tl[t].col; d->ntiles++;
                d->bsN += tl[t].size;
            }
            if (last) {
                memset((uint8_t *)d->bs.cpu + d->bsN, 0, 64);
                d->have_hdr = false;
                const int prev = shown ? d->cur_surf : -1;
                const int r = decode_frame(d);
                if (r < 0) return -1;
                if (r == 1) { if (prev >= 0) av1dec_release(d, prev); shown = 1; }
            }
        }
        pos += o.header_len + o.payload_len;
    }
    return shown;
}
