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
    atomic_uint_fast64_t _firstHostTime;
    atomic_uint_fast64_t _lastCaptureTick;
    atomic_int_fast64_t _lastPTSUnits;
}
@property(nonatomic) uint64_t generation;
@property(nonatomic) NSUInteger width;
@property(nonatomic) NSUInteger height;
@property(nonatomic) NSInteger fps;
@property(nonatomic) NSInteger bitrate;
@property(nonatomic, copy) NSString *codecName;
@property(nonatomic) NSInteger bufferCount;
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, strong) id<MTLComputePipelineState> pipelineBGRA;
@property(nonatomic, strong) id<MTLComputePipelineState> pipelineSRGB;
@property(nonatomic, strong) id<MTLCommandQueue> commandQueue;
@property(nonatomic, strong) NSArray<PTMCSlot *> *slots;
@property(nonatomic) CVMetalTextureCacheRef textureCache;
@property(nonatomic) VTCompressionSessionRef compressionSession;
@property(nonatomic, strong) AVAssetWriter *writer;
@property(nonatomic, strong) AVAssetWriterInput *writerInput;
@property(nonatomic, copy) NSString *outputPath;
@property(nonatomic, copy) NSString *writerPath;
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
    atomic_uint_fast64_t _skippedRate;
    atomic_uint_fast64_t _lastLogTick;
    atomic_uint_fast64_t _lastLogPresented;
    atomic_uint_fast64_t _lastLogEncoded;
    atomic_uint_fast64_t _drawableWidth;
    atomic_uint_fast64_t _drawableHeight;
    atomic_int_fast64_t _pixelFormat;
    atomic_int_fast64_t _framebufferOnly;
    atomic_int_fast64_t _displaySync;
    atomic_int_fast64_t _edr;
    atomic_uint_fast64_t _presentHookCount;
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
@property(nonatomic) BOOL captureEnabled;
@property(atomic, copy) NSString *colorSpaceName;
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
static NSInteger ptmc_spoof_max_fps = 0;

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
        status[@"skippedRate"] = @(atomic_load_explicit(&manager->_skippedRate, memory_order_relaxed));
        status[@"codec"] = manager.codecName ?: @"hevc";
        status[@"drawableWidth"] = @(atomic_load_explicit(&manager->_drawableWidth, memory_order_relaxed));
        status[@"drawableHeight"] = @(atomic_load_explicit(&manager->_drawableHeight, memory_order_relaxed));
        status[@"pixelFormat"] = @(atomic_load_explicit(&manager->_pixelFormat, memory_order_relaxed));
        status[@"framebufferOnly"] = @(atomic_load_explicit(&manager->_framebufferOnly, memory_order_relaxed));
        status[@"displaySync"] = @(atomic_load_explicit(&manager->_displaySync, memory_order_relaxed));
        status[@"edr"] = @(atomic_load_explicit(&manager->_edr, memory_order_relaxed));
        status[@"presentHookCount"] = @(atomic_load_explicit(&manager->_presentHookCount, memory_order_relaxed));
        status[@"captureEnabled"] = @(manager.captureEnabled);
        if (manager.colorSpaceName.length > 0) status[@"colorSpace"] = manager.colorSpaceName;
        if (session) {
            status[@"generation"] = @(session.generation);
            status[@"width"] = @(session.width);
            status[@"height"] = @(session.height);
            status[@"fps"] = @(session.fps);
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
    });
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
    NSDictionary *sourceAttrs = @{
        (__bridge NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
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
        atomic_init(&_droppedPool, 0); atomic_init(&_droppedEncoder, 0); atomic_init(&_droppedLate, 0); atomic_init(&_droppedUnsupported, 0); atomic_init(&_skippedRate, 0);
        atomic_init(&_lastLogTick, mach_continuous_time()); atomic_init(&_lastLogPresented, 0); atomic_init(&_lastLogEncoded, 0);
        atomic_init(&_drawableWidth, 0); atomic_init(&_drawableHeight, 0); atomic_init(&_pixelFormat, -1);
        atomic_init(&_framebufferOnly, -1); atomic_init(&_displaySync, -1); atomic_init(&_edr, -1);
        atomic_init(&_presentHookCount, 0);
        [self reloadConfiguration];
    }
    return self;
}

