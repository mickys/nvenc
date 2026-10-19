// nvencvt_dec.c (M8, T8.06): the H.264 decoder in libnvencvt, as a VideoToolbox decoder plugin. Interface: Apple's SPI
// declarations published in WebKit (vendor/webkit-spi/VTVideoDecoderSPI.h, CMBaseObjectSPI.h); the three class slots
// that header leaves "TBD" are given the shapes their names suggest and log every call. Decoding: src/dec.c on NVDEC.
// Registered by NVEncVT_Register next to the encoders (NVENCVT_NO_DEC=1 leaves it out), so every way the encoder gets
// into a process (NullMoth's NVMTL_NVENCVT hook, nvenc-run) brings the decoder too.
#include "dec.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
static double nowms(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1e6; }

typedef struct CMBaseProtocolVTable CMBaseProtocolVTable;
typedef struct CMBaseProtocolTable CMBaseProtocolTable;
#include "CMBaseObjectSPI.h"

typedef CMBaseObjectRef VTVideoDecoderRef;
typedef void *VTVideoDecoderSession;
typedef void *VTVideoDecoderFrame;
extern CMBaseClassID VTVideoDecoderGetClassID(void);
extern CVPixelBufferPoolRef VTDecoderSessionGetPixelBufferPool(VTVideoDecoderSession);
extern OSStatus VTDecoderSessionSetPixelBufferAttributes(VTVideoDecoderSession, CFDictionaryRef);
extern CFDictionaryRef VTDecoderSessionGetDestinationPixelBufferAttributes(VTVideoDecoderSession);
extern OSStatus VTDecoderSessionEmitDecodedFrame(VTVideoDecoderSession, VTVideoDecoderFrame, OSStatus, VTDecodeInfoFlags, CVImageBufferRef);

#pragma pack(push, 4)
typedef struct {
    uint32_t version;
    OSStatus (*startSession)(VTVideoDecoderRef, VTVideoDecoderSession, CMVideoFormatDescriptionRef);
    OSStatus (*decodeFrame)(VTVideoDecoderRef, VTVideoDecoderFrame, CMSampleBufferRef, VTDecodeFrameFlags, VTDecodeInfoFlags *);
    OSStatus (*copySupportedPropertyDictionary)(VTVideoDecoderRef, CFDictionaryRef *);
    OSStatus (*setProperties)(VTVideoDecoderRef, CFDictionaryRef);
    OSStatus (*copySerializableProperties)(VTVideoDecoderRef, CFAllocatorRef, CFDictionaryRef *);
    Boolean  (*canAcceptFormatDescription)(VTVideoDecoderRef, CMFormatDescriptionRef);
    OSStatus (*finishDelayedFrames)(VTVideoDecoderRef);
    void *reserved[3];
} DecoderClass;
#pragma pack(pop)
typedef struct { CMBaseVTable base; const DecoderClass *decoderClass; } DecoderVTable;

#define DECODER_ID "org.nvenc.videodecoder.avc"
static bool gDLog; static FILE *gDLogF;
#define DLOG(...) do { if (gDLog) { FILE *lf_ = gDLogF ? gDLogF : stderr; fprintf(lf_, "nvdecvt: " __VA_ARGS__); fputc('\n', lf_); fflush(lf_); } } while (0)

typedef struct {
    pthread_mutex_t lock;
    VTVideoDecoderSession session;
    CMVideoFormatDescriptionRef fmt;
    Rm rm; H264Dec dec; bool opened;
    int nalLen, w, h;
    CFDictionaryRef colour;         // colour attachments for the output (from the format description)
    unsigned frames, errors;
    double tIn, tEngine, tPool, tCopy, tEmit, tOut, tLast;   // ms: inside decodeFrame, engine wait, pool, untile, emit; between calls
    uint8_t *tmp; size_t tmpCap;    // a non-contiguous sample, flattened
} DState;

static DState *dstate(VTVideoDecoderRef o) { return *(DState **)CMBaseObjectGetDerivedStorage(o); }

