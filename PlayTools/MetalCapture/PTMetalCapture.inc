/*
 * PlayTools Metal Capture (PTMC)
 * Pre-compositor Metal capture overlay for PlayCover / PlayTools.
 *
 * Included into PlayLoader.m by scripts/apply_patch.py so upstream Xcode project
 * files remain untouched. Requires Metal, QuartzCore, CoreVideo, CoreMedia,
 * VideoToolbox and AVFoundation at link time (build_on_mac.sh supplies them).
 */

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <VideoToolbox/VideoToolbox.h>
#import <mach/mach_time.h>
#import <objc/message.h>
#import <objc/runtime.h>
#import <os/lock.h>
#import <stdatomic.h>

#define PTMC_LOG(fmt, ...) NSLog(@"[PTMC] " fmt, ##__VA_ARGS__)
#define PTMC_MAX_HOOKS 96

static bool ptmc_env_bool(const char *name, bool fallback) {
    const char *v = getenv(name);
    if (!v || !*v) return fallback;
    return strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "yes") == 0 || strcasecmp(v, "on") == 0;
}

static long long ptmc_env_ll(const char *name, long long fallback, long long minValue, long long maxValue) {
    const char *v = getenv(name);
    if (!v || !*v) return fallback;
    char *end = NULL;
    long long parsed = strtoll(v, &end, 10);
    if (end == v || *end != '\0') return fallback;
    if (parsed < minValue) parsed = minValue;
    if (parsed > maxValue) parsed = maxValue;
    return parsed;
}

static double ptmc_env_double(const char *name, double fallback, double minValue, double maxValue) {
    const char *v = getenv(name);
    if (!v || !*v) return fallback;
    char *end = NULL;
    double parsed = strtod(v, &end);
    if (end == v || *end != '\0') return fallback;
    if (parsed < minValue) parsed = minValue;
    if (parsed > maxValue) parsed = maxValue;
    return parsed;
}

static uint64_t ptmc_ticks_to_ns(uint64_t ticks) {
    static mach_timebase_info_data_t info;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ mach_timebase_info(&info); });
    __uint128_t scaled = (__uint128_t)ticks * info.numer;
    return (uint64_t)(scaled / info.denom);
}

static NSString *ptmc_real_user_home(void) {
    char executablePath[PATH_MAX];
    uint32_t executablePathSize = sizeof(executablePath);
    if (_NSGetExecutablePath(executablePath, &executablePathSize) == 0) {
        static const char marker[] = "/Library/Containers/io.playcover.PlayCover/";
        char *found = strstr(executablePath, marker);
        if (found) {
            size_t length = (size_t)(found - executablePath);
            if (length > 0 && length < PATH_MAX) {
                return [[NSString alloc] initWithBytes:executablePath length:length encoding:NSUTF8StringEncoding];
            }
        }
    }
    NSString *home = NSHomeDirectory();
    NSRange range = [home rangeOfString:@"/Library/Containers/io.playcover.PlayCover"];
    if (range.location != NSNotFound) return [home substringToIndex:range.location];
    return home;
}

static NSString *ptmc_default_output_path(void) {
    NSDateFormatter *fmt = [[NSDateFormatter alloc] init];
    fmt.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    fmt.dateFormat = @"yyyyMMdd-HHmmss";
    NSString *stamp = [fmt stringFromDate:[NSDate date]];
    return [ptmc_real_user_home() stringByAppendingPathComponent:[NSString stringWithFormat:@"Movies/PlayTools-Capture-%@.mov", stamp]];
}

@interface PTMCSlot : NSObject {
@public
    CVPixelBufferRef pixelBuffer;
    CVMetalTextureRef yTextureRef;
    CVMetalTextureRef uvTextureRef;
    atomic_bool inUse;
}
@property(nonatomic, strong) id<MTLTexture> yTexture;
@property(nonatomic, strong) id<MTLTexture> uvTexture;
@end

@implementation PTMCSlot
- (void)dealloc {
    if (yTextureRef) CFRelease(yTextureRef);
    if (uvTextureRef) CFRelease(uvTextureRef);
    if (pixelBuffer) CVPixelBufferRelease(pixelBuffer);
}
@end

@class PTMCSession;

@interface PTMCFrameContext : NSObject
@property(nonatomic, strong) PTMCSession *session;
@property(nonatomic, strong) PTMCSlot *slot;
@property(nonatomic) uint64_t generation;
@end
@implementation PTMCFrameContext @end

@interface PTMCSession : NSObject {
@public
    atomic_bool _accepting;
    atomic_uint_fast64_t _inFlight;
    atomic_uint_fast64_t _firstTick;
    atomic_int_fast64_t _lastPTSUnits;
}
@property(nonatomic) uint64_t generation;
@property(nonatomic) NSUInteger width;
@property(nonatomic) NSUInteger height;
@property(nonatomic) NSInteger fps;
@property(nonatomic) NSInteger bitrate;
@property(nonatomic) NSInteger bufferCount;
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, strong) id<MTLComputePipelineState> pipelineBGRA;
@property(nonatomic, strong) id<MTLComputePipelineState> pipelineSRGB;
@property(nonatomic, strong) NSArray<PTMCSlot *> *slots;
@property(nonatomic) CVMetalTextureCacheRef textureCache;
@property(nonatomic) VTCompressionSessionRef compressionSession;
@property(nonatomic, strong) AVAssetWriter *writer;
@property(nonatomic, strong) AVAssetWriterInput *writerInput;
@property(nonatomic, copy) NSString *outputPath;
@property(nonatomic) dispatch_queue_t encoderQueue;
@end

@implementation PTMCSession
- (void)dealloc {
    if (_compressionSession) {
        VTCompressionSessionInvalidate(_compressionSession);
        CFRelease(_compressionSession);
    }
    if (_textureCache) CFRelease(_textureCache);
}
@end

