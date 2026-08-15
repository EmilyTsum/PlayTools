# PTMC — current PlayTools notes

This is the only PTMC document in PlayTools that should be read by default. The cross-repository source of truth is `EmilyTsum/PlayCover:ptmc-nightly/PTMC_INTEGRATION.md`; it owns the exact PlayTools pin, accepted/deferred fork patches, release checklist, and host-side behavior.

Historical PTMC design/failure documents were removed from the working tree after their durable findings were folded into this file and the PlayCover integration document. Git history retains the original reports if a regression requires them.

## Durable real-device findings

Real Apple Silicon testing established the following facts:

- Environment propagation, PlayTools injection, targeted Darwin notifications, sandbox access to the PlayCover capture directory, and bundled/system framework freshness were all proven functional during the original capture failure investigation.
- Apple GPU command-buffer subclasses can inherit `presentDrawable:*` from `_MTLCommandBuffer` even when the base class itself does not report `MTLCommandBuffer` protocol conformance. Hook discovery therefore cannot rely on `class_conformsToProtocol` plus each concrete class's own method list.
- The decisive Unity path observed later was direct `CAMetalDrawable.present*`, so direct drawable hooks are the preferred path; command-buffer interception is a compatibility fallback.
- Repeated per-frame CAMetalLayer state writes caused measurable pacing disturbance. Capture-layer state is now prepared once when needed and restored on Stop.
- A 120 Hz source captured at 60 fps should intentionally report roughly 60 sampling skips/s. `samplingSkipped` is not an encoder-drop counter.
- A deeper raw ring can worsen game pacing at 4K by allowing more capture work to stay in flight. The normal default is 3 slots, plus one cooldown-limited preallocated emergency slot for isolated latency spikes.

## Current capture path

`CAMetalDrawable.texture -> Metal GPU convert/copy -> preallocated IOSurface CVPixelBuffer -> VideoToolbox -> compressed writer queue -> MOV`

Important invariants:

- no ScreenCaptureKit video;
- no full-frame CPU readback or CPU colorspace conversion;
- no per-frame raw-buffer allocation;
- no per-frame VideoToolbox callback-context allocation;
- no per-frame Objective-C class scan;
- no VideoToolbox/disk wait on the game render/present thread;
- original present IMPs are restored when recording stops.

HEVC converts/scales BGRA to video-range BT.709 NV12 on the GPU. ProRes 422 LT / 422 / 422 HQ use IOSurface-backed BGRA; native-size ProRes is a Metal blit and scaled ProRes is a GPU compute copy. Supported source drawable formats remain BGRA8Unorm and BGRA8Unorm_sRGB.

## Runtime hooks and presentation

Frame hooks are installed only while recording. Direct `CAMetalDrawable` hooks cover `present`, `presentAtTime:`, `presentAfterMinimumDuration:`, and the observed `presentWithOptions:` selector. Command-buffer `presentDrawable:*` hooks are installed only as fallback when the direct path is unavailable.

Optional display suppression makes the active CAMetalLayer transparent while still presenting. Optional full present bypass is deliberately unsafe/experimental. Framebuffer-only, EDR, colorspace, display-sync, opacity, and HUD state are restored on Stop.

Metal HUD recording defaults to excluded. PTMC uses the runtime `developerHUDProperties` selector only when present, temporarily disables the HUD for the active layer, resists later property changes while recording, and restores the original dictionary on Stop. This runtime/private-ish behavior needs real-device validation after macOS changes.

## Performance / encoder behavior

VideoToolbox submission and AVAssetWriter work are isolated on separate serial queues. Raw IOSurface slots are released from the VideoToolbox output callback before writer work. The callback context is stored in each preallocated slot instead of allocating an Objective-C frame object every frame.

The sampler tracks present cadence. Near the target capture rate it accepts each present to avoid 60 Hz jitter turning into artificial holes. For clearly faster sources it uses deadline sampling. Runtime status exposes present/capture/encode FPS, intentional sampling skips/s, actual drop categories, in-flight/pending counts, and emergency-slot use.

The last user-observed item still requiring hardware proof is rare ~1–2 frame loss around 4K60. CI cannot validate that workload.

## External fixes carried by this fork

In addition to PTMC, the current branch intentionally carries reviewed upstream patches for:

- preserving the system Metal HUD menu across UIKit menu rebuilds (PlayTools PR #229);
- resolving the actual account home directory rather than a sandbox HOME/container path (PR #233);
- macOS 26 microphone permission synchronization via AVAudioApplication with legacy fallback (first commit of PR #232; its later path workaround is superseded by #233);
- M5 iPad Pro and iPhone 17 Pro Max board IDs (PR #231).

Do not import unrelated forks wholesale. In particular, background keepalive patches that suppress lifecycle events/use silent audio have known behavioral tradeoffs and are intentionally not part of PTMC.

## Tests and build

Linux/static regression checks:

```sh
./tests/ptmc_static.sh
python3 tests/ptmc_scheduler_model.py
./tests/ptmc_patch_roundtrip.sh
./Scripts/verify-macos26-microphone.sh
```

The patch-roundtrip test exists to keep the historical overlay/rebase mechanism reproducible; the active product is the committed `metal-capture` branch, not a generated overlay build.

macOS CI additionally compiles the Metal shader, builds/validates the framework, verifies its Catalyst metadata/signature, and uploads an artifact. Physical game FPS, rare capture drops, HUD exclusion, and A/V sync are real-device acceptance tests, not CI claims.
