# Preview performance work, 2026-10-15

Status: experimental. The real-game visible-preview stall is **not resolved or attributed** by
these tests. The upstream bases remain PlayTools `fbc8880` and PlayCover `c8cf79f` (verified
against the upstream branch tips). Existing installed/released apps were not replaced.

## Changes

- Native-size sRGB HEVC conversion can read a non-sRGB view of the same drawable storage,
  avoiding hardware sRGB decoding followed by shader `pow()` re-encoding. A nonblocking,
  four-entry per-session cache bounds retained texture/view metadata. Unsupported views or
  cache contention/fullness fall back to the original kernel. No extra frame surface/copy,
  readback, resolution reduction, or FPS reduction is added. Scaled sampling keeps its existing
  linear-light interpolation; ProRes blits remain unchanged.
- GPU admission remains at the configured limit (normally 3); encoder-residency allocation
  is independently sized for a 75 ms budget, with a bounded experiment override.
- Compressed writer backlog is bounded by **64 samples and 64 MiB**, independently of the raw
  IOSurface ring. Overflow stops capture and finalizes the previously accepted video prefix;
  the result reports an error, not successful full-length capture. This limit does not bound
  VideoToolbox's or AVAssetWriter's internal memory. It prevents unbounded retained compressed
  CMSampleBuffers when disk/writer service falls behind.
- Callback handoff reserves pending writer ownership **before** dropping the final raw-input
  ownership count. Stop can no longer observe both counts as zero in that transition.
- Existing completed/partial output paths are preserved: writer creation refuses collisions,
  and finalize rename never removes an existing completed movie first.
- Writer errors stop only the corresponding session on the control queue. Start/Stop/config
  notifications, preparation, delayed hook discovery, and heartbeat operations are serialized.
  Discovery does not reinstall hooks after Stop and clamps concurrent class-list growth to
  allocated capacity. Nested present delegates capture once while every original is forwarded;
  the fast path opens only after hook installation. Resize-stop no longer
  performs hook restoration on the present thread. Duplicate Start does not reload active
  configuration. Duplicate Stop and idle heartbeats preserve the final outcome. Preparation
  failure restores hooks/layer state. Old-generation finalization cannot overwrite an active
  new generation's status.
- Audio host-time origin now uses the first admitted capture's present time, instead of a
  potentially rejected pool-admission attempt. This is a clock-origin correction, not proof
  of end-to-end A/V synchronization.
- Metal queues/encoders are labelled for Instruments. `droppedGPU` and `droppedResidency`
  partition `droppedPool`; `maxCaptureHookUs` measures capture admission/encoding CPU work,
  excluding layer preparation. `maxPresentCallUs` measures time inside the original present
  call, not nextDrawable, display latency, or scanout. Encoder queue and VT callback last/max
  latencies, writer byte count/high-water count, and native-view frame count are exported.

