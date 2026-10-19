// nvencvt.c: a VideoToolbox H.264 encoder on top of src/enc.c (M4). Interface: docs/vt-encoder.md.
// Built as libnvencvt.dylib. Injected, it registers the encoder in the process (constructor), e.g.
//   DYLD_INSERT_LIBRARIES=~/nvenc-dev/libnvencvt.dylib ./vttest hw avc1 1920 1080 60
// Loaded otherwise (the Metal plugin with NVMTL_NVENCVT set), call NVEncVT_Register(rating), or register
// NVEncVT_CreateInstance as the factory yourself. Environment: NVENCVT_RATING (default 1100,
// above nullmoth's 1000), NVENCVT_NO_REGISTER, NVENCVT_LOG, NVENCVT_DEPTH, NVENCVT_ENGINE, NVENCVT_STATS=<file> (or touch
// /tmp/nvencvt.stats: statistics every 2 s to /tmp/nvencvt-stats.log, also in apps that get no environment).
#include "enc.h"
#include "av1hdr.h"
#include "rm.h"
#include "ctrl/ctrl0080/ctrl0080gpu.h"
#include "clc9b7.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <dlfcn.h>
#include <objc/runtime.h>
#include <malloc/malloc.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// ---- CMBaseObject / encoder class declarations (WebKit's CMBaseObjectSPI.h, written for C++) ----
typedef struct CMBaseProtocolVTable CMBaseProtocolVTable;
typedef struct CMBaseProtocolTable CMBaseProtocolTable;
#include "CMBaseObjectSPI.h"

#ifndef NVENC_VERSION
#define NVENC_VERSION "dev"   // build.sh passes git describe --tags --always --dirty
#endif
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
#define ENCODER_ID_HEVC "org.nvenc.videoencoder.hevc"
#define ENCODER_ID_AV1 "org.nvenc.videoencoder.av1"
enum { CODEC_AV1 = 0x61763031 };   // 'av01' (kCMVideoCodecType_AV1)
static bool gLog;
static FILE *gLogF;   // NVENCVT_LOG: stderr; flag file /tmp/nvencvt.log (apps started by launchd): /tmp/nvencvt-<pid>.log
#define LOG(...) do { if (gLog) { FILE *lf_ = gLogF ? gLogF : stderr; fprintf(lf_, "nvencvt: " __VA_ARGS__); fputc('\n', lf_); fflush(lf_); } } while (0)

typedef struct {
    pthread_mutex_t lock;
    VTEncoderSessionRef session;
    int width, height;
    bool hevc;                      // hvc1 session (else avc1)
    bool av1;                       // av01 session (M12, T12.05)
    bool ten;                       // HEVC Main 10 (ProfileLevel HEVC_Main10_*): 10-bit input, 10-bit stream
    int h264Profile;                // ProfileLevel asked for: 66 Baseline, 77 Main, 100 High; 0 = not set (High)
    int entropy;                    // H264EntropyMode asked for: 0 CAVLC, 1 CABAC, -1 not set
    bool fullRange;                 // input 420f / xf20 (full range, encoded as is): VUI video_full_range_flag (T7.01)
    bool opened, failed;
    int engine;
    Enc enc;
    CMVideoFormatDescriptionRef fmt;
    // properties
    int bitrate, maxKeyInterval, fps, quality;
    int refs;                       // reference frames (ReferenceBufferCount; 0 = default)
    // diagnostic dump (flag file /tmp/nvencvt.dump): encoded stream from a forced IDR on, and the first 30 input frames
    FILE *dumpEs, *dumpYuv; int dumpYuvLeft; bool dumpIdr;
    bool realTime;
    unsigned frames;
    // frames in flight: encodeFrame submits, the completion thread waits (in order) and emits
    struct { VTEncoderFrameRef frame; CMTime pts, dur; NvU32 ticket; bool ok; double tEntry, tCall, tTile; } q[ENC_MAX_DEPTH];
    // statistics (NVENCVT_STATS=<file> or the flag file /tmp/nvencvt.stats: a line every 2 s to that file /
    // /tmp/nvencvt-stats.log)
    FILE *statsFile; double statsT0; unsigned sN, sIdr, sFail; double sCall, sCallMax, sTile, sEng, sEngMax, sTotal, sTotalMax, sBytes;
    OSType lastPf;
    unsigned qh, qt, depth;
    pthread_t thread; bool threadOn, stop;
    pthread_cond_t cv;
} State;

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static void stats_print(State *s);
static void stats_frame(State *s, double call, double tile, double eng, double total, size_t bytes, bool idr, bool fail)
{
    if (!s->statsFile) return;
    s->sN++; s->sIdr += idr; s->sFail += fail; s->sBytes += bytes;
    s->sCall += call; if (call > s->sCallMax) s->sCallMax = call;
    s->sTile += tile; s->sEng += eng; if (eng > s->sEngMax) s->sEngMax = eng;
    s->sTotal += total; if (total > s->sTotalMax) s->sTotalMax = total;
    if (now_ms() - s->statsT0 >= 2000) stats_print(s);
}

static void stats_print(State *s)
{
    if (!s->statsFile || !s->sN) return;
    double t = now_ms();
    double sec = (t - s->statsT0) / 1000, n = s->sN;
    char pf[5] = { (char)(s->lastPf >> 24), (char)(s->lastPf >> 16), (char)(s->lastPf >> 8), (char)s->lastPf, 0 };
    time_t now = time(NULL); char ts[32]; strftime(ts, sizeof ts, "%H:%M:%S", localtime(&now));
    fprintf(s->statsFile, "%s %s pid %d %dx%d %s NVENC%d: %.1f fps, call %.3f ms (max %.3f; tiling %.3f), engine %.3f ms (max %.3f), "
            "entry->emit %.3f ms (max %.3f), %.1f KB/frame, %u IDR, %u failed, rate %d bit/s rt %d\n", ts, getprogname(), getpid(),
            s->width, s->height, pf, s->engine, n / sec, s->sCall / n, s->sCallMax, s->sTile / n, s->sEng / n, s->sEngMax,
            s->sTotal / n, s->sTotalMax, s->sBytes / n / 1024, s->sIdr, s->sFail, s->bitrate, s->realTime);
    fflush(s->statsFile);
    s->statsT0 = t; s->sN = s->sIdr = s->sFail = 0; s->sCall = s->sCallMax = s->sTile = s->sEng = s->sEngMax = s->sTotal = s->sTotalMax = s->sBytes = 0;
}

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
    CFDictionarySetValue(d, kVTCompressionPropertyKey_ReferenceBufferCount, num);
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