// ---- base object ----
static OSStatus b_invalidate(CMBaseObjectRef o)
{
    DState *s = dstate(o); if (!s) return noErr;
    pthread_mutex_lock(&s->lock);
    if (s->opened) { h264dec_close(&s->dec); rm_close(&s->rm); s->opened = false; }
    if (s->fmt) { CFRelease(s->fmt); s->fmt = NULL; }
    if (s->colour) { CFRelease(s->colour); s->colour = NULL; }
    if (s->frames) DLOG("per frame: decodeFrame %.2f ms (engine %.2f, pool %.2f, copy %.2f, emit %.2f), outside %.2f ms",
         s->tIn / s->frames, s->tEngine / s->frames, s->tPool / s->frames, s->tCopy / s->frames, s->tEmit / s->frames, s->tOut / s->frames);
    DLOG("invalidate after %u frames, %u errors", s->frames, s->errors);
    pthread_mutex_unlock(&s->lock);
    return noErr;
}
static void b_finalize(CMBaseObjectRef o)
{
    DState *s = dstate(o); if (!s) return;
    b_invalidate(o); pthread_mutex_destroy(&s->lock); free(s->tmp); free(s);
    *(DState **)CMBaseObjectGetDerivedStorage(o) = NULL;
}
static CFStringRef b_desc(CMBaseObjectRef o) { (void)o; return CFSTR("nvenc H.264 decoder (NVDEC)"); }
static OSStatus b_copyProperty(CMBaseObjectRef o, CFStringRef key, CFAllocatorRef al, void *out)
{
    (void)o; (void)al;
    if (CFEqual(key, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder)) { *(CFTypeRef *)out = kCFBooleanTrue; return noErr; }
    if (gDLog) { char k[128]; CFStringGetCString(key, k, sizeof k, kCFStringEncodingUTF8); DLOG("copy %s: not available", k); }
    return kCMBaseObjectError_ValueNotAvailable;
}
static OSStatus b_setProperty(CMBaseObjectRef o, CFStringRef key, CFTypeRef v)
{
    (void)o; (void)v;
    if (gDLog) { char k[128]; CFStringGetCString(key, k, sizeof k, kCFStringEncodingUTF8); DLOG("set %s (ignored)", k); }
    return noErr;
}