Apple documents that texture views share storage and sRGB/linear views do not require
`pixelFormatView` usage: [texture views](https://developer.apple.com/documentation/metal/mtltexture/maketextureview%28pixelformat%3A%29),
[usage guidance](https://developer.apple.com/documentation/metal/mtltextureusage/pixelformatview).
The GPU harness compares both paths rather than assuming they are bit-identical.

## Local evidence

Apple M5, 24 GiB, macOS 27.2 (26B5101f), Xcode 27.0 (27A266a).

`tests/ptmc_native_view.m` alternates reference/view conversion of a deterministic 3840x2160
mixed-chroma image. Five warmups, 60 timed commands per path:

| Kernel | Reference GPU us | View GPU us | Maximum 8-bit code difference |
| --- | ---: | ---: | ---: |
| NV12 | 429.83 | 414.61 | 1 |
| NV16 | 376.55 | 376.52 | 1 |

These are single-run synthetic timings. NV16 showed no meaningful improvement; NV12's small
change needs repeat measurement under game load. Differences of one quantization code are
expected when avoiding decode/re-encode approximation. No perceptual or game-image parity
claim is made. BGRA unorm and scaled paths are unchanged.

`tests/ptmc_runtime_smoke.m` compiles the actual runtime into a native AppKit producer with a
4K sRGB layer displayed in a 960x540 window. It uses a clear pass, not a representative game
GPU workload, and a native UIScreen stub only for the optional FPS hook. Each trial runs
8 seconds plus finalization; startup/preparation frames are intentionally not captured.

The six-slot baseline after the lifecycle fixes produced this second matched sequence.
The first sequence was drop-free; variability is a reason to retain the failure evidence.

| Target | Preview | Captured / written | Pool / encoder / late drops | Max nextDrawable us |
| --- | --- | ---: | --- | ---: |
| 60 | Visible, sync on | 469 / 469 | 0 / 0 / 0 | 1363 |
| 60 | Visible, sync off | 469 / 469 | 0 / 0 / 0 | 1061 |
| 60 | Transparent, safe presents | 469 / 469 | 0 / 0 / 0 | 1585 |
| 120 | Visible, sync on | 471 / 471 | 0 / 0 / 0 | 17013 |
| 120 | Visible, sync off | 781 / 781 | 116 / 0 / 0 | 7289 |
| 120 | Transparent, safe presents | 940 / 940 | 0 / 0 / 0 | 9044 |

All six finalized and restored layer state/hooks, but **zero encoder drops is not a pacing
pass**. Visible sync-on at target 120 ran near 60 presents/s with ~17 ms nextDrawable wait.
Sync-off sustained ~120 presents/s but lost 116 admissions to **encoder residency**, not GPU
admission; VT callback latency peaked at 74,146 us. Transparent safe presents sustained ~120
and did not exhaust the ring. Scheduling-only capture gaps and intentional sampling skips
are separate from these pool failures. Physical display scanout at 120 Hz is not established.

The runtime now preallocates `bufferCount + max(3, ceil(fps * 0.075))` raw slots: normally
8 at 60 fps and 12 at 120 fps, while GPU admission remains capped at 3. This reserves roughly
75 ms of VT residency plus GPU capacity without enlarging the GPU queue or allocating during
capture. `PTMC_ENCODER_RESIDENCY_BUFFERS=3` reproduces the six-slot baseline for comparison.
The reserve is fixed at session preparation, not an unbounded adaptive ring. `allocatedRawBytes`
and `encoderResidencyBuffers` expose the memory cost. At 4K120 HEVC the twelve buffers used
149,402,880 bytes on this Mac; ProRes BGRA is larger (~4 bytes/pixel/buffer, excluding padding).

A subsequent sync-off 4K120 run with twelve slots captured/wrote **935/935**, with zero GPU,
residency, encoder or late drops. VT callback maximum was 36,904 us. This supports the enlarged
bounded reserve as burst tolerance; the callback-latency variation means it is **not** a controlled
proof that ring size alone fixes the reported game symptom. The display-bound sync-on case
still needs a separate preview/producer-pacing solution or measured sync-off performance.

A separate direct-drawable 4K120 smoke exercises the primary hook route; the six-way table
uses command-buffer presentation. Neither route is a translated Unity workload.

Short native codec tests (3 seconds each, no audio) finalized and fully decoded using ffmpeg:
HEVC422 Long GOP 169 frames; HEVC422 All-I 167; ProRes LT 168; ProRes Standard 168;
ProRes HQ 167. ffprobe reports HEVC Rext `yuv422p10le` and the matching ProRes profiles
with `yuv422p10le`. All recorded captured/written counts match, with no encoder drops.
All 167 All-I frames are marked keyframes. Packet/frame PTS are monotonic for the six
preview MOVs, the direct-route MOV and the All-I MOV. Scheduling gaps remain (for example,
the initial visible 4K120 synthetic MOV has a 20 ms maximum gap); zero encoder drops is not
proof of uniform pacing. These tests do not establish long-run bitrate, audio, or game
texture/color correctness.

A 10-second **Metal System Trace** was successfully recorded/exported with client present,
command-buffer submission/completion, compositor, display-queue and GPU event schemas.
The tracing time limit terminated the synthetic process at the end, so this trace is not a clean-exit assertion. Exported GPU execution data contains 2,126 events;
the selected client-present and command-buffer-submission exports have no rows. Complete
CPU-to-GPU/present attribution was therefore **not obtained** in this trace. The trace container
and event schemas alone do not identify a WindowServer bottleneck. Raw system-wide trace data stays local. Both fork repositories have Issues disabled;
remaining acceptance work is tracked in this document and the Draft PRs.

The 2-second writer-stall injection reached the hard cap of 64 pending samples and stopped
without blocking the producer. It preserved a decodable 64-frame prefix, retained `phase=error`
and the overflow reason, drained pending counts/bytes to zero, and restored the layer/hooks.
It also exposed delayed class discovery reinstalling hooks after Stop; the revised control-queue
and idle guard eliminated that reinstallation in the repeated fault test.

Rapid Stop/Start initially selected the direct route during partial reinstallation and reported
~240 presents/s from a 120 fps producer, with only ~89–99 captured fps. Installing hooks before
opening the fast path and guarding nested present delegates restored the subsequent recording to
~120 presents/capture fps. Both recording files finalized and the last session's captured/written
counts matched. A later direct-route Stop/Start run also finalized both files with matching
counts, while sync-on limited its producer to ~60 fps; the target remains 120 fps in settings,
so that run is explicitly not a 120 fps throughput pass. This is a synthetic lifecycle reproduction, not attribution of the ミリシタ symptom.

## ミリシタ short real-process smoke

The installed ミリシタ v9.0.300 was also launched with the experimental framework selected
through a process-local `DYLD_FRAMEWORK_PATH`. `lsof` confirmed the mapped framework under
an isolated `PTMC Experiments` directory in the PlayCover container. Neither the game bundle
nor the common installed PlayTools framework was replaced. The game kept its existing
app-bundled AKInterface plugin, so this is capture-runtime testing, not a complete fresh
4.x host/plugin installation acceptance test. The locally built runtime source is byte-identical
to the final branch's `PTMetalCapture.inc`; subsequent commits change CI metadata/docs only.

MV: **スペードのQ ぴえぴえver.**, 3D高画質, source drawable **3840x2160 BGRA8Unorm**.
A ~20-second **visible, display-sync-on**, HEVC 4:2:2 Long GOP, target-60 trial captured and
wrote **1190/1190 frames**, with GPU/pool/residency/encoder/late drops all zero. The observed
hook route was **CAMetalDrawable**. The steady heartbeat window was 60.01 presents/captures
per second; the maximum observed present interval including startup was 33,234 us. VT callback
maximum was 95,536 us; the eight-slot ring drained successfully. Native sRGB view frame count
was zero because this game used unorm input: the sRGB optimization is not the explanation
for this result. The resulting 19.867 s MOV is HEVC Rext, `yuv422p10le`, 3840x2160 and fully
decodes without ffmpeg errors. This trial contains video only; app audio/A/V sync were not tested.

Two subsequent ~20-second sync-off/transparent smokes finalized 898/898 and 1196/1196 frames
with zero pool/encoder/late drops. They include different MV segments or menu transitions;
**they are not matched MV performance comparisons**. In particular the sync-off producer
was not continuously 60 fps. No claim of improved ミリシタ performance follows from comparing
those frame totals. Each trial disabled unsafe skip-present and restored the original runtime
configuration byte-for-byte after Stop. The executable, Info.plist and original config hashes
were checked for preservation. Test recording and isolated framework files remain separate
from production captures. Sustained 4K60, 4K120, matched replay A/B, audio and full-plugin
installation validation are still required.

## Reproduce

From PlayTools on macOS with full Xcode selected:

```sh
python3 tests/extract_ptmc_shader.py > /tmp/PTMCShader.metal
xcrun clang -fobjc-arc -O2 -framework Foundation -framework Metal tests/ptmc_native_view.m -o /tmp/ptmc_native_view
/tmp/ptmc_native_view /tmp/PTMCShader.metal
xcrun clang -fobjc-arc -O2 -Werror -Wno-deprecated-declarations -framework Cocoa -framework Metal -framework QuartzCore -framework CoreVideo -framework CoreMedia -framework VideoToolbox -framework AVFoundation tests/ptmc_runtime_smoke.m -o /tmp/ptmc_runtime_smoke
PTMC_ENABLE=1 PTMC_AUTOSTART=0 PTMC_FPS=60 PTMC_OUTPUT=/tmp/ptmc-new-trial.mov PTMC_STATUS_FILE=/tmp/ptmc-new-trial.plist /tmp/ptmc_runtime_smoke 8 60
```

Use a **new** output path for each trial. Set `PTMC_DISABLE_DISPLAY_SYNC=1` for unsynced
preview, `PTMC_SUPPRESS_DISPLAY=1` for transparent preview, `PTMC_SMOKE_DIRECT=1` for the
direct-drawable route, `PTMC_SMOKE_REPEAT=1` for immediate Stop/Start into distinct output files, or `PTMC_SMOKE_STALL_WRITER=1` to insert a test-only 2-second writer
queue stall. Never use unsafe `PTMC_SKIP_DISPLAY_PRESENT` as the baseline.

`python3 tests/ptmc_backpressure.py` compiles the production bounded-reservation helpers and
stresses their count/byte caps using eight threads, checks overflow arithmetic, and verifies
the callback's final-input ownership ordering. CI compiles both GPU harnesses; executing GPU
work requires a Mac with an accessible Metal device and, for the runtime harness, a GUI session.

CI artifact checkouts explicitly select the PR head SHA rather than GitHub's temporary
merge commit. `BUILD_INFO.txt` records the actual fork commit and the verified upstream
commit from `PTMC_UPSTREAM_COMMIT`; PlayCover's artifact records both pinned revisions.
The earlier merge artifact had an identical tree, but its revision label was not the PR head.

## Required next game/device work

Primary acceptance target: **ミリシタ**, bundle `jp.co.bandainamcoent.BNEI0310`.
Use the same MV segment, idols, costumes, graphics settings and audio mode throughout.

1. Use the experimental PlayCover artifact pinned to this PlayTools revision. Record identical
   game/replay/scene, resolution, codec and thermal conditions in randomized order: ordinary
   preview, unsynced preview, transparent safe presents. Repeat at 4K60 and 4K120 for >=60 s,
   then a >=30 minute sustained run. Record display mode, window size, game render settings,
   Metal HUD setting, GPU model and exact revisions. Do not change capture quality between modes.
2. Start recording through **PlayCover** so audio is included. In a second terminal run:
   `./scripts/ptmcctl.swift trace GAME_BUNDLE_ID 60 NEW-trial.jsonl visible-4k60`
   then `python3 scripts/ptmc_trace_summary.py *.jsonl`. Trace is observational and does not
   post status/config notifications. It captures heartbeat timestamps/age, generation, counters,
   configuration and thermal state. The summary separates generations and deduplicates heartbeat
   files; missing/stale samples and counter resets are explicit. Snapshot deltas are windowed
   throughput, not a frame-by-frame latency distribution or a final file count.
3. Attach Instruments Metal System Trace to the **game PID** for the same interval:
   `xcrun xctrace record --template 'Metal System Trace' --attach GAME_PID --time-limit 30s --output NEW-trial.trace`.
   Compare nextDrawable stalls, scheduled-to-GPU-start time, PTMC encoder intervals, drawable
   lifetime/display queue, WindowServer/compositor load and VT callback latency. A short original
   present call does not exclude a stall in nextDrawable or subsequent display composition.
4. Check MOV packet PTS monotonicity/intervals, decoded frame counts, HEVC 4:2:2/ProRes profile,
   A/V start offset/drift and delivered PCM continuity. Repeat rapid start/stop/re-record,
   resize, encoder initialization failure, and low-space/writer-stall recovery. Do not fill a
   production volume to test ENOSPC. Preserve partial video and audio sidecars for diagnosis.
5. If direct-drawable capture is the bottleneck, map the actual producer command buffer before
   proposing queue decoupling. The current direct route submits a separate conversion command;
   the fallback appends conversion to the producer. **No explicit producer event is available
   on the direct route.** Cross-queue ownership/synchronization must be proven in a GPU capture
   before moving work or recycling the source earlier. Apple [event ordering](https://developer.apple.com/documentation/metal/mtlcommandbuffer/encodesignalevent%28_%3Avalue%3A%29)
   provides a candidate GPU dependency once the producer can be identified. Do not add a CPU
   wait or assume command submission order across queues is a dependency.
6. Only if traces show visible composition dominates, prototype a separate preview fed from a
   bounded GPU downsample surface and a latest-frame mailbox. Preview should drop stale preview
   frames independently of recording; original safe presents must continue for drawable recycling.
   Measure the added downsample/display pass against transparent-layer suppression, source-surface
   lifetime and input/window behavior before enabling it. This independent preview is **not yet
   implemented**, and the existing sync toggle is not a substitute.
