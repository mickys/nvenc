// nvencvt.c: a VideoToolbox H.264 encoder on top of src/enc.c (M4). Interface: docs/vt-encoder.md.
// Built as libnvencvt.dylib. Injected, it registers the encoder in the process (constructor), e.g.
//   DYLD_INSERT_LIBRARIES=~/nvenc-dev/libnvencvt.dylib ./vttest hw avc1 1920 1080 60
// Loaded otherwise (the Metal plugin with NVMTL_NVENC_IMPL=nvenc), call NVEncVT_Register(rating), or register
// NVEncVT_CreateInstance as the factory yourself. Environment: NVENCVT_RATING (default 1100,
// above nullmoth's 1000), NVENCVT_NO_REGISTER, NVENCVT_LOG.
#include "enc.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- CMBaseObject / encoder class declarations (WebKit's CMBaseObjectSPI.h, written for C++) ----
typedef struct CMBaseProtocolVTable CMBaseProtocolVTable;
typedef struct CMBaseProtocolTable CMBaseProtocolTable;
#include "CMBaseObjectSPI.h"
typedef CMBaseObjectRef VTVideoEncoderRef;
typedef void *VTEncoderSessionRef;
typedef void *VTEncoderFrameRef;
extern CMBaseClassID VTVideoEncoderGetClassID(void);
extern OSStatus VTRegisterVideoEncoderWithInfo(FourCharCode, CFDictionaryRef, void *);
extern OSStatus VTEncoderSessionEmitEncodedFrame(VTEncoderSessionRef, VTEncoderFrameRef, OSStatus, VTEncodeInfoFlags, CMSampleBufferRef);
extern OSStatus VTEncoderSessionSetPixelBufferAttributes(VTEncoderSessionRef, CFDictionaryRef);

#pragma pack(push, 4)
typedef struct {
    uint32_t version;
    OSStatus (*startSession)(VTVideoEncoderRef, VTEncoderSessionRef, CMVideoDimensions);
    OSStatus (*encodeFrame)(VTVideoEncoderRef, VTEncoderFrameRef, CVImageBufferRef, CMTime, CMTime, CFDictionaryRef, VTEncodeInfoFlags *);
    OSStatus (*completeFrames)(VTVideoEncoderRef, CMTime);
    OSStatus (*copySupportedPropertyDictionary)(VTVideoEncoderRef, CFDictionaryRef *);
    void *reserved[8];
} EncoderClass;
#pragma pack(pop)
typedef struct { CMBaseVTable base; const EncoderClass *encoderClass; } EncoderVTable;

#define ENCODER_ID "org.nvenc.videoencoder.avc"
static bool gLog;
#define LOG(...) do { if (gLog) { fprintf(stderr, "nvencvt: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

typedef struct {
    pthread_mutex_t lock;
    VTEncoderSessionRef session;
    int width, height;
    bool opened, failed;
    int engine;
    Enc enc;
    CMVideoFormatDescriptionRef fmt;
    // properties
    int bitrate, maxKeyInterval, fps, quality;
    bool realTime;
    unsigned frames;
    // frames in flight: encodeFrame submits, the completion thread waits (in order) and emits
    struct { VTEncoderFrameRef frame; CMTime pts, dur; NvU32 ticket; bool ok; } q[ENC_MAX_DEPTH];
    unsigned qh, qt, depth;
    pthread_t thread; bool threadOn, stop;
    pthread_cond_t cv;
} State;

static int gEngineUse[2];   // active sessions per NVENC engine in this process (guarded by gEngineLock)
static pthread_mutex_t gEngineLock = PTHREAD_MUTEX_INITIALIZER;

static State *state(VTVideoEncoderRef e) { return *(State **)CMBaseObjectGetDerivedStorage(e); }