// ---- decoder class ----
static OSStatus d_startSession(VTVideoDecoderRef o, VTVideoDecoderSession session, CMVideoFormatDescriptionRef fmt)
{
    DState *s = dstate(o);
    pthread_mutex_lock(&s->lock);
    s->session = session; s->fmt = (CMVideoFormatDescriptionRef)CFRetain(fmt);
    size_t count = 0; int nalLen = 4;
    OSStatus st = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 0, NULL, NULL, &count, &nalLen);
    if (st || !count) { DLOG("startSession: no H.264 parameter sets (%d)", (int)st); pthread_mutex_unlock(&s->lock); return kVTVideoDecoderBadDataErr; }
    s->nalLen = nalLen;
    // size the session from the SPS (coded size), check what NVDEC on Ada decodes: 8-bit 4:2:0
    H264Params *pp = calloc(1, sizeof *pp); int sps = -1;
    for (size_t i = 0; i < count; i++) {
        const uint8_t *ps; size_t n;
        if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, i, &ps, &n, NULL, NULL) || !n) continue;
        if ((ps[0] & 31) == H264_NAL_SPS && !h264_parse_sps(pp, ps, n)) for (int k = 0; k < 32; k++) if (pp->sps[k].valid) { sps = k; break; }
    }
    if (sps < 0) { free(pp); pthread_mutex_unlock(&s->lock); return kVTVideoDecoderBadDataErr; }
    const H264Sps *sp = &pp->sps[sps];
    const unsigned W = (unsigned)(sp->pic_width_in_mbs_minus1 + 1) * 16, H = (unsigned)(sp->pic_height_in_map_units_minus1 + 1) * 16 * (2u - (unsigned)sp->frame_mbs_only_flag);
    const bool ok = sp->chroma_format_idc == 1 && sp->bit_depth_luma_minus8 == 0 && sp->bit_depth_chroma_minus8 == 0 && W <= 4096 && H <= 4096;
    s->w = sp->width; s->h = sp->height;
    const bool full = sp->video_full_range_flag;
    DLOG("startSession: %dx%d (coded %ux%u) profile %d chroma %d depth %d %s", s->w, s->h, W, H, sp->profile_idc, sp->chroma_format_idc,
         sp->bit_depth_luma_minus8 + 8, ok ? "" : "- NOT SUPPORTED");
    free(pp);
    if (!ok) { pthread_mutex_unlock(&s->lock); return kVTVideoDecoderUnsupportedDataFormatErr; }
    if (rm_open(&s->rm) || rm_va_init(&s->rm) || h264dec_open(&s->dec, &s->rm, W, H)) {
        DLOG("startSession: NVDEC session failed"); pthread_mutex_unlock(&s->lock); return kVTVideoDecoderMalfunctionErr;
    }
    s->opened = true;
    for (size_t i = 0; i < count; i++) {                        // the decoder keeps its own parameter sets
        const uint8_t *ps; size_t n;
        if (!CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, i, &ps, &n, NULL, NULL) && n) h264dec_nal(&s->dec, ps, n);
    }
    if (gDLog) {   // what the client asked for (T8.06: matching it avoids a conversion inside EmitDecodedFrame)
        CFDictionaryRef dst = VTDecoderSessionGetDestinationPixelBufferAttributes(session);
        CFStringRef ds = dst ? CFCopyDescription(dst) : NULL; char buf[2048] = "(none)";
        if (ds) { CFStringGetCString(ds, buf, sizeof buf, kCFStringEncodingUTF8); CFRelease(ds); }
        for (char *c = buf; *c; c++) if (*c == '\n') *c = ' ';
        DLOG("destination attributes: %s", buf);
    }
    // NV12, the range the stream says; colour attachments as the format description has them
    const OSType pf = full ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef npf = CFNumberCreate(NULL, kCFNumberSInt32Type, &pf), nw = CFNumberCreate(NULL, kCFNumberIntType, &s->w), nh = CFNumberCreate(NULL, kCFNumberIntType, &s->h);
    CFDictionaryRef ios = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *k[] = { kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferWidthKey, kCVPixelBufferHeightKey, kCVPixelBufferIOSurfacePropertiesKey };
    const void *v[] = { npf, nw, nh, ios };
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, k, v, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    // When the client asks for the format we produce, hand out buffers made exactly to its attributes: otherwise
    // VideoToolbox copies every frame into a compatible one inside EmitDecodedFrame (ffmpeg asks for OpenGL-compatible
    // IOSurfaces: 18.7 ms per 4K frame).
    CFDictionaryRef dst = VTDecoderSessionGetDestinationPixelBufferAttributes(session);
    CFNumberRef dpf = dst ? CFDictionaryGetValue(dst, kCVPixelBufferPixelFormatTypeKey) : NULL; OSType want = 0;
    if (!getenv("NVDEC_OWNATTRS") && dpf && CFGetTypeID(dpf) == CFNumberGetTypeID() && CFNumberGetValue(dpf, kCFNumberSInt32Type, &want) && want == pf) {
        CFMutableDictionaryRef m = CFDictionaryCreateMutableCopy(NULL, 0, dst);
        CFDictionarySetValue(m, kCVPixelBufferWidthKey, nw); CFDictionarySetValue(m, kCVPixelBufferHeightKey, nh);
        if (!CFDictionaryContainsKey(m, kCVPixelBufferIOSurfacePropertiesKey)) CFDictionarySetValue(m, kCVPixelBufferIOSurfacePropertiesKey, ios);
        CFRelease(attrs); attrs = m;
    }
    if (!getenv("NVDEC_NOMETAL")) {   // MediaToolbox's conformer copies every frame that is not Metal-compatible (T8.06)
        CFMutableDictionaryRef m = CFDictionaryCreateMutableCopy(NULL, 0, attrs);
        CFDictionarySetValue(m, kCVPixelBufferMetalCompatibilityKey, kCFBooleanTrue);
        CFRelease(attrs); attrs = m;
    }
    st = VTDecoderSessionSetPixelBufferAttributes(session, attrs);
    CFRelease(attrs); CFRelease(ios); CFRelease(npf); CFRelease(nw); CFRelease(nh);
    CFMutableDictionaryRef col = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const CFStringRef ck[] = { kCMFormatDescriptionExtension_ColorPrimaries, kCMFormatDescriptionExtension_TransferFunction, kCMFormatDescriptionExtension_YCbCrMatrix };
    for (int i = 0; i < 3; i++) { CFTypeRef x = CMFormatDescriptionGetExtension(fmt, ck[i]); if (x) CFDictionarySetValue(col, ck[i], x); }
    s->colour = col;
    DLOG("startSession: pixel buffer attributes -> %d (%s range)", (int)st, full ? "full" : "video");
    pthread_mutex_unlock(&s->lock);
    return noErr;
}

