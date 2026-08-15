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
#import <CoreGraphics/CoreGraphics.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#import <VideoToolbox/VideoToolbox.h>
#import <mach/mach_time.h>
#import <mach-o/dyld.h>
#import <objc/message.h>
#import <objc/runtime.h>
#import <os/lock.h>
#import <stdatomic.h>

#define PTMC_LOG(fmt, ...) NSLog(@"[PTMC] " fmt, ##__VA_ARGS__)
#define PTMC_MAX_HOOKS 256

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

static NSString *ptmc_env_string(const char *name, NSString *fallback) {
    const char *v = getenv(name);
    if (!v || !*v) return fallback;
    NSString *value = [[NSString alloc] initWithUTF8String:v];
    return value.length > 0 ? value : fallback;
}

static uint64_t ptmc_ticks_to_ns(uint64_t ticks) {
    static mach_timebase_info_data_t info;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ mach_timebase_info(&info); });
    __uint128_t scaled = (__uint128_t)ticks * info.numer;
    return (uint64_t)(scaled / info.denom);
}

static uint64_t ptmc_ns_to_ticks(uint64_t ns) {
    static mach_timebase_info_data_t info;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ mach_timebase_info(&info); });
    __uint128_t scaled = (__uint128_t)ns * info.denom;
    return (uint64_t)(scaled / info.numer);
}

static NSString *ptmc_real_user_home(void) {
    // Share PlayLoader's getpwuid_r-based account lookup. Sandboxed iOS-on-Mac processes can report
    // their container Data directory as HOME/NSHomeDirectory, which is not the account home where
    // PlayCover's shared container lives.
    char accountHome[PATH_MAX];
    pt_copyCurrentUserHomeDirectory(accountHome, sizeof(accountHome));
    if (accountHome[0] != '\0') {
        return [NSString stringWithUTF8String:accountHome];
    }

    // Last-resort compatibility fallback if account lookup is unavailable.
    NSString *home = NSHomeDirectory();
    NSRange range = [home rangeOfString:@"/Library/Containers/io.playcover.PlayCover"];
    return range.location != NSNotFound ? [home substringToIndex:range.location] : home;
}

static NSString *ptmc_shared_container_path(void) {
    return [ptmc_real_user_home() stringByAppendingPathComponent:@"Library/Containers/io.playcover.PlayCover"];
}

static NSString *ptmc_bundle_identifier(void) {
    NSString *bundleIdentifier = NSBundle.mainBundle.bundleIdentifier;
    return bundleIdentifier.length > 0 ? bundleIdentifier : @"unknown";
}

static NSString *ptmc_runtime_config_path(void) {
    return [[[ptmc_shared_container_path() stringByAppendingPathComponent:@"PTMC Config"]
             stringByAppendingPathComponent:ptmc_bundle_identifier()] stringByAppendingPathExtension:@"plist"];
}

static NSString *ptmc_default_status_path(void) {
    return [[[ptmc_shared_container_path() stringByAppendingPathComponent:@"PTMC Status"]
             stringByAppendingPathComponent:ptmc_bundle_identifier()] stringByAppendingPathExtension:@"plist"];
}

static NSString *ptmc_default_capture_directory(void) {
    return [[ptmc_shared_container_path() stringByAppendingPathComponent:@"Captures"]
            stringByAppendingPathComponent:ptmc_bundle_identifier()];
}

static NSDictionary *ptmc_read_runtime_config(void) {
    NSData *data = [NSData dataWithContentsOfFile:ptmc_runtime_config_path()];
    if (!data) return @{};
    NSError *error = nil;
    id object = [NSPropertyListSerialization propertyListWithData:data
                                                          options:NSPropertyListImmutable
                                                           format:NULL
                                                            error:&error];
    if (![object isKindOfClass:[NSDictionary class]]) {
        if (error) PTMC_LOG(@"runtime config read failed: %@", error);
        return @{};
    }
    return (NSDictionary *)object;
}

static NSString *ptmc_timestamped_output_path(NSString *directory) {
    NSDateFormatter *fmt = [[NSDateFormatter alloc] init];
    fmt.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    fmt.dateFormat = @"yyyyMMdd-HHmmss-SSS";
    NSString *stamp = [fmt stringFromDate:[NSDate date]];
    return [directory stringByAppendingPathComponent:[NSString stringWithFormat:@"PlayTools-Capture-%@.mov", stamp]];
}

static NSString *ptmc_output_path_from_environment(void) {
    const char *out = getenv("PTMC_OUTPUT");
    if (out && *out) {
        return [[[NSString alloc] initWithUTF8String:out] stringByExpandingTildeInPath];
    }
    const char *outDir = getenv("PTMC_OUTPUT_DIR");
    if (outDir && *outDir) {
        NSString *directory = [[[NSString alloc] initWithUTF8String:outDir] stringByExpandingTildeInPath];
        return ptmc_timestamped_output_path(directory);
    }
    NSDictionary *config = ptmc_read_runtime_config();
    NSString *configuredDirectory = [config[@"outputDirectory"] isKindOfClass:[NSString class]]
        ? config[@"outputDirectory"] : nil;
    NSString *directory = configuredDirectory.length > 0
        ? [configuredDirectory stringByExpandingTildeInPath]
        : ptmc_default_capture_directory();
    return ptmc_timestamped_output_path(directory);
}

@class PTMCSession;

@interface PTMCSlot : NSObject {
@public
    CVPixelBufferRef pixelBuffer;
    CVMetalTextureRef yTextureRef;
    CVMetalTextureRef uvTextureRef;
    CVMetalTextureRef bgraTextureRef;
    atomic_bool inUse;
}
@property(nonatomic, strong) id<MTLTexture> yTexture;
@property(nonatomic, strong) id<MTLTexture> uvTexture;
@property(nonatomic, strong) id<MTLTexture> bgraTexture;
@property(nonatomic, weak) PTMCSession *session;
@property(nonatomic) uint64_t generation;
@end

@implementation PTMCSlot
- (void)dealloc {
    if (yTextureRef) CFRelease(yTextureRef);
    if (uvTextureRef) CFRelease(uvTextureRef);
    if (bgraTextureRef) CFRelease(bgraTextureRef);
    if (pixelBuffer) CVPixelBufferRelease(pixelBuffer);
}
@end

@interface PTMCSession : NSObject {
@public
    atomic_bool _accepting;
    atomic_uint_fast64_t _inFlight;
    atomic_uint_fast64_t _firstTick;
    atomic_uint_fast64_t _firstHostTime;
    atomic_uint_fast64_t _nextCaptureTick;
    atomic_uint_fast64_t _lastPresentTick;
    atomic_uint_fast64_t _presentIntervalEWMATicks;
    atomic_uint_fast64_t _burstSlotLastUseTick;
    atomic_uint_fast64_t _pendingWrites;
    atomic_int_fast64_t _lastPTSUnits;
}
@property(nonatomic) uint64_t generation;
@property(nonatomic) NSUInteger sourceWidth;
@property(nonatomic) NSUInteger sourceHeight;
@property(nonatomic) NSUInteger width;
@property(nonatomic) NSUInteger height;
@property(nonatomic) NSInteger fps;
@property(nonatomic) uint64_t captureIntervalTicks;
@property(nonatomic) NSInteger bitrate;
@property(nonatomic, copy) NSString *codecName;
@property(nonatomic) NSInteger bufferCount;
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, strong) id<MTLComputePipelineState> pipelineBGRA;
@property(nonatomic, strong) id<MTLComputePipelineState> pipelineSRGB;
@property(nonatomic, strong) id<MTLComputePipelineState> pipelineScale;
@property(nonatomic, strong) id<MTLCommandQueue> commandQueue;
@property(nonatomic, strong) NSArray<PTMCSlot *> *slots;
@property(nonatomic) CVMetalTextureCacheRef textureCache;
@property(nonatomic) VTCompressionSessionRef compressionSession;
@property(nonatomic, strong) AVAssetWriter *writer;
@property(nonatomic, strong) AVAssetWriterInput *writerInput;
@property(nonatomic, copy) NSString *outputPath;
@property(nonatomic, copy) NSString *writerPath;
@property(nonatomic) dispatch_queue_t encoderQueue;
@property(nonatomic) dispatch_queue_t writerQueue;
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
    atomic_uint_fast64_t _skippedRate;
    atomic_uint_fast64_t _lastLogSkipped;
    atomic_uint_fast64_t _samplingSkipMilliFPS;
    atomic_uint_fast64_t _lastLogTick;
    atomic_uint_fast64_t _lastLogPresented;
    atomic_uint_fast64_t _lastLogCaptured;
    atomic_uint_fast64_t _lastLogEncoded;
    atomic_uint_fast64_t _presentMilliFPS;
    atomic_uint_fast64_t _captureMilliFPS;
    atomic_uint_fast64_t _encodedMilliFPS;
    atomic_uint_fast64_t _burstSlotUses;
    atomic_uint_fast64_t _drawableWidth;
    atomic_uint_fast64_t _drawableHeight;
    atomic_int_fast64_t _pixelFormat;
    atomic_int_fast64_t _framebufferOnly;
    atomic_int_fast64_t _displaySync;
    atomic_int_fast64_t _edr;
    atomic_uint_fast64_t _presentHookCount;
    atomic_uint_fast64_t _skippedPresents;
    atomic_uint_fast64_t _ownedCaptureCommandBuffers;
    atomic_uint_fast64_t _lastOwnedCaptureGPUTimeUs;
    atomic_uint_fast64_t _lastOwnedCaptureCompletionUs;
    atomic_uintptr_t _captureLayerAddress;
    atomic_bool _layerNeedsRefresh;
    BOOL _originalFramebufferOnly;
    BOOL _originalEDR;
    BOOL _originalDisplaySync;
    float _originalOpacity;
    BOOL _hasOriginalEDR;
    BOOL _hasOriginalDisplaySync;
    BOOL _hasDeveloperHUDProperties;
    atomic_bool _metalHUDSuppressed;
    NSDictionary *_originalHUDProperties;
    CGColorSpaceRef _originalColorSpace;
}
@property(atomic, strong) PTMCSession *session;
@property(nonatomic) dispatch_queue_t controlQueue;
@property(nonatomic) NSInteger fps;
@property(nonatomic) NSInteger bitrate;
@property(nonatomic, copy) NSString *codecName;
@property(nonatomic) NSInteger bufferCount;
@property(nonatomic) double logInterval;
@property(nonatomic) BOOL disableDisplaySync;
@property(nonatomic) BOOL forceSDRDisplay;
@property(nonatomic) BOOL suppressDisplayOutput;
@property(nonatomic) BOOL skipDisplayPresent;
@property(nonatomic) BOOL includeMetalHUDInCapture;
@property(nonatomic, copy) NSString *captureResolutionMode;
@property(nonatomic) NSInteger captureWidth;
@property(nonatomic) NSInteger captureHeight;
@property(nonatomic) BOOL captureEnabled;
@property(atomic, copy) NSString *colorSpaceName;
@property(atomic, strong) CAMetalLayer *captureLayer;
@property(nonatomic) dispatch_source_t heartbeatTimer;
@property(nonatomic, copy) NSString *statusPath;
- (void)reloadConfiguration;
- (void)start;
- (void)stop;
- (void)prepareForTexture:(id<MTLTexture>)texture;
- (void)captureTexture:(id<MTLTexture>)texture commandBuffer:(id<MTLCommandBuffer>)commandBuffer tick:(uint64_t)tick;
- (void)logStatusForce:(BOOL)force tick:(uint64_t)tick;
@end

static PTMCManager *ptmc_manager(void);
static void ptmc_discover_runtime_classes(void);
static void ptmc_schedule_discovery(void);
static void ptmc_update_max_fps_hook(void);
static void ptmc_uninstall_runtime_hooks(void);
static void ptmc_restore_capture_layer(PTMCManager *manager);
static NSInteger ptmc_spoof_max_fps = 0;
static atomic_bool ptmc_recording_fast = false;

static dispatch_queue_t ptmc_status_queue(void) {
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ queue = dispatch_queue_create("io.playcover.ptmc.status", DISPATCH_QUEUE_SERIAL); });
    return queue;
}

