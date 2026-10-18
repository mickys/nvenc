// vtstress.m: create, use and tear down many VideoToolbox H.264 sessions in one process (M4, T4.07): checks the
// encoder picked, output, errors and memory growth. Input NV12 or BGRA (VideoToolbox converts BGRA for the encoder).
//   DYLD_INSERT_LIBRARIES=~/nvenc-dev/libnvencvt.dylib ./vtstress SESSIONS FRAMES W H [bgra]
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <mach/mach.h>

static double rss_mb(void)
{
    mach_task_basic_info_data_t i; mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
    task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&i, &n);
    return i.resident_size / 1048576.0;
}

int main(int argc, char **argv) { @autoreleasepool {
    if (argc < 5) { fprintf(stderr, "usage: vtstress SESSIONS FRAMES W H [bgra]\n"); return 2; }
    int S = atoi(argv[1]), F = atoi(argv[2]), W = atoi(argv[3]), H = atoi(argv[4]);
    bool bgra = argc > 5 && !strcmp(argv[5], "bgra");
    OSType pf = bgra ? kCVPixelFormatType_32BGRA : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CVPixelBufferRef pb = NULL;
    NSDictionary *attr = @{ (__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey: @{} };
    CVPixelBufferCreate(NULL, W, H, pf, (__bridge CFDictionaryRef)attr, &pb);
    CVPixelBufferLockBaseAddress(pb, 0);
    for (size_t p = 0; p < (bgra ? 1 : 2); p++) {
        uint8_t *b = bgra ? CVPixelBufferGetBaseAddress(pb) : CVPixelBufferGetBaseAddressOfPlane(pb, p);
        size_t bpr = bgra ? CVPixelBufferGetBytesPerRow(pb) : CVPixelBufferGetBytesPerRowOfPlane(pb, p);
        size_t rows = bgra || !p ? H : H / 2;
        for (size_t r = 0; r < rows; r++) for (size_t c = 0; c < bpr; c++) b[r * bpr + c] = (uint8_t)((r * 3 + c * 5) ^ (c >> 4));
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);
    NSDictionary *spec = @{ (__bridge NSString *)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @YES };
    double rss0 = 0; long frames = 0, bytes = 0, errors = 0; NSString *encoder = nil;
    for (int s = 0; s < S; s++) {
        VTCompressionSessionRef cs = NULL;
        OSStatus st = VTCompressionSessionCreate(NULL, W, H, kCMVideoCodecType_H264, (__bridge CFDictionaryRef)spec, NULL, NULL, NULL, NULL, &cs);
        if (st) { printf("session %d: create -> %d\n", s, (int)st); errors++; break; }
        if (!encoder) { CFStringRef id = NULL; VTSessionCopyProperty(cs, kVTCompressionPropertyKey_EncoderID, NULL, &id); encoder = CFBridgingRelease(id); }
        VTSessionSetProperty(cs, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
        VTSessionSetProperty(cs, kVTCompressionPropertyKey_AverageBitRate, (__bridge CFTypeRef)@(8000000));
        __block long nb = 0, nf = 0, ne = 0;
        for (int f = 0; f < F; f++)
            VTCompressionSessionEncodeFrameWithOutputHandler(cs, pb, CMTimeMake(f, 60), CMTimeMake(1, 60), NULL, NULL,
                ^(OSStatus s2, VTEncodeInfoFlags fl, CMSampleBufferRef sb) { if (s2 || !sb) ne++; else { nf++; nb += CMSampleBufferGetTotalSampleSize(sb); } });
        VTCompressionSessionCompleteFrames(cs, kCMTimeInvalid);
        VTCompressionSessionInvalidate(cs); CFRelease(cs);
        frames += nf; bytes += nb; errors += ne;
        if (s == 0) rss0 = rss_mb();
        if (s == 0 || (s + 1) % (S >= 10 ? S / 10 : 1) == 0)
            printf("session %4d: %ld frames, %ld bytes, %ld errors so far; RSS %.1f MB (after first session %.1f)\n", s + 1, frames, bytes, errors, rss_mb(), rss0);
    }
    printf("RESULT encoder=%s input=%s sessions=%d frames=%ld errors=%ld rss_growth_mb=%.1f\n", encoder.UTF8String, bgra ? "BGRA" : "NV12", S,
           frames, errors, rss_mb() - rss0);
    return errors ? 1 : 0;
} }