// ---- properties ----
static CFDictionaryRef gSupported;
static CFDictionaryRef supported(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    void build(void);
    pthread_once(&once, build);
    return gSupported;
}
static CFDictionaryRef prop(CFStringRef type, CFStringRef rw)
{
    const void *k[] = { kVTPropertyTypeKey, kVTPropertyReadWriteStatusKey }, *v[] = { type, rw };
    return CFDictionaryCreate(NULL, k, v, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
}
void build(void)
{
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef num = prop(kVTPropertyType_Number, kVTPropertyReadWriteStatus_ReadWrite);
    CFDictionaryRef bol = prop(kVTPropertyType_Boolean, kVTPropertyReadWriteStatus_ReadWrite);
    CFDictionaryRef enm = prop(kVTPropertyType_Enumeration, kVTPropertyReadWriteStatus_ReadWrite);
    CFDictionaryRef numRO = prop(kVTPropertyType_Number, kVTPropertyReadWriteStatus_ReadOnly);
    CFDictionarySetValue(d, kVTCompressionPropertyKey_AverageBitRate, num);
    CFDictionarySetValue(d, kVTCompressionPropertyKey_MaxKeyFrameInterval, num);
    CFDictionarySetValue(d, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, num);
    CFDictionarySetValue(d, kVTCompressionPropertyKey_ExpectedFrameRate, num);
    CFDictionarySetValue(d, kVTCompressionPropertyKey_Quality, num);
    CFDictionarySetValue(d, kVTCompressionPropertyKey_RealTime, bol);
    CFDictionarySetValue(d, kVTCompressionPropertyKey_AllowFrameReordering, bol);
    CFDictionarySetValue(d, kVTCompressionPropertyKey_ProfileLevel, enm);
    CFDictionarySetValue(d, kVTCompressionPropertyKey_H264EntropyMode, enm);
    CFDictionarySetValue(d, kVTCompressionPropertyKey_DataRateLimits, prop(kVTPropertyType_Enumeration, kVTPropertyReadWriteStatus_ReadWrite));
    CFDictionarySetValue(d, kVTCompressionPropertyKey_NumberOfPendingFrames, numRO);
    CFDictionarySetValue(d, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder, prop(kVTPropertyType_Boolean, kVTPropertyReadWriteStatus_ReadOnly));
    gSupported = d;
}

static int num(CFTypeRef v, int dflt)
{
    int x = dflt;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) { double f; CFNumberGetValue(v, kCFNumberDoubleType, &f); x = (int)f; }
    return x;
}

static OSStatus b_setProperty(CMBaseObjectRef o, CFStringRef key, CFTypeRef v)
{
    State *s = state(o);
    pthread_mutex_lock(&s->lock);
    OSStatus st = noErr;
    if (CFEqual(key, kVTCompressionPropertyKey_AverageBitRate)) s->bitrate = num(v, 0);
    else if (CFEqual(key, kVTCompressionPropertyKey_MaxKeyFrameInterval)) s->maxKeyInterval = num(v, 0);
    else if (CFEqual(key, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration)) { if (!s->maxKeyInterval && num(v, 0) > 0) s->maxKeyInterval = num(v, 0) * (s->fps ? s->fps : 60); }
    else if (CFEqual(key, kVTCompressionPropertyKey_ExpectedFrameRate)) s->fps = num(v, 0);
    else if (CFEqual(key, kVTCompressionPropertyKey_Quality)) { double q = 0; if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue(v, kCFNumberDoubleType, &q); s->quality = (int)(q * 100); }
    else if (CFEqual(key, kVTCompressionPropertyKey_RealTime)) s->realTime = v && CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue(v);
    else if (CFEqual(key, kVTCompressionPropertyKey_AllowFrameReordering)) { if (v && CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue(v)) LOG("frame reordering requested: not supported, no B-frames"); }
    else if (CFEqual(key, kVTCompressionPropertyKey_ProfileLevel) || CFEqual(key, kVTCompressionPropertyKey_H264EntropyMode) ||
             CFEqual(key, kVTCompressionPropertyKey_DataRateLimits)) { /* accepted: Main profile, CAVLC, rate limits from AverageBitRate */ }
    else st = kVTPropertyNotSupportedErr;
    if (gLog) { CFStringRef d = v ? CFCopyDescription(v) : NULL; char k[128], vb[256] = "";
        CFStringGetCString(key, k, sizeof k, kCFStringEncodingUTF8); if (d) { CFStringGetCString(d, vb, sizeof vb, kCFStringEncodingUTF8); CFRelease(d); }
        LOG("set %s = %s -> %d", k, vb, (int)st); }
    pthread_mutex_unlock(&s->lock);
    return st;
}