static void ptmc_publish_status(PTMCManager *manager, PTMCSession *session, NSString *phase, NSString *message) {
    NSString *path = manager.statusPath;
    if (path.length == 0) return;
    dispatch_async(ptmc_status_queue(), ^{
        NSMutableDictionary *status = [NSMutableDictionary dictionary];
        status[@"phase"] = phase ?: @"unknown";
        status[@"message"] = message ?: @"";
        status[@"timestamp"] = @([[NSDate date] timeIntervalSince1970]);
        status[@"requested"] = @(atomic_load_explicit(&manager->_requested, memory_order_acquire));
        status[@"presented"] = @(atomic_load_explicit(&manager->_presented, memory_order_relaxed));
        status[@"captured"] = @(atomic_load_explicit(&manager->_captured, memory_order_relaxed));
        status[@"encoded"] = @(atomic_load_explicit(&manager->_encoded, memory_order_relaxed));
        status[@"droppedPool"] = @(atomic_load_explicit(&manager->_droppedPool, memory_order_relaxed));
        status[@"droppedEncoder"] = @(atomic_load_explicit(&manager->_droppedEncoder, memory_order_relaxed));
        status[@"droppedLate"] = @(atomic_load_explicit(&manager->_droppedLate, memory_order_relaxed));
        status[@"unsupported"] = @(atomic_load_explicit(&manager->_droppedUnsupported, memory_order_relaxed));
        uint64_t samplingSkipped = atomic_load_explicit(&manager->_skippedRate, memory_order_relaxed);
        status[@"skippedRate"] = @(samplingSkipped); // legacy key
        status[@"samplingSkipped"] = @(samplingSkipped);
        status[@"samplingSkipPerSecond"] = @((double)atomic_load_explicit(
            &manager->_samplingSkipMilliFPS, memory_order_relaxed) / 1000.0);
        status[@"presentFPS"] = @((double)atomic_load_explicit(
            &manager->_presentMilliFPS, memory_order_relaxed) / 1000.0);
        status[@"captureFPS"] = @((double)atomic_load_explicit(
            &manager->_captureMilliFPS, memory_order_relaxed) / 1000.0);
        status[@"encodedFPS"] = @((double)atomic_load_explicit(
            &manager->_encodedMilliFPS, memory_order_relaxed) / 1000.0);
        status[@"burstSlotUses"] = @(atomic_load_explicit(&manager->_burstSlotUses, memory_order_relaxed));
        status[@"codec"] = manager.codecName ?: @"hevc";
        status[@"drawableWidth"] = @(atomic_load_explicit(&manager->_drawableWidth, memory_order_relaxed));
        status[@"drawableHeight"] = @(atomic_load_explicit(&manager->_drawableHeight, memory_order_relaxed));
        status[@"pixelFormat"] = @(atomic_load_explicit(&manager->_pixelFormat, memory_order_relaxed));
        status[@"framebufferOnly"] = @(atomic_load_explicit(&manager->_framebufferOnly, memory_order_relaxed));
        status[@"displaySync"] = @(atomic_load_explicit(&manager->_displaySync, memory_order_relaxed));
        status[@"edr"] = @(atomic_load_explicit(&manager->_edr, memory_order_relaxed));
        status[@"presentHookCount"] = @(atomic_load_explicit(&manager->_presentHookCount, memory_order_relaxed));
        status[@"captureEnabled"] = @(manager.captureEnabled);
        status[@"displaySuppressed"] = @(manager.suppressDisplayOutput);
        status[@"displayPresentSkipped"] = @(manager.skipDisplayPresent);
        status[@"includeMetalHUDInCapture"] = @(manager.includeMetalHUDInCapture);
        status[@"metalHUDSuppressed"] = @(atomic_load_explicit(&manager->_metalHUDSuppressed, memory_order_relaxed));
        status[@"skippedPresents"] = @(atomic_load_explicit(&manager->_skippedPresents, memory_order_relaxed));
        status[@"ownedCaptureCommandBuffers"] = @(atomic_load_explicit(&manager->_ownedCaptureCommandBuffers, memory_order_relaxed));
        status[@"lastOwnedCaptureGPUTimeUs"] = @(atomic_load_explicit(&manager->_lastOwnedCaptureGPUTimeUs, memory_order_relaxed));
        status[@"lastOwnedCaptureCompletionUs"] = @(atomic_load_explicit(&manager->_lastOwnedCaptureCompletionUs, memory_order_relaxed));
        status[@"captureResolutionMode"] = manager.captureResolutionMode ?: @"source";
        status[@"memoryPath"] = @"Metal -> IOSurface CVPixelBuffer -> VideoToolbox (no CPU readback)";
        if (manager.colorSpaceName.length > 0) status[@"colorSpace"] = manager.colorSpaceName;
        if (session) {
            status[@"generation"] = @(session.generation);
            status[@"sourceWidth"] = @(session.sourceWidth);
            status[@"sourceHeight"] = @(session.sourceHeight);
            status[@"width"] = @(session.width);
            status[@"height"] = @(session.height);
            status[@"fps"] = @(session.fps);
            status[@"bufferCount"] = @(session.bufferCount);
            status[@"inFlight"] = @(atomic_load_explicit(&session->_inFlight, memory_order_relaxed));
            status[@"pendingWrites"] = @(atomic_load_explicit(&session->_pendingWrites, memory_order_relaxed));
            status[@"bitrate"] = @(session.bitrate);
            status[@"codec"] = session.codecName ?: manager.codecName ?: @"hevc";
            uint64_t firstHostTime = atomic_load_explicit(&session->_firstHostTime, memory_order_acquire);
            status[@"firstVideoHostTime"] = @(firstHostTime);
            status[@"firstVideoHostTimeNs"] = @(firstHostTime ? ptmc_ticks_to_ns(firstHostTime) : 0);
            if (session.outputPath.length > 0) status[@"outputPath"] = session.outputPath;
        }
        NSURL *url = [NSURL fileURLWithPath:path];
        NSError *error = nil;
        [[NSFileManager defaultManager] createDirectoryAtURL:[url URLByDeletingLastPathComponent]
                                 withIntermediateDirectories:YES attributes:nil error:&error];
        if (error) {
            PTMC_LOG(@"status directory failed: %@", error);
        } else if (![status writeToFile:path atomically:YES]) {
            PTMC_LOG(@"status write failed: %@", path);
        }
    });
}

static NSUInteger ptmc_even_dimension(NSUInteger value) {
    if (value < 2) return 2;
    return value & ~((NSUInteger)1);
}

static void ptmc_resolve_output_size(NSUInteger sourceWidth,
                                     NSUInteger sourceHeight,
                                     NSString *mode,
                                     NSInteger customWidth,
                                     NSInteger customHeight,
                                     NSUInteger *outputWidth,
                                     NSUInteger *outputHeight) {
    double scale = 1.0;
    NSString *normalized = mode.lowercaseString ?: @"source";
    NSInteger preset = 0;
    if ([normalized isEqualToString:@"2160p"]) preset = 2160;
    else if ([normalized isEqualToString:@"1440p"]) preset = 1440;
    else if ([normalized isEqualToString:@"1080p"]) preset = 1080;
    else if ([normalized isEqualToString:@"720p"]) preset = 720;

    if (preset > 0) {
        NSUInteger shortEdge = MIN(sourceWidth, sourceHeight);
        if (shortEdge > (NSUInteger)preset) scale = (double)preset / (double)shortEdge;
    } else if ([normalized isEqualToString:@"custom"] && customWidth > 0 && customHeight > 0) {
        double sx = (double)customWidth / (double)MAX(sourceWidth, 1);
        double sy = (double)customHeight / (double)MAX(sourceHeight, 1);
        scale = MIN(1.0, MIN(sx, sy));
    }

    NSUInteger width = ptmc_even_dimension((NSUInteger)floor((double)sourceWidth * scale));
    NSUInteger height = ptmc_even_dimension((NSUInteger)floor((double)sourceHeight * scale));
    width = MIN(width, ptmc_even_dimension(sourceWidth));
    height = MIN(height, ptmc_even_dimension(sourceHeight));
    if (outputWidth) *outputWidth = width;
    if (outputHeight) *outputHeight = height;
}

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
    "inline float luma709(float3 rgb) {\n"
    "    return (16.0/255.0) + (219.0/255.0) * dot(rgb, float3(0.2126, 0.7152, 0.0722));\n"
    "}\n"
    "\n"
    "kernel void ptmc_bgra_to_nv12(texture2d<float, access::read> src [[texture(0)]],\n"
    "                              texture2d<float, access::write> yPlane [[texture(1)]],\n"
    "                              texture2d<float, access::write> uvPlane [[texture(2)]],\n"
    "                              constant uint &sourceIsSRGB [[buffer(0)]],\n"
    "                              uint2 cid [[thread_position_in_grid]]) {\n"
    "    const uint width = src.get_width();\n"
    "    const uint height = src.get_height();\n"
    "    uint2 p0 = cid * 2u;\n"
    "    if (p0.x >= width || p0.y >= height) return;\n"
    "    uint2 p1 = uint2(min(p0.x + 1u, width - 1u), p0.y);\n"
    "    uint2 p2 = uint2(p0.x, min(p0.y + 1u, height - 1u));\n"
    "    uint2 p3 = uint2(p1.x, p2.y);\n"
    "    bool srgb = sourceIsSRGB != 0;\n"
    "    float3 r0 = load_rgb(src, p0, srgb);\n"
    "    float3 r1 = load_rgb(src, p1, srgb);\n"
    "    float3 r2 = load_rgb(src, p2, srgb);\n"
    "    float3 r3 = load_rgb(src, p3, srgb);\n"
    "\n"
    "    yPlane.write(float4(clamp(luma709(r0), 0.0, 1.0)), p0);\n"
    "    if (p1.x != p0.x) yPlane.write(float4(clamp(luma709(r1), 0.0, 1.0)), p1);\n"
    "    if (p2.y != p0.y) yPlane.write(float4(clamp(luma709(r2), 0.0, 1.0)), p2);\n"
    "    if (p3.x != p0.x && p3.y != p0.y) yPlane.write(float4(clamp(luma709(r3), 0.0, 1.0)), p3);\n"
    "\n"
    "    float3 avg = (r0 + r1 + r2 + r3) * 0.25;\n"
    "    float cb = (128.0/255.0) + (224.0/255.0) * dot(avg, float3(-0.114572, -0.385428, 0.5));\n"
    "    float cr = (128.0/255.0) + (224.0/255.0) * dot(avg, float3(0.5, -0.454153, -0.045847));\n"
    "    uvPlane.write(float4(clamp(cb, 0.0, 1.0), clamp(cr, 0.0, 1.0), 0.0, 1.0), cid);\n"
    "}\n"
    "\n"
    "inline float3 sample_rgb(texture2d<float, access::sample> src, sampler smp, float2 uv, bool sourceIsSRGB) {\n"
    "    float3 rgb = src.sample(smp, uv).rgb;\n"
    "    return sourceIsSRGB ? linear_to_srgb(rgb) : rgb;\n"
    "}\n"
    "\n"
    "kernel void ptmc_bgra_to_nv12_scaled(texture2d<float, access::sample> src [[texture(0)]],\n"
    "                                     texture2d<float, access::write> yPlane [[texture(1)]],\n"
    "                                     texture2d<float, access::write> uvPlane [[texture(2)]],\n"
    "                                     constant uint &sourceIsSRGB [[buffer(0)]],\n"
    "                                     uint2 cid [[thread_position_in_grid]]) {\n"
    "    const uint width = yPlane.get_width();\n"
    "    const uint height = yPlane.get_height();\n"
    "    uint2 p0 = cid * 2u;\n"
    "    if (p0.x >= width || p0.y >= height) return;\n"
    "    uint2 p1 = uint2(min(p0.x + 1u, width - 1u), p0.y);\n"
    "    uint2 p2 = uint2(p0.x, min(p0.y + 1u, height - 1u));\n"
    "    uint2 p3 = uint2(p1.x, p2.y);\n"
    "    constexpr sampler smp(coord::normalized, address::clamp_to_edge, filter::linear);\n"
    "    float2 dims = float2(width, height);\n"
    "    bool srgb = sourceIsSRGB != 0;\n"
    "    float3 r0 = sample_rgb(src, smp, (float2(p0) + 0.5) / dims, srgb);\n"
    "    float3 r1 = sample_rgb(src, smp, (float2(p1) + 0.5) / dims, srgb);\n"
    "    float3 r2 = sample_rgb(src, smp, (float2(p2) + 0.5) / dims, srgb);\n"
    "    float3 r3 = sample_rgb(src, smp, (float2(p3) + 0.5) / dims, srgb);\n"
    "    yPlane.write(float4(clamp(luma709(r0), 0.0, 1.0)), p0);\n"
    "    if (p1.x != p0.x) yPlane.write(float4(clamp(luma709(r1), 0.0, 1.0)), p1);\n"
    "    if (p2.y != p0.y) yPlane.write(float4(clamp(luma709(r2), 0.0, 1.0)), p2);\n"
    "    if (p3.x != p0.x && p3.y != p0.y) yPlane.write(float4(clamp(luma709(r3), 0.0, 1.0)), p3);\n"
    "    float3 avg = (r0 + r1 + r2 + r3) * 0.25;\n"
    "    float cb = (128.0/255.0) + (224.0/255.0) * dot(avg, float3(-0.114572, -0.385428, 0.5));\n"
    "    float cr = (128.0/255.0) + (224.0/255.0) * dot(avg, float3(0.5, -0.454153, -0.045847));\n"
    "    uvPlane.write(float4(clamp(cb, 0.0, 1.0), clamp(cr, 0.0, 1.0), 0.0, 1.0), cid);\n"
    "}\n"
    "\n"
    "kernel void ptmc_bgra_scale_copy(texture2d<float, access::sample> src [[texture(0)]],\n"
    "                                 texture2d<float, access::write> dst [[texture(1)]],\n"
    "                                 constant uint &sourceIsSRGB [[buffer(0)]],\n"
    "                                 uint2 gid [[thread_position_in_grid]]) {\n"
    "    const uint width = dst.get_width();\n"
    "    const uint height = dst.get_height();\n"
    "    if (gid.x >= width || gid.y >= height) return;\n"
    "    constexpr sampler smp(coord::normalized, address::clamp_to_edge, filter::linear);\n"
    "    float2 uv = (float2(gid) + 0.5) / float2(width, height);\n"
    "    float3 rgb = sample_rgb(src, smp, uv, sourceIsSRGB != 0);\n"
    "    dst.write(float4(rgb, 1.0), gid);\n"
    "}\n"
;

static void ptmc_release_slot(PTMCSlot *slot) {
    atomic_store_explicit(&slot->inUse, false, memory_order_release);
}

static PTMCSlot *ptmc_acquire_slot(PTMCSession *session, uint64_t tick, BOOL *usedBurst) {
    if (usedBurst) *usedBurst = NO;
    NSUInteger regularCount = MIN((NSUInteger)MAX(session.bufferCount, 0), session.slots.count);
    for (NSUInteger index = 0; index < regularCount; index++) {
        PTMCSlot *slot = session.slots[index];
        bool expected = false;
        if (atomic_compare_exchange_strong_explicit(&slot->inUse, &expected, true,
                                                     memory_order_acq_rel, memory_order_relaxed)) {
            return slot;
        }
    }

    // A single preallocated burst slot absorbs isolated VideoToolbox latency spikes. The cooldown
    // keeps it from becoming a permanently deeper raw-frame queue under sustained overload.
    if (session.slots.count > regularCount) {
        uint64_t cooldown = ptmc_ns_to_ticks(250000000ull);
        uint64_t last = atomic_load_explicit(&session->_burstSlotLastUseTick, memory_order_relaxed);
        if (last == 0 || tick - last >= cooldown) {
            PTMCSlot *burst = session.slots[regularCount];
            bool expected = false;
            if (atomic_compare_exchange_strong_explicit(&burst->inUse, &expected, true,
                                                         memory_order_acq_rel, memory_order_relaxed)) {
                atomic_store_explicit(&session->_burstSlotLastUseTick, tick, memory_order_relaxed);
                if (usedBurst) *usedBurst = YES;
                return burst;
            }
        }
    }
    return nil;
}