@interface PTMCManager : NSObject {
@public
    atomic_bool _requested;
    atomic_bool _preparing;
    atomic_uint_fast64_t _generation;
    atomic_uint_fast64_t _presented;
    atomic_uint_fast64_t _captured;
    atomic_uint_fast64_t _encoded;
    atomic_uint_fast64_t _droppedPool;
    atomic_uint_fast64_t _droppedEncoder;
    atomic_uint_fast64_t _droppedLate;
    atomic_uint_fast64_t _droppedUnsupported;
    atomic_uint_fast64_t _lastLogTick;
    atomic_uint_fast64_t _lastLogPresented;
    atomic_uint_fast64_t _lastLogEncoded;
    atomic_uint_fast64_t _drawableWidth;
    atomic_uint_fast64_t _drawableHeight;
    atomic_int_fast64_t _pixelFormat;
    atomic_int_fast64_t _framebufferOnly;
    atomic_int_fast64_t _displaySync;
}
@property(atomic, strong) PTMCSession *session;
@property(nonatomic) dispatch_queue_t controlQueue;
@property(nonatomic) NSInteger fps;
@property(nonatomic) NSInteger bitrate;
@property(nonatomic) NSInteger bufferCount;
@property(nonatomic) double logInterval;
@property(nonatomic) BOOL disableDisplaySync;
- (void)start;
- (void)stop;
- (void)prepareForTexture:(id<MTLTexture>)texture;
- (void)captureTexture:(id<MTLTexture>)texture commandBuffer:(id<MTLCommandBuffer>)commandBuffer tick:(uint64_t)tick;
- (void)logStatusForce:(BOOL)force tick:(uint64_t)tick;
@end

static PTMCManager *ptmc_manager(void);

static const char *ptmc_shader_source =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "inline float3 linear_to_srgb(float3 x) {\n"
    "    float3 lo = x * 12.92;\n"
    "    float3 hi = 1.055 * pow(max(x, float3(0.0)), float3(1.0 / 2.4)) - 0.055;\n"
    "    return select(hi, lo, x <= float3(0.0031308));\n"
    "}\n"
    "\n"
    "inline float3 load_rgb(texture2d<float, access::read> src, uint2 p, bool sourceIsSRGB) {\n"
    "    float3 rgb = src.read(p).rgb;\n"
    "    return sourceIsSRGB ? linear_to_srgb(rgb) : rgb;\n"
    "}\n"
    "\n"
    "kernel void ptmc_bgra_to_nv12(texture2d<float, access::read> src [[texture(0)]],\n"
    "                              texture2d<float, access::write> yPlane [[texture(1)]],\n"
    "                              texture2d<float, access::write> uvPlane [[texture(2)]],\n"
    "                              constant uint &sourceIsSRGB [[buffer(0)]],\n"
    "                              uint2 gid [[thread_position_in_grid]]) {\n"
    "    const uint width = src.get_width();\n"
    "    const uint height = src.get_height();\n"
    "    if (gid.x >= width || gid.y >= height) return;\n"
    "\n"
    "    float3 rgb = load_rgb(src, gid, sourceIsSRGB != 0);\n"
    "    // BT.709 non-constant-luminance RGB' -> Y'CbCr, video range (16..235 / 16..240).\n"
    "    float y = (16.0/255.0) + (219.0/255.0) * dot(rgb, float3(0.2126, 0.7152, 0.0722));\n"
    "    yPlane.write(float4(clamp(y, 0.0, 1.0)), gid);\n"
    "\n"
    "    if (((gid.x | gid.y) & 1u) == 0u) {\n"
    "        uint2 p1 = uint2(min(gid.x + 1, width - 1), gid.y);\n"
    "        uint2 p2 = uint2(gid.x, min(gid.y + 1, height - 1));\n"
    "        uint2 p3 = uint2(min(gid.x + 1, width - 1), min(gid.y + 1, height - 1));\n"
    "        float3 avg = (rgb + load_rgb(src, p1, sourceIsSRGB != 0) +\n"
    "                      load_rgb(src, p2, sourceIsSRGB != 0) + load_rgb(src, p3, sourceIsSRGB != 0)) * 0.25;\n"
    "        float cb = (128.0/255.0) + (224.0/255.0) * dot(avg, float3(-0.114572, -0.385428, 0.5));\n"
    "        float cr = (128.0/255.0) + (224.0/255.0) * dot(avg, float3(0.5, -0.454153, -0.045847));\n"
    "        uvPlane.write(float4(clamp(cb, 0.0, 1.0), clamp(cr, 0.0, 1.0), 0.0, 1.0), gid / 2u);\n"
    "    }\n"
    "}\n"
;

static void ptmc_release_slot(PTMCSlot *slot) {
    atomic_store_explicit(&slot->inUse, false, memory_order_release);
}

static PTMCSlot *ptmc_acquire_slot(PTMCSession *session) {
    for (PTMCSlot *slot in session.slots) {
        bool expected = false;
        if (atomic_compare_exchange_strong_explicit(&slot->inUse, &expected, true,
                                                     memory_order_acq_rel, memory_order_relaxed)) {
            return slot;
        }
    }
    return nil;
}

static void ptmc_finalize_session_when_drained(PTMCSession *session) {
    dispatch_async(session.encoderQueue, ^{
        if (atomic_load_explicit(&session->_inFlight, memory_order_acquire) != 0) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC), session.encoderQueue, ^{
                ptmc_finalize_session_when_drained(session);
            });
            return;
        }
        if (session.compressionSession) {
            VTCompressionSessionCompleteFrames(session.compressionSession, kCMTimeInvalid);
        }
        if (session.writerInput) [session.writerInput markAsFinished];
        AVAssetWriter *writer = session.writer;
        if (writer && writer.status == AVAssetWriterStatusWriting) {
            [writer finishWritingWithCompletionHandler:^{
                if (writer.status == AVAssetWriterStatusCompleted) {
                    PTMC_LOG(@"recording finalized: %@", session.outputPath);
                } else {
                    PTMC_LOG(@"writer finalize failed: %@", writer.error);
                }
            }];
        }
    });
}