// Profiles we produce: H.264 Baseline/Main/High (a P-only CAVLC Main stream decodes on all three) at every level,
// HEVC Main and Main 10 (T6.14). Registered as SupportedSelectionProperties, so VideoToolbox picks another encoder
// (Apple's) when an app asks for a profile not listed (HEVC Main10, H.264 High 4:2:2 ...), as Apple's encoders declare.
static CFArrayRef profiles(bool hevc)
{
    static CFArrayRef list[2]; static dispatch_once_t once;
    dispatch_once(&once, ^{
        CFMutableArrayRef h = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks), v = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
        static const char *prof[] = { "Baseline", "Main", "High" };
        static const char *lev[] = { "1_3", "3_0", "3_1", "3_2", "4_0", "4_1", "4_2", "5_0", "5_1", "5_2", "AutoLevel" };
        for (int i = 0; i < 3; i++) for (int j = 0; j < 11; j++) {
            CFStringRef x = CFStringCreateWithFormat(NULL, NULL, CFSTR("H264_%s_%s"), prof[i], lev[j]); CFArrayAppendValue(h, x); CFRelease(x);
        }
        CFArrayAppendValue(h, CFSTR("H264_ConstrainedBaseline_AutoLevel")); CFArrayAppendValue(h, CFSTR("H264_ConstrainedHigh_AutoLevel"));
        CFArrayAppendValue(v, kVTProfileLevel_HEVC_Main_AutoLevel); CFArrayAppendValue(v, kVTProfileLevel_HEVC_Main10_AutoLevel);
        list[0] = h; list[1] = v;
    });
    return list[hevc];
}
static bool profile_ok(const State *s, CFTypeRef v)
{
    CFArrayRef l = profiles(s->hevc);
    return v && CFGetTypeID(v) == CFStringGetTypeID() && CFArrayContainsValue(l, CFRangeMake(0, CFArrayGetCount(l)), v);
}

static OSStatus set_pixel_attrs(State *s);

static OSStatus b_setProperty(CMBaseObjectRef o, CFStringRef key, CFTypeRef v)
{
    State *s = state(o);
    pthread_mutex_lock(&s->lock);
    OSStatus st = noErr;
    if (CFEqual(key, kVTCompressionPropertyKey_AverageBitRate)) s->bitrate = num(v, 0);
    else if (CFEqual(key, kVTCompressionPropertyKey_MaxKeyFrameInterval)) s->maxKeyInterval = num(v, 0);
    else if (CFEqual(key, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration)) { if (!s->maxKeyInterval && num(v, 0) > 0) s->maxKeyInterval = num(v, 0) * (s->fps ? s->fps : 60); }
    else if (CFEqual(key, kVTCompressionPropertyKey_ExpectedFrameRate)) s->fps = num(v, 0);
    else if (CFEqual(key, kVTCompressionPropertyKey_ReferenceBufferCount)) s->refs = num(v, 0);
    else if (CFEqual(key, kVTCompressionPropertyKey_Quality)) { double q = 0; if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue(v, kCFNumberDoubleType, &q); s->quality = (int)(q * 100); }
    else if (CFEqual(key, kVTCompressionPropertyKey_RealTime)) s->realTime = v && CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue(v);
    else if (CFEqual(key, kVTCompressionPropertyKey_AllowFrameReordering)) { if (v && CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue(v)) LOG("frame reordering requested: not supported, no B-frames"); }
    else if (CFEqual(key, kVTCompressionPropertyKey_ProfileLevel) && !profile_ok(s, v)) st = kVTPropertyNotSupportedErr;
    else if (CFEqual(key, kVTCompressionPropertyKey_ProfileLevel) && !s->hevc && !s->av1) {
        char pr[64] = ""; CFStringGetCString(v, pr, sizeof pr, kCFStringEncodingUTF8);
        s->h264Profile = strstr(pr, "Baseline") ? 66 : strstr(pr, "High") ? 100 : 77;
    }
    else if (CFEqual(key, kVTCompressionPropertyKey_H264EntropyMode) && v && CFGetTypeID(v) == CFStringGetTypeID())
        s->entropy = CFEqual(v, kVTH264EntropyMode_CABAC) ? 1 : 0;
    else if (CFEqual(key, kVTCompressionPropertyKey_ProfileLevel) && s->hevc) {
        bool ten = CFEqual(v, kVTProfileLevel_HEVC_Main10_AutoLevel);
        if (ten != s->ten && !s->opened) { s->ten = ten; set_pixel_attrs(s); }
    }
    else if (CFEqual(key, kVTCompressionPropertyKey_ProfileLevel) || CFEqual(key, kVTCompressionPropertyKey_H264EntropyMode) ||
             CFEqual(key, kVTCompressionPropertyKey_DataRateLimits)) { /* accepted: rate limits from AverageBitRate */ }
    else st = kVTPropertyNotSupportedErr;
    if (gLog) { CFStringRef d = v ? CFCopyDescription(v) : NULL; char k[128], vb[256] = "";
        CFStringGetCString(key, k, sizeof k, kCFStringEncodingUTF8); if (d) { CFStringGetCString(d, vb, sizeof vb, kCFStringEncodingUTF8); CFRelease(d); }
        LOG("set %s = %s -> %d", k, vb, (int)st); }
    pthread_mutex_unlock(&s->lock);
    return st;
}

// H.264 profile and entropy coding: High + CABAC (+ 8x8 transform) unless the client asks for less: Baseline gets Main
// with CAVLC (a P-only CAVLC stream; T7.02), Main gets CABAC unless H264EntropyMode says CAVLC
static bool h264_high(const State *s) { return s->h264Profile == 0 || s->h264Profile == 100; }
static bool h264_cabac(const State *s) { return s->h264Profile != 66 && s->entropy != 0; }