static void ptmc_finalize_session_when_drained(PTMCSession *session) {
    // VideoToolbox input completion and AVAssetWriter output are independent. Finalize only after
    // both sides have drained; never hold an encoder input slot while waiting for disk I/O.
    if (atomic_load_explicit(&session->_inFlight, memory_order_acquire) != 0 ||
        atomic_load_explicit(&session->_pendingWrites, memory_order_acquire) != 0) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_MSEC), session.writerQueue, ^{
            ptmc_finalize_session_when_drained(session);
        });
        return;
    }
    if (session.writerInput) [session.writerInput markAsFinished];
    AVAssetWriter *writer = session.writer;
    if (writer && writer.status == AVAssetWriterStatusWriting) {
        [writer finishWritingWithCompletionHandler:^{
            if (writer.status == AVAssetWriterStatusCompleted) {
                NSError *moveError = nil;
                NSFileManager *fm = [NSFileManager defaultManager];
                NSURL *finalURL = [NSURL fileURLWithPath:session.outputPath];
                NSURL *writerURL = [NSURL fileURLWithPath:session.writerPath ?: session.outputPath];
                [fm removeItemAtURL:finalURL error:nil];
                if (![writerURL isEqual:finalURL] && ![fm moveItemAtURL:writerURL toURL:finalURL error:&moveError]) {
                    PTMC_LOG(@"recording finalize rename failed: %@", moveError);
                    ptmc_publish_status(ptmc_manager(), session, @"error", moveError.localizedDescription ?: @"Recording finalize rename failed");
                } else {
                    PTMC_LOG(@"recording finalized: %@", session.outputPath);
                    ptmc_publish_status(ptmc_manager(), session, @"finalized", @"Recording finalized successfully");
                }
            } else {
                PTMC_LOG(@"writer finalize failed: %@", writer.error);
                ptmc_publish_status(ptmc_manager(), session, @"error", writer.error.localizedDescription ?: @"Writer finalize failed");
            }
        }];
    } else if (!writer) {
        ptmc_publish_status(ptmc_manager(), session, @"stopped", @"No encoded frames were written");
    } else {
        NSString *message = writer.error.localizedDescription ?: @"Writer was not in a finalizable state";
        ptmc_publish_status(ptmc_manager(), session, @"error", message);
    }
}

static BOOL ptmc_ensure_writer(PTMCSession *session, CMSampleBufferRef sampleBuffer) {
    if (session.writer) return session.writer.status == AVAssetWriterStatusWriting;
    NSError *error = nil;
    NSString *base = [session.outputPath stringByDeletingPathExtension];
    session.writerPath = [base stringByAppendingString:@".partial.mov"];
    NSURL *url = [NSURL fileURLWithPath:session.writerPath];
    NSURL *finalURL = [NSURL fileURLWithPath:session.outputPath];
    [[NSFileManager defaultManager] createDirectoryAtURL:[finalURL URLByDeletingLastPathComponent]
                             withIntermediateDirectories:YES attributes:nil error:&error];
    if (error) {
        PTMC_LOG(@"cannot create output directory: %@", error);
        ptmc_publish_status(ptmc_manager(), session, @"error", error.localizedDescription ?: @"Cannot create output directory");
        return NO;
    }
    [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
    AVAssetWriter *writer = [[AVAssetWriter alloc] initWithURL:url fileType:AVFileTypeQuickTimeMovie error:&error];
    if (!writer) {
        PTMC_LOG(@"AVAssetWriter init failed: %@", error);
        ptmc_publish_status(ptmc_manager(), session, @"error", error.localizedDescription ?: @"AVAssetWriter initialization failed");
        return NO;
    }
    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sampleBuffer);
    AVAssetWriterInput *input = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeVideo
                                                               outputSettings:nil
                                                            sourceFormatHint:format];
    input.expectsMediaDataInRealTime = YES;
    if (![writer canAddInput:input]) {
        PTMC_LOG(@"AVAssetWriter cannot add compressed-video passthrough input");
        ptmc_publish_status(ptmc_manager(), session, @"error", @"AVAssetWriter cannot add compressed-video passthrough input");
        return NO;
    }
    [writer addInput:input];
    if (![writer startWriting]) {
        PTMC_LOG(@"AVAssetWriter start failed: %@", writer.error);
        ptmc_publish_status(ptmc_manager(), session, @"error", writer.error.localizedDescription ?: @"AVAssetWriter failed to start");
        return NO;
    }
    [writer startSessionAtSourceTime:kCMTimeZero];
    session.writer = writer;
    session.writerInput = input;
    ptmc_publish_status(ptmc_manager(), session, @"recording", [NSString stringWithFormat:@"%@ writer started", session.codecName.uppercaseString ?: @"VIDEO"]);
    return YES;
}

static void ptmc_vt_output_callback(void *outputCallbackRefCon,
                                    void *sourceFrameRefCon,
                                    OSStatus status,
                                    VTEncodeInfoFlags infoFlags,
                                    CMSampleBufferRef sampleBuffer) {
    (void)outputCallbackRefCon;
    if (!sourceFrameRefCon) return;
    PTMCSlot *slot = (__bridge PTMCSlot *)sourceFrameRefCon;
    PTMCSession *session = slot.session;
    if (!session || slot.generation != session.generation) return;

    // VideoToolbox has finished consuming the IOSurface input by the time this callback fires.
    // Release the scarce capture slot immediately; AVAssetWriter can continue asynchronously with
    // the compressed CMSampleBuffer and must not extend GPU/encoder backpressure to the render path.
    BOOL good = status == noErr && sampleBuffer != NULL && !(infoFlags & kVTEncodeInfo_FrameDropped);
    if (sampleBuffer) CFRetain(sampleBuffer);
    ptmc_release_slot(slot);
    atomic_fetch_sub_explicit(&session->_inFlight, 1, memory_order_acq_rel);

    if (!good) {
        atomic_fetch_add_explicit(&ptmc_manager()->_droppedEncoder, 1, memory_order_relaxed);
        if (sampleBuffer) CFRelease(sampleBuffer);
        return;
    }

    atomic_fetch_add_explicit(&session->_pendingWrites, 1, memory_order_acq_rel);
    dispatch_async(session.writerQueue, ^{
        PTMCManager *manager = ptmc_manager();
        if (ptmc_ensure_writer(session, sampleBuffer)) {
            // Writer pressure is isolated to this dedicated utility queue. Waiting here cannot
            // occupy an IOSurface slot, cannot block VideoToolbox submission, and cannot stall the
            // game's render/present thread. Keep ordering intact by waiting serially for readiness.
            int waits = 0;
            while (!session.writerInput.readyForMoreMediaData && waits < 250) {
                usleep(1000);
                waits++;
            }
            if (session.writerInput.readyForMoreMediaData && [session.writerInput appendSampleBuffer:sampleBuffer]) {
                atomic_fetch_add_explicit(&manager->_encoded, 1, memory_order_relaxed);
            } else {
                atomic_fetch_add_explicit(&manager->_droppedEncoder, 1, memory_order_relaxed);
                PTMC_LOG(@"writer backpressure/failure after asynchronous wait; stopping this capture safely");
                if (manager.session == session) [manager stop];
            }
        } else {
            atomic_fetch_add_explicit(&manager->_droppedEncoder, 1, memory_order_relaxed);
        }
        if (sampleBuffer) CFRelease(sampleBuffer);
        atomic_fetch_sub_explicit(&session->_pendingWrites, 1, memory_order_acq_rel);
    });
}

static CMVideoCodecType ptmc_codec_type(NSString *codecName) {
    NSString *name = codecName.lowercaseString;
    if ([name isEqualToString:@"prores422lt"]) return kCMVideoCodecType_AppleProRes422LT;
    if ([name isEqualToString:@"prores422"]) return kCMVideoCodecType_AppleProRes422;
    if ([name isEqualToString:@"prores422hq"]) return kCMVideoCodecType_AppleProRes422HQ;
    return kCMVideoCodecType_HEVC;
}

static BOOL ptmc_codec_is_hevc(NSString *codecName) {
    return codecName.length == 0 || [codecName.lowercaseString isEqualToString:@"hevc"];
}