static BOOL ptmc_ensure_writer(PTMCSession *session, CMSampleBufferRef sampleBuffer) {
    if (session.writer) return session.writer.status == AVAssetWriterStatusWriting;
    NSError *error = nil;
    NSURL *url = [NSURL fileURLWithPath:session.outputPath];
    [[NSFileManager defaultManager] createDirectoryAtURL:[url URLByDeletingLastPathComponent]
                             withIntermediateDirectories:YES attributes:nil error:&error];
    if (error) {
        PTMC_LOG(@"cannot create output directory: %@", error);
        return NO;
    }
    [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
    AVAssetWriter *writer = [[AVAssetWriter alloc] initWithURL:url fileType:AVFileTypeQuickTimeMovie error:&error];
    if (!writer) {
        PTMC_LOG(@"AVAssetWriter init failed: %@", error);
        return NO;
    }
    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sampleBuffer);
    AVAssetWriterInput *input = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeVideo
                                                               outputSettings:nil
                                                            sourceFormatHint:format];
    input.expectsMediaDataInRealTime = YES;
    if (![writer canAddInput:input]) {
        PTMC_LOG(@"AVAssetWriter cannot add HEVC passthrough input");
        return NO;
    }
    [writer addInput:input];
    if (![writer startWriting]) {
        PTMC_LOG(@"AVAssetWriter start failed: %@", writer.error);
        return NO;
    }
    [writer startSessionAtSourceTime:kCMTimeZero];
    session.writer = writer;
    session.writerInput = input;
    return YES;
}

static void ptmc_vt_output_callback(void *outputCallbackRefCon,
                                    void *sourceFrameRefCon,
                                    OSStatus status,
                                    VTEncodeInfoFlags infoFlags,
                                    CMSampleBufferRef sampleBuffer) {
    (void)outputCallbackRefCon;
    PTMCFrameContext *ctx = (__bridge_transfer PTMCFrameContext *)sourceFrameRefCon;
    PTMCSession *session = ctx.session;
    PTMCSlot *slot = ctx.slot;
    if (!session || !slot) return;

    if (sampleBuffer) CFRetain(sampleBuffer);
    dispatch_async(session.encoderQueue, ^{
        PTMCManager *manager = ptmc_manager();
        BOOL sameGeneration = atomic_load_explicit(&manager->_generation, memory_order_acquire) == ctx.generation;
        BOOL good = status == noErr && sampleBuffer != NULL && !(infoFlags & kVTEncodeInfo_FrameDropped);
        if (good && sameGeneration) {
            if (ptmc_ensure_writer(session, sampleBuffer)) {
                // Writer backpressure occurs off the render thread. Holding the slot here bounds queued work.
                int spins = 0;
                while (!session.writerInput.readyForMoreMediaData && spins < 250 &&
                       atomic_load_explicit(&session->_accepting, memory_order_acquire)) {
                    usleep(1000);
                    spins++;
                }
                if (session.writerInput.readyForMoreMediaData && [session.writerInput appendSampleBuffer:sampleBuffer]) {
                    atomic_fetch_add_explicit(&manager->_encoded, 1, memory_order_relaxed);
                } else {
                    atomic_fetch_add_explicit(&manager->_droppedEncoder, 1, memory_order_relaxed);
                    PTMC_LOG(@"writer backpressure/failure; stopping capture to avoid a corrupt GOP");
                    [manager stop];
                }
            } else {
                atomic_fetch_add_explicit(&manager->_droppedEncoder, 1, memory_order_relaxed);
            }
        } else if (sameGeneration) {
            atomic_fetch_add_explicit(&manager->_droppedEncoder, 1, memory_order_relaxed);
        }
        if (sampleBuffer) CFRelease(sampleBuffer);
        ptmc_release_slot(slot);
        atomic_fetch_sub_explicit(&session->_inFlight, 1, memory_order_acq_rel);
    });
}

