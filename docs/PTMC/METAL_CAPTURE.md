# Metal capture details

PTMC captures `CAMetalDrawable.texture` rather than ScreenCaptureKit/WindowServer output. The authoritative dimensions are `texture.width` and `texture.height`, not logical window size. The latest observed dimensions and pixel format are logged.

`framebufferOnly` is changed only while recording is requested. Unsupported drawable formats are counted and skipped; the initial implementation supports `MTLPixelFormatBGRA8Unorm` and `MTLPixelFormatBGRA8Unorm_sRGB` and does not crash on other formats.

The compute conversion writes directly into IOSurface-backed NV12 planes. It never calls `MTLTexture getBytes:` and never performs CPU color conversion. For an sRGB Metal texture, sampling produces linear values, so the shader reapplies the sRGB transfer function before the BT.709 matrix. NV12 uses video range with BT.709 primaries, transfer, and matrix attachments.

Drawable lifetime is not extended beyond the game command buffer completion. PTMC retains only the fixed destination slot/session state needed by the GPU and encoder.