static BOOL ptmc_create_video_session(PTMCSession *session) {
    NSDictionary *encoderSpec = @{};
    if (@available(iOS 13.0, *)) {
        encoderSpec = @{
            (__bridge NSString *)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @YES,
            (__bridge NSString *)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @YES
        };
    }
    OSType sourcePixelFormat = ptmc_codec_is_hevc(session.codecName)
        ? kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        : kCVPixelFormatType_32BGRA;
    NSDictionary *sourceAttrs = @{
        (__bridge NSString *)kCVPixelBufferPixelFormatTypeKey: @(sourcePixelFormat),
        (__bridge NSString *)kCVPixelBufferWidthKey: @(session.width),
        (__bridge NSString *)kCVPixelBufferHeightKey: @(session.height),
        (__bridge NSString *)kCVPixelBufferMetalCompatibilityKey: @YES,
        (__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey: @{}
    };
    VTCompressionSessionRef vt = NULL;
    CMVideoCodecType codec = ptmc_codec_type(session.codecName);
    OSStatus err = VTCompressionSessionCreate(kCFAllocatorDefault,
                                               (int32_t)session.width,
                                               (int32_t)session.height,
                                               codec,
                                               (__bridge CFDictionaryRef)encoderSpec,
                                               (__bridge CFDictionaryRef)sourceAttrs,
                                               NULL,
                                               ptmc_vt_output_callback,
                                               NULL,
                                               &vt);
    if (err != noErr || !vt) {
        PTMC_LOG(@"%@ hardware video session creation failed: %d", session.codecName.uppercaseString, (int)err);
        return NO;
    }
    VTSessionSetProperty(vt, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    VTSessionSetProperty(vt, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
    NSInteger expectedFPS = session.fps;
    CFNumberRef fps = CFNumberCreate(kCFAllocatorDefault, kCFNumberNSIntegerType, &expectedFPS);
    if (fps) { VTSessionSetProperty(vt, kVTCompressionPropertyKey_ExpectedFrameRate, fps); CFRelease(fps); }
    if (ptmc_codec_is_hevc(session.codecName)) {
        VTSessionSetProperty(vt, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_HEVC_Main_AutoLevel);
        NSInteger averageBitrate = session.bitrate;
        CFNumberRef br = CFNumberCreate(kCFAllocatorDefault, kCFNumberNSIntegerType, &averageBitrate);
        if (br) { VTSessionSetProperty(vt, kVTCompressionPropertyKey_AverageBitRate, br); CFRelease(br); }
        NSInteger keyInterval = MAX(session.fps * 2, 1);
        CFNumberRef key = CFNumberCreate(kCFAllocatorDefault, kCFNumberNSIntegerType, &keyInterval);
        if (key) { VTSessionSetProperty(vt, kVTCompressionPropertyKey_MaxKeyFrameInterval, key); CFRelease(key); }
    }
    VTSessionSetProperty(vt, kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2);
    VTSessionSetProperty(vt, kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2);
    VTSessionSetProperty(vt, kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2);
    err = VTCompressionSessionPrepareToEncodeFrames(vt);
    if (err != noErr) {
        PTMC_LOG(@"VT prepare failed for %@: %d", session.codecName, (int)err);
        VTCompressionSessionInvalidate(vt);
        CFRelease(vt);
        return NO;
    }
    session.compressionSession = vt;
    PTMC_LOG(@"encoder %@ hardware required, %ldx%ld @ %ld fps%@",
             session.codecName.uppercaseString,
             (long)session.width, (long)session.height, (long)session.fps,
             ptmc_codec_is_hevc(session.codecName)
                ? [NSString stringWithFormat:@", bitrate=%ld", (long)session.bitrate]
                : @"");
    return YES;
}

static BOOL ptmc_create_slots(PTMCSession *session) {
    BOOL hevc = ptmc_codec_is_hevc(session.codecName);
    OSType pixelFormat = hevc
        ? kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        : kCVPixelFormatType_32BGRA;
    NSDictionary *attrs = @{
        (__bridge NSString *)kCVPixelBufferPixelFormatTypeKey: @(pixelFormat),
        (__bridge NSString *)kCVPixelBufferWidthKey: @(session.width),
        (__bridge NSString *)kCVPixelBufferHeightKey: @(session.height),
        (__bridge NSString *)kCVPixelBufferMetalCompatibilityKey: @YES,
        (__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey: @{},
    };
    CVPixelBufferPoolRef pool = NULL;
    NSInteger allocatedSlotCount = session.bufferCount + 1; // cooldown-limited burst slot
    NSDictionary *poolAttrs = @{ (__bridge NSString *)kCVPixelBufferPoolMinimumBufferCountKey: @(allocatedSlotCount) };
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
    NSMutableArray *slots = [NSMutableArray arrayWithCapacity:allocatedSlotCount];
    for (NSInteger i = 0; i < allocatedSlotCount; i++) {
        CVPixelBufferRef pb = NULL;
        rv = CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pb);
        if (rv != kCVReturnSuccess || !pb) break;
        CVBufferSetAttachment(pb, kCVImageBufferColorPrimariesKey,
                              kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(pb, kCVImageBufferTransferFunctionKey,
                              kCVImageBufferTransferFunction_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(pb, kCVImageBufferYCbCrMatrixKey,
                              kCVImageBufferYCbCrMatrix_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
        PTMCSlot *slot = [[PTMCSlot alloc] init];
        slot->pixelBuffer = pb;
        slot.session = session;
        slot.generation = session.generation;
        atomic_init(&slot->inUse, false);
        if (hevc) {
            rv = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, cache, pb, NULL,
                                                           MTLPixelFormatR8Unorm,
                                                           session.width, session.height, 0,
                                                           &slot->yTextureRef);
            if (rv != kCVReturnSuccess || !slot->yTextureRef) break;
            rv = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, cache, pb, NULL,
                                                           MTLPixelFormatRG8Unorm,
                                                           session.width / 2, session.height / 2, 1,
                                                           &slot->uvTextureRef);
            if (rv != kCVReturnSuccess || !slot->uvTextureRef) break;
            slot.yTexture = CVMetalTextureGetTexture(slot->yTextureRef);
            slot.uvTexture = CVMetalTextureGetTexture(slot->uvTextureRef);
            if (!slot.yTexture || !slot.uvTexture) break;
        } else {
            rv = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, cache, pb, NULL,
                                                           MTLPixelFormatBGRA8Unorm,
                                                           session.width, session.height, 0,
                                                           &slot->bgraTextureRef);
            if (rv != kCVReturnSuccess || !slot->bgraTextureRef) break;
            slot.bgraTexture = CVMetalTextureGetTexture(slot->bgraTextureRef);
            if (!slot.bgraTexture) break;
        }
        [slots addObject:slot];
    }
    CFRelease(pool);
    if (slots.count != allocatedSlotCount) {
        PTMC_LOG(@"failed to preallocate %@ ring (%lu/%ld regular + 1 burst)",
                 hevc ? @"NV12" : @"BGRA", (unsigned long)slots.count, (long)session.bufferCount);
        return NO;
    }
    session.slots = slots;
    return YES;
}

static BOOL ptmc_create_pipelines(PTMCSession *session) {
    BOOL hevc = ptmc_codec_is_hevc(session.codecName);
    BOOL scaling = session.sourceWidth != session.width || session.sourceHeight != session.height;
    if (!hevc && !scaling) return YES;

    NSError *error = nil;
    NSString *source = [NSString stringWithUTF8String:ptmc_shader_source];
    id<MTLLibrary> library = [session.device newLibraryWithSource:source options:nil error:&error];
    if (!library) {
        PTMC_LOG(@"Metal shader compile failed: %@", error);
        return NO;
    }

    if (hevc) {
        NSString *functionName = scaling ? @"ptmc_bgra_to_nv12_scaled" : @"ptmc_bgra_to_nv12";
        id<MTLFunction> function = [library newFunctionWithName:functionName];
        if (!function) return NO;
        id<MTLComputePipelineState> pipeline =
            [session.device newComputePipelineStateWithFunction:function error:&error];
        if (!pipeline) {
            PTMC_LOG(@"Metal HEVC conversion pipeline failed: %@", error);
            return NO;
        }
        if (scaling) session.pipelineScale = pipeline;
        else session.pipelineBGRA = pipeline;
        session.pipelineSRGB = pipeline;
        return YES;
    }

    id<MTLFunction> function = [library newFunctionWithName:@"ptmc_bgra_scale_copy"];
    if (!function) return NO;
    session.pipelineScale = [session.device newComputePipelineStateWithFunction:function error:&error];
    if (!session.pipelineScale) {
        PTMC_LOG(@"Metal ProRes scale pipeline failed: %@", error);
        return NO;
    }
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
        atomic_init(&_droppedPool, 0); atomic_init(&_droppedEncoder, 0); atomic_init(&_droppedLate, 0);
        atomic_init(&_droppedUnsupported, 0); atomic_init(&_skippedRate, 0);
        atomic_init(&_lastLogSkipped, 0); atomic_init(&_samplingSkipMilliFPS, 0);
        atomic_init(&_lastLogTick, mach_continuous_time());
        atomic_init(&_lastLogPresented, 0); atomic_init(&_lastLogCaptured, 0); atomic_init(&_lastLogEncoded, 0);
        atomic_init(&_presentMilliFPS, 0); atomic_init(&_captureMilliFPS, 0); atomic_init(&_encodedMilliFPS, 0);
        atomic_init(&_burstSlotUses, 0);
        atomic_init(&_drawableWidth, 0); atomic_init(&_drawableHeight, 0); atomic_init(&_pixelFormat, -1);
        atomic_init(&_framebufferOnly, -1); atomic_init(&_displaySync, -1); atomic_init(&_edr, -1);
        atomic_init(&_presentHookCount, 0);
        atomic_init(&_skippedPresents, 0);
        atomic_init(&_ownedCaptureCommandBuffers, 0);
        atomic_init(&_lastOwnedCaptureGPUTimeUs, 0);
        atomic_init(&_lastOwnedCaptureCompletionUs, 0);
        atomic_init(&_captureLayerAddress, 0);
        atomic_init(&_layerNeedsRefresh, true);
        _originalColorSpace = NULL;
        _originalHUDProperties = nil;
        _hasDeveloperHUDProperties = NO;
        atomic_init(&_metalHUDSuppressed, false);
        [self reloadConfiguration];
    }
    return self;
}

- (void)reloadConfiguration {
    NSDictionary *config = ptmc_read_runtime_config();
    NSNumber *enabledValue = [config[@"enabled"] isKindOfClass:[NSNumber class]] ? config[@"enabled"] : nil;
    long long configFPS = [config[@"fps"] respondsToSelector:@selector(longLongValue)] ? [config[@"fps"] longLongValue] : 120;
    long long configBitrate = [config[@"bitrate"] respondsToSelector:@selector(longLongValue)] ? [config[@"bitrate"] longLongValue] : 120000000;
    long long configBuffers = [config[@"buffers"] respondsToSelector:@selector(longLongValue)] ? [config[@"buffers"] longLongValue] : 3;
    double configLogInterval = [config[@"logInterval"] respondsToSelector:@selector(doubleValue)] ? [config[@"logInterval"] doubleValue] : 1.0;
    BOOL configDisableDisplaySync = [config[@"disableDisplaySync"] respondsToSelector:@selector(boolValue)] ? [config[@"disableDisplaySync"] boolValue] : NO;
    BOOL configForceSDR = [config[@"forceSDRDisplay"] respondsToSelector:@selector(boolValue)] ? [config[@"forceSDRDisplay"] boolValue] : YES;
    BOOL configSuppressDisplay = [config[@"suppressDisplayOutput"] respondsToSelector:@selector(boolValue)] ? [config[@"suppressDisplayOutput"] boolValue] : NO;
    BOOL configSkipDisplayPresent = [config[@"skipDisplayPresent"] respondsToSelector:@selector(boolValue)] ? [config[@"skipDisplayPresent"] boolValue] : NO;
    BOOL configIncludeMetalHUD = [config[@"includeMetalHUDInCapture"] respondsToSelector:@selector(boolValue)] ? [config[@"includeMetalHUDInCapture"] boolValue] : NO;
    NSString *configResolution = [config[@"resolutionMode"] isKindOfClass:[NSString class]] ? [config[@"resolutionMode"] lowercaseString] : @"source";
    long long configCaptureWidth = [config[@"captureWidth"] respondsToSelector:@selector(longLongValue)] ? [config[@"captureWidth"] longLongValue] : 1920;
    long long configCaptureHeight = [config[@"captureHeight"] respondsToSelector:@selector(longLongValue)] ? [config[@"captureHeight"] longLongValue] : 1080;
    long long configSpoofFPS = [config[@"spoofMaxFPS"] respondsToSelector:@selector(longLongValue)] ? [config[@"spoofMaxFPS"] longLongValue] : 0;
    NSString *configCodec = [config[@"codec"] isKindOfClass:[NSString class]] ? [config[@"codec"] lowercaseString] : @"hevc";

    self.captureEnabled = enabledValue ? enabledValue.boolValue : ptmc_env_bool("PTMC_ENABLE", false);
    self.fps = (NSInteger)ptmc_env_ll("PTMC_FPS", configFPS, 1, 240);
    self.bitrate = (NSInteger)ptmc_env_ll("PTMC_BITRATE", configBitrate, 1000000, 1000000000);
    self.bufferCount = (NSInteger)ptmc_env_ll("PTMC_BUFFERS", configBuffers, 3, 16);
    self.logInterval = ptmc_env_double("PTMC_LOG_INTERVAL", configLogInterval, 0.25, 60.0);
    self.disableDisplaySync = ptmc_env_bool("PTMC_DISABLE_DISPLAY_SYNC", configDisableDisplaySync);
    self.forceSDRDisplay = ptmc_env_bool("PTMC_FORCE_SDR_DISPLAY", configForceSDR);
    self.suppressDisplayOutput = ptmc_env_bool("PTMC_SUPPRESS_DISPLAY", configSuppressDisplay);
    self.skipDisplayPresent = ptmc_env_bool("PTMC_SKIP_DISPLAY_PRESENT", configSkipDisplayPresent);
    self.includeMetalHUDInCapture = ptmc_env_bool("PTMC_CAPTURE_METAL_HUD", configIncludeMetalHUD);
    NSString *resolutionMode = [ptmc_env_string("PTMC_RESOLUTION_MODE", configResolution) lowercaseString];
    NSSet *allowedResolutions = [NSSet setWithArray:@[@"source", @"2160p", @"1440p", @"1080p", @"720p", @"custom"]];
    if (![allowedResolutions containsObject:resolutionMode]) resolutionMode = @"source";
    self.captureResolutionMode = resolutionMode;
    self.captureWidth = (NSInteger)ptmc_env_ll("PTMC_CAPTURE_WIDTH", configCaptureWidth, 2, 16384);
    self.captureHeight = (NSInteger)ptmc_env_ll("PTMC_CAPTURE_HEIGHT", configCaptureHeight, 2, 16384);
    ptmc_spoof_max_fps = (NSInteger)ptmc_env_ll("PTMC_SPOOF_MAX_FPS", configSpoofFPS, 0, 240);
    NSString *codec = [ptmc_env_string("PTMC_CODEC", configCodec) lowercaseString];
    if (![codec isEqualToString:@"hevc"] &&
        ![codec isEqualToString:@"prores422lt"] &&
        ![codec isEqualToString:@"prores422"] &&
        ![codec isEqualToString:@"prores422hq"]) {
        PTMC_LOG(@"unknown codec '%@'; falling back to HEVC", codec);
        codec = @"hevc";
    }
    self.codecName = codec;
    atomic_store_explicit(&_layerNeedsRefresh, true, memory_order_release);

    const char *statusFile = getenv("PTMC_STATUS_FILE");
    if (statusFile && *statusFile) {
        self.statusPath = [[[NSString alloc] initWithUTF8String:statusFile] stringByExpandingTildeInPath];
    } else {
        NSString *configuredStatus = [config[@"statusFile"] isKindOfClass:[NSString class]] ? config[@"statusFile"] : nil;
        self.statusPath = configuredStatus.length > 0 ? [configuredStatus stringByExpandingTildeInPath] : ptmc_default_status_path();
    }
}

- (void)start {
    [self reloadConfiguration];
    if (atomic_exchange_explicit(&_requested, true, memory_order_acq_rel)) {
        atomic_store_explicit(&_layerNeedsRefresh, true, memory_order_release);
        PTMC_LOG(@"start requested while already recording; keeping active session");
        ptmc_publish_status(self, self.session, self.session ? @"recording" : @"requested",
                            @"Capture already active; runtime settings refresh queued");
        return;
    }
    atomic_store_explicit(&_presented, 0, memory_order_relaxed);
    atomic_store_explicit(&_captured, 0, memory_order_relaxed);
    atomic_store_explicit(&_encoded, 0, memory_order_relaxed);
    atomic_store_explicit(&_droppedPool, 0, memory_order_relaxed);
    atomic_store_explicit(&_droppedEncoder, 0, memory_order_relaxed);
    atomic_store_explicit(&_droppedLate, 0, memory_order_relaxed);
    atomic_store_explicit(&_droppedUnsupported, 0, memory_order_relaxed);
    atomic_store_explicit(&_skippedRate, 0, memory_order_relaxed);
    atomic_store_explicit(&_lastLogSkipped, 0, memory_order_relaxed);
    atomic_store_explicit(&_samplingSkipMilliFPS, 0, memory_order_relaxed);
    atomic_store_explicit(&_lastLogPresented, 0, memory_order_relaxed);
    atomic_store_explicit(&_lastLogCaptured, 0, memory_order_relaxed);
    atomic_store_explicit(&_lastLogEncoded, 0, memory_order_relaxed);
    atomic_store_explicit(&_presentMilliFPS, 0, memory_order_relaxed);
    atomic_store_explicit(&_captureMilliFPS, 0, memory_order_relaxed);
    atomic_store_explicit(&_encodedMilliFPS, 0, memory_order_relaxed);
    atomic_store_explicit(&_burstSlotUses, 0, memory_order_relaxed);
    atomic_store_explicit(&_skippedPresents, 0, memory_order_relaxed);
    atomic_store_explicit(&_ownedCaptureCommandBuffers, 0, memory_order_relaxed);
    atomic_store_explicit(&_lastOwnedCaptureGPUTimeUs, 0, memory_order_relaxed);
    atomic_store_explicit(&_lastOwnedCaptureCompletionUs, 0, memory_order_relaxed);
    self.captureLayer = nil;
    atomic_store_explicit(&_layerNeedsRefresh, true, memory_order_release);
    atomic_store_explicit(&ptmc_recording_fast, true, memory_order_release);
    ptmc_update_max_fps_hook();
    // Install expensive frame-path hooks only when recording is actually requested. Games that
    // never record now run with their original Metal dispatch untouched.
    ptmc_discover_runtime_classes();
    ptmc_schedule_discovery();
    PTMC_LOG(@"start requested; capture will arm on the next supported drawable");
    ptmc_publish_status(self, self.session, @"requested", @"Waiting for the next supported Metal drawable");
}

- (void)stop {
    atomic_store_explicit(&_requested, false, memory_order_release);
    atomic_store_explicit(&ptmc_recording_fast, false, memory_order_release);
    ptmc_restore_capture_layer(self);
    ptmc_uninstall_runtime_hooks();
    ptmc_spoof_max_fps = 0;
    ptmc_update_max_fps_hook();
    PTMCSession *session = self.session;
    self.session = nil;
    atomic_store_explicit(&_preparing, false, memory_order_release);
    atomic_fetch_add_explicit(&_generation, 1, memory_order_acq_rel);
    if (session) {
        atomic_store_explicit(&session->_accepting, false, memory_order_release);
        dispatch_async(session.encoderQueue, ^{
            if (session.compressionSession) VTCompressionSessionCompleteFrames(session.compressionSession, kCMTimeInvalid);
            dispatch_async(session.writerQueue, ^{ ptmc_finalize_session_when_drained(session); });
        });
    }
    PTMC_LOG(@"stop requested");
    ptmc_publish_status(self, session, session ? @"stopping" : @"stopped", session ? @"Finalizing recording" : @"No active capture session");
}

- (void)prepareForTexture:(id<MTLTexture>)texture {
    if (!atomic_load_explicit(&_requested, memory_order_acquire) || self.session) return;
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&_preparing, &expected, true, memory_order_acq_rel, memory_order_relaxed)) return;
    NSUInteger sourceWidth = texture.width, sourceHeight = texture.height;
    NSUInteger width = sourceWidth, height = sourceHeight;
    ptmc_resolve_output_size(sourceWidth, sourceHeight, self.captureResolutionMode,
                             self.captureWidth, self.captureHeight, &width, &height);
    id<MTLDevice> device = texture.device;
    dispatch_async(_controlQueue, ^{
        if (!atomic_load_explicit(&self->_requested, memory_order_acquire)) {
            atomic_store_explicit(&self->_preparing, false, memory_order_release);
            return;
        }
        PTMCSession *s = [[PTMCSession alloc] init];
        s.generation = atomic_fetch_add_explicit(&self->_generation, 1, memory_order_acq_rel) + 1;
        s.sourceWidth = sourceWidth; s.sourceHeight = sourceHeight;
        s.width = width; s.height = height; s.fps = self.fps;
        s.captureIntervalTicks = ptmc_ns_to_ticks(1000000000ull / (uint64_t)MAX(s.fps, 1));
        s.bitrate = self.bitrate; s.codecName = self.codecName; s.bufferCount = self.bufferCount;
        s.device = device;
        // Allocate the direct-drawable capture queue off the present thread.
        s.commandQueue = [device newCommandQueue];
        dispatch_queue_attr_t encoderAttr = dispatch_queue_attr_make_with_qos_class(
            DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0);
        dispatch_queue_attr_t writerAttr = dispatch_queue_attr_make_with_qos_class(
            DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0);
        s.encoderQueue = dispatch_queue_create("io.playcover.ptmc.encoder", encoderAttr);
        s.writerQueue = dispatch_queue_create("io.playcover.ptmc.writer", writerAttr);
        atomic_init(&s->_accepting, true); atomic_init(&s->_inFlight, 0);
        atomic_init(&s->_firstTick, 0); atomic_init(&s->_firstHostTime, 0);
        atomic_init(&s->_nextCaptureTick, 0); atomic_init(&s->_lastPresentTick, 0);
        atomic_init(&s->_presentIntervalEWMATicks, 0); atomic_init(&s->_burstSlotLastUseTick, 0);
        atomic_init(&s->_pendingWrites, 0);
        atomic_init(&s->_lastPTSUnits, -1);
        s.outputPath = ptmc_output_path_from_environment();
        BOOL ok = ptmc_create_pipelines(s) && ptmc_create_slots(s) && ptmc_create_video_session(s);
        if (ok && atomic_load_explicit(&self->_requested, memory_order_acquire)) {
            self.session = s;
            PTMC_LOG(@"capture armed generation=%llu source=%lux%lu capture=%lux%lu codec=%@ buffers=%ld output=%@",
                     (unsigned long long)s.generation,
                     (unsigned long)s.sourceWidth, (unsigned long)s.sourceHeight,
                     (unsigned long)s.width, (unsigned long)s.height,
                     s.codecName, (long)s.bufferCount, s.outputPath);
            ptmc_publish_status(self, s, @"armed", @"Capture session armed");
        } else {
            atomic_store_explicit(&s->_accepting, false, memory_order_release);
            atomic_store_explicit(&self->_requested, false, memory_order_release);
            PTMC_LOG(@"capture prepare failed");
            ptmc_publish_status(self, s, @"error", @"Capture preparation failed");
        }
        atomic_store_explicit(&self->_preparing, false, memory_order_release);
    });
}