static BOOL ptmc_create_video_session(PTMCSession *session) {
    NSMutableDictionary *encoderSpec = [NSMutableDictionary dictionary];
    // RequireHardwareAcceleratedVideoEncoder was added after our iOS 15 build target.
    // Resolve it dynamically so the patched iOS binary keeps its upstream deployment target.
    CFStringRef requireHWKey = NULL;
    void *requireHWSymbol = dlsym(RTLD_DEFAULT, "kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder");
    if (requireHWSymbol) requireHWKey = *(CFStringRef *)requireHWSymbol;
    if (requireHWKey) {
        encoderSpec[(__bridge NSString *)requireHWKey] = @YES;
    } else {
        CFStringRef enableHWKey = NULL;
        void *enableHWSymbol = dlsym(RTLD_DEFAULT, "kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder");
        if (enableHWSymbol) enableHWKey = *(CFStringRef *)enableHWSymbol;
        if (enableHWKey) {
            encoderSpec[(__bridge NSString *)enableHWKey] = @YES;
            PTMC_LOG(@"hardware-required key unavailable; hardware acceleration is requested");
        } else {
            PTMC_LOG(@"hardware encoder-selection keys unavailable at runtime");
        }
    }
    NSDictionary *sourceAttrs = @{
        (__bridge NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (__bridge NSString *)kCVPixelBufferWidthKey: @(session.width),
        (__bridge NSString *)kCVPixelBufferHeightKey: @(session.height),
        (__bridge NSString *)kCVPixelBufferMetalCompatibilityKey: @YES,
        (__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey: @{}
    };
    VTCompressionSessionRef vt = NULL;
    OSStatus err = VTCompressionSessionCreate(kCFAllocatorDefault,
                                               (int32_t)session.width,
                                               (int32_t)session.height,
                                               kCMVideoCodecType_HEVC,
                                               (__bridge CFDictionaryRef)encoderSpec,
                                               (__bridge CFDictionaryRef)sourceAttrs,
                                               NULL,
                                               ptmc_vt_output_callback,
                                               NULL,
                                               &vt);
    if (err != noErr || !vt) {
        PTMC_LOG(@"hardware HEVC session creation failed: %d", (int)err);
        return NO;
    }
    session.compressionSession = vt;
    VTSessionSetProperty(vt, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    VTSessionSetProperty(vt, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
    VTSessionSetProperty(vt, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_HEVC_Main_AutoLevel);
    NSInteger expectedFPS = session.fps;
    CFNumberRef fps = CFNumberCreate(kCFAllocatorDefault, kCFNumberNSIntegerType, &expectedFPS);
    if (fps) { VTSessionSetProperty(vt, kVTCompressionPropertyKey_ExpectedFrameRate, fps); CFRelease(fps); }
    NSInteger bitrate = session.bitrate;
    CFNumberRef br = CFNumberCreate(kCFAllocatorDefault, kCFNumberNSIntegerType, &bitrate);
    if (br) { VTSessionSetProperty(vt, kVTCompressionPropertyKey_AverageBitRate, br); CFRelease(br); }
    NSInteger keyInterval = MAX(session.fps * 2, 1);
    CFNumberRef key = CFNumberCreate(kCFAllocatorDefault, kCFNumberNSIntegerType, &keyInterval);
    if (key) { VTSessionSetProperty(vt, kVTCompressionPropertyKey_MaxKeyFrameInterval, key); CFRelease(key); }
    VTSessionSetProperty(vt, kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2);
    VTSessionSetProperty(vt, kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2);
    VTSessionSetProperty(vt, kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2);
    err = VTCompressionSessionPrepareToEncodeFrames(vt);
    if (err != noErr) {
        PTMC_LOG(@"VT prepare failed: %d", (int)err);
        return NO;
    }
    PTMC_LOG(@"encoder HEVC hardware required, %ldx%ld @ %ld fps, bitrate=%ld",
             (long)session.width, (long)session.height, (long)session.fps, (long)session.bitrate);
    return YES;
}

static BOOL ptmc_create_slots(PTMCSession *session) {
    NSDictionary *attrs = @{
        (__bridge NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (__bridge NSString *)kCVPixelBufferWidthKey: @(session.width),
        (__bridge NSString *)kCVPixelBufferHeightKey: @(session.height),
        (__bridge NSString *)kCVPixelBufferMetalCompatibilityKey: @YES,
        (__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey: @{},
    };
    CVPixelBufferPoolRef pool = NULL;
    NSDictionary *poolAttrs = @{ (__bridge NSString *)kCVPixelBufferPoolMinimumBufferCountKey: @(session.bufferCount) };
    CVReturn rv = CVPixelBufferPoolCreate(kCFAllocatorDefault, (__bridge CFDictionaryRef)poolAttrs,
                                          (__bridge CFDictionaryRef)attrs, &pool);
    if (rv != kCVReturnSuccess || !pool) {
        PTMC_LOG(@"CVPixelBufferPoolCreate failed: %d", rv);
        return NO;
    }
    CVMetalTextureCacheRef cache = NULL;
    rv = CVMetalTextureCacheCreate(kCFAllocatorDefault, NULL, session.device, NULL, &cache);
    if (rv != kCVReturnSuccess || !cache) {
        CFRelease(pool);
        PTMC_LOG(@"CVMetalTextureCacheCreate failed: %d", rv);
        return NO;
    }
    session.textureCache = cache;
    NSMutableArray *slots = [NSMutableArray arrayWithCapacity:session.bufferCount];
    for (NSInteger i = 0; i < session.bufferCount; i++) {
        CVPixelBufferRef pb = NULL;
        rv = CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pb);
        if (rv != kCVReturnSuccess || !pb) break;
        CVBufferSetAttachment(pb, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(pb, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(pb, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
        PTMCSlot *slot = [[PTMCSlot alloc] init];
        slot->pixelBuffer = pb;
        atomic_init(&slot->inUse, false);
        rv = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, cache, pb, NULL,
                                                       MTLPixelFormatR8Unorm, session.width, session.height, 0,
                                                       &slot->yTextureRef);
        if (rv != kCVReturnSuccess || !slot->yTextureRef) break;
        rv = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, cache, pb, NULL,
                                                       MTLPixelFormatRG8Unorm, session.width / 2, session.height / 2, 1,
                                                       &slot->uvTextureRef);
        if (rv != kCVReturnSuccess || !slot->uvTextureRef) break;
        slot.yTexture = CVMetalTextureGetTexture(slot->yTextureRef);
        slot.uvTexture = CVMetalTextureGetTexture(slot->uvTextureRef);
        if (!slot.yTexture || !slot.uvTexture) break;
        [slots addObject:slot];
    }
    CFRelease(pool); // fixed PTMCSlot objects retain the buffers; no per-frame allocation occurs.
    if (slots.count != session.bufferCount) {
        PTMC_LOG(@"failed to preallocate NV12 ring (%lu/%ld)", (unsigned long)slots.count, (long)session.bufferCount);
        return NO;
    }
    session.slots = slots;
    return YES;
}

static BOOL ptmc_create_pipelines(PTMCSession *session) {
    NSError *error = nil;
    NSString *source = [NSString stringWithUTF8String:ptmc_shader_source];
    id<MTLLibrary> library = [session.device newLibraryWithSource:source options:nil error:&error];
    if (!library) {
        PTMC_LOG(@"Metal shader compile failed: %@", error);
        return NO;
    }
    id<MTLFunction> fn = [library newFunctionWithName:@"ptmc_bgra_to_nv12"];
    if (!fn) return NO;
    id<MTLComputePipelineState> pipeline = [session.device newComputePipelineStateWithFunction:fn error:&error];
    if (!pipeline) {
        PTMC_LOG(@"Metal compute pipeline failed: %@", error);
        return NO;
    }
    session.pipelineBGRA = pipeline;
    session.pipelineSRGB = pipeline;
    return YES;
}

@implementation PTMCManager
- (instancetype)init {
    if ((self = [super init])) {
        _controlQueue = dispatch_queue_create("io.playcover.ptmc.control", DISPATCH_QUEUE_SERIAL);
        atomic_init(&_requested, false);
        atomic_init(&_preparing, false);
        atomic_init(&_generation, 1);
        atomic_init(&_presented, 0); atomic_init(&_captured, 0); atomic_init(&_encoded, 0);
        atomic_init(&_droppedPool, 0); atomic_init(&_droppedEncoder, 0); atomic_init(&_droppedLate, 0); atomic_init(&_droppedUnsupported, 0);
        atomic_init(&_lastLogTick, mach_continuous_time()); atomic_init(&_lastLogPresented, 0); atomic_init(&_lastLogEncoded, 0);
        atomic_init(&_drawableWidth, 0); atomic_init(&_drawableHeight, 0); atomic_init(&_pixelFormat, -1);
        atomic_init(&_framebufferOnly, -1); atomic_init(&_displaySync, -1);
        _fps = (NSInteger)ptmc_env_ll("PTMC_FPS", 120, 1, 240);
        _bitrate = (NSInteger)ptmc_env_ll("PTMC_BITRATE", 120000000, 1000000, 1000000000);
        _bufferCount = (NSInteger)ptmc_env_ll("PTMC_BUFFERS", 6, 3, 16);
        _logInterval = ptmc_env_double("PTMC_LOG_INTERVAL", 1.0, 0.25, 60.0);
        _disableDisplaySync = ptmc_env_bool("PTMC_DISABLE_DISPLAY_SYNC", false);
    }
    return self;
}

- (void)start {
    atomic_store_explicit(&_requested, true, memory_order_release);
    PTMC_LOG(@"start requested; capture will arm on the next supported drawable");
}

- (void)stop {
    atomic_store_explicit(&_requested, false, memory_order_release);
    PTMCSession *session = self.session;
    self.session = nil;
    atomic_store_explicit(&_preparing, false, memory_order_release);
    atomic_fetch_add_explicit(&_generation, 1, memory_order_acq_rel);
    if (session) {
        atomic_store_explicit(&session->_accepting, false, memory_order_release);
        dispatch_async(_controlQueue, ^{
            if (session.compressionSession) VTCompressionSessionCompleteFrames(session.compressionSession, kCMTimeInvalid);
            ptmc_finalize_session_when_drained(session);
        });
    }
    PTMC_LOG(@"stop requested");
}

- (void)prepareForTexture:(id<MTLTexture>)texture {
    if (!atomic_load_explicit(&_requested, memory_order_acquire) || self.session) return;
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&_preparing, &expected, true, memory_order_acq_rel, memory_order_relaxed)) return;
    NSUInteger width = texture.width, height = texture.height;
    id<MTLDevice> device = texture.device;
    dispatch_async(_controlQueue, ^{
        if (!atomic_load_explicit(&self->_requested, memory_order_acquire)) {
            atomic_store_explicit(&self->_preparing, false, memory_order_release);
            return;
        }
        PTMCSession *s = [[PTMCSession alloc] init];
        s.generation = atomic_fetch_add_explicit(&self->_generation, 1, memory_order_acq_rel) + 1;
        s.width = width; s.height = height; s.fps = self.fps; s.bitrate = self.bitrate; s.bufferCount = self.bufferCount;
        s.device = device;
        s.encoderQueue = dispatch_queue_create("io.playcover.ptmc.encoder", DISPATCH_QUEUE_SERIAL);
        atomic_init(&s->_accepting, true); atomic_init(&s->_inFlight, 0); atomic_init(&s->_firstTick, 0); atomic_init(&s->_lastPTSUnits, -1);
        const char *out = getenv("PTMC_OUTPUT");
        s.outputPath = (out && *out) ? [NSString stringWithUTF8String:out] : ptmc_default_output_path();
        BOOL ok = ptmc_create_pipelines(s) && ptmc_create_slots(s) && ptmc_create_video_session(s);
        if (ok && atomic_load_explicit(&self->_requested, memory_order_acquire)) {
            self.session = s;
            PTMC_LOG(@"capture armed generation=%llu drawable=%lux%lu buffers=%ld output=%@",
                     (unsigned long long)s.generation, (unsigned long)s.width, (unsigned long)s.height,
                     (long)s.bufferCount, s.outputPath);
        } else {
            atomic_store_explicit(&s->_accepting, false, memory_order_release);
            atomic_store_explicit(&self->_requested, false, memory_order_release);
            PTMC_LOG(@"capture prepare failed");
        }
        atomic_store_explicit(&self->_preparing, false, memory_order_release);
    });
}

- (void)captureTexture:(id<MTLTexture>)texture commandBuffer:(id<MTLCommandBuffer>)commandBuffer tick:(uint64_t)tick {
    if (!atomic_load_explicit(&_requested, memory_order_acquire)) return;
    PTMCSession *session = self.session;
    if (!session) { [self prepareForTexture:texture]; return; }
    if (!atomic_load_explicit(&session->_accepting, memory_order_acquire)) return;
    if (texture.width != session.width || texture.height != session.height) {
        atomic_fetch_add_explicit(&_droppedUnsupported, 1, memory_order_relaxed);
        PTMC_LOG(@"drawable size changed %lux%lu -> %lux%lu; stopping safely (restart capture to re-arm)",
                 (unsigned long)session.width, (unsigned long)session.height,
                 (unsigned long)texture.width, (unsigned long)texture.height);
        [self stop];
        return;
    }
    BOOL sourceSRGB = texture.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB;
    if (texture.pixelFormat != MTLPixelFormatBGRA8Unorm && !sourceSRGB) {
        atomic_fetch_add_explicit(&_droppedUnsupported, 1, memory_order_relaxed);
        return;
    }
    PTMCSlot *slot = ptmc_acquire_slot(session);
    if (!slot) {
        atomic_fetch_add_explicit(&_droppedPool, 1, memory_order_relaxed);
        return;
    }
    id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
    if (!encoder) {
        ptmc_release_slot(slot);
        atomic_fetch_add_explicit(&_droppedEncoder, 1, memory_order_relaxed);
        return;
    }
    [encoder setComputePipelineState:session.pipelineBGRA];
    [encoder setTexture:texture atIndex:0];
    [encoder setTexture:slot.yTexture atIndex:1];
    [encoder setTexture:slot.uvTexture atIndex:2];
    uint32_t srgb = sourceSRGB ? 1 : 0;
    [encoder setBytes:&srgb length:sizeof(srgb) atIndex:0];
    NSUInteger w = session.pipelineBGRA.threadExecutionWidth;
    NSUInteger h = MAX(1, session.pipelineBGRA.maxTotalThreadsPerThreadgroup / w);
    MTLSize tg = MTLSizeMake(w, MIN(h, 16), 1);
    MTLSize grid = MTLSizeMake(texture.width, texture.height, 1);
    [encoder dispatchThreads:grid threadsPerThreadgroup:tg];
    [encoder endEncoding];

    uint64_t zero = 0;
    atomic_compare_exchange_strong_explicit(&session->_firstTick, &zero, tick, memory_order_acq_rel, memory_order_relaxed);
    uint64_t base = atomic_load_explicit(&session->_firstTick, memory_order_acquire);
    uint64_t ns = ptmc_ticks_to_ns(tick - base);
    int64_t units = (int64_t)(((__uint128_t)ns * 60000u) / 1000000000u);
    int64_t last = atomic_load_explicit(&session->_lastPTSUnits, memory_order_relaxed);
    while (units <= last || !atomic_compare_exchange_weak_explicit(&session->_lastPTSUnits, &last, units,
                                                                    memory_order_acq_rel, memory_order_relaxed)) {
        if (units <= last) units = last + 1;
    }
    CMTime pts = CMTimeMake(units, 60000);
    CMTime duration = CMTimeMake(1, (int32_t)session.fps);
    uint64_t generation = session.generation;
    atomic_fetch_add_explicit(&session->_inFlight, 1, memory_order_acq_rel);
    atomic_fetch_add_explicit(&_captured, 1, memory_order_relaxed);

    [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> cb) {
        if (cb.status != MTLCommandBufferStatusCompleted || cb.error) {
            ptmc_release_slot(slot);
            atomic_fetch_sub_explicit(&session->_inFlight, 1, memory_order_acq_rel);
            atomic_fetch_add_explicit(&self->_droppedEncoder, 1, memory_order_relaxed);
            return;
        }
        if (atomic_load_explicit(&self->_generation, memory_order_acquire) != generation ||
            !atomic_load_explicit(&session->_accepting, memory_order_acquire)) {
            ptmc_release_slot(slot);
            atomic_fetch_sub_explicit(&session->_inFlight, 1, memory_order_acq_rel);
            atomic_fetch_add_explicit(&self->_droppedLate, 1, memory_order_relaxed);
            return;
        }
        PTMCFrameContext *ctx = [[PTMCFrameContext alloc] init];
        ctx.session = session; ctx.slot = slot; ctx.generation = generation;
        VTEncodeInfoFlags flags = 0;
        void *frameRefCon = (__bridge_retained void *)ctx;
        OSStatus err = VTCompressionSessionEncodeFrame(session.compressionSession, slot->pixelBuffer, pts, duration,
                                                        NULL, frameRefCon, &flags);
        if (err != noErr) {
            CFRelease((CFTypeRef)frameRefCon);
            ptmc_release_slot(slot);
            atomic_fetch_sub_explicit(&session->_inFlight, 1, memory_order_acq_rel);
            atomic_fetch_add_explicit(&self->_droppedEncoder, 1, memory_order_relaxed);
        }
    }];
}

- (void)logStatusForce:(BOOL)force tick:(uint64_t)tick {
    uint64_t previous = atomic_load_explicit(&_lastLogTick, memory_order_relaxed);
    uint64_t elapsedNs = ptmc_ticks_to_ns(tick - previous);
    uint64_t threshold = (uint64_t)(_logInterval * 1000000000.0);
    if (!force && elapsedNs < threshold) return;
    if (!atomic_compare_exchange_strong_explicit(&_lastLogTick, &previous, tick, memory_order_acq_rel, memory_order_relaxed) && !force) return;
    double seconds = MAX((double)elapsedNs / 1e9, 0.001);
    uint64_t p = atomic_load_explicit(&_presented, memory_order_relaxed);
    uint64_t e = atomic_load_explicit(&_encoded, memory_order_relaxed);
    uint64_t lp = atomic_exchange_explicit(&_lastLogPresented, p, memory_order_acq_rel);
    uint64_t le = atomic_exchange_explicit(&_lastLogEncoded, e, memory_order_acq_rel);
    PTMC_LOG(@"present=%.2f fps drawable=%llux%llu pixelFormat=%lld displaySync=%lld framebufferOnly=%lld captured=%llu encoded=%.2f fps dropped_pool=%llu dropped_encoder=%llu dropped_late=%llu unsupported=%llu requested=%d",
             (p - lp) / seconds,
             (unsigned long long)atomic_load(&_drawableWidth), (unsigned long long)atomic_load(&_drawableHeight),
             (long long)atomic_load(&_pixelFormat), (long long)atomic_load(&_displaySync), (long long)atomic_load(&_framebufferOnly),
             (unsigned long long)atomic_load(&_captured), (e - le) / seconds,
             (unsigned long long)atomic_load(&_droppedPool), (unsigned long long)atomic_load(&_droppedEncoder),
             (unsigned long long)atomic_load(&_droppedLate), (unsigned long long)atomic_load(&_droppedUnsupported),
             atomic_load(&_requested));
}
@end

static PTMCManager *ptmc_manager(void) {
    static PTMCManager *manager;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ manager = [[PTMCManager alloc] init]; });
    return manager;
}

// ----- Runtime hooks -------------------------------------------------------

typedef struct {
    __unsafe_unretained Class cls;
    SEL selector;
    IMP original;
} PTMCHookEntry;

static PTMCHookEntry ptmc_hooks[PTMC_MAX_HOOKS];
static atomic_uint ptmc_hook_count;
static os_unfair_lock ptmc_hook_lock = OS_UNFAIR_LOCK_INIT;
static IMP ptmc_next_drawable_original = NULL;
static atomic_bool ptmc_discovery_scheduled;
static NSInteger ptmc_spoof_max_fps = 0;
static IMP ptmc_max_fps_original = NULL;

static IMP ptmc_original_imp(id self, SEL selector) {
    unsigned count = atomic_load_explicit(&ptmc_hook_count, memory_order_acquire);
    Class cls = object_getClass(self);
    for (unsigned i = 0; i < count; i++) {
        if (ptmc_hooks[i].cls == cls && ptmc_hooks[i].selector == selector) return ptmc_hooks[i].original;
    }
    return NULL;
}

static BOOL ptmc_class_implements_selector(Class cls, SEL selector, Method *methodOut) {
    unsigned count = 0;
    Method *methods = class_copyMethodList(cls, &count);
    BOOL found = NO;
    for (unsigned i = 0; i < count; i++) {
        if (method_getName(methods[i]) == selector) {
            if (methodOut) *methodOut = methods[i];
            found = YES;
            break;
        }
    }
    free(methods);
    return found;
}

static void ptmc_record_present(id commandBuffer, id<CAMetalDrawable> drawable) {
    PTMCManager *manager = ptmc_manager();
    uint64_t tick = mach_continuous_time();
    atomic_fetch_add_explicit(&manager->_presented, 1, memory_order_relaxed);
    id<MTLTexture> texture = drawable.texture;
    if (texture) {
        atomic_store_explicit(&manager->_drawableWidth, texture.width, memory_order_relaxed);
        atomic_store_explicit(&manager->_drawableHeight, texture.height, memory_order_relaxed);
        atomic_store_explicit(&manager->_pixelFormat, texture.pixelFormat, memory_order_relaxed);
        [manager captureTexture:texture commandBuffer:(id<MTLCommandBuffer>)commandBuffer tick:tick];
    }
    [manager logStatusForce:NO tick:tick];
}

static void ptmc_present_drawable(id self, SEL _cmd, id<CAMetalDrawable> drawable) {
    ptmc_record_present(self, drawable);
    IMP original = ptmc_original_imp(self, _cmd);
    if (original) ((void (*)(id, SEL, id<CAMetalDrawable>))original)(self, _cmd, drawable);
}

static void ptmc_present_drawable_at_time(id self, SEL _cmd, id<CAMetalDrawable> drawable, CFTimeInterval time) {
    ptmc_record_present(self, drawable);
    IMP original = ptmc_original_imp(self, _cmd);
    if (original) ((void (*)(id, SEL, id<CAMetalDrawable>, CFTimeInterval))original)(self, _cmd, drawable, time);
}

static void ptmc_present_drawable_after_duration(id self, SEL _cmd, id<CAMetalDrawable> drawable, CFTimeInterval duration) {
    ptmc_record_present(self, drawable);
    IMP original = ptmc_original_imp(self, _cmd);
    if (original) ((void (*)(id, SEL, id<CAMetalDrawable>, CFTimeInterval))original)(self, _cmd, drawable, duration);
}

static BOOL ptmc_install_one_hook(Class cls, SEL selector, IMP replacement) {
    Method ownMethod = NULL;
    if (!ptmc_class_implements_selector(cls, selector, &ownMethod) || !ownMethod) return NO;
    os_unfair_lock_lock(&ptmc_hook_lock);
    unsigned count = atomic_load_explicit(&ptmc_hook_count, memory_order_relaxed);
    for (unsigned i = 0; i < count; i++) {
        if (ptmc_hooks[i].cls == cls && ptmc_hooks[i].selector == selector) {
            os_unfair_lock_unlock(&ptmc_hook_lock);
            return NO;
        }
    }
    if (count >= PTMC_MAX_HOOKS) {
        os_unfair_lock_unlock(&ptmc_hook_lock);
        PTMC_LOG(@"hook table full");
        return NO;
    }
    IMP original = method_getImplementation(ownMethod);
    if (original == replacement) {
        os_unfair_lock_unlock(&ptmc_hook_lock);
        return NO;
    }
    ptmc_hooks[count] = (PTMCHookEntry){ .cls = cls, .selector = selector, .original = original };
    method_setImplementation(ownMethod, replacement);
    atomic_store_explicit(&ptmc_hook_count, count + 1, memory_order_release);
    os_unfair_lock_unlock(&ptmc_hook_lock);
    return YES;
}

static void ptmc_discover_command_buffer_classes(void) {
    int count = objc_getClassList(NULL, 0);
    if (count <= 0) return;
    __unsafe_unretained Class *classes = (__unsafe_unretained Class *)calloc((size_t)count, sizeof(Class));
    if (!classes) return;
    count = objc_getClassList(classes, count);
    Protocol *protocol = @protocol(MTLCommandBuffer);
    unsigned installed = 0;
    SEL s1 = @selector(presentDrawable:);
    SEL s2 = @selector(presentDrawable:atTime:);
    SEL s3 = @selector(presentDrawable:afterMinimumDuration:);
    for (int i = 0; i < count; i++) {
        Class cls = classes[i];
        if (!class_conformsToProtocol(cls, protocol)) continue;
        installed += ptmc_install_one_hook(cls, s1, (IMP)ptmc_present_drawable);
        installed += ptmc_install_one_hook(cls, s2, (IMP)ptmc_present_drawable_at_time);
        installed += ptmc_install_one_hook(cls, s3, (IMP)ptmc_present_drawable_after_duration);
    }
    free(classes);
    if (installed) PTMC_LOG(@"installed %u new present hooks (total=%u)", installed, atomic_load(&ptmc_hook_count));
}

static void ptmc_schedule_discovery(void) {
    bool expected = false;
    if (!atomic_compare_exchange_strong(&ptmc_discovery_scheduled, &expected, true)) return;
    static const double delays[] = {0.0, 0.05, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0};
    dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);
    for (unsigned i = 0; i < sizeof(delays)/sizeof(delays[0]); i++) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delays[i] * NSEC_PER_SEC)), q, ^{
            ptmc_discover_command_buffer_classes();
        });
    }
}

