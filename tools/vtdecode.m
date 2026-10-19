// vtdecode.m: is there a hardware H.264 / HEVC decoder in this process, and does it decode correctly? Creates a Metal
// device first (the driver's plugin registers its decoders when a process does), then decodes every frame of a movie
// twice through VTDecompressionSession: once with a hardware decoder required, once with the software decoder, and
// compares the luma (PSNR). Prints which decoder ran and how fast.
//   ./vtdecode in.mp4 [frames]
// Build: clang -fobjc-arc -O2 vtdecode.m -o vtdecode -framework Foundation -framework AVFoundation -framework CoreMedia
//        -framework CoreVideo -framework VideoToolbox -framework Metal
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <VideoToolbox/VideoToolbox.h>
#import <Metal/Metal.h>
#include <math.h>

typedef struct { int n; NSMutableArray *frames; double ms; OSStatus err; } Out;

static void cb(void *ref, void *src, OSStatus st, VTDecodeInfoFlags f, CVImageBufferRef img, CMTime pts, CMTime dur)
{
    Out *o = ref;
    if (st) { o->err = st; return; }
    if (img) { [o->frames addObject:(__bridge id)img]; o->n++; }
}

static int decode(NSArray *samples, CMFormatDescriptionRef fd, bool hw, Out *o, bool *usedHW)
{
    NSDictionary *spec = hw ? @{ (id)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: @YES }
                            : @{ (id)kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder: @NO };
    NSDictionary *attrs = @{ (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) };
    VTDecompressionOutputCallbackRecord rec = { cb, o };
    VTDecompressionSessionRef s = NULL;
    OSStatus st = VTDecompressionSessionCreate(NULL, fd, (__bridge CFDictionaryRef)spec, (__bridge CFDictionaryRef)attrs, &rec, &s);
    if (st) { printf("  %s decoder: VTDecompressionSessionCreate -> %d\n", hw ? "hardware" : "software", (int)st); return -1; }
    CFBooleanRef b = NULL;
    VTSessionCopyProperty(s, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, NULL, &b);
    *usedHW = b == kCFBooleanTrue; if (b) CFRelease(b);
    o->frames = [NSMutableArray array];
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    for (id x in samples) {
        VTDecodeFrameFlags fl = 0; VTDecodeInfoFlags info = 0;
        st = VTDecompressionSessionDecodeFrame(s, (__bridge CMSampleBufferRef)x, fl, NULL, &info);
        if (st) { o->err = st; break; }
    }
    VTDecompressionSessionWaitForAsynchronousFrames(s);
    o->ms = (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - t0) / 1e6;
    VTDecompressionSessionInvalidate(s); CFRelease(s);
    return 0;
}