- (void)captureTexture:(id<MTLTexture>)texture commandBuffer:(id<MTLCommandBuffer>)commandBuffer tick:(uint64_t)tick {
    if (!atomic_load_explicit(&_requested, memory_order_acquire)) return;
    PTMCSession *session = self.session;
    if (!session) { [self prepareForTexture:texture]; return; }
    if (!atomic_load_explicit(&session->_accepting, memory_order_acquire)) return;
    if (texture.width != session.sourceWidth || texture.height != session.sourceHeight) {
        atomic_fetch_add_explicit(&_droppedUnsupported, 1, memory_order_relaxed);
        PTMC_LOG(@"drawable size changed %lux%lu -> %lux%lu; stopping safely (restart capture to re-arm)",
                 (unsigned long)session.sourceWidth, (unsigned long)session.sourceHeight,
                 (unsigned long)texture.width, (unsigned long)texture.height);
        [self stop];
        return;
    }
    if ([texture respondsToSelector:@selector(framebufferOnly)] && texture.framebufferOnly) {
        atomic_fetch_add_explicit(&_droppedUnsupported, 1, memory_order_relaxed);
        atomic_store_explicit(&_layerNeedsRefresh, true, memory_order_release);
        return;
    }
    BOOL sourceSRGB = texture.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB;
    if (texture.pixelFormat != MTLPixelFormatBGRA8Unorm && !sourceSRGB) {
        atomic_fetch_add_explicit(&_droppedUnsupported, 1, memory_order_relaxed);
        return;
    }
    // The configured FPS is a real capture-rate ceiling, not just encoder metadata. A 120 Hz
    // game captured at 60 fps now performs roughly half as many BGRA->NV12 conversions/encodes.
    // Deadline-based sampling with a small jitter tolerance. Comparing only against the previous
    // captured frame can accidentally turn a nominal 60 Hz stream into ~30 fps when presents land
    // a fraction of a millisecond before the exact 16.667 ms boundary. Keep a stable deadline instead.
    uint64_t interval = session.captureIntervalTicks;
    uint64_t lastPresent = atomic_exchange_explicit(&session->_lastPresentTick, tick, memory_order_acq_rel);
    uint64_t presentEWMA = atomic_load_explicit(&session->_presentIntervalEWMATicks, memory_order_relaxed);
    if (lastPresent != 0 && tick > lastPresent) {
        uint64_t sample = tick - lastPresent;
        presentEWMA = presentEWMA == 0 ? sample : (presentEWMA * 7 + sample) / 8;
        atomic_store_explicit(&session->_presentIntervalEWMATicks, presentEWMA, memory_order_relaxed);
    }

    // If the source is effectively at the requested capture rate, accept every present. Tiny
    // frame-pacing jitter around 60 Hz must not become visible one/two-frame capture holes. Only
    // engage the rate limiter once the source is clearly faster (~>6%), e.g. 120 Hz -> 60 fps.
    BOOL sourceClearlyFaster = presentEWMA != 0 && presentEWMA * 100 < interval * 94;
    uint64_t nextTick = atomic_load_explicit(&session->_nextCaptureTick, memory_order_relaxed);
    if (!sourceClearlyFaster) {
        atomic_store_explicit(&session->_nextCaptureTick, tick + interval, memory_order_relaxed);
    } else {
        uint64_t tolerance = MIN(interval / 16, ptmc_ns_to_ticks(250000ull)); // <= 0.25 ms
        if (nextTick == 0) {
            atomic_store_explicit(&session->_nextCaptureTick, tick + interval, memory_order_relaxed);
        } else if (tick + tolerance < nextTick) {
            // Intentional sampling skip from a genuinely faster source, not an encoder drop.
            atomic_fetch_add_explicit(&_skippedRate, 1, memory_order_relaxed);
            return;
        } else {
            uint64_t advanced = nextTick;
            if (tick > nextTick + interval * 4) {
                // Long suspend/stall: discard accumulated timing debt instead of burst-catching up.
                advanced = tick + interval;
            } else {
                do { advanced += interval; } while (advanced <= tick);
            }
            atomic_store_explicit(&session->_nextCaptureTick, advanced, memory_order_relaxed);
        }
    }
    if (atomic_load_explicit(&session->_firstHostTime, memory_order_relaxed) == 0) {
        uint64_t zeroHost = 0;
        uint64_t hostNow = mach_absolute_time();
        atomic_compare_exchange_strong_explicit(&session->_firstHostTime, &zeroHost, hostNow,
                                                memory_order_acq_rel, memory_order_relaxed);
    }
    BOOL usedBurstSlot = NO;
    PTMCSlot *slot = ptmc_acquire_slot(session, tick, &usedBurstSlot);
    if (!slot) {
        atomic_fetch_add_explicit(&_droppedPool, 1, memory_order_relaxed);
        return;
    }
    if (usedBurstSlot) atomic_fetch_add_explicit(&_burstSlotUses, 1, memory_order_relaxed);
    id<MTLCommandBuffer> activeCmdBuffer = commandBuffer;
    BOOL ownsCmdBuffer = NO;
    if (!activeCmdBuffer) {
        if (!session.commandQueue) {
            session.commandQueue = [session.device newCommandQueue];
        }
        activeCmdBuffer = [session.commandQueue commandBuffer];
        ownsCmdBuffer = YES;
    }
    if (!activeCmdBuffer) {
        ptmc_release_slot(slot);
        atomic_fetch_add_explicit(&_droppedEncoder, 1, memory_order_relaxed);
        return;
    }
    BOOL scaling = session.sourceWidth != session.width || session.sourceHeight != session.height;
    if (ptmc_codec_is_hevc(session.codecName)) {
        id<MTLComputePipelineState> pipeline = scaling ? session.pipelineScale : session.pipelineBGRA;
        id<MTLComputeCommandEncoder> encoder = [activeCmdBuffer computeCommandEncoder];
        if (!encoder || !pipeline) {
            [encoder endEncoding];
            ptmc_release_slot(slot);
            atomic_fetch_add_explicit(&_droppedEncoder, 1, memory_order_relaxed);
            return;
        }
        [encoder setComputePipelineState:pipeline];
        [encoder setTexture:texture atIndex:0];
        [encoder setTexture:slot.yTexture atIndex:1];
        [encoder setTexture:slot.uvTexture atIndex:2];
        uint32_t srgb = sourceSRGB ? 1 : 0;
        [encoder setBytes:&srgb length:sizeof(srgb) atIndex:0];
        NSUInteger w = pipeline.threadExecutionWidth;
        NSUInteger h = MAX(1, pipeline.maxTotalThreadsPerThreadgroup / w);
        MTLSize tg = MTLSizeMake(w, MIN(h, 16), 1);
        MTLSize grid = MTLSizeMake((session.width + 1) / 2, (session.height + 1) / 2, 1);
        [encoder dispatchThreads:grid threadsPerThreadgroup:tg];
        [encoder endEncoding];
    } else if (scaling) {
        // Scale directly from the drawable into the IOSurface-backed BGRA encoder buffer. No
        // intermediate texture and no CPU-visible memcpy/readback are introduced.
        id<MTLComputeCommandEncoder> encoder = [activeCmdBuffer computeCommandEncoder];
        if (!encoder || !session.pipelineScale || !slot.bgraTexture) {
            [encoder endEncoding];
            ptmc_release_slot(slot);
            atomic_fetch_add_explicit(&_droppedEncoder, 1, memory_order_relaxed);
            return;
        }
        [encoder setComputePipelineState:session.pipelineScale];
        [encoder setTexture:texture atIndex:0];
        [encoder setTexture:slot.bgraTexture atIndex:1];
        uint32_t srgb = sourceSRGB ? 1 : 0;
        [encoder setBytes:&srgb length:sizeof(srgb) atIndex:0];
        NSUInteger w = session.pipelineScale.threadExecutionWidth;
        NSUInteger h = MAX(1, session.pipelineScale.maxTotalThreadsPerThreadgroup / w);
        [encoder dispatchThreads:MTLSizeMake(session.width, session.height, 1)
           threadsPerThreadgroup:MTLSizeMake(w, MIN(h, 16), 1)];
        [encoder endEncoding];
    } else {
        // Native-size ProRes is the cheapest path: one GPU blit from the drawable into the same
        // IOSurface-backed CVPixelBuffer that VideoToolbox consumes.
        id<MTLBlitCommandEncoder> blit = [activeCmdBuffer blitCommandEncoder];
        if (!blit || !slot.bgraTexture) {
            [blit endEncoding];
            ptmc_release_slot(slot);
            atomic_fetch_add_explicit(&_droppedEncoder, 1, memory_order_relaxed);
            return;
        }
        [blit copyFromTexture:texture
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(session.width, session.height, 1)
                    toTexture:slot.bgraTexture
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
    }

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
    uint64_t ownedSubmitTick = ownsCmdBuffer ? mach_continuous_time() : 0;
    if (ownsCmdBuffer) atomic_fetch_add_explicit(&_ownedCaptureCommandBuffers, 1, memory_order_relaxed);

    [activeCmdBuffer addCompletedHandler:^(id<MTLCommandBuffer> cb) {
        if (ownsCmdBuffer) {
            uint64_t completedTick = mach_continuous_time();
            atomic_store_explicit(&self->_lastOwnedCaptureCompletionUs,
                                  ptmc_ticks_to_ns(completedTick - ownedSubmitTick) / 1000u,
                                  memory_order_relaxed);
            SEL startSelector = NSSelectorFromString(@"GPUStartTime");
            SEL endSelector = NSSelectorFromString(@"GPUEndTime");
            if ([cb respondsToSelector:startSelector] && [cb respondsToSelector:endSelector]) {
                CFTimeInterval startTime = ((CFTimeInterval (*)(id, SEL))objc_msgSend)(cb, startSelector);
                CFTimeInterval endTime = ((CFTimeInterval (*)(id, SEL))objc_msgSend)(cb, endSelector);
                if (endTime >= startTime && startTime > 0) {
                    atomic_store_explicit(&self->_lastOwnedCaptureGPUTimeUs,
                                          (uint64_t)((endTime - startTime) * 1000000.0),
                                          memory_order_relaxed);
                }
            }
        }
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
        dispatch_async(session.encoderQueue, ^{
            if (atomic_load_explicit(&self->_generation, memory_order_acquire) != generation ||
                !atomic_load_explicit(&session->_accepting, memory_order_acquire)) {
                ptmc_release_slot(slot);
                atomic_fetch_sub_explicit(&session->_inFlight, 1, memory_order_acq_rel);
                atomic_fetch_add_explicit(&self->_droppedLate, 1, memory_order_relaxed);
                return;
            }
            VTEncodeInfoFlags flags = 0;
            // Slot/session callback metadata is preallocated with the IOSurface ring. The slot cannot
            // be reused until VideoToolbox calls us back, so no per-frame context allocation/retain is
            // needed here.
            void *frameRefCon = (__bridge void *)slot;
            OSStatus err = VTCompressionSessionEncodeFrame(session.compressionSession, slot->pixelBuffer, pts, duration,
                                                            NULL, frameRefCon, &flags);
            if (err != noErr) {
                ptmc_release_slot(slot);
                atomic_fetch_sub_explicit(&session->_inFlight, 1, memory_order_acq_rel);
                atomic_fetch_add_explicit(&self->_droppedEncoder, 1, memory_order_relaxed);
            }
        });
    }];
    if (ownsCmdBuffer) {
        [activeCmdBuffer commit];
    }
}

- (void)logStatusForce:(BOOL)force tick:(uint64_t)tick {
    uint64_t previous = atomic_load_explicit(&_lastLogTick, memory_order_relaxed);
    uint64_t elapsedNs = ptmc_ticks_to_ns(tick - previous);
    uint64_t threshold = (uint64_t)(_logInterval * 1000000000.0);
    if (!force && elapsedNs < threshold) return;
    if (!atomic_compare_exchange_strong_explicit(&_lastLogTick, &previous, tick, memory_order_acq_rel, memory_order_relaxed) && !force) return;
    double seconds = MAX((double)elapsedNs / 1e9, 0.001);
    uint64_t p = atomic_load_explicit(&_presented, memory_order_relaxed);
    uint64_t c = atomic_load_explicit(&_captured, memory_order_relaxed);
    uint64_t e = atomic_load_explicit(&_encoded, memory_order_relaxed);
    uint64_t samplingSkipped = atomic_load_explicit(&_skippedRate, memory_order_relaxed);
    uint64_t lp = atomic_exchange_explicit(&_lastLogPresented, p, memory_order_acq_rel);
    uint64_t lc = atomic_exchange_explicit(&_lastLogCaptured, c, memory_order_acq_rel);
    uint64_t le = atomic_exchange_explicit(&_lastLogEncoded, e, memory_order_acq_rel);
    uint64_t ls = atomic_exchange_explicit(&_lastLogSkipped, samplingSkipped, memory_order_acq_rel);
    double presentPerSecond = (p - lp) / seconds;
    double capturePerSecond = (c - lc) / seconds;
    double encodedPerSecond = (e - le) / seconds;
    double skipPerSecond = (samplingSkipped - ls) / seconds;
    atomic_store_explicit(&_presentMilliFPS, (uint64_t)llround(presentPerSecond * 1000.0), memory_order_relaxed);
    atomic_store_explicit(&_captureMilliFPS, (uint64_t)llround(capturePerSecond * 1000.0), memory_order_relaxed);
    atomic_store_explicit(&_encodedMilliFPS, (uint64_t)llround(encodedPerSecond * 1000.0), memory_order_relaxed);
    atomic_store_explicit(&_samplingSkipMilliFPS, (uint64_t)llround(skipPerSecond * 1000.0), memory_order_relaxed);
    PTMC_LOG(@"present=%.2f fps drawable=%llux%llu pixelFormat=%lld displaySync=%lld framebufferOnly=%lld captured=%llu capture=%.2f fps encoded=%.2f fps dropped_pool=%llu dropped_encoder=%llu dropped_late=%llu unsupported=%llu sampling_skips=%llu (%.2f/s) codec=%@ resolution=%@ suppressDisplay=%d skipPresent=%d requested=%d",
             presentPerSecond,
             (unsigned long long)atomic_load(&_drawableWidth), (unsigned long long)atomic_load(&_drawableHeight),
             (long long)atomic_load(&_pixelFormat), (long long)atomic_load(&_displaySync), (long long)atomic_load(&_framebufferOnly),
             (unsigned long long)c, capturePerSecond, encodedPerSecond,
             (unsigned long long)atomic_load(&_droppedPool), (unsigned long long)atomic_load(&_droppedEncoder),
             (unsigned long long)atomic_load(&_droppedLate), (unsigned long long)atomic_load(&_droppedUnsupported),
             (unsigned long long)samplingSkipped, skipPerSecond, self.codecName ?: @"hevc",
             self.captureResolutionMode ?: @"source", self.suppressDisplayOutput, self.skipDisplayPresent,
             atomic_load(&_requested));
    BOOL requested = atomic_load_explicit(&_requested, memory_order_acquire);
    if (force || requested) {
        PTMCSession *session = self.session;
        NSString *phase = !requested ? @"ready" : (session ? (session.writer ? @"recording" : @"armed") : @"requested");
        NSString *message = !requested ? @"PTMC hooks loaded and waiting for capture commands" : @"Capture heartbeat updated";
        ptmc_publish_status(self, session, phase, message);
    }
}
@end

static PTMCManager *ptmc_manager(void) {
    static PTMCManager *manager;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ manager = [[PTMCManager alloc] init]; });
    return manager;
}