- (void)reloadConfiguration {
    NSDictionary *config = ptmc_read_runtime_config();
    NSNumber *enabledValue = [config[@"enabled"] isKindOfClass:[NSNumber class]] ? config[@"enabled"] : nil;
    long long configFPS = [config[@"fps"] respondsToSelector:@selector(longLongValue)] ? [config[@"fps"] longLongValue] : 120;
    long long configBitrate = [config[@"bitrate"] respondsToSelector:@selector(longLongValue)] ? [config[@"bitrate"] longLongValue] : 120000000;
    long long configBuffers = [config[@"buffers"] respondsToSelector:@selector(longLongValue)] ? [config[@"buffers"] longLongValue] : 6;
    double configLogInterval = [config[@"logInterval"] respondsToSelector:@selector(doubleValue)] ? [config[@"logInterval"] doubleValue] : 1.0;
    BOOL configDisableDisplaySync = [config[@"disableDisplaySync"] respondsToSelector:@selector(boolValue)] ? [config[@"disableDisplaySync"] boolValue] : NO;
    BOOL configForceSDR = [config[@"forceSDRDisplay"] respondsToSelector:@selector(boolValue)] ? [config[@"forceSDRDisplay"] boolValue] : YES;
    long long configSpoofFPS = [config[@"spoofMaxFPS"] respondsToSelector:@selector(longLongValue)] ? [config[@"spoofMaxFPS"] longLongValue] : 0;
    NSString *configCodec = [config[@"codec"] isKindOfClass:[NSString class]] ? [config[@"codec"] lowercaseString] : @"hevc";

    self.captureEnabled = enabledValue ? enabledValue.boolValue : ptmc_env_bool("PTMC_ENABLE", false);
    self.fps = (NSInteger)ptmc_env_ll("PTMC_FPS", configFPS, 1, 240);
    self.bitrate = (NSInteger)ptmc_env_ll("PTMC_BITRATE", configBitrate, 1000000, 1000000000);
    self.bufferCount = (NSInteger)ptmc_env_ll("PTMC_BUFFERS", configBuffers, 3, 16);
    self.logInterval = ptmc_env_double("PTMC_LOG_INTERVAL", configLogInterval, 0.25, 60.0);
    self.disableDisplaySync = ptmc_env_bool("PTMC_DISABLE_DISPLAY_SYNC", configDisableDisplaySync);
    self.forceSDRDisplay = ptmc_env_bool("PTMC_FORCE_SDR_DISPLAY", configForceSDR);
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
    atomic_store_explicit(&_requested, true, memory_order_release);
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
            ptmc_finalize_session_when_drained(session);
        });
    }
    PTMC_LOG(@"stop requested");
    ptmc_publish_status(self, session, session ? @"stopping" : @"stopped", session ? @"Finalizing recording" : @"No active capture session");
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
        s.width = width; s.height = height; s.fps = self.fps; s.bitrate = self.bitrate; s.codecName = self.codecName; s.bufferCount = self.bufferCount;
        s.device = device;
        s.encoderQueue = dispatch_queue_create("io.playcover.ptmc.encoder", DISPATCH_QUEUE_SERIAL);
        atomic_init(&s->_accepting, true); atomic_init(&s->_inFlight, 0); atomic_init(&s->_firstTick, 0); atomic_init(&s->_firstHostTime, 0); atomic_init(&s->_lastCaptureTick, 0); atomic_init(&s->_lastPTSUnits, -1);
        s.outputPath = ptmc_output_path_from_environment();
        BOOL ok = ptmc_create_pipelines(s) && ptmc_create_slots(s) && ptmc_create_video_session(s);
        if (ok && atomic_load_explicit(&self->_requested, memory_order_acquire)) {
            self.session = s;
            PTMC_LOG(@"capture armed generation=%llu drawable=%lux%lu codec=%@ buffers=%ld output=%@",
                     (unsigned long long)s.generation, (unsigned long)s.width, (unsigned long)s.height,
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
    if (texture.width != session.width || texture.height != session.height) {
        atomic_fetch_add_explicit(&_droppedUnsupported, 1, memory_order_relaxed);
        PTMC_LOG(@"drawable size changed %lux%lu -> %lux%lu; stopping safely (restart capture to re-arm)",
                 (unsigned long)session.width, (unsigned long)session.height,
                 (unsigned long)texture.width, (unsigned long)texture.height);
        [self stop];
        return;
    }
    if ([texture respondsToSelector:@selector(framebufferOnly)] && texture.framebufferOnly) {
        atomic_fetch_add_explicit(&_droppedUnsupported, 1, memory_order_relaxed);
        return;
    }
    BOOL sourceSRGB = texture.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB;
    if (texture.pixelFormat != MTLPixelFormatBGRA8Unorm && !sourceSRGB) {
        atomic_fetch_add_explicit(&_droppedUnsupported, 1, memory_order_relaxed);
        return;
    }
    // The configured FPS is a real capture-rate ceiling, not just encoder metadata. A 120 Hz
    // game captured at 60 fps now performs roughly half as many BGRA->NV12 conversions/encodes.
    uint64_t lastCapture = atomic_load_explicit(&session->_lastCaptureTick, memory_order_relaxed);
    uint64_t intervalNs = 1000000000ull / (uint64_t)MAX(session.fps, 1);
    if (lastCapture != 0 && ptmc_ticks_to_ns(tick - lastCapture) < intervalNs) {
        atomic_fetch_add_explicit(&_skippedRate, 1, memory_order_relaxed);
        return;
    }
    atomic_store_explicit(&session->_lastCaptureTick, tick, memory_order_relaxed);
    uint64_t zeroHost = 0;
    uint64_t hostNow = mach_absolute_time();
    atomic_compare_exchange_strong_explicit(&session->_firstHostTime, &zeroHost, hostNow, memory_order_acq_rel, memory_order_relaxed);
    PTMCSlot *slot = ptmc_acquire_slot(session);
    if (!slot) {
        atomic_fetch_add_explicit(&_droppedPool, 1, memory_order_relaxed);
        return;
    }
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
    id<MTLComputeCommandEncoder> encoder = [activeCmdBuffer computeCommandEncoder];
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

    [activeCmdBuffer addCompletedHandler:^(id<MTLCommandBuffer> cb) {
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
    uint64_t e = atomic_load_explicit(&_encoded, memory_order_relaxed);
    uint64_t lp = atomic_exchange_explicit(&_lastLogPresented, p, memory_order_acq_rel);
    uint64_t le = atomic_exchange_explicit(&_lastLogEncoded, e, memory_order_acq_rel);
    PTMC_LOG(@"present=%.2f fps drawable=%llux%llu pixelFormat=%lld displaySync=%lld framebufferOnly=%lld captured=%llu encoded=%.2f fps dropped_pool=%llu dropped_encoder=%llu dropped_late=%llu unsupported=%llu skipped_rate=%llu codec=%@ requested=%d",
             (p - lp) / seconds,
             (unsigned long long)atomic_load(&_drawableWidth), (unsigned long long)atomic_load(&_drawableHeight),
             (long long)atomic_load(&_pixelFormat), (long long)atomic_load(&_displaySync), (long long)atomic_load(&_framebufferOnly),
             (unsigned long long)atomic_load(&_captured), (e - le) / seconds,
             (unsigned long long)atomic_load(&_droppedPool), (unsigned long long)atomic_load(&_droppedEncoder),
             (unsigned long long)atomic_load(&_droppedLate), (unsigned long long)atomic_load(&_droppedUnsupported),
             (unsigned long long)atomic_load(&_skippedRate), self.codecName ?: @"hevc", atomic_load(&_requested));
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
} PTMCHookEntry;

static PTMCHookEntry ptmc_hooks[PTMC_MAX_HOOKS];
static atomic_uint ptmc_hook_count;
static os_unfair_lock ptmc_hook_lock = OS_UNFAIR_LOCK_INIT;
static atomic_bool ptmc_discovery_scheduled;
static atomic_bool ptmc_image_scan_pending;
static IMP ptmc_max_fps_original = NULL;

typedef struct {
    __unsafe_unretained Class cls;
    SEL selector;
    IMP original;
} PTMCImpCacheEntry;

static _Thread_local PTMCImpCacheEntry ptmc_imp_cache[8];

static IMP ptmc_original_imp(id self, SEL selector) {
    Class runtimeClass = object_getClass(self);
    uintptr_t hash = (((uintptr_t)runtimeClass >> 4) ^ ((uintptr_t)selector >> 4)) & 7u;
    PTMCImpCacheEntry cached = ptmc_imp_cache[hash];
    if (cached.cls == runtimeClass && cached.selector == selector && cached.original) return cached.original;

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
    ptmc_imp_cache[hash] = (PTMCImpCacheEntry){ .cls = runtimeClass, .selector = selector, .original = original };
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

static void ptmc_record_present(id commandBuffer, id<CAMetalDrawable> drawable) {
    PTMCManager *manager = ptmc_manager();
    if (!atomic_load_explicit(&manager->_requested, memory_order_acquire)) return;
    uint64_t tick = mach_continuous_time();
    atomic_fetch_add_explicit(&manager->_presented, 1, memory_order_relaxed);
    id<MTLTexture> texture = drawable.texture;
    if (texture) {
        atomic_store_explicit(&manager->_drawableWidth, texture.width, memory_order_relaxed);
        atomic_store_explicit(&manager->_drawableHeight, texture.height, memory_order_relaxed);
        atomic_store_explicit(&manager->_pixelFormat, texture.pixelFormat, memory_order_relaxed);
        if (atomic_load_explicit(&manager->_requested, memory_order_acquire)) {
            [manager captureTexture:texture commandBuffer:(id<MTLCommandBuffer>)commandBuffer tick:tick];
        }
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

static void ptmc_drawable_present(id self, SEL _cmd) {
    ptmc_record_present(nil, (id<CAMetalDrawable>)self);
    IMP original = ptmc_original_imp(self, _cmd);
    if (original) ((void (*)(id, SEL))original)(self, _cmd);
}

static void ptmc_drawable_present_at_time(id self, SEL _cmd, CFTimeInterval time) {
    ptmc_record_present(nil, (id<CAMetalDrawable>)self);
    IMP original = ptmc_original_imp(self, _cmd);
    if (original) ((void (*)(id, SEL, CFTimeInterval))original)(self, _cmd, time);
}

static void ptmc_drawable_present_after_duration(id self, SEL _cmd, CFTimeInterval duration) {
    ptmc_record_present(nil, (id<CAMetalDrawable>)self);
    IMP original = ptmc_original_imp(self, _cmd);
    if (original) ((void (*)(id, SEL, CFTimeInterval))original)(self, _cmd, duration);
}

static void ptmc_drawable_present_with_options(id self, SEL _cmd, id options) {
    ptmc_record_present(nil, (id<CAMetalDrawable>)self);
    IMP original = ptmc_original_imp(self, _cmd);
    if (original) ((void (*)(id, SEL, id))original)(self, _cmd, options);
}

static BOOL ptmc_install_one_hook(Class cls, SEL selector, IMP replacement) {
    Method ownMethod = NULL;
    if (!ptmc_class_implements_selector(cls, selector, &ownMethod) || !ownMethod) return NO;
    IMP original = method_getImplementation(ownMethod);
    if (!original || original == replacement) return NO;

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
    ptmc_hooks[count] = (PTMCHookEntry){ .cls = cls, .selector = selector, .original = original };
    atomic_store_explicit(&ptmc_hook_count, count + 1, memory_order_release);
    method_setImplementation(ownMethod, replacement);
    os_unfair_lock_unlock(&ptmc_hook_lock);
    return YES;
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

static id<CAMetalDrawable> ptmc_next_drawable(CAMetalLayer *self, SEL _cmd);
static void ptmc_set_wants_edr(id self, SEL _cmd, BOOL wantsEDR);
static void ptmc_set_colorspace(id self, SEL _cmd, CGColorSpaceRef colorSpace);

static void ptmc_discover_runtime_classes(void) {
    int count = objc_getClassList(NULL, 0);
    if (count <= 0) return;
    __unsafe_unretained Class *classes = (__unsafe_unretained Class *)calloc((size_t)count, sizeof(Class));
    if (!classes) return;
    count = objc_getClassList(classes, count);
    Protocol *protocol = @protocol(MTLCommandBuffer);
    Protocol *drawableProtocol = @protocol(MTLDrawable);
    Class metalLayerClass = [CAMetalLayer class];
    Class metalDrawableClass = NSClassFromString(@"CAMetalDrawable");
    Class baseCmdBufferClass = NSClassFromString(@"_MTLCommandBuffer");
    unsigned installed = 0;
    SEL s1 = @selector(presentDrawable:);
    SEL s2 = @selector(presentDrawable:atTime:);
    SEL s3 = @selector(presentDrawable:afterMinimumDuration:);
    SEL d1 = @selector(present);
    SEL d2 = @selector(presentAtTime:);
    SEL d3 = @selector(presentAfterMinimumDuration:);
    SEL d4 = NSSelectorFromString(@"presentWithOptions:");
    SEL next = @selector(nextDrawable);
    SEL setEDR = NSSelectorFromString(@"setWantsExtendedDynamicRangeContent:");
    SEL setColorSpace = @selector(setColorspace:);

    if (baseCmdBufferClass) {
        unsigned presentInstalled = 0;
        presentInstalled += ptmc_install_owner_hook(baseCmdBufferClass, s1, (IMP)ptmc_present_drawable);
        presentInstalled += ptmc_install_owner_hook(baseCmdBufferClass, s2, (IMP)ptmc_present_drawable_at_time);
        presentInstalled += ptmc_install_owner_hook(baseCmdBufferClass, s3, (IMP)ptmc_present_drawable_after_duration);
        if (presentInstalled) {
            atomic_fetch_add_explicit(&ptmc_manager()->_presentHookCount, presentInstalled, memory_order_relaxed);
            PTMC_LOG(@"base command-buffer hooks class=%@ installed=%u", NSStringFromClass(baseCmdBufferClass), presentInstalled);
            installed += presentInstalled;
        }
    }

    if (metalDrawableClass) {
        unsigned drawableInstalled = 0;
        drawableInstalled += ptmc_install_owner_hook(metalDrawableClass, d1, (IMP)ptmc_drawable_present);
        drawableInstalled += ptmc_install_owner_hook(metalDrawableClass, d2, (IMP)ptmc_drawable_present_at_time);
        drawableInstalled += ptmc_install_owner_hook(metalDrawableClass, d3, (IMP)ptmc_drawable_present_after_duration);
        drawableInstalled += ptmc_install_owner_hook(metalDrawableClass, d4, (IMP)ptmc_drawable_present_with_options);
        if (drawableInstalled) {
            atomic_fetch_add_explicit(&ptmc_manager()->_presentHookCount, drawableInstalled, memory_order_relaxed);
            PTMC_LOG(@"base drawable hooks class=%@ installed=%u", NSStringFromClass(metalDrawableClass), drawableInstalled);
            installed += drawableInstalled;
        }
    }

    for (int i = 0; i < count; i++) {
        Class cls = classes[i];
        if (class_conformsToProtocol(cls, protocol) || (baseCmdBufferClass && ptmc_class_is_subclass_of(cls, baseCmdBufferClass))) {
            unsigned presentInstalled = 0;
            presentInstalled += ptmc_install_owner_hook(cls, s1, (IMP)ptmc_present_drawable);
            presentInstalled += ptmc_install_owner_hook(cls, s2, (IMP)ptmc_present_drawable_at_time);
            presentInstalled += ptmc_install_owner_hook(cls, s3, (IMP)ptmc_present_drawable_after_duration);
            if (presentInstalled) {
                atomic_fetch_add_explicit(&ptmc_manager()->_presentHookCount, presentInstalled, memory_order_relaxed);
                PTMC_LOG(@"command-buffer hooks class=%@ installed=%u", NSStringFromClass(cls), presentInstalled);
                installed += presentInstalled;
            }
        }
        if (class_conformsToProtocol(cls, drawableProtocol) || (metalDrawableClass && ptmc_class_is_subclass_of(cls, metalDrawableClass))) {
            unsigned drawableInstalled = 0;
            drawableInstalled += ptmc_install_owner_hook(cls, d1, (IMP)ptmc_drawable_present);
            drawableInstalled += ptmc_install_owner_hook(cls, d2, (IMP)ptmc_drawable_present_at_time);
            drawableInstalled += ptmc_install_owner_hook(cls, d3, (IMP)ptmc_drawable_present_after_duration);
            drawableInstalled += ptmc_install_owner_hook(cls, d4, (IMP)ptmc_drawable_present_with_options);
            if (drawableInstalled) {
                atomic_fetch_add_explicit(&ptmc_manager()->_presentHookCount, drawableInstalled, memory_order_relaxed);
                installed += drawableInstalled;
            }
        }
        if (ptmc_class_is_subclass_of(cls, metalLayerClass)) {
            installed += ptmc_install_owner_hook(cls, next, (IMP)ptmc_next_drawable);
            installed += ptmc_install_owner_hook(cls, setEDR, (IMP)ptmc_set_wants_edr);
            installed += ptmc_install_owner_hook(cls, setColorSpace, (IMP)ptmc_set_colorspace);
        }
    }
    free(classes);
    if (installed) PTMC_LOG(@"installed %u new Metal hooks (total=%u)", installed, atomic_load(&ptmc_hook_count));
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

static void ptmc_observe_and_normalize_layer(CAMetalLayer *layer, PTMCManager *manager) {
    BOOL forceSDR = ptmc_should_force_sdr(manager);
    if (!forceSDR) return;

    SEL edrGetter = NSSelectorFromString(@"wantsExtendedDynamicRangeContent");
    SEL edrSetter = NSSelectorFromString(@"setWantsExtendedDynamicRangeContent:");

    if ([layer respondsToSelector:edrGetter]) {
        BOOL edr = ((BOOL (*)(id, SEL))objc_msgSend)(layer, edrGetter);
        atomic_store_explicit(&manager->_edr, edr ? 1 : 0, memory_order_relaxed);
        if (edr && [layer respondsToSelector:edrSetter]) {
            ((void (*)(id, SEL, BOOL))objc_msgSend)(layer, edrSetter, NO);
            atomic_store_explicit(&manager->_edr, 0, memory_order_relaxed);
        }
    }

    CGColorSpaceRef sRGB = ptmc_srgb_colorspace();
    if (sRGB) {
        CGColorSpaceRef current = layer.colorspace;
        if (current != sRGB) {
            if (!current || !CFEqual(current, sRGB)) {
                layer.colorspace = sRGB;
            }
        }
    }
}

static id<CAMetalDrawable> ptmc_next_drawable(CAMetalLayer *self, SEL _cmd) {
    PTMCManager *manager = ptmc_manager();
    BOOL requested = atomic_load_explicit(&manager->_requested, memory_order_acquire);
    if (requested) {
        ptmc_observe_and_normalize_layer(self, manager);
        if (self.framebufferOnly) self.framebufferOnly = NO;
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
    }
    IMP original = ptmc_original_imp(self, _cmd);
    return original ? ((id<CAMetalDrawable> (*)(id, SEL))original)(self, _cmd) : nil;
}

static void ptmc_set_wants_edr(id self, SEL _cmd, BOOL wantsEDR) {
    PTMCManager *manager = ptmc_manager();
    BOOL effective = ptmc_should_force_sdr(manager) ? NO : wantsEDR;
    atomic_store_explicit(&manager->_edr, effective ? 1 : 0, memory_order_relaxed);
    IMP original = ptmc_original_imp(self, _cmd);
    if (original) ((void (*)(id, SEL, BOOL))original)(self, _cmd, effective);
}

static void ptmc_set_colorspace(id self, SEL _cmd, CGColorSpaceRef colorSpace) {
    PTMCManager *manager = ptmc_manager();
    CGColorSpaceRef effective = colorSpace;
    if (ptmc_should_force_sdr(manager) && ptmc_srgb_colorspace()) effective = ptmc_srgb_colorspace();
    manager.colorSpaceName = ptmc_colorspace_name(effective);
    IMP original = ptmc_original_imp(self, _cmd);
    if (original) ((void (*)(id, SEL, CGColorSpaceRef))original)(self, _cmd, effective);
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
    PTMC_LOG(@"runtime ready bundle=%@ codec=%@ fps=%ld bitrate=%ld buffers=%ld disableDisplaySync=%d forceSDR=%d spoofMaxFPS=%ld enabled=%d",
             bundleIdentifier ?: @"(unknown)", manager.codecName ?: @"hevc",
             (long)manager.fps, (long)manager.bitrate, (long)manager.bufferCount,
             manager.disableDisplaySync, manager.forceSDRDisplay, (long)ptmc_spoof_max_fps, enabled);
    ptmc_publish_status(manager, nil, @"ready", @"PTMC runtime loaded; frame hooks are dormant until Start");
    manager.heartbeatTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                                   dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_timer(manager.heartbeatTimer,
                              dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC),
                              NSEC_PER_SEC,
                              100 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(manager.heartbeatTimer, ^{
        BOOL requested = atomic_load_explicit(&manager->_requested, memory_order_acquire);
        PTMCSession *session = manager.session;
        NSString *phase = !requested ? @"ready" : (session ? (session.writer ? @"recording" : @"armed") : @"requested");
        NSString *message = !requested ? @"PTMC runtime heartbeat" : @"Capture runtime heartbeat";
        ptmc_publish_status(manager, session, phase, message);
    });
    dispatch_resume(manager.heartbeatTimer);
    if (enabled && autostart) [manager start];
}