static OSStatus b_copyProperty(CMBaseObjectRef o, CFStringRef key, CFAllocatorRef al, void *out)
{
    State *s = state(o); CFTypeRef v = NULL; int n;
    if (CFEqual(key, kVTCompressionPropertyKey_AverageBitRate)) { n = s->bitrate; v = CFNumberCreate(al, kCFNumberIntType, &n); }
    else if (CFEqual(key, kVTCompressionPropertyKey_MaxKeyFrameInterval)) { n = s->maxKeyInterval; v = CFNumberCreate(al, kCFNumberIntType, &n); }
    else if (CFEqual(key, kVTCompressionPropertyKey_ExpectedFrameRate)) { n = s->fps; v = CFNumberCreate(al, kCFNumberIntType, &n); }
    else if (CFEqual(key, kVTCompressionPropertyKey_RealTime)) v = CFRetain(s->realTime ? kCFBooleanTrue : kCFBooleanFalse);
    else if (CFEqual(key, kVTCompressionPropertyKey_AllowFrameReordering)) v = CFRetain(kCFBooleanFalse);
    else if (CFEqual(key, kVTCompressionPropertyKey_ProfileLevel)) v = CFRetain(kVTProfileLevel_H264_Main_AutoLevel);
    else if (CFEqual(key, kVTCompressionPropertyKey_H264EntropyMode)) v = CFRetain(kVTH264EntropyMode_CAVLC);
    else if (CFEqual(key, kVTCompressionPropertyKey_NumberOfPendingFrames)) { n = 0; v = CFNumberCreate(al, kCFNumberIntType, &n); }
    else if (CFEqual(key, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder)) v = CFRetain(kCFBooleanTrue);
    else return kCMBaseObjectError_ValueNotAvailable;
    *(CFTypeRef *)out = v;
    return noErr;
}

// ---- encoder functions ----
static OSStatus e_startSession(VTVideoEncoderRef e, VTEncoderSessionRef session, CMVideoDimensions dims)
{
    State *s = state(e);
    s->session = session; s->width = dims.width; s->height = dims.height;
    // NV12 in; VideoToolbox converts anything else (BGRA from screen capture, ...) before encodeFrame
    int f1 = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, f2 = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
    CFNumberRef n1 = CFNumberCreate(NULL, kCFNumberIntType, &f1), n2 = CFNumberCreate(NULL, kCFNumberIntType, &f2);
    const void *fv[] = { n1, n2 }; CFArrayRef fmts = CFArrayCreate(NULL, fv, 2, &kCFTypeArrayCallBacks);
    CFNumberRef w = CFNumberCreate(NULL, kCFNumberIntType, &s->width), h = CFNumberCreate(NULL, kCFNumberIntType, &s->height);
    CFDictionaryRef ios = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *k[] = { kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferWidthKey, kCVPixelBufferHeightKey, kCVPixelBufferIOSurfacePropertiesKey };
    const void *v[] = { fmts, w, h, ios };
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, k, v, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    OSStatus st = VTEncoderSessionSetPixelBufferAttributes(session, attrs);
    CFRelease(attrs); CFRelease(ios); CFRelease(w); CFRelease(h); CFRelease(fmts); CFRelease(n1); CFRelease(n2);
    LOG("startSession %dx%d (pixel buffer attributes -> %d)", s->width, s->height, (int)st);
    return noErr;
}