static CGColorSpaceRef ptmc_srgb_colorspace(void) {
    static CGColorSpaceRef colorSpace;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    });
    return colorSpace;
}

static NSString *ptmc_colorspace_name(CGColorSpaceRef colorSpace) {
    if (!colorSpace) return @"(none)";
    CFStringRef name = CGColorSpaceGetName(colorSpace);
    return name ? [(__bridge NSString *)name copy] : @"(unnamed)";
}

// ----- Runtime hooks -------------------------------------------------------

typedef struct {
    __unsafe_unretained Class cls;
    SEL selector;
    IMP original;
    IMP replacement;
} PTMCHookEntry;

static PTMCHookEntry ptmc_hooks[PTMC_MAX_HOOKS];
static atomic_uint ptmc_hook_count;
static atomic_uint ptmc_imp_generation;
static atomic_bool ptmc_drawable_path_active;
static os_unfair_lock ptmc_hook_lock = OS_UNFAIR_LOCK_INIT;
static atomic_bool ptmc_discovery_scheduled;
static atomic_bool ptmc_image_scan_pending;
static IMP ptmc_max_fps_original = NULL;

typedef struct {
    __unsafe_unretained Class cls;
    SEL selector;
    IMP original;
    unsigned generation;
} PTMCImpCacheEntry;

static _Thread_local PTMCImpCacheEntry ptmc_imp_cache[8];