static id<CAMetalDrawable> ptmc_next_drawable(CAMetalLayer *self, SEL _cmd) {
    PTMCManager *manager = ptmc_manager();
    BOOL requested = atomic_load_explicit(&manager->_requested, memory_order_acquire);
    if (requested && self.framebufferOnly) self.framebufferOnly = NO;
    if (manager.disableDisplaySync) {
        SEL setter = NSSelectorFromString(@"setDisplaySyncEnabled:");
        if ([self respondsToSelector:setter]) ((void (*)(id, SEL, BOOL))objc_msgSend)(self, setter, NO);
    }
    atomic_store_explicit(&manager->_framebufferOnly, self.framebufferOnly ? 1 : 0, memory_order_relaxed);
    SEL getter = NSSelectorFromString(@"displaySyncEnabled");
    if ([self respondsToSelector:getter]) {
        BOOL sync = ((BOOL (*)(id, SEL))objc_msgSend)(self, getter);
        atomic_store_explicit(&manager->_displaySync, sync ? 1 : 0, memory_order_relaxed);
    }
    ptmc_schedule_discovery();
    return ((id<CAMetalDrawable> (*)(id, SEL))ptmc_next_drawable_original)(self, _cmd);
}

static NSInteger ptmc_maximum_frames_per_second(id self, SEL _cmd) {
    if (ptmc_spoof_max_fps > 0) return ptmc_spoof_max_fps;
    return ptmc_max_fps_original ? ((NSInteger (*)(id, SEL))ptmc_max_fps_original)(self, _cmd) : 60;
}

