# The VideoToolbox encoder plugin interface (as worked out for M4)

Sources: Apple's SPI declarations published in WebKit (`vendor/webkit-spi/CMBaseObjectSPI.h`,
`VTVideoDecoderSPI.h`: CMBaseObject machinery and the *decoder* plugin interface), VideoToolbox's export table
(`dyld_info -exports`), black-box probing with our own stub encoder (`tools/vtprobe.m`), and the layout of the encoder
object nullmoth's `libnvenc_h264` hands to VideoToolbox (observed at runtime by `trace/nvtrace.c` with
`NVTRACE_VT=1`; no disassembly).

## Registration

    OSStatus VTRegisterVideoEncoderWithInfo(FourCharCode codec, CFDictionaryRef info, CreateInstanceFn factory);
    info: CMClassImplementationID (encoder ID), CMClassImplementationName, VTRating (nullmoth: 1000),
          VTIsHardwareAccelerated
    OSStatus factory(FourCharCode codecType, CFAllocatorRef allocator, VTVideoEncoderRef *encoderOut);

Per process; a session picks the encoder by `kVTVideoEncoderSpecification_EncoderID`, or by rating among the
encoders for the codec.

## The encoder object

`CMDerivedObjectCreate(allocator, &vtable.base, VTVideoEncoderGetClassID(), &obj)`; private data in
`CMBaseObjectGetDerivedStorage(obj)` (`CMBaseClass.derivedStorageSize` bytes; nullmoth: 0xa0).

    vtable (default alignment):  { const void *reserved = NULL; const CMBaseClass *base; const EncoderClass *enc; }
    CMBaseClass (pack 4):        version 1, derivedStorageSize, equal NULL, invalidate, finalize, copyDebugDescription,
                                 copyProperty, setProperty, notificationBarrier NULL, protocolTable NULL
    EncoderClass (pack 4!):      { uint32_t version = 1;
                                   OSStatus (*startSession)(VTVideoEncoderRef, VTEncoderSessionRef, CMVideoDimensions dims);
                                   OSStatus (*encodeFrame)(VTVideoEncoderRef, VTEncoderFrameRef frame, CVImageBufferRef image,
                                                           CMTime pts, CMTime duration, CFDictionaryRef frameProperties,
                                                           VTEncodeInfoFlags *infoFlagsOut);
                                   OSStatus (*completeFrames)(VTVideoEncoderRef, CMTime completeUntilPresentationTimeStamp);
                                   OSStatus (*copySupportedPropertyDictionary)(VTVideoEncoderRef, CFDictionaryRef *out);
                                   NULL ... }

The encoder class is packed to 4 bytes like CMBaseClass: the first function pointer is at offset 4. (Unpacked, it
sits at 8 and VideoToolbox reads garbage; that was the first crash.) nullmoth's class has exactly these four
functions; slots 5+ are NULL and never called by VideoToolbox in the flows tried.

## Call flow (VTCompressionSession, 256x128 H.264)

1. factory → `startSession(enc, session, {256, 128})` (two more argument registers hold leftovers; unused).
2. `copySupportedPropertyDictionary` is called on creation, before every property set/get and many times before the
   first frame. Its result has the shape of `VTSessionCopySupportedPropertyDictionary` (key → {PropertyType,
   ReadWriteStatus, ...}). Unlisted keys fail with `kVTPropertyNotSupportedErr` (-12900).
3. `VTSessionSetProperty` / `CopyProperty` of a listed key → the CMBaseObject `setProperty` / `copyProperty`
   (`AverageBitRate` arrives as a CFNumber, `RealTime` as a CFBoolean). `PrepareToEncodeFrames` reads properties.
4. `encodeFrame(enc, frame, pixelBuffer, pts, duration, frameProperties, &infoFlags)` per frame; `frame` is an opaque
   token (1, 2, ...). Output goes back with
   `VTEncoderSessionEmitEncodedFrame(session, frame, OSStatus, VTEncodeInfoFlags, CMSampleBufferRef)`. Called inside
   `encodeFrame`, the app's output handler runs synchronously.
5. `VTCompressionSessionCompleteFrames` → `completeFrames(enc, kCMTimeInvalid)`.
6. Invalidate → base `invalidate`, then `finalize`.

Other exports a full encoder will want: `VTEncoderSessionSetPixelBufferAttributes` (ask for NV12),
`VTEncoderSessionCreateVideoFormatDescription`, `VTEncoderSessionCreateCMBlockBuffer`. The format description can
also come from the public `CMVideoFormatDescriptionCreateFromH264ParameterSets`, as the probe does.
