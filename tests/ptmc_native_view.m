// Synthetic Metal correctness/performance check; CPU upload/readback is test-only.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdlib.h>
#include <stdio.h>
static id<MTLTexture> texture(id<MTLDevice> d, MTLPixelFormat f, NSUInteger w, NSUInteger h) {
    MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    desc.storageMode = MTLStorageModeShared;
    desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    return [d newTextureWithDescriptor:desc];
}
int main(int argc, const char **argv) { @autoreleasepool {
    if (argc != 2) return 2;
    id<MTLDevice> d = MTLCreateSystemDefaultDevice();
    if (!d) { fprintf(stderr, "No Metal device\n"); return 2; }
    NSError *error = nil;
    NSString *source = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:&error];
    id<MTLLibrary> lib = [d newLibraryWithSource:source options:nil error:&error];
    if (!lib) { NSLog(@"%@", error); return 1; }
    const NSUInteger w = 3840, h = 2160;
    id<MTLTexture> src = texture(d, MTLPixelFormatBGRA8Unorm_sRGB, w, h);
    uint8_t *pixels = malloc(w*h*4);
    for (NSUInteger i=0; i<w*h*4; i++) pixels[i] = (uint8_t)((i*73 + i/17) & 255);
    [src replaceRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0 withBytes:pixels bytesPerRow:w*4];
    free(pixels);
    id<MTLTexture> view = [src newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm];
    if (!view) { fprintf(stderr, "View unavailable\n"); return 1; }
    id<MTLCommandQueue> queue = [d newCommandQueue];
    for (int codec=0; codec<2; codec++) {
        NSString *kernel = codec ? @"ptmc_bgra_to_nv16" : @"ptmc_bgra_to_nv12";
        id<MTLComputePipelineState> pipeline = [d newComputePipelineStateWithFunction:[lib newFunctionWithName:kernel] error:&error];
        if (!pipeline) { NSLog(@"%@", error); return 1; }
        NSUInteger uvHeight = codec ? h : h/2;
        id<MTLTexture> y[2] = { texture(d,MTLPixelFormatR8Unorm,w,h), texture(d,MTLPixelFormatR8Unorm,w,h) };
        id<MTLTexture> uv[2] = { texture(d,MTLPixelFormatRG8Unorm,w/2,uvHeight), texture(d,MTLPixelFormatRG8Unorm,w/2,uvHeight) };
        double gpu[2] = {0,0};
        for (int frame=0; frame<65; frame++) for (int mode=0; mode<2; mode++) {
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
            [e setComputePipelineState:pipeline];
            [e setTexture:mode ? view : src atIndex:0];
            [e setTexture:y[mode] atIndex:1]; [e setTexture:uv[mode] atIndex:2];
            uint32_t srgb = mode ? 0 : 1; [e setBytes:&srgb length:4 atIndex:0];
            NSUInteger tw = pipeline.threadExecutionWidth;
            [e dispatchThreads:MTLSizeMake(w/2,uvHeight,1)
                threadsPerThreadgroup:MTLSizeMake(tw,MIN(16,pipeline.maxTotalThreadsPerThreadgroup/tw),1)];
            [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
            if (cb.status != MTLCommandBufferStatusCompleted) { NSLog(@"%@",cb.error); return 1; }
            if (frame>=5) gpu[mode] += (cb.GPUEndTime-cb.GPUStartTime)*1e6;
        }
        unsigned maxDelta=0; NSUInteger changed=0;
        for (int plane=0; plane<2; plane++) {
            NSUInteger height = plane ? uvHeight : h;
            uint8_t *a = malloc(w*height), *b = malloc(w*height);
            [(plane ? uv[0] : y[0]) getBytes:a bytesPerRow:w fromRegion:MTLRegionMake2D(0,0,plane?w/2:w,height) mipmapLevel:0];
            [(plane ? uv[1] : y[1]) getBytes:b bytesPerRow:w fromRegion:MTLRegionMake2D(0,0,plane?w/2:w,height) mipmapLevel:0];
            for (NSUInteger i=0;i<w*height;i++) { unsigned delta=abs((int)a[i]-(int)b[i]); maxDelta=MAX(maxDelta,delta); changed+=delta>0; }
            free(a); free(b);
        }
        printf("%s device=%s 3840x2160 reference_gpu_us=%.2f view_gpu_us=%.2f max_code_delta=%u changed=%lu\n",
               kernel.UTF8String,d.name.UTF8String,gpu[0]/60,gpu[1]/60,maxDelta,(unsigned long)changed);
        if (maxDelta>1) return 1;
    }
    return 0;
} }