static OSStatus b_copyProperty(CMBaseObjectRef o, CFStringRef key, CFAllocatorRef al, void *out)
{
    State *s = state(o); CFTypeRef v = NULL; int n;
    if (CFEqual(key, kVTCompressionPropertyKey_AverageBitRate)) { n = s->bitrate; v = CFNumberCreate(al, kCFNumberIntType, &n); }
    else if (CFEqual(key, kVTCompressionPropertyKey_MaxKeyFrameInterval)) { n = s->maxKeyInterval; v = CFNumberCreate(al, kCFNumberIntType, &n); }
    else if (CFEqual(key, kVTCompressionPropertyKey_ExpectedFrameRate)) { n = s->fps; v = CFNumberCreate(al, kCFNumberIntType, &n); }
    else if (CFEqual(key, kVTCompressionPropertyKey_ReferenceBufferCount)) { n = s->enc.cfg.num_refs ? s->enc.cfg.num_refs : (s->refs ? s->refs : ENC_MAX_REFS); v = CFNumberCreate(al, kCFNumberIntType, &n); }
    else if (CFEqual(key, kVTCompressionPropertyKey_RealTime)) v = CFRetain(s->realTime ? kCFBooleanTrue : kCFBooleanFalse);
    else if (CFEqual(key, kVTCompressionPropertyKey_AllowFrameReordering)) v = CFRetain(kCFBooleanFalse);
    else if (CFEqual(key, kVTCompressionPropertyKey_ProfileLevel) && !s->av1) v = CFRetain(s->hevc ? (s->ten ? kVTProfileLevel_HEVC_Main10_AutoLevel : kVTProfileLevel_HEVC_Main_AutoLevel)
                                                                             : h264_high(s) ? kVTProfileLevel_H264_High_AutoLevel : kVTProfileLevel_H264_Main_AutoLevel);
    else if (CFEqual(key, kVTCompressionPropertyKey_H264EntropyMode) && !s->hevc && !s->av1) v = CFRetain(h264_cabac(s) ? kVTH264EntropyMode_CABAC : kVTH264EntropyMode_CAVLC);
    else if (CFEqual(key, kVTCompressionPropertyKey_NumberOfPendingFrames)) { n = 0; v = CFNumberCreate(al, kCFNumberIntType, &n); }
    else if (CFEqual(key, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder)) v = CFRetain(kCFBooleanTrue);
    else { if (gLog) { char k[128]; CFStringGetCString(key, k, sizeof k, kCFStringEncodingUTF8); LOG("copy %s: not available", k); } return kCMBaseObjectError_ValueNotAvailable; }
    *(CFTypeRef *)out = v;
    return noErr;
}

// the input formats we take: NV12 or BGRA (BGRA is converted by us, in the tiling pass, much faster than VideoToolbox's
// conversion: T5.05); a Main 10 session takes 16-bit 4:2:0 (x420 / xf20, P010 layout). Set again when the profile changes.
static OSStatus set_pixel_attrs(State *s)
{
    if (!s->session) return noErr;
    int f[12] = { kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, kCVPixelFormatType_32BGRA };
    // Main 10: P010 or l10r (ScreenCaptureKit's 10-bit RGB, what Parsec sends; converted by us: else VideoToolbox converts it
    // through Metal on its serial queue, 20+ ms per 4K frame and frames queued behind it, T6.14)
    if (s->ten) { f[0] = kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange; f[1] = kCVPixelFormatType_420YpCbCr10BiPlanarFullRange; f[2] = kCVPixelFormatType_ARGB2101010LEPacked; }
    int nf = 3;
    if (s->av1) {   // AV1: 8 or 10-bit, decided by the first frame (VideoToolbox converts only buffers in none of these)
        f[0] = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange; f[1] = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange; f[2] = kCVPixelFormatType_32BGRA;
        f[3] = kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange; f[4] = kCVPixelFormatType_420YpCbCr10BiPlanarFullRange; f[5] = kCVPixelFormatType_ARGB2101010LEPacked;
        nf = 6;
    }
    if (access("/tmp/nvencvt.anyfmt", F_OK) == 0) {   // diagnostics: offer every format a capture source might deliver, log what comes
        int all[] = { kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange, kCVPixelFormatType_420YpCbCr10BiPlanarFullRange,
                      kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, kCVPixelFormatType_32BGRA,
                      kCVPixelFormatType_ARGB2101010LEPacked, kCVPixelFormatType_64RGBAHalf, kCVPixelFormatType_30RGB,
                      kCVPixelFormatType_64ARGB, kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange, kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange, kCVPixelFormatType_32ARGB };
        nf = 12; memcpy(f, all, sizeof all);
    }
    CFNumberRef n[12]; for (int i = 0; i < nf; i++) n[i] = CFNumberCreate(NULL, kCFNumberIntType, &f[i]);
    CFArrayRef fmts = CFArrayCreate(NULL, (const void **)n, nf, &kCFTypeArrayCallBacks);
    CFNumberRef w = CFNumberCreate(NULL, kCFNumberIntType, &s->width), h = CFNumberCreate(NULL, kCFNumberIntType, &s->height);
    CFDictionaryRef ios = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *k[] = { kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferWidthKey, kCVPixelBufferHeightKey, kCVPixelBufferIOSurfacePropertiesKey };
    const void *v[] = { fmts, w, h, ios };
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, k, v, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    OSStatus st = VTEncoderSessionSetPixelBufferAttributes(s->session, attrs);
    CFRelease(attrs); CFRelease(ios); CFRelease(w); CFRelease(h); CFRelease(fmts); for (int i = 0; i < nf; i++) CFRelease(n[i]);
    return st;
}

// ---- encoder functions ----
static OSStatus e_startSession(VTVideoEncoderRef e, VTEncoderSessionRef session, CMVideoDimensions dims)
{
    State *s = state(e);
    s->session = session; s->width = dims.width; s->height = dims.height;
    OSStatus st = set_pixel_attrs(s);
    LOG("startSession %dx%d (pixel buffer attributes -> %d)", s->width, s->height, (int)st);
    return noErr;
}