static double psnr(CVPixelBufferRef a, CVPixelBufferRef b)
{
    CVPixelBufferLockBaseAddress(a, kCVPixelBufferLock_ReadOnly); CVPixelBufferLockBaseAddress(b, kCVPixelBufferLock_ReadOnly);
    size_t w = CVPixelBufferGetWidthOfPlane(a, 0), h = CVPixelBufferGetHeightOfPlane(a, 0);
    size_t ra = CVPixelBufferGetBytesPerRowOfPlane(a, 0), rb = CVPixelBufferGetBytesPerRowOfPlane(b, 0);
    const uint8_t *pa = CVPixelBufferGetBaseAddressOfPlane(a, 0), *pb = CVPixelBufferGetBaseAddressOfPlane(b, 0);
    double se = 0;
    for (size_t y = 0; y < h; y++) for (size_t x = 0; x < w; x++) { int d = pa[y * ra + x] - pb[y * rb + x]; se += d * d; }
    CVPixelBufferUnlockBaseAddress(a, kCVPixelBufferLock_ReadOnly); CVPixelBufferUnlockBaseAddress(b, kCVPixelBufferLock_ReadOnly);
    double mse = se / (w * h);
    return mse == 0 ? 99.0 : 10 * log10(255.0 * 255.0 / mse);
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 2) { printf("usage: vtdecode in.mp4 [frames]\n"); return 2; }
        int maxf = argc > 2 ? atoi(argv[2]) : 600;
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        printf("Metal device: %s\n", dev ? dev.name.UTF8String : "none");
        AVAsset *as = [AVAsset assetWithURL:[NSURL fileURLWithPath:@(argv[1])]];
        AVAssetTrack *tr = [as tracksWithMediaType:AVMediaTypeVideo].firstObject;
        if (!tr) { printf("no video track\n"); return 1; }
        NSError *e = nil; AVAssetReader *rd = [AVAssetReader assetReaderWithAsset:as error:&e];
        AVAssetReaderTrackOutput *out = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:tr outputSettings:nil];
        [rd addOutput:out]; [rd startReading];
        NSMutableArray *samples = [NSMutableArray array]; CMFormatDescriptionRef fd = NULL;
        CMSampleBufferRef sb;
        while ((int)samples.count < maxf && (sb = [out copyNextSampleBuffer])) {
            if (CMSampleBufferGetNumSamples(sb) > 0) { if (!fd) fd = (CMFormatDescriptionRef)CFRetain(CMSampleBufferGetFormatDescription(sb)); [samples addObject:(__bridge_transfer id)sb]; }
            else CFRelease(sb);
        }
        if (!fd) { printf("no samples\n"); return 1; }
        FourCharCode c = CMFormatDescriptionGetMediaSubType(fd); CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(fd);
        printf("stream: %c%c%c%c %dx%d, %lu frames; VTIsHardwareDecodeSupported: %d\n", c >> 24, (c >> 16) & 255, (c >> 8) & 255, c & 255,
               d.width, d.height, (unsigned long)samples.count, VTIsHardwareDecodeSupported(c));
        Out h = {0}, s = {0}; bool hwH = false, hwS = false;
        int rh = decode(samples, fd, true, &h, &hwH), rs = decode(samples, fd, false, &s, &hwS);
        if (!rh) printf("  hardware decoder: %d frames in %.1f ms (%.0f fps), using hardware %d, error %d\n", h.n, h.ms, h.n * 1000.0 / h.ms, hwH, (int)h.err);
        if (!rs) printf("  software decoder: %d frames in %.1f ms (%.0f fps), using hardware %d, error %d\n", s.n, s.ms, s.n * 1000.0 / s.ms, hwS, (int)s.err);
        if (!rh && !rs && h.n && s.n) {
            int n = h.n < s.n ? h.n : s.n; double sum = 0, mn = 99;
            for (int i = 0; i < n; i++) {
                double p = psnr((__bridge CVPixelBufferRef)h.frames[i], (__bridge CVPixelBufferRef)s.frames[i]); sum += p; if (p < mn) mn = p;
                if (getenv("VTDECODE_DUMP") && i == atoi(getenv("VTDECODE_DUMP"))) {   // both outputs of frame i as NV12 (tight)
                    for (int w = 0; w < 2; w++) {
                        CVPixelBufferRef pbx = (__bridge CVPixelBufferRef)(w ? s.frames[i] : h.frames[i]);
                        CVPixelBufferLockBaseAddress(pbx, kCVPixelBufferLock_ReadOnly);
                        char fn[64]; snprintf(fn, sizeof fn, "/tmp/vtdecode-%s-%d.nv12", w ? "sw" : "hw", i); FILE *df = fopen(fn, "wb");
                        size_t W = CVPixelBufferGetWidth(pbx), H = CVPixelBufferGetHeight(pbx);
                        for (int pl = 0; pl < 2; pl++) { const uint8_t *b0 = CVPixelBufferGetBaseAddressOfPlane(pbx, pl); size_t st = CVPixelBufferGetBytesPerRowOfPlane(pbx, pl);
                            for (size_t r = 0; r < (pl ? H / 2 : H); r++) fwrite(b0 + r * st, 1, W, df); }
                        fclose(df); CVPixelBufferUnlockBaseAddress(pbx, kCVPixelBufferLock_ReadOnly);
                    }
                }
                if (p < 99 && getenv("VTDECODE_V")) {
                    CVPixelBufferRef a = (__bridge CVPixelBufferRef)h.frames[i], b = (__bridge CVPixelBufferRef)s.frames[i];
                    printf("  frame %d: %.2f dB; hw %zux%zu stride %zu, sw %zux%zu stride %zu, pts hw %.3f sw %.3f\n", i, p,
                           CVPixelBufferGetWidth(a), CVPixelBufferGetHeight(a), CVPixelBufferGetBytesPerRowOfPlane(a, 0),
                           CVPixelBufferGetWidth(b), CVPixelBufferGetHeight(b), CVPixelBufferGetBytesPerRowOfPlane(b, 0), 0.0, 0.0);
                }
            }
            printf("RESULT hw=%d frames=%d/%d psnr_hw_vs_sw avg %.2f min %.2f dB (99 = identical)\n", hwH, h.n, (int)samples.count, sum / n, mn);
        } else printf("RESULT hw=0 (no hardware decode)\n");
    }
    return 0;
}
