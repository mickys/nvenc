// vtencfile.m (M12, T12.05): encode synthetic frames through VTCompressionSession and write the samples to a file:
// AV1 (av01) as IVF with temporal delimiters put back (for dav1d / ffmpeg), H.264 / HEVC as length-prefixed samples.
// Prints the encoder VideoToolbox picked and the format description's extensions; also writes the source frames.
//   ./vtencfile av01|hvc1|avc1 W H FRAMES out.ivf src.nv12      VT_BITRATE=<bit/s>  VT_10BIT=1 (x420 input)  VT_REALTIME=1
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <VideoToolbox/VideoToolbox.h>
#include <math.h>
#include <time.h>
static double now(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1e9; }

static int W, H, N; static FILE *gOut; static int gFrames; static bool gTen;

static void fill(CVPixelBufferRef pb, int f, FILE *src)
{
    CVPixelBufferLockBaseAddress(pb, 0);
    uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pb, 0), *uv = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
    size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    uint8_t *row = malloc((size_t)W);
    for (int r = 0; r < H; r++) {
        for (int c = 0; c < W; c++) {
            int v = ((c + r + f * 8) & 255) / 2 + (((((c - f * 4) >> 6) ^ (r >> 6)) & 1) ? 90 : 0);
            if (abs(c - (f * 24) % W) < 16) v = 235;
            row[c] = (uint8_t)(16 + v * 219 / 255);
            if (gTen) ((uint16_t *)(y + r * ys))[c] = (uint16_t)(row[c] << 8); else y[r * ys + c] = row[c];
        }
        fwrite(row, 1, (size_t)W, src);
    }
    for (int r = 0; r < H / 2; r++) {
        for (int c = 0; c < W; c++) {
            row[c] = (uint8_t)(c & 1 ? 128 + 60 * cos((r + f) * 0.03) : 128 + 60 * sin((c / 2 + f) * 0.02));
            if (gTen) ((uint16_t *)(uv + r * uvs))[c] = (uint16_t)(row[c] << 8); else uv[r * uvs + c] = row[c];
        }
        fwrite(row, 1, (size_t)W, src);
    }
    free(row);
    CVPixelBufferUnlockBaseAddress(pb, 0);
}

static void out_cb(void *ref, void *src, OSStatus st, VTEncodeInfoFlags fl, CMSampleBufferRef sb)
{
    if (st || !sb) { printf("frame error %d\n", (int)st); return; }
    CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb); size_t n = CMBlockBufferGetDataLength(bb);
    uint8_t *d = malloc(n + 2); CMBlockBufferCopyDataBytes(bb, 0, n, d + 2);
    CMVideoCodecType c = CMFormatDescriptionGetMediaSubType(CMSampleBufferGetFormatDescription(sb));
    if (c == 'av01') {   // IVF frame: the temporal delimiter OBU, then the sample
        d[0] = 0x12; d[1] = 0; uint32_t sz = (uint32_t)n + 2; uint64_t pts = (uint64_t)gFrames;
        fwrite(&sz, 4, 1, gOut); fwrite(&pts, 8, 1, gOut); fwrite(d, 1, sz, gOut);
    } else fwrite(d + 2, 1, n, gOut);
    if (gFrames == 0) {
        CFDictionaryRef ext = CMFormatDescriptionGetExtensions(CMSampleBufferGetFormatDescription(sb));
        NSLog(@"format description extensions: %@", ext);
    }
    gFrames++; free(d);
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 7) { fprintf(stderr, "usage: vtencfile av01|hvc1|avc1 W H FRAMES out src.nv12\n"); return 2; }
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); (void)dev;   // the driver's plugin registers encoders then
        const char *cs = argv[1]; CMVideoCodecType codec = (CMVideoCodecType)((uint32_t)cs[0] << 24 | (uint32_t)cs[1] << 16 | (uint32_t)cs[2] << 8 | (uint32_t)cs[3]);
        W = atoi(argv[2]); H = atoi(argv[3]); N = atoi(argv[4]); gTen = getenv("VT_10BIT") != NULL;
        gOut = fopen(argv[5], "wb"); FILE *src = fopen(argv[6], "wb");
        if (codec == 'av01') { uint8_t ih[32] = "DKIF"; ih[6] = 32; memcpy(ih + 8, "AV01", 4); ih[12] = W; ih[13] = W >> 8; ih[14] = H; ih[15] = H >> 8; ih[16] = 60; ih[20] = 1; ih[24] = N; fwrite(ih, 1, 32, gOut); }
        NSDictionary *spec = @{ (id)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @YES };
        OSType pf = gTen ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        NSDictionary *pbAttr = @{ (id)kCVPixelBufferPixelFormatTypeKey: @(pf), (id)kCVPixelBufferWidthKey: @(W), (id)kCVPixelBufferHeightKey: @(H),
                                  (id)kCVPixelBufferIOSurfacePropertiesKey: @{} };
        VTCompressionSessionRef enc = NULL;
        OSStatus st = VTCompressionSessionCreate(NULL, W, H, codec, (__bridge CFDictionaryRef)spec, (__bridge CFDictionaryRef)pbAttr, NULL, out_cb, NULL, &enc);
        if (st) { printf("VTCompressionSessionCreate -> %d\n", (int)st); return 1; }
        CFStringRef eid = NULL; VTSessionCopyProperty(enc, kVTCompressionPropertyKey_EncoderID, NULL, &eid);
        printf("encoder: %s\n", eid ? CFStringGetCStringPtr(eid, kCFStringEncodingUTF8) : "?");
        if (getenv("VT_BITRATE")) VTSessionSetProperty(enc, kVTCompressionPropertyKey_AverageBitRate, (__bridge CFNumberRef)@(atoi(getenv("VT_BITRATE"))));
        if (getenv("VT_REALTIME")) VTSessionSetProperty(enc, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
        VTSessionSetProperty(enc, kVTCompressionPropertyKey_ExpectedFrameRate, (__bridge CFNumberRef)@60);
        CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(enc);
        double t0 = now();
        for (int f = 0; f < N; f++) {
            CVPixelBufferRef pb = NULL; CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pb);
            fill(pb, f, src);
            st = VTCompressionSessionEncodeFrame(enc, pb, CMTimeMake(f, 60), CMTimeMake(1, 60), NULL, NULL, NULL);
            CVPixelBufferRelease(pb);
            if (st) { printf("EncodeFrame %d -> %d\n", f, (int)st); break; }
        }
        VTCompressionSessionCompleteFrames(enc, kCMTimeInvalid);
        double ms = (now() - t0) * 1000;
        VTCompressionSessionInvalidate(enc); CFRelease(enc);
        fclose(gOut); fclose(src);
        printf("RESULT %d of %d frames in %.1f ms (%.0f fps)\n", gFrames, N, ms, gFrames * 1000.0 / ms);
    }
    return 0;
}