static OSStatus d_decodeFrame(VTVideoDecoderRef o, VTVideoDecoderFrame frame, CMSampleBufferRef sb, VTDecodeFrameFlags flags, VTDecodeInfoFlags *info)
{
    DState *s = dstate(o); (void)flags;
    if (info) *info = 0;
    const double tA = nowms();
    pthread_mutex_lock(&s->lock);
    if (s->tLast > 0) s->tOut += tA - s->tLast;
    OSStatus res = noErr; CVPixelBufferRef pb = NULL;
    CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
    size_t len = bb ? CMBlockBufferGetDataLength(bb) : 0, avail = 0; char *p = NULL;
    if (!len || !s->opened) res = kVTVideoDecoderBadDataErr;
    else if (CMBlockBufferGetDataPointer(bb, 0, &avail, NULL, &p) || avail < len) {
        if (s->tmpCap < len) { free(s->tmp); s->tmp = malloc(len); s->tmpCap = s->tmp ? len : 0; }
        if (!s->tmp || CMBlockBufferCopyDataBytes(bb, 0, len, s->tmp)) res = kVTVideoDecoderBadDataErr; else p = (char *)s->tmp;
    }
    int r = 0;
    if (!res) {
        size_t pos = 0; const uint8_t *nal; size_t n;
        while (r >= 0 && h264_next_lenpfx((const uint8_t *)p, len, s->nalLen, &pos, &nal, &n)) { int q = h264dec_nal(&s->dec, nal, n); if (q) r = q; }
        if (r >= 0) { int q = h264dec_flush(&s->dec); if (q) r = q; }   // one sample is one access unit
        if (r < 0) res = kVTVideoDecoderBadDataErr;
    }
    if (!res && r == 1) {
        s->tEngine += s->dec.engine_ms; double tp = nowms();
        CVPixelBufferPoolRef pool = VTDecoderSessionGetPixelBufferPool(s->session); OSStatus cst = pool ? CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pb) : -1;
        if (!cst && pb && gDLog && s->frames == 0) {   // does the client consider our buffer compatible? (else VT transfers every frame)
            typedef Boolean (*CompatFn)(CVPixelBufferRef, CFDictionaryRef);
            CompatFn compat = (CompatFn)dlsym(RTLD_DEFAULT, "CVPixelBufferIsCompatibleWithAttributes");
            CFDictionaryRef dst = VTDecoderSessionGetDestinationPixelBufferAttributes(s->session);
            if (compat && dst) {
                DLOG("our buffer compatible with the destination attributes: %d", (int)compat(pb, dst));
                CFDictionaryRef pa = pool ? CVPixelBufferPoolGetPixelBufferAttributes(pool) : NULL;
                CFStringRef ds = pa ? CFCopyDescription(pa) : NULL; char buf[1024] = "(none)";
                if (ds) { CFStringGetCString(ds, buf, sizeof buf, kCFStringEncodingUTF8); CFRelease(ds); }
                for (char *c = buf; *c; c++) if (*c == '\n') *c = ' ';
                DLOG("  pool attributes: %s", buf);
                CFDictionaryRef ios = CVBufferCopyAttachment(pb, kCVPixelBufferIOSurfacePropertiesKey, NULL);
                DLOG("  buffer: IOSurface %p, bytes per row %zu/%zu", CVPixelBufferGetIOSurface(pb), CVPixelBufferGetBytesPerRowOfPlane(pb, 0), CVPixelBufferGetBytesPerRowOfPlane(pb, 1));
                if (ios) CFRelease(ios);
                CFIndex n = CFDictionaryGetCount(dst); const void **ks = malloc(sizeof(void *) * (size_t)n), **vs = malloc(sizeof(void *) * (size_t)n);
                CFDictionaryGetKeysAndValues(dst, ks, vs);
                for (CFIndex i = 0; i < n; i++) {
                    CFDictionaryRef one = CFDictionaryCreate(NULL, &ks[i], &vs[i], 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                    char kn[128]; CFStringGetCString(ks[i], kn, sizeof kn, kCFStringEncodingUTF8);
                    DLOG("  compatible with {%s} alone: %d", kn, (int)compat(pb, one)); CFRelease(one);
                }
                free(ks); free(vs);
            } else DLOG("CVPixelBufferIsCompatibleWithAttributes %s", compat ? "found, no destination attributes" : "not found");
        }
        if (cst || !pb) res = kVTVideoDecoderMalfunctionErr;
        else {
            double tc = nowms(); s->tPool += tc - tp;
            CVPixelBufferLockBaseAddress(pb, 0);
            h264dec_read_nv12(&s->dec, s->dec.cur_surf, CVPixelBufferGetBaseAddressOfPlane(pb, 0), CVPixelBufferGetBytesPerRowOfPlane(pb, 0),
                              CVPixelBufferGetBaseAddressOfPlane(pb, 1), CVPixelBufferGetBytesPerRowOfPlane(pb, 1),
                              (unsigned)CVPixelBufferGetWidth(pb), (unsigned)CVPixelBufferGetHeight(pb));
            CVPixelBufferUnlockBaseAddress(pb, 0);
            if (s->colour && !getenv("NVDEC_NOCOLOUR")) CVBufferSetAttachments(pb, s->colour, kCVAttachmentMode_ShouldPropagate);
            s->tCopy += nowms() - tc;
        }
        h264dec_release(&s->dec, s->dec.cur_surf);
        s->frames++;
    }
    if (res) s->errors++;
    if (gDLog && (s->frames <= 3 || res)) DLOG("decodeFrame %p: %zu bytes -> %d, picture %d, %u MBs ok, %u err, %.2f ms", frame, len, (int)res, r,
                                              s->dec.mbs_ok, s->dec.mbs_err, s->dec.engine_ms);
    const double te = nowms();
    VTDecoderSessionEmitDecodedFrame(s->session, frame, res, (!res && r != 1) ? kVTDecodeInfo_FrameDropped : 0, pb);
    if (pb) CVPixelBufferRelease(pb);
    s->tLast = nowms(); s->tEmit += s->tLast - te; s->tIn += s->tLast - tA;
    pthread_mutex_unlock(&s->lock);
    return res;
}

