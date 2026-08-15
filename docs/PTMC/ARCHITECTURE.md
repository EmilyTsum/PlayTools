# Architecture

PlayTools is injected into the iOS game and runs in the same process. PTMC therefore does not need an IPA-side texture IPC bridge or a PlayCover-side raw-frame receiver.

The capture path is:

1. `CAMetalLayer.nextDrawable` is swizzled only when `PTMC_ENABLE=1`. While capture is requested it sets `framebufferOnly = NO` before drawable creation. `displaySyncEnabled = NO` is an explicit opt-in (`PTMC_DISABLE_DISPLAY_SYNC=1`).
2. Concrete Objective-C classes conforming to `MTLCommandBuffer` are discovered at startup and a bounded set of delayed retries. PTMC hooks only selectors present in each class's **own** method list, never inherited methods.
3. `presentDrawable:`, `presentDrawable:atTime:`, and `presentDrawable:afterMinimumDuration:` read `drawable.texture`, record present metrics, and append a Metal compute encoder to that same command buffer before calling the original IMP. Using the game's command buffer preserves GPU ordering: conversion runs after the game's preceding render encoders and before the drawable is presented.
4. A fixed 3-16 slot ring (default 6) owns IOSurface-backed bi-planar video-range NV12 `CVPixelBuffer`s. Each slot's R8 Y texture and RG8 UV texture are created once through `CVMetalTextureCache`; no full-frame buffer or Metal plane texture is allocated per captured frame.
5. A Metal kernel converts supported `BGRA8Unorm` / `BGRA8Unorm_sRGB` sources to BT.709 video-range NV12. Chroma is 2x2 averaged for 4:2:0.
6. After the game command buffer completes, the NV12 pixel buffer goes to a `VTCompressionSession` configured for HEVC and `kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder=true`.
7. Encoded `CMSampleBuffer`s are muxed without re-encoding through an `AVAssetWriterInput` passthrough input into a QuickTime `.mov`.

The render thread never waits for VideoToolbox or disk. If all fixed slots are busy, capture drops the frame and leaves game rendering alone.