static void *completion(void *arg);
static OSStatus open_encoder(State *s)
{
    // reference frames: ReferenceBufferCount if the client set it, else NVENCVT_REFS, else 3. Pictures that come back
    // (window switching, blinking content) cost far less with more references: 1080p, three pictures cycling, HEVC:
    // 27.8 Mbit/s with one, 1.7 with three (T6.11); steady motion about -2 %, +0.1 ms per frame. No reordering, no added latency.
    int refs = s->refs > 0 ? s->refs : getenv("NVENCVT_REFS") ? atoi(getenv("NVENCVT_REFS")) : 3;
    { FILE *rf = fopen("/tmp/nvencvt.refs", "r"); int v = 0;   // diagnostics: overrides everything, read at each session start
      if (rf) { if (fscanf(rf, "%d", &v) == 1 && v > 0) refs = v; fclose(rf); } }
    refs = refs < 1 ? 1 : refs > ENC_MAX_REFS ? ENC_MAX_REFS : refs;
    // HEVC: announce 2 pictures more than the references need in the VPS/SPS DPB size. With exactly refs + 1, a hardware
    // decoder (AMD Polaris through macOS Sonoma, Parsec client) overwrote references still in use: moving windows showed
    // content ahead of the background (T6.12). NVENCVT_DPB_EXTRA or /tmp/nvencvt.dpb (0..2) override.
    int dpb_extra = getenv("NVENCVT_DPB_EXTRA") ? atoi(getenv("NVENCVT_DPB_EXTRA")) : 2;
    { FILE *df = fopen("/tmp/nvencvt.dpb", "r"); int v = 0;
      if (df) { if (fscanf(df, "%d", &v) == 1 && v >= 0) dpb_extra = v; fclose(df); } }
    dpb_extra = dpb_extra < 0 ? 0 : dpb_extra > 2 ? 2 : dpb_extra;
    if (s->av1) refs = 1;   // AV1: one reference (LAST), as NVIDIA's driver in low-latency mode (enc.c)
    EncConfig c = { .codec = s->av1 ? ENC_AV1 : s->hevc ? ENC_HEVC : ENC_H264, .width = s->width, .height = s->height, .num_refs = refs, .dpb_extra = dpb_extra, .fps = s->fps > 0 ? s->fps : 60,
                    .idr_period = s->maxKeyInterval > 0 ? s->maxKeyInterval : 0, .bit_depth = s->ten ? 10 : 8, .full_range = s->fullRange,
                    .h264_profile = h264_high(s) ? 100 : 77, .cabac = h264_cabac(s) };
    // constant QP from Quality (0..1) when there is no bitrate; else CBR (MB-row), one-frame VBV in real time mode
    int qp = s->quality ? 40 - s->quality * 22 / 100 : 23;
    c.qp_i = qp; c.qp_p = qp + 2;
    if (s->av1) { c.qp_i = qp * 4 - 12; c.qp_p = c.qp_i + 20; }   // AV1 quantizer indices (0..255): 23 -> 80 / 100, the driver's pair
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
    uint8_t hdr[256]; size_t n = s->av1 ? 0 : enc_headers(&s->enc, hdr);
    if (s->av1) {   // av1C (AV1 Codec ISO Media File Format Binding 2.3): 4 bytes, then the sequence header OBU
        uint8_t cfg[64] = { 0x81, (uint8_t)av1_level_for(s->width, s->height), (uint8_t)((s->ten ? 0x40 : 0) | 0x0c), 0 };
        const size_t cn = 4 + enc_av1_config(&s->enc, cfg + 4);
        CFDataRef d = CFDataCreate(NULL, cfg, (CFIndex)cn);
        const void *ak[] = { CFSTR("av1C") }, *av[] = { d };
        CFDictionaryRef atoms = CFDictionaryCreate(NULL, ak, av, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        const void *ek[] = { kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms, kCMFormatDescriptionExtension_FullRangeVideo }, *ev[] = { atoms, s->fullRange ? kCFBooleanTrue : kCFBooleanFalse };
        CFDictionaryRef ext = CFDictionaryCreate(NULL, ek, ev, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        OSStatus st = CMVideoFormatDescriptionCreate(NULL, CODEC_AV1, s->width, s->height, ext, &s->fmt);
        CFRelease(ext); CFRelease(atoms); CFRelease(d);
        LOG("AV1%s encoder open on NVENC%d: %dx%d range %s rc_mode %d bitrate %d vbv %d q %d/%d idr_period %d fps %d -> fmt %d", s->ten ? " 10-bit" : "", c.engine,
            c.width, c.height, c.full_range ? "full" : "video", c.rc_mode, c.bitrate, c.vbv_bits, c.qp_i, c.qp_p, c.idr_period, c.fps, (int)st);
        if (st) { enc_close(&s->enc); return st; }
        goto opened;
    }
    // the parameter sets (H.264: SPS + PPS, HEVC: VPS + SPS + PPS) from the Annex B header block
    const uint8_t *ps[3]; size_t pl[3]; int cnt = 0, want = s->hevc ? 3 : 2;
    for (size_t i = 0; i + 4 <= n && cnt < want; i++)
        if (!hdr[i] && !hdr[i + 1] && !hdr[i + 2] && hdr[i + 3] == 1) { ps[cnt++] = hdr + i + 4; i += 3; }
    for (int k = 0; k < cnt; k++) pl[k] = (size_t)((k + 1 < cnt ? ps[k + 1] - 4 : hdr + n) - ps[k]);
    OSStatus st = cnt != want ? kVTVideoEncoderMalfunctionErr : s->hevc
        ? CMVideoFormatDescriptionCreateFromHEVCParameterSets(NULL, 3, ps, pl, 4, NULL, &s->fmt)
        : CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, 2, ps, pl, 4, &s->fmt);
    LOG("%s%s encoder open on NVENC%d: %dx%d refs %d dpb+%d range %s profile %d cabac %d rc_mode %d bitrate %d vbv %d qp %d/%d idr_period %d fps %d -> fmt %d", s->hevc ? "HEVC" : "H.264", s->ten ? " Main10" : "", c.engine, c.width, c.height, c.num_refs, c.dpb_extra, c.full_range ? "full" : "video", s->hevc ? 0 : c.h264_profile, s->hevc ? 0 : (int)c.cabac,
        c.rc_mode, c.bitrate, c.vbv_bits, c.qp_i, c.qp_p, c.idr_period, c.fps, (int)st);
    if (st) { enc_close(&s->enc); return st; }
opened:
    s->opened = true;
    s->stop = false;
    const char *sf = getenv("NVENCVT_STATS");
    if (sf || access("/tmp/nvencvt.stats", F_OK) == 0) { s->statsFile = fopen(sf ? sf : "/tmp/nvencvt-stats.log", "a"); s->statsT0 = now_ms(); }
    s->threadOn = pthread_create(&s->thread, NULL, completion, s) == 0;
    if (!s->threadOn) { enc_close(&s->enc); s->opened = false; return kVTVideoEncoderMalfunctionErr; }
    return noErr;
}

static void emit(State *s, VTEncoderFrameRef frame, CMTime pts, CMTime dur, const EncFrame *fr, OSStatus err)
{
    if (err) { VTEncoderSessionEmitEncodedFrame(s->session, frame, err, 0, NULL); return; }
    if (s->dumpEs) { fwrite(fr->data, 1, fr->size, s->dumpEs); fflush(s->dumpEs); }
    // AV1: the temporal unit without its temporal delimiter (ISOBMFF samples carry none); else Annex B (4-byte start
    // codes) -> AVCC (4-byte big-endian lengths), same size
    const size_t skip = s->av1 && fr->size >= 2 && fr->data[0] == 0x12 && fr->data[1] == 0 ? 2 : 0;
    uint8_t *buf = malloc(fr->size);
    memcpy(buf, fr->data + skip, fr->size - skip);
    size_t start = SIZE_MAX;
    for (size_t i = 0; !s->av1 && i + 4 <= fr->size; i++) {
        if (!buf[i] && !buf[i + 1] && !buf[i + 2] && buf[i + 3] == 1) {
            if (start != SIZE_MAX) { uint32_t l = (uint32_t)(i - start - 4); buf[start] = l >> 24; buf[start + 1] = l >> 16; buf[start + 2] = l >> 8; buf[start + 3] = l; }
            start = i; i += 3;
        }
    }
    if (!s->av1 && start != SIZE_MAX) { uint32_t l = (uint32_t)(fr->size - start - 4); buf[start] = l >> 24; buf[start + 1] = l >> 16; buf[start + 2] = l >> 8; buf[start + 3] = l; }
    CMBlockBufferRef bb = NULL; CMSampleBufferRef sb = NULL;
    OSStatus st = CMBlockBufferCreateWithMemoryBlock(NULL, buf, fr->size, kCFAllocatorMalloc, NULL, 0, fr->size - skip, 0, &bb);
    CMSampleTimingInfo ti = { .duration = dur, .presentationTimeStamp = pts, .decodeTimeStamp = kCMTimeInvalid };
    size_t sz = fr->size - skip;
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
        double tEntry = s->q[i].tEntry, tCall = s->q[i].tCall, tTile = s->q[i].tTile;
        pthread_mutex_unlock(&s->lock);
        EncFrame fr; memset(&fr, 0, sizeof fr);
        int rc = ok ? enc_wait(&s->enc, t, &fr) : -1;
        if (rc) LOG("frame %u: encode failed (ucode error %u)", t, fr.ucode_error);
        else if (gLog && (t <= 3 || fr.idr)) LOG("frame %u %s: %zu bytes, %.3f ms, avgQP %u", t, fr.idr ? "IDR" : "P", fr.size, fr.ms, fr.avg_qp);
        emit(s, frame, pts, dur, &fr, rc ? kVTVideoEncoderMalfunctionErr : noErr);   // fr.data stays valid until qh moves on
        pthread_mutex_lock(&s->lock);
        stats_frame(s, tCall, tTile, fr.ms, now_ms() - tEntry, rc ? 0 : fr.size, fr.idr, rc != 0);
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
    double tEntry = now_ms();
    pthread_mutex_lock(&s->lock);
    OSStatus st = noErr;
    // the stream's range follows the first frame: full-range YCbCr (420f, xf20) is encoded as it is and signalled in the
    // VUI; BGRA and l10r are converted by us to video range, and 420v / x420 are video range (T7.01)
    if (!s->opened) { OSType p0 = CVPixelBufferGetPixelFormatType(img);
        s->fullRange = p0 == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange || p0 == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange; }
    if (!s->opened && s->av1) { OSType p0 = CVPixelBufferGetPixelFormatType(img);   // AV1: 10-bit when the input is
        s->ten = p0 == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange || p0 == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange || p0 == kCVPixelFormatType_ARGB2101010LEPacked; }
    if (!s->opened && !s->failed && (st = open_encoder(s))) s->failed = true;
    if (s->failed) { pthread_mutex_unlock(&s->lock); VTEncoderSessionEmitEncodedFrame(s->session, frame, kVTVideoEncoderMalfunctionErr, 0, NULL); return kVTVideoEncoderMalfunctionErr; }
    OSType pf = CVPixelBufferGetPixelFormatType(img);
    bool bgra = pf == kCVPixelFormatType_32BGRA;
    bool p010 = pf == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange || pf == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
    bool l10r = pf == kCVPixelFormatType_ARGB2101010LEPacked;
    if ((pf != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange && pf != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange && !bgra && !((p010 || l10r) && s->ten)) ||
        (bgra && s->ten) ||
        (int)CVPixelBufferGetWidth(img) < s->width || (int)CVPixelBufferGetHeight(img) < s->height) {
        { OSType be = CFSwapInt32HostToBig(pf); LOG("unexpected pixel buffer %.4s %zux%zu", (char *)&be, CVPixelBufferGetWidth(img), CVPixelBufferGetHeight(img)); }
        pthread_mutex_unlock(&s->lock);
        VTEncoderSessionEmitEncodedFrame(s->session, frame, kVTPixelTransferNotSupportedErr, 0, NULL);
        return noErr;
    }
    while (s->qt - s->qh >= s->depth) pthread_cond_wait(&s->cv, &s->lock);   // a free input slot
    double tTile0 = now_ms();
    if (gLog && s->frames == 0) { OSType be = CFSwapInt32HostToBig(pf); CFTypeRef m = CVBufferCopyAttachment(img, kCVImageBufferYCbCrMatrixKey, NULL), t = CVBufferCopyAttachment(img, kCVImageBufferTransferFunctionKey, NULL);
        char ms[64] = "-", ts[64] = "-"; if (m) { CFStringGetCString(m, ms, sizeof ms, kCFStringEncodingUTF8); CFRelease(m); } if (t) { CFStringGetCString(t, ts, sizeof ts, kCFStringEncodingUTF8); CFRelease(t); }
        LOG("first frame %.4s %zux%zu matrix %s transfer %s", (char *)&be, CVPixelBufferGetWidth(img), CVPixelBufferGetHeight(img), ms, ts); }
    s->lastPf = pf;
    CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
    if (!s->dumpEs && (s->frames % 30) == 0 && access("/tmp/nvencvt.dump", F_OK) == 0) {   // start a dump (diagnostics)
        char p[128]; snprintf(p, sizeof p, "/tmp/nvencvt-dump-%d.es", (int)getpid()); s->dumpEs = fopen(p, "wb");
        snprintf(p, sizeof p, "/tmp/nvencvt-dump-%d.nv12", (int)getpid()); s->dumpYuv = bgra || p010 || l10r ? NULL : fopen(p, "wb");
        s->dumpYuvLeft = 30; s->dumpIdr = true;
        if (s->dumpEs) { uint8_t h[256]; size_t n = enc_headers(&s->enc, h); fwrite(h, 1, n, s->dumpEs); }
        LOG("dump started (%dx%d, %s)", s->width, s->height, bgra ? "BGRA: stream only" : "NV12");
    }
    if (s->dumpYuv && s->dumpYuvLeft > 0) {   // the input as Parsec delivered it, tightly packed NV12
        for (int pl = 0; pl < 2; pl++) {
            const uint8_t *b = CVPixelBufferGetBaseAddressOfPlane(img, pl); size_t st = CVPixelBufferGetBytesPerRowOfPlane(img, pl);
            int rows = pl ? (s->height + 1) / 2 : s->height;
            for (int r = 0; r < rows; r++) fwrite(b + (size_t)r * st, 1, (size_t)s->width, s->dumpYuv);
        }
        if (--s->dumpYuvLeft == 0) { fclose(s->dumpYuv); s->dumpYuv = NULL; }
    }
    if (l10r) enc_load_l10r(&s->enc, CVPixelBufferGetBaseAddress(img), CVPixelBufferGetBytesPerRow(img));
    else if (p010) enc_load_p010(&s->enc, CVPixelBufferGetBaseAddressOfPlane(img, 0), CVPixelBufferGetBytesPerRowOfPlane(img, 0),
                            CVPixelBufferGetBaseAddressOfPlane(img, 1), CVPixelBufferGetBytesPerRowOfPlane(img, 1));
    else if (bgra) enc_load_bgra(&s->enc, CVPixelBufferGetBaseAddress(img), CVPixelBufferGetBytesPerRow(img));
    else enc_load_nv12(&s->enc, CVPixelBufferGetBaseAddressOfPlane(img, 0), CVPixelBufferGetBytesPerRowOfPlane(img, 0),
                       CVPixelBufferGetBaseAddressOfPlane(img, 1), CVPixelBufferGetBytesPerRowOfPlane(img, 1));
    CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
    bool forceIdr = false;
    if (props) { CFTypeRef k = CFDictionaryGetValue(props, kVTEncodeFrameOptionKey_ForceKeyFrame); forceIdr = k && CFGetTypeID(k) == CFBooleanGetTypeID() && CFBooleanGetValue(k); }
    unsigned i = s->qt % s->depth;
    s->q[i].frame = frame; s->q[i].pts = pts; s->q[i].dur = dur;
    s->q[i].tEntry = tEntry; s->q[i].tTile = now_ms() - tTile0;
    s->q[i].ok = enc_submit(&s->enc, forceIdr || s->dumpIdr, &s->q[i].ticket) == 0; s->dumpIdr = false;
    s->q[i].tCall = now_ms() - tEntry;
    s->qt++; s->frames++;
    pthread_cond_broadcast(&s->cv);
    // RealTime (streaming): return only once this frame is emitted, so the output reaches the app inside its encode call.
    // Parsec picks up output when it encodes the next frame otherwise: one frame period of extra latency (T5.05).
    bool sync = s->realTime || getenv("NVENCVT_SYNC");
    if (sync) { unsigned mine = s->qt; while (s->qh < mine && s->threadOn) pthread_cond_wait(&s->cv, &s->lock); }
    pthread_mutex_unlock(&s->lock);
    if (infoOut) *infoOut = sync ? 0 : kVTEncodeInfo_Asynchronous;
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
    if (s->statsFile) { stats_print(s); fclose(s->statsFile); s->statsFile = NULL; }
    if (s->opened) {
        enc_close(&s->enc); s->opened = false;
        if (s->dumpEs) { fclose(s->dumpEs); s->dumpEs = NULL; } if (s->dumpYuv) { fclose(s->dumpYuv); s->dumpYuv = NULL; }
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
    if (codec != kCMVideoCodecType_H264 && codec != kCMVideoCodecType_HEVC && codec != CODEC_AV1) return kVTCouldNotFindVideoEncoderErr;
    CMBaseObjectRef obj = NULL;
    OSStatus st = CMDerivedObjectCreate(al, &gVT.base, VTVideoEncoderGetClassID(), &obj);
    if (st) return st;
    State *s = calloc(1, sizeof *s);
    s->entropy = -1;
    pthread_mutex_init(&s->lock, NULL); pthread_cond_init(&s->cv, NULL);
    s->hevc = codec == kCMVideoCodecType_HEVC; s->av1 = codec == CODEC_AV1;
    *(State **)CMBaseObjectGetDerivedStorage(obj) = s;
    *out = obj;
    LOG("instance %p (%s)", obj, s->av1 ? "av01" : s->hevc ? "hvc1" : "avc1");
    return noErr;
}

// The encoder drives the Ada NVENC class (NVC9B7_VIDEO_ENCODER, RTX 40). Registering on a GPU without it would make
// VideoToolbox pick an encoder that cannot start, so look at the GPU's class list first.
static bool gpu_has_ada_nvenc(void)
{
    Rm rm = { 0 };
    if (rm_open(&rm)) { LOG("no NVIDIA RM device"); return false; }
    static NV0080_CTRL_GPU_GET_CLASSLIST_V2_PARAMS cl;
    memset(&cl, 0, sizeof cl);
    bool ok = false;
    if (rm_ctrl(&rm, rm.device, NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2, &cl, sizeof cl) == NV_OK)
        for (NvU32 i = 0; i < cl.numClasses && !ok; i++) ok = cl.classList[i] == NVC9B7_VIDEO_ENCODER;
    rm_close(&rm);
    return ok;
}

static OSStatus register_one(FourCharCode codec, CFStringRef id, CFStringRef name, int rating)
{
    CFNumberRef r = CFNumberCreate(NULL, kCFNumberIntType, &rating);
    const void *sk[] = { kVTCompressionPropertyKey_ProfileLevel }, *sv[] = { profiles(codec == kCMVideoCodecType_HEVC) };
    CFDictionaryRef sel = CFDictionaryCreate(NULL, sk, sv, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *k[] = { CFSTR("CMClassImplementationID"), CFSTR("CMClassImplementationName"), CFSTR("VTRating"), CFSTR("VTIsHardwareAccelerated"),
                        kVTVideoEncoderList_SupportedSelectionProperties };
    const void *v[] = { id, name, r, kCFBooleanTrue, sel };
    // AV1: no profile list (Main, 8 / 10-bit, is all there is)
    CFDictionaryRef info = CFDictionaryCreate(NULL, k, v, codec == CODEC_AV1 ? 4 : 5, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    OSStatus st = VTRegisterVideoEncoderWithInfo(codec, info, (void *)NVEncVT_CreateInstance);
    CFRelease(info); CFRelease(sel); CFRelease(r);
    return st;
}

// The plugin calls NVEncVT_Register in every process it is enabled for, while the Metal device is created, so the
// check must be cheap: the GPU's PCI device ID from the IORegistry (~0.1 ms) instead of an RM client and the class list
// (6-9 ms). Ada (AD102-AD107: RTX 40, RTX Ada, L4/L40) is device IDs 0x2600-0x28ff. -1: no NVIDIA GPU in the registry.
// the card's name for the encoder names, from the GPU's IORegistry "model" ("NVIDIA GeForce RTX 4090" -> "RTX 4090")
static char gCardName[64] = "GPU";
static void card_name_from(io_object_t d)
{
    CFTypeRef m = IORegistryEntryCreateCFProperty(d, CFSTR("model"), NULL, 0);
    if (m && CFGetTypeID(m) == CFDataGetTypeID() && CFDataGetLength(m) > 0) {
        char b[96] = ""; CFIndex n = CFDataGetLength(m) < (CFIndex)sizeof b - 1 ? CFDataGetLength(m) : (CFIndex)sizeof b - 1;
        CFDataGetBytes(m, CFRangeMake(0, n), (UInt8 *)b); b[n] = 0;
        const char *p = b;
        if (!strncmp(p, "NVIDIA ", 7)) p += 7;
        if (!strncmp(p, "GeForce ", 8)) p += 8;
        if (*p) snprintf(gCardName, sizeof gCardName, "%s", p);
    }
    if (m) CFRelease(m);
}

static int pci_has_ada(void)
{
    io_iterator_t it = 0; io_object_t d; int found = -1;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOPCIDevice"), &it) != KERN_SUCCESS) return -1;
    while ((d = IOIteratorNext(it))) {
        CFTypeRef ven = IORegistryEntryCreateCFProperty(d, CFSTR("vendor-id"), NULL, 0), dev = IORegistryEntryCreateCFProperty(d, CFSTR("device-id"), NULL, 0);
        unsigned v = 0, id = 0;
        if (ven && CFGetTypeID(ven) == CFDataGetTypeID() && CFDataGetLength(ven) >= 2) CFDataGetBytes(ven, CFRangeMake(0, 2), (UInt8 *)&v);
        if (dev && CFGetTypeID(dev) == CFDataGetTypeID() && CFDataGetLength(dev) >= 2) CFDataGetBytes(dev, CFRangeMake(0, 2), (UInt8 *)&id);
        if (ven) CFRelease(ven);
        if (dev) CFRelease(dev);
        if (v == 0x10de && found < 1) { found = id >= 0x2600 && id <= 0x28ff; if (found == 1) card_name_from(d); }
        IOObjectRelease(d);
    }
    IOObjectRelease(it);
    return found;
}

// ---- encoder names ----
// VTRegisterVideoEncoderWithInfo always names an encoder "Dynamically Registered <codec> Video Encoder": it passes that
// name, not ours, to the registry entry it creates (VideoToolbox's vtRegisterVideoEncoderInternal), and plug-in bundles,
// which can carry a VTEncoderName, are only accepted when Apple signed them (CoreMedia's figRegistryAddResultItem checks
// kFigRegistryAppleCodeSignRequirement for every folder VideoToolbox scans; T7.00). The entry's description is a
// mutable dictionary that VideoToolbox reads VTEncoderName / VTCodecName from when it lists encoders, so we rename our
// entries there: find VideoToolbox's encoder FigRegistry (a pointer in VideoToolbox's data sections to a FigRegistry of
// class com.apple.videotoolbox.videoencoder), our items by ID, and set the two names if the dictionary is mutable and
// still holds VideoToolbox's default name. Anything unexpected: leave the default name. NVENCVT_NO_RENAME=1 skips it.
typedef OSStatus (*FigRegistryCreateFn)(CFAllocatorRef, CFStringRef, void **);
typedef OSStatus (*FigRegistryCopyFilteredItemListFn)(void *, void *, void *, CFArrayRef *, int *);
typedef OSStatus (*FigRegistryItemCopyDescriptionFn)(const void *, CFDictionaryRef *);
// mutable: Foundation's mutable dictionary class (CFDictionaryCreateMutable makes one); _CFDictionaryIsMutable only knows
// the old CF-native type and says 0 for every dictionary on macOS 26, frozen copies are __NSFrozenDictionaryM
static bool dict_is_mutable(CFDictionaryRef d) { const char *c = object_getClassName((id)d); return c && !strcmp(c, "__NSDictionaryM"); }

static void *find_vt_encoder_registry(void)
{
    FigRegistryCreateFn create = (FigRegistryCreateFn)dlsym(RTLD_DEFAULT, "FigRegistryCreate");
    if (!create) return NULL;
    void *mine = NULL;   // a registry of our own: its isa and type ID identify FigRegistry objects
    if (create(NULL, CFSTR("org.nvenc.registry-probe"), &mine) || !mine) return NULL;
    uintptr_t isa = ((const uintptr_t *)mine)[0]; CFTypeID tid = CFGetTypeID(mine);
    CFRelease(mine);
    CFStringRef cls = CFSTR("com.apple.videotoolbox.videoencoder");
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *n = _dyld_get_image_name(i);
        if (!n || !strstr(n, "/VideoToolbox.framework/")) continue;
        const struct mach_header_64 *h = (const struct mach_header_64 *)_dyld_get_image_header(i);
        const struct load_command *lc = (const struct load_command *)(h + 1);
        for (uint32_t k = 0; k < h->ncmds; k++, lc = (const struct load_command *)((const char *)lc + lc->cmdsize)) {
            if (lc->cmd != LC_SEGMENT_64) continue;
            const struct segment_command_64 *sg = (const struct segment_command_64 *)lc;
            // the registry pointer is written at run time: writable __DATA / __DATA_DIRTY only, not __DATA_CONST
            if (strncmp(sg->segname, "__DATA", 6) || !strcmp(sg->segname, "__DATA_CONST") || !(sg->initprot & VM_PROT_WRITE)) continue;
            const struct section_64 *sc = (const struct section_64 *)(sg + 1);
            for (uint32_t j = 0; j < sg->nsects; j++) {
                unsigned long size = 0;
                const uintptr_t *w = (const uintptr_t *)getsectiondata(h, sc[j].segname, sc[j].sectname, &size);
                for (unsigned long q = 0; w && q < size / sizeof *w; q++) {
                    const uintptr_t *o = (const uintptr_t *)w[q];
                    if ((uintptr_t)o < 0x100000000ull || ((uintptr_t)o & 7) || !malloc_zone_from_ptr(o) || malloc_size(o) < 64) continue;
                    if (o[0] != isa || CFGetTypeID((CFTypeRef)o) != tid) continue;   // a FigRegistry
                    CFTypeRef c = ((CFTypeRef const *)o)[2];
                    if (c && CFGetTypeID(c) == CFStringGetTypeID() && CFEqual(c, cls)) return (void *)o;
                }
            }
        }
    }
    return NULL;
}

static void rename_encoders(const char *const ids[], const char *const names[], const char *const codecs[], int n)
{
    if (getenv("NVENCVT_NO_RENAME")) return;
    FigRegistryCopyFilteredItemListFn list = (FigRegistryCopyFilteredItemListFn)dlsym(RTLD_DEFAULT, "FigRegistryCopyFilteredItemList");
    FigRegistryItemCopyDescriptionFn desc = (FigRegistryItemCopyDescriptionFn)dlsym(RTLD_DEFAULT, "FigRegistryItemCopyDescription");
    if (!list || !desc) { LOG("rename: registry functions missing"); return; }
    void *reg = find_vt_encoder_registry();
    if (!reg) { LOG("rename: VideoToolbox's encoder registry not found"); return; }
    CFArrayRef items = NULL; int cnt = 0;
    if (list(reg, NULL, NULL, &items, &cnt) || !items) { LOG("rename: no item list"); return; }
    int done = 0;
    for (CFIndex i = 0; i < CFArrayGetCount(items); i++) {
        CFDictionaryRef d = NULL;
        if (desc(CFArrayGetValueAtIndex(items, i), &d) || !d) continue;
        CFTypeRef iid = CFDictionaryGetValue(d, CFSTR("CMClassImplementationID")), cur = CFDictionaryGetValue(d, CFSTR("VTEncoderName"));
        for (int k = 0; k < n && iid && CFGetTypeID(iid) == CFStringGetTypeID(); k++) {
            CFStringRef want = CFStringCreateWithCString(NULL, ids[k], kCFStringEncodingUTF8);
            if (CFEqual(iid, want)) LOG("rename: %s: name %s, class %s", ids[k], cur && CFGetTypeID(cur) == CFStringGetTypeID() && CFStringHasPrefix(cur, CFSTR("Dynamically Registered")) ? "default" : "other", object_getClassName((id)d));
            if (CFEqual(iid, want) && cur && CFGetTypeID(cur) == CFStringGetTypeID() &&
                CFStringHasPrefix(cur, CFSTR("Dynamically Registered")) && dict_is_mutable(d)) {
                CFStringRef nm = CFStringCreateWithCString(NULL, names[k], kCFStringEncodingUTF8), cn = CFStringCreateWithCString(NULL, codecs[k], kCFStringEncodingUTF8);
                CFDictionarySetValue((CFMutableDictionaryRef)d, CFSTR("VTEncoderName"), nm);
                CFDictionarySetValue((CFMutableDictionaryRef)d, CFSTR("VTCodecName"), cn);
                CFRelease(nm); CFRelease(cn); done++;
            }
            CFRelease(want);
        }
        CFRelease(d);
    }
    CFRelease(items);
    LOG("rename: %d of %d encoder names set", done, n);
}

// registers the H.264 (avc1) and HEVC (hvc1) encoders; returns the H.264 registration's status
__attribute__((visibility("default"))) OSStatus NVEncVT_Register(int rating)
{
    int ada = getenv("NVENCVT_ANY_GPU") ? 1 : pci_has_ada();
    if (ada < 0) ada = gpu_has_ada_nvenc();   // no PCI match (unusual): ask RM for the class list
    if (!ada) {
        LOG("GPU is not Ada (no NVENC class c9b7): not registering");
        return kVTCouldNotFindVideoEncoderErr;
    }
    // "NVENC - H.264 - (NVIDIA RTX 4090) - mickys/nvenc - 0.7.0": codec, the card, this project and its version (git describe)
    char nameAvc[160], nameHevc[160], nameAv1[160];
    const char *ver = NVENC_VERSION[0] == 'v' ? NVENC_VERSION + 1 : NVENC_VERSION;
    snprintf(nameAvc, sizeof nameAvc, "NVENC - H.264 - (NVIDIA %s) - mickys/nvenc - %s", gCardName, ver);
    snprintf(nameHevc, sizeof nameHevc, "NVENC - HEVC - (NVIDIA %s) - mickys/nvenc - %s", gCardName, ver);
    snprintf(nameAv1, sizeof nameAv1, "NVENC - AV1 - (NVIDIA %s) - mickys/nvenc - %s", gCardName, ver);
    CFStringRef na = CFStringCreateWithCString(NULL, nameAvc, kCFStringEncodingUTF8), nh = CFStringCreateWithCString(NULL, nameHevc, kCFStringEncodingUTF8);
    OSStatus st = register_one(kCMVideoCodecType_H264, CFSTR(ENCODER_ID), na, rating);
    OSStatus sh = getenv("NVENCVT_NO_HEVC") ? kVTCouldNotFindVideoEncoderErr
                : register_one(kCMVideoCodecType_HEVC, CFSTR(ENCODER_ID_HEVC), nh, rating);
    CFStringRef n1 = CFStringCreateWithCString(NULL, nameAv1, kCFStringEncodingUTF8);
    OSStatus s1 = getenv("NVENCVT_NO_AV1") ? kVTCouldNotFindVideoEncoderErr : register_one(CODEC_AV1, CFSTR(ENCODER_ID_AV1), n1, rating);
    CFRelease(na); CFRelease(nh); CFRelease(n1);
    LOG("registered %s -> %d, %s -> %d, %s -> %d (rating %d)", ENCODER_ID, (int)st, ENCODER_ID_HEVC, (int)sh, ENCODER_ID_AV1, (int)s1, rating);
    const char *ids[3] = { ENCODER_ID }, *names[3] = { nameAvc }, *codecs[3] = { "H.264" }; int nreg = 1;
    if (!sh) { ids[nreg] = ENCODER_ID_HEVC; names[nreg] = nameHevc; codecs[nreg++] = "HEVC"; }
    if (!s1) { ids[nreg] = ENCODER_ID_AV1; names[nreg] = nameAv1; codecs[nreg++] = "AV1"; }
    rename_encoders(ids, names, codecs, nreg);
    extern OSStatus NVDecVT_Register(int rating);   // vt/nvencvt_dec.c: the H.264 decoder (NVDEC), same rating
    NVDecVT_Register(rating);
    return st;
}

__attribute__((constructor)) static void init(void)
{
    // register by itself only when injected (DYLD_INSERT_LIBRARIES); loaded by the Metal plugin (NVMTL_NVENCVT),
    // the plugin calls NVEncVT_Register
    gLog = getenv("NVENCVT_LOG") != NULL;
    if (!gLog && access("/tmp/nvencvt.log", F_OK) == 0) {
        char p[64]; snprintf(p, sizeof p, "/tmp/nvencvt-%d.log", (int)getpid());
        if ((gLogF = fopen(p, "a"))) gLog = true;
    }
    const char *ins = getenv("DYLD_INSERT_LIBRARIES");
    if (ins && strstr(ins, "nvencvt") && !getenv("NVENCVT_NO_REGISTER"))
        NVEncVT_Register(getenv("NVENCVT_RATING") ? atoi(getenv("NVENCVT_RATING")) : 1100);
}
