// Native synthetic Metal producer; not a translated iOS game or an A/V acceptance test.
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
@interface UIScreen : NSObject
@property(nonatomic, readonly) NSInteger maximumFramesPerSecond;
@end
@implementation UIScreen
- (NSInteger)maximumFramesPerSecond { return 120; }
@end
// Test-only stand-in for PlayLoader's account-home helper; tests never change the installation.
static void pt_copyCurrentUserHomeDirectory(char *buffer, size_t size) {
    strlcpy(buffer, NSHomeDirectory().fileSystemRepresentation, size);
}
#include "../PlayTools/MetalCapture/PTMetalCapture.inc"
int main(int argc, const char **argv) { @autoreleasepool {
    // Verify the stable hardware-only dictionary ABI against the SDK's exported macOS keys.
    assert([(__bridge NSString *)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder
        isEqualToString:@"EnableHardwareAcceleratedVideoEncoder"]);
    assert([(__bridge NSString *)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder
        isEqualToString:@"RequireHardwareAcceleratedVideoEncoder"]);
    if (argc != 3) return 2;
    double seconds = atof(argv[1]); NSInteger fps = atoi(argv[2]);
    if (seconds <= 0 || fps <= 0) return 2;
    [NSApplication sharedApplication];
    NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(100,100,960,540)
        styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    window.title = @"PTMC synthetic 4K test";
    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.device = MTLCreateSystemDefaultDevice(); layer.pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
    layer.framebufferOnly = NO; layer.drawableSize = CGSizeMake(3840,2160);
    layer.frame = window.contentView.bounds;
    window.contentView.wantsLayer = YES;
    CALayer *root = [CALayer layer]; root.frame = window.contentView.bounds;
    BOOL rootLayer = ptmc_env_bool("PTMC_SMOKE_ROOT_LAYER", NO);
    if (rootLayer) { window.contentView.layer = layer; }
    else { window.contentView.layer = root; [root addSublayer:layer]; }
    [window orderFront:nil];
    id<MTLCommandQueue> queue = [layer.device newCommandQueue];
    PTMCManager *manager = ptmc_manager();
    dispatch_async(manager.controlQueue, ^{ [manager start]; });
    BOOL direct = ptmc_env_bool("PTMC_SMOKE_DIRECT", NO);
    CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent()+seconds;
    BOOL stallWriter = ptmc_env_bool("PTMC_SMOKE_STALL_WRITER", NO);
    __block BOOL stalled = NO;
    __block BOOL previewStalled = NO;
    BOOL repeat = ptmc_env_bool("PTMC_SMOKE_REPEAT", NO);
    __block BOOL restarted = NO;
    __block NSUInteger frames=0; __block uint64_t maxDrawableUs=0;
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_main_queue());
    dispatch_source_set_timer(timer,dispatch_time(DISPATCH_TIME_NOW,100*NSEC_PER_MSEC),NSEC_PER_SEC/fps,0);
    dispatch_source_set_event_handler(timer, ^{ @autoreleasepool {
        if (CFAbsoluteTimeGetCurrent() >= deadline) {
            dispatch_source_cancel(timer);
            dispatch_async(manager.controlQueue, ^{ [manager stop]; }); return;
        }
        if (repeat && !restarted && CFAbsoluteTimeGetCurrent() >= deadline-seconds/2) {
            restarted = YES;
            dispatch_async(manager.controlQueue, ^{
                [manager stop];
                NSString *next = [[NSString stringWithUTF8String:getenv("PTMC_OUTPUT") ?: "/tmp/ptmc.mov"]
                    stringByAppendingString:@".second.mov"];
                setenv("PTMC_OUTPUT", next.UTF8String, 1);
                [manager start];
            });
        }
        if (stallWriter && !stalled && manager.session) {
            stalled = YES;
            dispatch_async(manager.session.writerQueue, ^{ usleep(2 * 1000000); });
        }
        if (ptmc_env_bool("PTMC_SMOKE_STALL_PREVIEW", NO) && !previewStalled && manager.preview &&
            CFAbsoluteTimeGetCurrent() >= deadline-seconds/2) {
            previewStalled = YES;
            dispatch_async(manager.preview.worker, ^{ usleep(2 * 1000000); });
        }
        uint64_t start=mach_continuous_time(); id<CAMetalDrawable> drawable=[layer nextDrawable];
        maxDrawableUs=MAX(maxDrawableUs,ptmc_ticks_to_ns(mach_continuous_time()-start)/1000);
        if (!drawable) return;
        id<MTLCommandBuffer> cb=[queue commandBuffer];
        MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture=drawable.texture;
        pass.colorAttachments[0].loadAction=MTLLoadActionClear; pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        pass.colorAttachments[0].clearColor=MTLClearColorMake((frames%256)/255.0,0.3,0.7,1);
        id<MTLRenderCommandEncoder> e=[cb renderCommandEncoderWithDescriptor:pass];
        [e endEncoding];
        if (direct) { [cb commit]; [drawable present]; }
        else { [cb presentDrawable:drawable]; [cb commit]; }
        frames++;
    }});
    dispatch_resume(timer);
    while (CFAbsoluteTimeGetCurrent() < deadline+5) {
        [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    }
    printf("synthetic frames=%lu target_fps=%ld maxNextDrawableUs=%llu displaySyncRestored=%d opacityRestored=%.1f hooksAfterStop=%llu\n",
        (unsigned long)frames,(long)fps,(unsigned long long)maxDrawableUs,
        layer.displaySyncEnabled,layer.opacity,(unsigned long long)atomic_load(&manager->_presentHookCount));
    NSDictionary *status = [NSDictionary dictionaryWithContentsOfFile:manager.statusPath];
    NSString *expectedPhase = stallWriter ? @"error" : @"finalized";
    BOOL valid = !atomic_load(&manager->_requested) && !manager.session &&
        atomic_load(&manager->_presentHookCount) == 0 && layer.displaySyncEnabled &&
        layer.opacity == 1.0f && !layer.framebufferOnly && [status[@"phase"] isEqual:expectedPhase];
    if (!stallWriter) valid = valid && [status[@"captured"] isEqual:status[@"encoded"]];
    if (stallWriter) valid = valid && [status[@"pendingWritesHighWater"] unsignedLongLongValue] <= 64 &&
        [status[@"pendingWrites"] unsignedLongLongValue] == 0 &&
        [status[@"pendingWriteBytes"] unsignedLongLongValue] == 0;
    // Detect duplicated logical presents without requiring precise timer scheduling under load.
    double activeSeconds = repeat ? seconds/2 : seconds;
    valid = valid && [status[@"presented"] doubleValue] <= activeSeconds*fps*1.25;
    if (ptmc_env_bool("PTMC_INDEPENDENT_PREVIEW", NO)) {
        NSLog(@"preview submitted=%@ presented=%@ skipped=%@ size=%@x%@ remainingLayers=%lu",
            status[@"previewSubmitted"], status[@"previewPresented"], status[@"previewSkipped"],
            status[@"previewWidth"], status[@"previewHeight"], (unsigned long)root.sublayers.count);
        valid = valid && [status[@"previewPresented"] unsignedLongLongValue] > 0;
        if (!rootLayer) valid = valid && root.sublayers.count == 1;
    }
    [window orderOut:nil]; return valid ? 0 : 1;
} }
