// h264check.m: decode an Annex B H.264 stream (one picture) with Apple's software decoder and compare it with the
// source NV12 frame. Lists the NAL units first.
//   ./h264check W H in.h264 src.nv12 [dump.pgm]
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <math.h>

int main(int argc, char **argv) { @autoreleasepool {
    if (argc < 5) { fprintf(stderr, "usage: h264check W H in.h264 src.nv12 [dump.pgm]\n"); return 2; }
    int W = atoi(argv[1]), H = atoi(argv[2]);
    NSData *es = [NSData dataWithContentsOfFile:@(argv[3])], *src = [NSData dataWithContentsOfFile:@(argv[4])];
    if (!es || !src) { printf("cannot read inputs\n"); return 1; }
    const uint8_t *p = es.bytes; size_t n = es.length;
    // split on start codes
    NSMutableArray *nals = [NSMutableArray array];
    size_t i = 0, start = SIZE_MAX;
    while (i + 3 <= n) {
        if (p[i] == 0 && p[i + 1] == 0 && (p[i + 2] == 1 || (i + 3 < n && p[i + 2] == 0 && p[i + 3] == 1))) {
            size_t sc = p[i + 2] == 1 ? 3 : 4;
            if (start != SIZE_MAX) { size_t e = i; while (e > start && p[e - 1] == 0) e--; [nals addObject:[NSData dataWithBytes:p + start length:e - start]]; }
            i += sc; start = i;
        } else i++;
    }
    if (start != SIZE_MAX && start < n) [nals addObject:[NSData dataWithBytes:p + start length:n - start]];
    NSData *sps = nil, *pps = nil; NSMutableData *sample = [NSMutableData data];
    for (NSData *nal in nals) {
        int t = ((const uint8_t *)nal.bytes)[0] & 0x1f;
        printf("NAL type %2d, %6lu bytes, header 0x%02x\n", t, (unsigned long)nal.length, ((const uint8_t *)nal.bytes)[0]);
        if (t == 7) sps = nal; else if (t == 8) pps = nal;
        else if (t == 1 || t == 5) { uint32_t be = CFSwapInt32HostToBig((uint32_t)nal.length); [sample appendBytes:&be length:4]; [sample appendData:nal]; }
    }
    if (!sps || !pps || !sample.length) { printf("missing SPS/PPS/slices\n"); return 1; }
    const uint8_t *ps[2] = { sps.bytes, pps.bytes }; size_t pl[2] = { sps.length, pps.length };
    CMVideoFormatDescriptionRef fmt = NULL;
    OSStatus st = CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, 2, ps, pl, 4, &fmt);
    if (st) { printf("format description from SPS/PPS -> %d\n", (int)st); return 1; }
    CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(fmt);
    printf("SPS says %dx%d\n", d.width, d.height);
    CMBlockBufferRef bb = NULL;
    CMBlockBufferCreateWithMemoryBlock(NULL, (void *)sample.bytes, sample.length, kCFAllocatorNull, NULL, 0, sample.length, 0, &bb);
    CMSampleBufferRef sb = NULL; size_t ss = sample.length;
    CMSampleBufferCreateReady(NULL, bb, fmt, 1, 0, NULL, 1, &ss, &sb);
    NSDictionary *spec = @{ (__bridge NSString *)kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder: @NO };
    NSDictionary *attr = @{ (__bridge NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) };
    VTDecompressionSessionRef dec = NULL;
    st = VTDecompressionSessionCreate(NULL, fmt, (__bridge CFDictionaryRef)spec, (__bridge CFDictionaryRef)attr, NULL, &dec);
    if (st) { printf("decoder create -> %d\n", (int)st); return 1; }
    __block CVPixelBufferRef out = NULL; __block OSStatus ds = 0;
    VTDecompressionSessionDecodeFrameWithOutputHandler(dec, sb, 0, NULL, ^(OSStatus s, VTDecodeInfoFlags fl, CVImageBufferRef img, CMTime pts, CMTime dur) {
        ds = s; if (img) out = CVPixelBufferRetain(img); });
    VTDecompressionSessionWaitForAsynchronousFrames(dec);
    if (ds || !out) { printf("decode failed: status %d\n", (int)ds); return 1; }
    CVPixelBufferLockBaseAddress(out, kCVPixelBufferLock_ReadOnly);
    const uint8_t *sy = src.bytes, *suv = sy + (size_t)W * H;
    double se[2] = { 0, 0 }; size_t cnt[2] = { 0, 0 };
    for (int pl2 = 0; pl2 < 2; pl2++) {
        const uint8_t *o = CVPixelBufferGetBaseAddressOfPlane(out, pl2); size_t os = CVPixelBufferGetBytesPerRowOfPlane(out, pl2);
        const uint8_t *s = pl2 ? suv : sy; int rows = pl2 ? (H + 1) / 2 : H;
        for (int r = 0; r < rows; r++) for (int c = 0; c < W; c++) { double df = (double)o[r * os + c] - s[r * W + c]; se[pl2] += df * df; cnt[pl2]++; }
    }
    if (argc > 5) {
        FILE *f = fopen(argv[5], "wb"); fprintf(f, "P5 %d %d 255\n", W, H);
        const uint8_t *o = CVPixelBufferGetBaseAddressOfPlane(out, 0); size_t os = CVPixelBufferGetBytesPerRowOfPlane(out, 0);
        for (int r = 0; r < H; r++) fwrite(o + r * os, 1, W, f); fclose(f);
    }
    CVPixelBufferUnlockBaseAddress(out, kCVPixelBufferLock_ReadOnly);
    double py = 10 * log10(255.0 * 255.0 / (se[0] / cnt[0] + 1e-9)), pc = 10 * log10(255.0 * 255.0 / (se[1] / cnt[1] + 1e-9));
    printf("RESULT decoded %zux%zu, luma PSNR %.2f dB, chroma PSNR %.2f dB\n", CVPixelBufferGetWidth(out), CVPixelBufferGetHeight(out), py, pc);
    return 0;
} }