static IMP ptmc_original_imp(id self, SEL selector) {
    Class runtimeClass = object_getClass(self);
    uintptr_t hash = (((uintptr_t)runtimeClass >> 4) ^ ((uintptr_t)selector >> 4)) & 7u;
    unsigned generation = atomic_load_explicit(&ptmc_imp_generation, memory_order_acquire);
    PTMCImpCacheEntry cached = ptmc_imp_cache[hash];
    if (cached.generation == generation && cached.cls == runtimeClass &&
        cached.selector == selector && cached.original) return cached.original;

    unsigned count = atomic_load_explicit(&ptmc_hook_count, memory_order_acquire);
    IMP original = NULL;
    for (Class cls = runtimeClass; cls != Nil && !original; cls = class_getSuperclass(cls)) {
        for (unsigned i = 0; i < count; i++) {
            if (ptmc_hooks[i].cls == cls && ptmc_hooks[i].selector == selector) {
                original = ptmc_hooks[i].original;
                break;
            }
        }
    }
    ptmc_imp_cache[hash] = (PTMCImpCacheEntry){
        .cls = runtimeClass, .selector = selector, .original = original, .generation = generation
    };
    return original;
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

static void ptmc_call_original_bool_setter(id object, SEL selector, BOOL value) {
    IMP original = ptmc_original_imp(object, selector);
    if (original) {
        ((void (*)(id, SEL, BOOL))original)(object, selector, value);
    } else if ([object respondsToSelector:selector]) {
        ((void (*)(id, SEL, BOOL))objc_msgSend)(object, selector, value);
    }
}

static void ptmc_call_original_colorspace_setter(CAMetalLayer *layer, CGColorSpaceRef colorSpace) {
    SEL selector = @selector(setColorspace:);
    IMP original = ptmc_original_imp(layer, selector);
    if (original) {
        ((void (*)(id, SEL, CGColorSpaceRef))original)(layer, selector, colorSpace);
    } else {
        layer.colorspace = colorSpace;
    }
}

static void ptmc_release_original_colorspace(PTMCManager *manager) {
    if (manager->_originalColorSpace) {
        CGColorSpaceRelease(manager->_originalColorSpace);
        manager->_originalColorSpace = NULL;
    }
}

static NSDictionary *ptmc_get_developer_hud_properties(CAMetalLayer *layer) {
    SEL getter = NSSelectorFromString(@"developerHUDProperties");
    if (![layer respondsToSelector:getter]) return nil;
    id value = ((id (*)(id, SEL))objc_msgSend)(layer, getter);
    return [value isKindOfClass:[NSDictionary class]] ? [value copy] : nil;
}

static void ptmc_call_original_hud_properties_setter(CAMetalLayer *layer, NSDictionary *properties) {
    SEL setter = NSSelectorFromString(@"setDeveloperHUDProperties:");
    IMP original = ptmc_original_imp(layer, setter);
    if (original) ((void (*)(id, SEL, id))original)(layer, setter, properties);
    else if ([layer respondsToSelector:setter]) ((void (*)(id, SEL, id))objc_msgSend)(layer, setter, properties);
}

static void ptmc_apply_hud_capture_policy(CAMetalLayer *layer, PTMCManager *manager) {
    if (!manager->_hasDeveloperHUDProperties) return;
    if (manager.includeMetalHUDInCapture) {
        if (atomic_load_explicit(&manager->_metalHUDSuppressed, memory_order_acquire)) {
            ptmc_call_original_hud_properties_setter(layer, manager->_originalHUDProperties);
            atomic_store_explicit(&manager->_metalHUDSuppressed, false, memory_order_release);
        }
        return;
    }
    NSMutableDictionary *properties = manager->_originalHUDProperties
        ? [manager->_originalHUDProperties mutableCopy]
        : [NSMutableDictionary dictionary];
    properties[@"mode"] = @"disabled";
    ptmc_call_original_hud_properties_setter(layer, properties);
    atomic_store_explicit(&manager->_metalHUDSuppressed, true, memory_order_release);
}

static void ptmc_restore_capture_layer(PTMCManager *manager) {
    CAMetalLayer *layer = manager.captureLayer;
    if (!layer) {
        atomic_store_explicit(&manager->_captureLayerAddress, 0, memory_order_release);
        ptmc_release_original_colorspace(manager);
        return;
    }

    ptmc_call_original_bool_setter(layer, @selector(setFramebufferOnly:), manager->_originalFramebufferOnly);
    SEL syncSetter = NSSelectorFromString(@"setDisplaySyncEnabled:");
    if (manager->_hasOriginalDisplaySync) {
        ptmc_call_original_bool_setter(layer, syncSetter, manager->_originalDisplaySync);
    }
    SEL edrSetter = NSSelectorFromString(@"setWantsExtendedDynamicRangeContent:");
    if (manager->_hasOriginalEDR) {
        ptmc_call_original_bool_setter(layer, edrSetter, manager->_originalEDR);
    }
    ptmc_call_original_colorspace_setter(layer, manager->_originalColorSpace);
    if (manager->_hasDeveloperHUDProperties) {
        ptmc_call_original_hud_properties_setter(layer, manager->_originalHUDProperties);
    }
    atomic_store_explicit(&manager->_metalHUDSuppressed, false, memory_order_release);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer.opacity = manager->_originalOpacity;
    [CATransaction commit];

    manager.captureLayer = nil;
    atomic_store_explicit(&manager->_captureLayerAddress, 0, memory_order_release);
    manager->_hasOriginalEDR = NO;
    manager->_hasOriginalDisplaySync = NO;
    manager->_hasDeveloperHUDProperties = NO;
    manager->_originalHUDProperties = nil;
    ptmc_release_original_colorspace(manager);
    atomic_store_explicit(&manager->_layerNeedsRefresh, true, memory_order_release);
}

static void ptmc_prepare_capture_layer(CAMetalLayer *layer, PTMCManager *manager) {
    if (!layer) return;
    uintptr_t address = (uintptr_t)(__bridge void *)layer;
    BOOL sameLayer = atomic_load_explicit(&manager->_captureLayerAddress, memory_order_acquire) == address;
    if (sameLayer && !atomic_load_explicit(&manager->_layerNeedsRefresh, memory_order_acquire)) return;
    BOOL refresh = atomic_exchange_explicit(&manager->_layerNeedsRefresh, false, memory_order_acq_rel);
    if (sameLayer && !refresh) return;

    SEL syncGetter = NSSelectorFromString(@"displaySyncEnabled");
    SEL syncSetter = NSSelectorFromString(@"setDisplaySyncEnabled:");
    SEL edrGetter = NSSelectorFromString(@"wantsExtendedDynamicRangeContent");
    SEL edrSetter = NSSelectorFromString(@"setWantsExtendedDynamicRangeContent:");

    if (!sameLayer) {
        if (manager.captureLayer) ptmc_restore_capture_layer(manager);
        // A game normally owns one CAMetalLayer. Save its presentation state exactly once and
        // restore it at Stop so merely using PTMC cannot permanently change post-capture pacing/HDR.
        ptmc_release_original_colorspace(manager);
        manager->_originalFramebufferOnly = layer.framebufferOnly;
        manager->_originalOpacity = layer.opacity;
        manager->_hasOriginalDisplaySync = [layer respondsToSelector:syncGetter];
        if (manager->_hasOriginalDisplaySync) {
            manager->_originalDisplaySync = ((BOOL (*)(id, SEL))objc_msgSend)(layer, syncGetter);
        }
        manager->_hasOriginalEDR = [layer respondsToSelector:edrGetter];
        if (manager->_hasOriginalEDR) {
            manager->_originalEDR = ((BOOL (*)(id, SEL))objc_msgSend)(layer, edrGetter);
        }
        CGColorSpaceRef original = layer.colorspace;
        if (original) manager->_originalColorSpace = CGColorSpaceRetain(original);
        SEL hudGetter = NSSelectorFromString(@"developerHUDProperties");
        SEL hudSetter = NSSelectorFromString(@"setDeveloperHUDProperties:");
        manager->_hasDeveloperHUDProperties = [layer respondsToSelector:hudGetter] &&
                                               [layer respondsToSelector:hudSetter];
        manager->_originalHUDProperties = manager->_hasDeveloperHUDProperties
            ? ptmc_get_developer_hud_properties(layer)
            : nil;
        atomic_store_explicit(&manager->_metalHUDSuppressed, false, memory_order_release);
        manager.captureLayer = layer;
        atomic_store_explicit(&manager->_captureLayerAddress, address, memory_order_release);
    }
    ptmc_apply_hud_capture_policy(layer, manager);

    if (layer.framebufferOnly) layer.framebufferOnly = NO;
    atomic_store_explicit(&manager->_framebufferOnly, layer.framebufferOnly ? 1 : 0, memory_order_relaxed);

    if (manager->_hasOriginalDisplaySync && [layer respondsToSelector:syncSetter]) {
        // In headless mode the drawable is never submitted to CoreAnimation, so avoid mutating
        // presentation-only state entirely. This minimizes CA/WindowServer work while recording.
        BOOL desiredSync = manager.skipDisplayPresent
            ? manager->_originalDisplaySync
            : (manager.disableDisplaySync ? NO : manager->_originalDisplaySync);
        BOOL currentSync = ((BOOL (*)(id, SEL))objc_msgSend)(layer, syncGetter);
        if (currentSync != desiredSync) {
            ((void (*)(id, SEL, BOOL))objc_msgSend)(layer, syncSetter, desiredSync);
        }
        atomic_store_explicit(&manager->_displaySync, desiredSync ? 1 : 0, memory_order_relaxed);
    }

    if (manager.forceSDRDisplay && !manager.skipDisplayPresent) {
        if (manager->_hasOriginalEDR && [layer respondsToSelector:edrSetter]) {
            BOOL currentEDR = ((BOOL (*)(id, SEL))objc_msgSend)(layer, edrGetter);
            if (currentEDR) ((void (*)(id, SEL, BOOL))objc_msgSend)(layer, edrSetter, NO);
            atomic_store_explicit(&manager->_edr, 0, memory_order_relaxed);
        }
        CGColorSpaceRef sRGB = ptmc_srgb_colorspace();
        CGColorSpaceRef current = layer.colorspace;
        if (sRGB && (!current || !CFEqual(current, sRGB))) layer.colorspace = sRGB;
    } else {
        if (manager->_hasOriginalEDR && [layer respondsToSelector:edrSetter]) {
            BOOL currentEDR = ((BOOL (*)(id, SEL))objc_msgSend)(layer, edrGetter);
            if (currentEDR != manager->_originalEDR) {
                ((void (*)(id, SEL, BOOL))objc_msgSend)(layer, edrSetter, manager->_originalEDR);
            }
            atomic_store_explicit(&manager->_edr, manager->_originalEDR ? 1 : 0, memory_order_relaxed);
        }
        CGColorSpaceRef current = layer.colorspace;
        CGColorSpaceRef original = manager->_originalColorSpace;
        BOOL differs = (current != original) && (!current || !original || !CFEqual(current, original));
        if (differs) layer.colorspace = original;
    }
    // Full headless mode skips present itself; changing opacity would only create an extra CA transaction.
    float desiredOpacity = (!manager.skipDisplayPresent && manager.suppressDisplayOutput) ? 0.0f : manager->_originalOpacity;
    if (fabsf(layer.opacity - desiredOpacity) > 0.0001f) {
        // Keep calling present so CAMetalDrawable objects recycle normally. Making the layer fully
        // transparent is the safe optional "capture-only" mode; WindowServer may cull the visible
        // composition without risking nextDrawable starvation.
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        layer.opacity = desiredOpacity;
        [CATransaction commit];
    }
    manager.colorSpaceName = ptmc_colorspace_name(layer.colorspace);
}

static BOOL ptmc_record_present(id commandBuffer, id<CAMetalDrawable> drawable) {
    if (!atomic_load_explicit(&ptmc_recording_fast, memory_order_acquire)) return NO;
    PTMCManager *manager = ptmc_manager();
    ptmc_prepare_capture_layer(drawable.layer, manager);
    uint64_t tick = mach_continuous_time();
    atomic_fetch_add_explicit(&manager->_presented, 1, memory_order_relaxed);
    id<MTLTexture> texture = drawable.texture;
    if (!texture) return NO;

    atomic_store_explicit(&manager->_drawableWidth, texture.width, memory_order_relaxed);
    atomic_store_explicit(&manager->_drawableHeight, texture.height, memory_order_relaxed);
    atomic_store_explicit(&manager->_pixelFormat, texture.pixelFormat, memory_order_relaxed);
    if (atomic_load_explicit(&manager->_requested, memory_order_acquire)) {
        [manager captureTexture:texture commandBuffer:(id<MTLCommandBuffer>)commandBuffer tick:tick];
    }

    // Reuse the manager/texture already loaded for capture instead of performing a second drawable
    // lookup in every present hook. The normal path exits here without touching session state.
    if (!manager.skipDisplayPresent || !atomic_load_explicit(&manager->_requested, memory_order_acquire)) return NO;
    BOOL supported = texture.pixelFormat == MTLPixelFormatBGRA8Unorm ||
                     texture.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB;
    if (!supported) return NO;
    PTMCSession *session = manager.session;
    if (!session || !atomic_load_explicit(&session->_accepting, memory_order_acquire)) return NO;
    atomic_fetch_add_explicit(&manager->_skippedPresents, 1, memory_order_relaxed);
    return YES;
}

static void ptmc_present_drawable(id self, SEL _cmd, id<CAMetalDrawable> drawable) {
    BOOL skip = !atomic_load_explicit(&ptmc_drawable_path_active, memory_order_acquire)
        && ptmc_record_present(self, drawable);
    if (skip) return;
    IMP original = ptmc_original_imp(self, _cmd);
    if (original) ((void (*)(id, SEL, id<CAMetalDrawable>))original)(self, _cmd, drawable);
}

static void ptmc_present_drawable_at_time(id self, SEL _cmd, id<CAMetalDrawable> drawable, CFTimeInterval time) {
    BOOL skip = !atomic_load_explicit(&ptmc_drawable_path_active, memory_order_acquire)
        && ptmc_record_present(self, drawable);
    if (skip) return;
    IMP original = ptmc_original_imp(self, _cmd);
    if (original) ((void (*)(id, SEL, id<CAMetalDrawable>, CFTimeInterval))original)(self, _cmd, drawable, time);
}

static void ptmc_present_drawable_after_duration(id self, SEL _cmd, id<CAMetalDrawable> drawable, CFTimeInterval duration) {
    BOOL skip = !atomic_load_explicit(&ptmc_drawable_path_active, memory_order_acquire)
        && ptmc_record_present(self, drawable);
    if (skip) return;
    IMP original = ptmc_original_imp(self, _cmd);
    if (original) ((void (*)(id, SEL, id<CAMetalDrawable>, CFTimeInterval))original)(self, _cmd, drawable, duration);
}

static void ptmc_drawable_present(id self, SEL _cmd) {
    id<CAMetalDrawable> drawable = (id<CAMetalDrawable>)self;
    if (ptmc_record_present(nil, drawable)) return;
    IMP original = ptmc_original_imp(self, _cmd);
    if (original) ((void (*)(id, SEL))original)(self, _cmd);
}

static void ptmc_drawable_present_at_time(id self, SEL _cmd, CFTimeInterval time) {
    id<CAMetalDrawable> drawable = (id<CAMetalDrawable>)self;
    if (ptmc_record_present(nil, drawable)) return;
    IMP original = ptmc_original_imp(self, _cmd);
    if (original) ((void (*)(id, SEL, CFTimeInterval))original)(self, _cmd, time);
}

static void ptmc_drawable_present_after_duration(id self, SEL _cmd, CFTimeInterval duration) {
    id<CAMetalDrawable> drawable = (id<CAMetalDrawable>)self;
    if (ptmc_record_present(nil, drawable)) return;
    IMP original = ptmc_original_imp(self, _cmd);
    if (original) ((void (*)(id, SEL, CFTimeInterval))original)(self, _cmd, duration);
}

static void ptmc_drawable_present_with_options(id self, SEL _cmd, id options) {
    id<CAMetalDrawable> drawable = (id<CAMetalDrawable>)self;
    if (ptmc_record_present(nil, drawable)) return;
    IMP original = ptmc_original_imp(self, _cmd);
    if (original) ((void (*)(id, SEL, id))original)(self, _cmd, options);
}

static BOOL ptmc_install_one_hook(Class cls, SEL selector, IMP replacement) {
    Method ownMethod = NULL;
    if (!ptmc_class_implements_selector(cls, selector, &ownMethod) || !ownMethod) return NO;

    os_unfair_lock_lock(&ptmc_hook_lock);
    unsigned count = atomic_load_explicit(&ptmc_hook_count, memory_order_relaxed);
    for (unsigned i = 0; i < count; i++) {
        if (ptmc_hooks[i].cls != cls || ptmc_hooks[i].selector != selector) continue;
        IMP current = method_getImplementation(ownMethod);
        if (current == replacement) {
            os_unfair_lock_unlock(&ptmc_hook_lock);
            return NO;
        }
        // If somebody legitimately changed this method while PTMC was dormant, chain to their
        // current implementation instead of restoring a stale IMP.
        ptmc_hooks[i].original = current;
        ptmc_hooks[i].replacement = replacement;
        method_setImplementation(ownMethod, replacement);
        atomic_fetch_add_explicit(&ptmc_imp_generation, 1, memory_order_release);
        os_unfair_lock_unlock(&ptmc_hook_lock);
        return YES;
    }

    IMP original = method_getImplementation(ownMethod);
    if (!original || original == replacement || count >= PTMC_MAX_HOOKS) {
        if (count >= PTMC_MAX_HOOKS) PTMC_LOG(@"hook table full");
        os_unfair_lock_unlock(&ptmc_hook_lock);
        return NO;
    }
    ptmc_hooks[count] = (PTMCHookEntry){
        .cls = cls, .selector = selector, .original = original, .replacement = replacement
    };
    atomic_store_explicit(&ptmc_hook_count, count + 1, memory_order_release);
    method_setImplementation(ownMethod, replacement);
    atomic_fetch_add_explicit(&ptmc_imp_generation, 1, memory_order_release);
    os_unfair_lock_unlock(&ptmc_hook_lock);
    return YES;
}

static void ptmc_uninstall_runtime_hooks(void) {
    os_unfair_lock_lock(&ptmc_hook_lock);
    unsigned count = atomic_load_explicit(&ptmc_hook_count, memory_order_acquire);
    unsigned restored = 0;
    for (unsigned i = 0; i < count; i++) {
        PTMCHookEntry entry = ptmc_hooks[i];
        Method ownMethod = NULL;
        if (!entry.cls || !ptmc_class_implements_selector(entry.cls, entry.selector, &ownMethod) || !ownMethod) continue;
        if (method_getImplementation(ownMethod) == entry.replacement) {
            method_setImplementation(ownMethod, entry.original);
            restored++;
        }
    }
    atomic_fetch_add_explicit(&ptmc_imp_generation, 1, memory_order_release);
    atomic_store_explicit(&ptmc_drawable_path_active, false, memory_order_release);
    atomic_store_explicit(&ptmc_manager()->_presentHookCount, 0, memory_order_relaxed);
    os_unfair_lock_unlock(&ptmc_hook_lock);
    if (restored) PTMC_LOG(@"restored %u Metal methods; idle path returned to native dispatch", restored);
}

static Class ptmc_method_owner(Class cls, SEL selector) {
    for (Class current = cls; current != Nil; current = class_getSuperclass(current)) {
        if (ptmc_class_implements_selector(current, selector, NULL)) return current;
    }
    return Nil;
}

static BOOL ptmc_install_owner_hook(Class cls, SEL selector, IMP replacement) {
    Class owner = ptmc_method_owner(cls, selector);
    return owner ? ptmc_install_one_hook(owner, selector, replacement) : NO;
}

static BOOL ptmc_class_is_subclass_of(Class cls, Class base) {
    for (Class current = cls; current != Nil; current = class_getSuperclass(current)) {
        if (current == base) return YES;
    }
    return NO;
}

static void ptmc_set_wants_edr(id self, SEL _cmd, BOOL wantsEDR);
static void ptmc_set_developer_hud_properties(id self, SEL _cmd, NSDictionary *properties);
static void ptmc_set_colorspace(id self, SEL _cmd, CGColorSpaceRef colorSpace);
static void ptmc_set_framebuffer_only(id self, SEL _cmd, BOOL framebufferOnly);

static void ptmc_discover_runtime_classes(void) {
    int count = objc_getClassList(NULL, 0);
    if (count <= 0) return;
    __unsafe_unretained Class *classes = (__unsafe_unretained Class *)calloc((size_t)count, sizeof(Class));
    if (!classes) return;
    count = objc_getClassList(classes, count);

    Protocol *drawableProtocol = @protocol(MTLDrawable);
    Protocol *commandBufferProtocol = @protocol(MTLCommandBuffer);
    Class metalDrawableClass = NSClassFromString(@"CAMetalDrawable");
    Class baseCmdBufferClass = NSClassFromString(@"_MTLCommandBuffer");
    Class metalLayerClass = [CAMetalLayer class];
    SEL d1 = @selector(present);
    SEL d2 = @selector(presentAtTime:);
    SEL d3 = @selector(presentAfterMinimumDuration:);
    SEL d4 = NSSelectorFromString(@"presentWithOptions:");
    SEL s1 = @selector(presentDrawable:);
    SEL s2 = @selector(presentDrawable:atTime:);
    SEL s3 = @selector(presentDrawable:afterMinimumDuration:);
    SEL setEDR = NSSelectorFromString(@"setWantsExtendedDynamicRangeContent:");
    SEL setColorSpace = @selector(setColorspace:);
    SEL setFramebufferOnly = @selector(setFramebufferOnly:);
    SEL setDeveloperHUDProperties = NSSelectorFromString(@"setDeveloperHUDProperties:");
    unsigned installed = 0;
    unsigned drawableInstalled = 0;

    if (metalDrawableClass) {
        drawableInstalled += ptmc_install_owner_hook(metalDrawableClass, d1, (IMP)ptmc_drawable_present);
        drawableInstalled += ptmc_install_owner_hook(metalDrawableClass, d2, (IMP)ptmc_drawable_present_at_time);
        drawableInstalled += ptmc_install_owner_hook(metalDrawableClass, d3, (IMP)ptmc_drawable_present_after_duration);
        drawableInstalled += ptmc_install_owner_hook(metalDrawableClass, d4, (IMP)ptmc_drawable_present_with_options);
    }
    for (int i = 0; i < count; i++) {
        Class cls = classes[i];
        if (class_conformsToProtocol(cls, drawableProtocol) ||
            (metalDrawableClass && ptmc_class_is_subclass_of(cls, metalDrawableClass))) {
            drawableInstalled += ptmc_install_owner_hook(cls, d1, (IMP)ptmc_drawable_present);
            drawableInstalled += ptmc_install_owner_hook(cls, d2, (IMP)ptmc_drawable_present_at_time);
            drawableInstalled += ptmc_install_owner_hook(cls, d3, (IMP)ptmc_drawable_present_after_duration);
            drawableInstalled += ptmc_install_owner_hook(cls, d4, (IMP)ptmc_drawable_present_with_options);
        }
        if (ptmc_class_is_subclass_of(cls, metalLayerClass)) {
            installed += ptmc_install_owner_hook(cls, setEDR, (IMP)ptmc_set_wants_edr);
            installed += ptmc_install_owner_hook(cls, setColorSpace, (IMP)ptmc_set_colorspace);
            installed += ptmc_install_owner_hook(cls, setFramebufferOnly, (IMP)ptmc_set_framebuffer_only);
            installed += ptmc_install_owner_hook(cls, setDeveloperHUDProperties,
                                                 (IMP)ptmc_set_developer_hud_properties);
        }
    }

    if (drawableInstalled > 0) {
        atomic_store_explicit(&ptmc_drawable_path_active, true, memory_order_release);
        atomic_fetch_add_explicit(&ptmc_manager()->_presentHookCount, drawableInstalled, memory_order_relaxed);
        PTMC_LOG(@"direct CAMetalDrawable hooks installed=%u", drawableInstalled);
        installed += drawableInstalled;
    }

    // Real-device Unity uses CAMetalDrawable.present* directly. Keep MTLCommandBuffer interception
    // only as a compatibility fallback so the normal path has fewer swizzled hot methods.
    if (!atomic_load_explicit(&ptmc_drawable_path_active, memory_order_acquire)) {
        unsigned commandInstalled = 0;
        if (baseCmdBufferClass) {
            commandInstalled += ptmc_install_owner_hook(baseCmdBufferClass, s1, (IMP)ptmc_present_drawable);
            commandInstalled += ptmc_install_owner_hook(baseCmdBufferClass, s2, (IMP)ptmc_present_drawable_at_time);
            commandInstalled += ptmc_install_owner_hook(baseCmdBufferClass, s3, (IMP)ptmc_present_drawable_after_duration);
        }
        for (int i = 0; i < count; i++) {
            Class cls = classes[i];
            if (class_conformsToProtocol(cls, commandBufferProtocol) ||
                (baseCmdBufferClass && ptmc_class_is_subclass_of(cls, baseCmdBufferClass))) {
                commandInstalled += ptmc_install_owner_hook(cls, s1, (IMP)ptmc_present_drawable);
                commandInstalled += ptmc_install_owner_hook(cls, s2, (IMP)ptmc_present_drawable_at_time);
                commandInstalled += ptmc_install_owner_hook(cls, s3, (IMP)ptmc_present_drawable_after_duration);
            }
        }
        if (commandInstalled) {
            atomic_fetch_add_explicit(&ptmc_manager()->_presentHookCount, commandInstalled, memory_order_relaxed);
            PTMC_LOG(@"command-buffer fallback hooks installed=%u", commandInstalled);
            installed += commandInstalled;
        }
    }

    free(classes);
    if (installed) PTMC_LOG(@"activated %u Metal hooks", installed);
}

static void ptmc_image_added(const struct mach_header *header, intptr_t slide) {
    (void)header; (void)slide;
    PTMCManager *manager = ptmc_manager();
    if (!atomic_load_explicit(&manager->_requested, memory_order_acquire)) return;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&ptmc_image_scan_pending, &expected, true)) return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC),
                   dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        ptmc_discover_runtime_classes();
        atomic_store_explicit(&ptmc_image_scan_pending, false, memory_order_release);
    });
}