static void ptmc_control_notification(CFNotificationCenterRef center, void *observer, CFStringRef name,
                                      const void *object, CFDictionaryRef userInfo) {
    (void)center; (void)observer; (void)object; (void)userInfo;
    PTMCManager *manager = ptmc_manager();
    if (CFEqual(name, CFSTR("io.playcover.ptmc.start"))) [manager start];
    else if (CFEqual(name, CFSTR("io.playcover.ptmc.stop"))) [manager stop];
    else if (CFEqual(name, CFSTR("io.playcover.ptmc.status"))) {
        [manager logStatusForce:YES tick:mach_continuous_time()];
        ptmc_discover_command_buffer_classes();
    }
}

static void __attribute__((constructor)) ptmc_initialize(void) {
    if (!ptmc_env_bool("PTMC_ENABLE", false)) return;
    atomic_init(&ptmc_hook_count, 0);
    atomic_init(&ptmc_discovery_scheduled, false);
    PTMCManager *manager = ptmc_manager();
    Method next = class_getInstanceMethod([CAMetalLayer class], @selector(nextDrawable));
    if (next) {
        ptmc_next_drawable_original = method_getImplementation(next);
        method_setImplementation(next, (IMP)ptmc_next_drawable);
    }
    ptmc_spoof_max_fps = (NSInteger)ptmc_env_ll("PTMC_SPOOF_MAX_FPS", 0, 0, 240);
    if (ptmc_spoof_max_fps > 0) {
        Method maxFPS = class_getInstanceMethod([UIScreen class], @selector(maximumFramesPerSecond));
        if (maxFPS) {
            ptmc_max_fps_original = method_getImplementation(maxFPS);
            method_setImplementation(maxFPS, (IMP)ptmc_maximum_frames_per_second);
        }
    }
    CFNotificationCenterRef darwin = CFNotificationCenterGetDarwinNotifyCenter();
    CFNotificationCenterAddObserver(darwin, NULL, ptmc_control_notification, CFSTR("io.playcover.ptmc.start"), NULL, CFNotificationSuspensionBehaviorDeliverImmediately);
    CFNotificationCenterAddObserver(darwin, NULL, ptmc_control_notification, CFSTR("io.playcover.ptmc.stop"), NULL, CFNotificationSuspensionBehaviorDeliverImmediately);
    CFNotificationCenterAddObserver(darwin, NULL, ptmc_control_notification, CFSTR("io.playcover.ptmc.status"), NULL, CFNotificationSuspensionBehaviorDeliverImmediately);
    ptmc_schedule_discovery();
    PTMC_LOG(@"hooks enabled fps=%ld bitrate=%ld buffers=%ld disableDisplaySync=%d spoofMaxFPS=%ld",
             (long)manager.fps, (long)manager.bitrate, (long)manager.bufferCount,
             manager.disableDisplaySync, (long)ptmc_spoof_max_fps);
    if (ptmc_env_bool("PTMC_AUTOSTART", false)) [manager start];
}