static void *completion(void *arg);
static OSStatus open_encoder(State *s)
{
    EncConfig c = { .width = s->width, .height = s->height, .num_refs = 1, .fps = s->fps > 0 ? s->fps : 60,
                    .idr_period = s->maxKeyInterval > 0 ? s->maxKeyInterval : 0 };
    // constant QP from Quality (0..1) when there is no bitrate; else CBR (MB-row), one-frame VBV in real time mode
    int qp = s->quality ? 40 - s->quality * 22 / 100 : 23;
    c.qp_i = qp; c.qp_p = qp + 2;
    if (s->bitrate > 0) {
        c.rc_mode = 18; c.bitrate = s->bitrate; c.max_bitrate = s->bitrate;
        c.vbv_bits = s->realTime ? s->bitrate / c.fps * 2 : s->bitrate;
    }
    c.depth = getenv("NVENCVT_DEPTH") ? atoi(getenv("NVENCVT_DEPTH")) : 2;   // frames in flight
    // the engine with fewer sessions in this process (a second session runs on NVENC1, in parallel)
    pthread_mutex_lock(&gEngineLock);
    c.engine = gEngineUse[1] < gEngineUse[0] ? 1 : 0;
    if (getenv("NVENCVT_ENGINE")) c.engine = atoi(getenv("NVENCVT_ENGINE")) & 1;
    gEngineUse[c.engine]++; s->engine = c.engine;
    pthread_mutex_unlock(&gEngineLock);
    if (enc_open(&s->enc, &c)) {
        enc_close(&s->enc);
        pthread_mutex_lock(&gEngineLock); gEngineUse[s->engine]--; pthread_mutex_unlock(&gEngineLock);
        return kVTVideoEncoderMalfunctionErr;
    }
    s->depth = (unsigned)s->enc.cfg.depth;
    uint8_t hdr[256]; size_t n = enc_headers(&s->enc, hdr);
    // SPS and PPS from the Annex B header block
    const uint8_t *ps[2]; size_t pl[2]; int cnt = 0;
    for (size_t i = 0; i + 4 <= n && cnt < 2; i++)
        if (!hdr[i] && !hdr[i + 1] && !hdr[i + 2] && hdr[i + 3] == 1) { ps[cnt++] = hdr + i + 4; }
    pl[0] = (size_t)(ps[1] - 4 - ps[0]); pl[1] = (size_t)(hdr + n - ps[1]);
    OSStatus st = CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, 2, ps, pl, 4, &s->fmt);
    LOG("encoder open on NVENC%d: %dx%d rc_mode %d bitrate %d vbv %d qp %d/%d idr_period %d fps %d -> fmt %d", c.engine, c.width, c.height, c.rc_mode, c.bitrate,
        c.vbv_bits, c.qp_i, c.qp_p, c.idr_period, c.fps, (int)st);
    if (st) { enc_close(&s->enc); return st; }
    s->opened = true;
    s->stop = false;
    s->threadOn = pthread_create(&s->thread, NULL, completion, s) == 0;
    if (!s->threadOn) { enc_close(&s->enc); s->opened = false; return kVTVideoEncoderMalfunctionErr; }
    return noErr;
}

static void emit(State *s, VTEncoderFrameRef frame, CMTime pts, CMTime dur, const EncFrame *fr, OSStatus err)
{
    if (err) { VTEncoderSessionEmitEncodedFrame(s->session, frame, err, 0, NULL); return; }
    // Annex B (4-byte start codes) -> AVCC (4-byte big-endian lengths), same size
    uint8_t *buf = malloc(fr->size);
    memcpy(buf, fr->data, fr->size);
    size_t start = SIZE_MAX;
    for (size_t i = 0; i + 4 <= fr->size; i++) {
        if (!buf[i] && !buf[i + 1] && !buf[i + 2] && buf[i + 3] == 1) {
            if (start != SIZE_MAX) { uint32_t l = (uint32_t)(i - start - 4); buf[start] = l >> 24; buf[start + 1] = l >> 16; buf[start + 2] = l >> 8; buf[start + 3] = l; }
            start = i; i += 3;
        }
    }
    if (start != SIZE_MAX) { uint32_t l = (uint32_t)(fr->size - start - 4); buf[start] = l >> 24; buf[start + 1] = l >> 16; buf[start + 2] = l >> 8; buf[start + 3] = l; }
    CMBlockBufferRef bb = NULL; CMSampleBufferRef sb = NULL;
    OSStatus st = CMBlockBufferCreateWithMemoryBlock(NULL, buf, fr->size, kCFAllocatorMalloc, NULL, 0, fr->size, 0, &bb);
    CMSampleTimingInfo ti = { .duration = dur, .presentationTimeStamp = pts, .decodeTimeStamp = kCMTimeInvalid };
    size_t sz = fr->size;
    if (!st) st = CMSampleBufferCreateReady(NULL, bb, s->fmt, 1, 1, &ti, 1, &sz, &sb);
    if (!st) {
        CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, true);
        CFMutableDictionaryRef a = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(att, 0);
        CFDictionarySetValue(a, kCMSampleAttachmentKey_NotSync, fr->idr ? kCFBooleanFalse : kCFBooleanTrue);
        CFDictionarySetValue(a, kCMSampleAttachmentKey_DependsOnOthers, fr->idr ? kCFBooleanFalse : kCFBooleanTrue);
    }
    VTEncoderSessionEmitEncodedFrame(s->session, frame, st, 0, st ? NULL : sb);
    if (sb) CFRelease(sb);
    if (bb) CFRelease(bb);
}