static void ptmc_schedule_discovery(void) {
    bool expected = false;
    if (!atomic_compare_exchange_strong(&ptmc_discovery_scheduled, &expected, true)) return;
    static const double delays[] = {0.0, 0.1, 0.5, 2.0};
    dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);
    for (unsigned i = 0; i < sizeof(delays)/sizeof(delays[0]); i++) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delays[i] * NSEC_PER_SEC)), q, ^{
            ptmc_discover_runtime_classes();
        });
    }
}

static BOOL ptmc_should_force_sdr(PTMCManager *manager) {
    return atomic_load_explicit(&manager->_requested, memory_order_acquire) && manager.forceSDRDisplay;
}

static void ptmc_set_wants_edr(id self, SEL _cmd, BOOL wantsEDR) {
    BOOL recording = atomic_load_explicit(&ptmc_recording_fast, memory_order_acquire);
    PTMCManager *manager = recording ? ptmc_manager() : nil;
    BOOL effective = manager && ptmc_should_force_sdr(manager) ? NO : wantsEDR;
    if (manager) atomic_store_explicit(&manager->_edr, effective ? 1 : 0, memory_order_relaxed);
    IMP original = ptmc_original_imp(self, _cmd);
    if (original) ((void (*)(id, SEL, BOOL))original)(self, _cmd, effective);
}

static void ptmc_set_colorspace(id self, SEL _cmd, CGColorSpaceRef colorSpace) {
    BOOL recording = atomic_load_explicit(&ptmc_recording_fast, memory_order_acquire);
    PTMCManager *manager = recording ? ptmc_manager() : nil;
    CGColorSpaceRef effective = colorSpace;
    if (manager && ptmc_should_force_sdr(manager) && ptmc_srgb_colorspace()) effective = ptmc_srgb_colorspace();
    if (manager) manager.colorSpaceName = ptmc_colorspace_name(effective);
    IMP original = ptmc_original_imp(self, _cmd);
    if (original) ((void (*)(id, SEL, CGColorSpaceRef))original)(self, _cmd, effective);
}

static void ptmc_set_framebuffer_only(id self, SEL _cmd, BOOL framebufferOnly) {
    BOOL recording = atomic_load_explicit(&ptmc_recording_fast, memory_order_acquire);
    BOOL effective = recording ? NO : framebufferOnly;
    IMP original = ptmc_original_imp(self, _cmd);
    if (original) ((void (*)(id, SEL, BOOL))original)(self, _cmd, effective);
}

static void ptmc_set_developer_hud_properties(id self, SEL _cmd, NSDictionary *properties) {
    BOOL recording = atomic_load_explicit(&ptmc_recording_fast, memory_order_acquire);
    PTMCManager *manager = recording ? ptmc_manager() : nil;
    NSDictionary *effective = properties;
    if (manager && manager.captureLayer == self && !manager.includeMetalHUDInCapture) {
        manager->_originalHUDProperties = [properties copy];
        NSMutableDictionary *hidden = properties ? [properties mutableCopy] : [NSMutableDictionary dictionary];
        hidden[@"mode"] = @"disabled";
        effective = hidden;
        atomic_store_explicit(&manager->_metalHUDSuppressed, true, memory_order_release);
    }
    IMP original = ptmc_original_imp(self, _cmd);
    if (original) ((void (*)(id, SEL, id))original)(self, _cmd, effective);
}

static NSInteger ptmc_maximum_frames_per_second(id self, SEL _cmd) {
    if (ptmc_spoof_max_fps > 0) return ptmc_spoof_max_fps;
    return ptmc_max_fps_original ? ((NSInteger (*)(id, SEL))ptmc_max_fps_original)(self, _cmd) : 60;
}

static void ptmc_update_max_fps_hook(void) {
    Method maxFPS = class_getInstanceMethod([UIScreen class], @selector(maximumFramesPerSecond));
    if (!maxFPS) return;
    IMP current = method_getImplementation(maxFPS);
    if (ptmc_spoof_max_fps > 0) {
        if (current != (IMP)ptmc_maximum_frames_per_second) {
            ptmc_max_fps_original = current;
            method_setImplementation(maxFPS, (IMP)ptmc_maximum_frames_per_second);
        }
    } else if (current == (IMP)ptmc_maximum_frames_per_second && ptmc_max_fps_original) {
        method_setImplementation(maxFPS, ptmc_max_fps_original);
    }
}

static CFStringRef ptmc_target_start_notification = NULL;
static CFStringRef ptmc_target_stop_notification = NULL;
static CFStringRef ptmc_target_status_notification = NULL;

static BOOL ptmc_notification_matches(CFStringRef name, CFStringRef globalName, CFStringRef targetName) {
    return CFEqual(name, globalName) || (targetName && CFEqual(name, targetName));
}

static void ptmc_control_notification(CFNotificationCenterRef center, void *observer, CFStringRef name,
                                      const void *object, CFDictionaryRef userInfo) {
    (void)center; (void)observer; (void)object; (void)userInfo;
    PTMCManager *manager = ptmc_manager();
    if (ptmc_notification_matches(name, CFSTR("io.playcover.ptmc.start"), ptmc_target_start_notification)) [manager start];
    else if (ptmc_notification_matches(name, CFSTR("io.playcover.ptmc.stop"), ptmc_target_stop_notification)) [manager stop];
    else if (ptmc_notification_matches(name, CFSTR("io.playcover.ptmc.status"), ptmc_target_status_notification)) {
        [manager reloadConfiguration];
        [manager logStatusForce:YES tick:mach_continuous_time()];
    }
}

static void __attribute__((constructor)) ptmc_initialize(void) {
    NSDictionary *config = ptmc_read_runtime_config();
    BOOL configEnabled = [config[@"enabled"] respondsToSelector:@selector(boolValue)] ? [config[@"enabled"] boolValue] : NO;
    BOOL configAutostart = [config[@"autostart"] respondsToSelector:@selector(boolValue)] ? [config[@"autostart"] boolValue] : NO;
    BOOL enabled = ptmc_env_bool("PTMC_ENABLE", configEnabled);
    BOOL autostart = ptmc_env_bool("PTMC_AUTOSTART", configAutostart);
    atomic_init(&ptmc_hook_count, 0);
    atomic_init(&ptmc_imp_generation, 1);
    atomic_init(&ptmc_drawable_path_active, false);
    atomic_store_explicit(&ptmc_recording_fast, false, memory_order_relaxed);
    atomic_init(&ptmc_discovery_scheduled, false);
    atomic_init(&ptmc_image_scan_pending, false);
    PTMCManager *manager = ptmc_manager();
    CFNotificationCenterRef darwin = CFNotificationCenterGetDarwinNotifyCenter();
    CFNotificationCenterAddObserver(darwin, NULL, ptmc_control_notification, CFSTR("io.playcover.ptmc.start"), NULL, CFNotificationSuspensionBehaviorDeliverImmediately);
    CFNotificationCenterAddObserver(darwin, NULL, ptmc_control_notification, CFSTR("io.playcover.ptmc.stop"), NULL, CFNotificationSuspensionBehaviorDeliverImmediately);
    CFNotificationCenterAddObserver(darwin, NULL, ptmc_control_notification, CFSTR("io.playcover.ptmc.status"), NULL, CFNotificationSuspensionBehaviorDeliverImmediately);
    NSString *bundleIdentifier = NSBundle.mainBundle.bundleIdentifier;
    if (bundleIdentifier.length > 0) {
        ptmc_target_start_notification = CFBridgingRetain([NSString stringWithFormat:@"io.playcover.ptmc.start.%@", bundleIdentifier]);
        ptmc_target_stop_notification = CFBridgingRetain([NSString stringWithFormat:@"io.playcover.ptmc.stop.%@", bundleIdentifier]);
        ptmc_target_status_notification = CFBridgingRetain([NSString stringWithFormat:@"io.playcover.ptmc.status.%@", bundleIdentifier]);
        CFNotificationCenterAddObserver(darwin, NULL, ptmc_control_notification, ptmc_target_start_notification, NULL, CFNotificationSuspensionBehaviorDeliverImmediately);
        CFNotificationCenterAddObserver(darwin, NULL, ptmc_control_notification, ptmc_target_stop_notification, NULL, CFNotificationSuspensionBehaviorDeliverImmediately);
        CFNotificationCenterAddObserver(darwin, NULL, ptmc_control_notification, ptmc_target_status_notification, NULL, CFNotificationSuspensionBehaviorDeliverImmediately);
    }
    _dyld_register_func_for_add_image(ptmc_image_added);
    PTMC_LOG(@"runtime ready bundle=%@ codec=%@ fps=%ld bitrate=%ld buffers=%ld disableDisplaySync=%d forceSDR=%d captureHUD=%d spoofMaxFPS=%ld enabled=%d",
             bundleIdentifier ?: @"(unknown)", manager.codecName ?: @"hevc",
             (long)manager.fps, (long)manager.bitrate, (long)manager.bufferCount,
             manager.disableDisplaySync, manager.forceSDRDisplay, manager.includeMetalHUDInCapture,
             (long)ptmc_spoof_max_fps, enabled);
    ptmc_publish_status(manager, nil, @"ready", @"PTMC runtime loaded; frame hooks are dormant until Start");
    manager.heartbeatTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                                   dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_timer(manager.heartbeatTimer,
                              dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC),
                              NSEC_PER_SEC,
                              100 * NSEC_PER_MSEC);
    __block unsigned idleHeartbeat = 0;
    dispatch_source_set_event_handler(manager.heartbeatTimer, ^{
        BOOL requested = atomic_load_explicit(&manager->_requested, memory_order_acquire);
        if (requested) {
            [manager logStatusForce:YES tick:mach_continuous_time()];
            idleHeartbeat = 0;
        } else if (++idleHeartbeat >= 5) {
            idleHeartbeat = 0;
            ptmc_publish_status(manager, nil, @"ready", @"PTMC runtime idle; native Metal dispatch restored");
        }
    });
    dispatch_resume(manager.heartbeatTimer);
    if (enabled && autostart) [manager start];
}