static OSStatus d_copySupported(VTVideoDecoderRef o, CFDictionaryRef *out)
{
    (void)o;
    // VideoToolbox forwards a property query to the decoder only for keys listed here (as with the encoder)
    const void *pk[] = { kVTPropertyTypeKey, kVTPropertyReadWriteStatusKey };
    const void *pv[] = { kVTPropertyType_Boolean, kVTPropertyReadWriteStatus_ReadOnly };
    CFDictionaryRef ro = CFDictionaryCreate(NULL, pk, pv, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *k[] = { kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder };
    const void *v[] = { ro };
    *out = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(ro);
    return noErr;
}
static OSStatus d_setProperties(VTVideoDecoderRef o, CFDictionaryRef d) { (void)o; DLOG("setProperties %p", d); return noErr; }
static OSStatus d_copySerializable(VTVideoDecoderRef o, CFAllocatorRef al, CFDictionaryRef *out)
{
    (void)o; DLOG("copySerializableProperties allocator %p out %p", al, out);
    if (out) *out = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    return noErr;
}
static Boolean d_canAccept(VTVideoDecoderRef o, CMFormatDescriptionRef f) { (void)o; DLOG("canAcceptFormatDescription %p: no", f); return false; }
static OSStatus d_finishDelayed(VTVideoDecoderRef o) { (void)o; DLOG("finishDelayedFrames"); return noErr; }

static CMBaseClass gDBase = { .version = 1, .derivedStorageSize = sizeof(void *), .invalidate = b_invalidate, .finalize = b_finalize,
                              .copyDebugDescription = b_desc, .copyProperty = b_copyProperty, .setProperty = b_setProperty };
static const DecoderClass gDec = { .version = 1, .startSession = d_startSession, .decodeFrame = d_decodeFrame,
                                   .copySupportedPropertyDictionary = d_copySupported, .setProperties = d_setProperties,
                                   .copySerializableProperties = d_copySerializable, .canAcceptFormatDescription = d_canAccept,
                                   .finishDelayedFrames = d_finishDelayed };
static const DecoderVTable gDVT = { .base = { NULL, &gDBase }, .decoderClass = &gDec };

__attribute__((visibility("default"))) OSStatus NVDecVT_CreateInstance(FourCharCode codec, CFAllocatorRef al, VTVideoDecoderRef *out)
{
    if (codec != kCMVideoCodecType_H264) return kVTCouldNotFindVideoDecoderErr;
    CMBaseObjectRef obj = NULL;
    OSStatus st = CMDerivedObjectCreate(al, &gDVT.base, VTVideoDecoderGetClassID(), &obj);
    if (st) return st;
    DState *s = calloc(1, sizeof *s);
    pthread_mutex_init(&s->lock, NULL);
    *(DState **)CMBaseObjectGetDerivedStorage(obj) = s;
    *out = obj;
    DLOG("instance %p (avc1)", obj);
    return noErr;
}

// Called by NVEncVT_Register once the GPU checked out (Ada). Rated above NullMoth's own H.264 decoder (1000).
OSStatus NVDecVT_Register(int rating)
{
    gDLog = getenv("NVENCVT_LOG") != NULL;
    if (!gDLog && access("/tmp/nvencvt.log", F_OK) == 0) { char p[64]; snprintf(p, sizeof p, "/tmp/nvencvt-%d.log", (int)getpid()); if ((gDLogF = fopen(p, "a"))) gDLog = true; }
    if (getenv("NVENCVT_NO_DEC")) return kVTCouldNotFindVideoDecoderErr;
    typedef OSStatus (*RegFn)(FourCharCode, CFDictionaryRef, void *);
    RegFn reg = (RegFn)dlsym(RTLD_DEFAULT, "VTRegisterVideoDecoderWithInfo");
    if (!reg) { DLOG("VTRegisterVideoDecoderWithInfo not found"); return kVTCouldNotFindVideoDecoderErr; }
    CFNumberRef nr = CFNumberCreate(NULL, kCFNumberIntType, &rating);
    const void *k[] = { CFSTR("CMClassImplementationID"), CFSTR("CMClassImplementationName"), CFSTR("VTRating"), CFSTR("VTIsHardwareAccelerated") };
    const void *v[] = { CFSTR(DECODER_ID), CFSTR("NVDEC - H.264 - mickys/nvenc"), nr, kCFBooleanTrue };
    CFDictionaryRef info = CFDictionaryCreate(NULL, k, v, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    OSStatus st = reg(kCMVideoCodecType_H264, info, (void *)NVDecVT_CreateInstance);
    CFRelease(info); CFRelease(nr);
    DLOG("registered %s -> %d (rating %d)", DECODER_ID, (int)st, rating);
    return st;
}