static void *completion(void *arg)
{
    State *s = arg;
    pthread_mutex_lock(&s->lock);
    for (;;) {
        while (s->qh == s->qt && !s->stop) pthread_cond_wait(&s->cv, &s->lock);
        if (s->qh == s->qt) break;
        unsigned i = s->qh % s->depth;
        VTEncoderFrameRef frame = s->q[i].frame; CMTime pts = s->q[i].pts, dur = s->q[i].dur; NvU32 t = s->q[i].ticket; bool ok = s->q[i].ok;
        pthread_mutex_unlock(&s->lock);
        EncFrame fr; memset(&fr, 0, sizeof fr);
        int rc = ok ? enc_wait(&s->enc, t, &fr) : -1;
        if (rc) LOG("frame %u: encode failed (ucode error %u)", t, fr.ucode_error);
        else if (gLog && (t <= 3 || fr.idr)) LOG("frame %u %s: %zu bytes, %.3f ms, avgQP %u", t, fr.idr ? "IDR" : "P", fr.size, fr.ms, fr.avg_qp);
        emit(s, frame, pts, dur, &fr, rc ? kVTVideoEncoderMalfunctionErr : noErr);   // fr.data stays valid until qh moves on
        pthread_mutex_lock(&s->lock);
        s->qh++;
        pthread_cond_broadcast(&s->cv);
    }
    pthread_mutex_unlock(&s->lock);
    return NULL;
}

static void drain(State *s) { while (s->qh != s->qt) pthread_cond_wait(&s->cv, &s->lock); }   // lock held

static OSStatus e_encodeFrame(VTVideoEncoderRef e, VTEncoderFrameRef frame, CVImageBufferRef img, CMTime pts, CMTime dur,
                              CFDictionaryRef props, VTEncodeInfoFlags *infoOut)
{
    State *s = state(e);
    pthread_mutex_lock(&s->lock);
    OSStatus st = noErr;
    if (!s->opened && !s->failed && (st = open_encoder(s))) s->failed = true;
    if (s->failed) { pthread_mutex_unlock(&s->lock); VTEncoderSessionEmitEncodedFrame(s->session, frame, kVTVideoEncoderMalfunctionErr, 0, NULL); return kVTVideoEncoderMalfunctionErr; }
    OSType pf = CVPixelBufferGetPixelFormatType(img);
    if ((pf != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange && pf != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) ||
        (int)CVPixelBufferGetWidth(img) < s->width || (int)CVPixelBufferGetHeight(img) < s->height) {
        LOG("unexpected pixel buffer %.4s %zux%zu", (char *)&pf, CVPixelBufferGetWidth(img), CVPixelBufferGetHeight(img));
        pthread_mutex_unlock(&s->lock);
        VTEncoderSessionEmitEncodedFrame(s->session, frame, kVTPixelTransferNotSupportedErr, 0, NULL);
        return noErr;
    }
    while (s->qt - s->qh >= s->depth) pthread_cond_wait(&s->cv, &s->lock);   // a free input slot
    CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
    enc_load_nv12(&s->enc, CVPixelBufferGetBaseAddressOfPlane(img, 0), CVPixelBufferGetBytesPerRowOfPlane(img, 0),
                  CVPixelBufferGetBaseAddressOfPlane(img, 1), CVPixelBufferGetBytesPerRowOfPlane(img, 1));
    CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
    bool forceIdr = false;
    if (props) { CFTypeRef k = CFDictionaryGetValue(props, kVTEncodeFrameOptionKey_ForceKeyFrame); forceIdr = k && CFGetTypeID(k) == CFBooleanGetTypeID() && CFBooleanGetValue(k); }
    unsigned i = s->qt % s->depth;
    s->q[i].frame = frame; s->q[i].pts = pts; s->q[i].dur = dur;
    s->q[i].ok = enc_submit(&s->enc, forceIdr, &s->q[i].ticket) == 0;
    s->qt++; s->frames++;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->lock);
    if (infoOut) *infoOut = kVTEncodeInfo_Asynchronous;
    return noErr;
}

static OSStatus e_completeFrames(VTVideoEncoderRef e, CMTime until)
{
    (void)until;
    State *s = state(e);
    pthread_mutex_lock(&s->lock); drain(s); pthread_mutex_unlock(&s->lock);
    return noErr;
}
static OSStatus e_copySupported(VTVideoEncoderRef e, CFDictionaryRef *out) { (void)e; *out = CFRetain(supported()); return noErr; }

static OSStatus b_invalidate(CMBaseObjectRef o)
{
    State *s = state(o);
    pthread_mutex_lock(&s->lock);
    if (s->threadOn) {
        drain(s); s->stop = true; pthread_cond_broadcast(&s->cv);
        pthread_mutex_unlock(&s->lock); pthread_join(s->thread, NULL); pthread_mutex_lock(&s->lock);
        s->threadOn = false;
    }
    if (s->opened) {
        enc_close(&s->enc); s->opened = false;
        pthread_mutex_lock(&gEngineLock); gEngineUse[s->engine]--; pthread_mutex_unlock(&gEngineLock);
    }
    if (s->fmt) { CFRelease(s->fmt); s->fmt = NULL; }
    LOG("invalidate after %u frames", s->frames);
    pthread_mutex_unlock(&s->lock);
    return noErr;
}
static void b_finalize(CMBaseObjectRef o)
{
    State *s = state(o);
    if (!s) return;
    b_invalidate(o);
    pthread_mutex_destroy(&s->lock); pthread_cond_destroy(&s->cv);
    free(s); *(State **)CMBaseObjectGetDerivedStorage(o) = NULL;
}
static CFStringRef b_desc(CMBaseObjectRef o) { (void)o; return CFSTR("nvenc H.264 encoder"); }

static CMBaseClass gBase = { .version = 1, .derivedStorageSize = sizeof(void *), .invalidate = b_invalidate, .finalize = b_finalize,
                             .copyDebugDescription = b_desc, .copyProperty = b_copyProperty, .setProperty = b_setProperty };
static const EncoderClass gEnc = { .version = 1, .startSession = e_startSession, .encodeFrame = e_encodeFrame,
                                   .completeFrames = e_completeFrames, .copySupportedPropertyDictionary = e_copySupported };
static const EncoderVTable gVT = { .base = { NULL, &gBase }, .encoderClass = &gEnc };

__attribute__((visibility("default"))) OSStatus NVEncVT_CreateInstance(FourCharCode codec, CFAllocatorRef al, VTVideoEncoderRef *out)
{
    if (codec != kCMVideoCodecType_H264) return kVTCouldNotFindVideoEncoderErr;
    CMBaseObjectRef obj = NULL;
    OSStatus st = CMDerivedObjectCreate(al, &gVT.base, VTVideoEncoderGetClassID(), &obj);
    if (st) return st;
    State *s = calloc(1, sizeof *s);
    pthread_mutex_init(&s->lock, NULL); pthread_cond_init(&s->cv, NULL);
    *(State **)CMBaseObjectGetDerivedStorage(obj) = s;
    *out = obj;
    LOG("instance %p", obj);
    return noErr;
}

__attribute__((visibility("default"))) OSStatus NVEncVT_Register(int rating)
{
    CFNumberRef r = CFNumberCreate(NULL, kCFNumberIntType, &rating);
    const void *k[] = { CFSTR("CMClassImplementationID"), CFSTR("CMClassImplementationName"), CFSTR("VTRating"), CFSTR("VTIsHardwareAccelerated") };
    const void *v[] = { CFSTR(ENCODER_ID), CFSTR("nvenc H.264 (NVIDIA pre-Blackwell)"), r, kCFBooleanTrue };
    CFDictionaryRef info = CFDictionaryCreate(NULL, k, v, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    OSStatus st = VTRegisterVideoEncoderWithInfo(kCMVideoCodecType_H264, info, (void *)NVEncVT_CreateInstance);
    CFRelease(info); CFRelease(r);
    LOG("registered %s, rating %d -> %d", ENCODER_ID, rating, (int)st);
    return st;
}

__attribute__((constructor)) static void init(void)
{
    // register by itself only when injected (DYLD_INSERT_LIBRARIES); loaded by the Metal plugin (NVMTL_NVENC_IMPL=nvenc),
    // the plugin calls NVEncVT_Register
    gLog = getenv("NVENCVT_LOG") != NULL;
    const char *ins = getenv("DYLD_INSERT_LIBRARIES");
    if (ins && strstr(ins, "nvencvt") && !getenv("NVENCVT_NO_REGISTER"))
        NVEncVT_Register(getenv("NVENCVT_RATING") ? atoi(getenv("NVENCVT_RATING")) : 1100);
}
