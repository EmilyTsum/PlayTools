# Independent preview experiment (2026-10-16)

The user reports that the same MLTD workload records at 60 fps without drops when
screen output is suppressed, while visible output stutters. The supplied visible
recording contains 1523 frames over a 36.293333-second PTS span (41.94 fps), with
14 intervals over 50 ms and a maximum interval of 296.667 ms. The runtime's maximum
present gap is 296.623 ms, while GPU/residency/encoder/late drop counters are zero.
This establishes uneven arrival at the capture hook; it does not identify whether
WindowServer, drawable acquisition, game GPU work, or competing memory traffic is
responsible. Increasing encoder buffers cannot repair frames never presented.

## New path

`independentPreview` / `PTMC_INDEPENDENT_PREVIEW=1` opts into a separate display.
Recording format, dimensions, bitrate and target cadence remain unchanged.

- Three private BGRA textures, each at most 1280x720, allocated once at preparation
  (at most 10.55 MiB total). Aspect ratio is preserved and there is no upscaling.
- At most 60 snapshots/second, downsampled on the existing capture command buffer.
  No CPU readback, full-resolution copy, or per-frame surface allocation.
- GPU completion hands off only the small texture to a separate serial worker and
  Metal queue. That worker obtains its own CAMetalDrawable. Game/capture threads
  never obtain or wait for preview drawables.
- A busy snapshot is released only after its display blit completes. Admission is
  nonblocking, at most three handoffs can exist, and superseded snapshots are skipped.
- Source drawables still receive their original present calls and recycle normally.
  After preview starts, its source layer is transparent so the compositor can cull
  the full-resolution surface. `skipDisplayPresent` is not enabled.
- A main-thread watchdog restores source visibility after 250 ms without completed
  preview work. Stop removes the preview, restores opacity and cancels the timer.
- A nonactivating AppKit NSPanel owns the preview CAMetalLayer. The iOS-built runtime
  resolves public AppKit classes already loaded by PlayCover/AKInterface. It does
  not modify the game's UIKit view/controller tree. Missing AppKit falls back to
  native display. The panel is display-only; Stop removes it and restores the game.
- Preview presents are explicitly excluded from the capture hooks to prevent recursion,
  presentation-path selection and incorrect source-size-change shutdown.

This is an opt-in experiment. The 250 ms watchdog measures GPU completion rather
than physical scanout and cannot detect every compositor/occlusion failure. The
original direct-drawable capture queue's producer ordering has not been redesigned.
The extra downsample adds GPU traffic; only matched game tests can establish benefit.
The panel does not forward touch input. Use recording shortcuts to stop and restore
the original game view before navigating. It primarily targets MV viewing.

## Initial validation

Native 4K60 clear-pass producer with an independent panel: 465 captured/encoded,
zero pool/encoder drops; 465 preview submissions/presents, source max nextDrawable
call 0.695 ms. Source
sync/opacity and hook restoration pass after Stop.

Injecting a two-second preview-worker stall mid-recording: 465 captured/encoded,
zero pool/encoder drops, 345 preview presents and 120 preview skips. Present cadence
remained 60 fps in the final window. These are synthetic, short runs, not MLTD proof.

MLTD view integration and matched MV measurements remain under active verification.
Do not infer visible smoothness from preview completion counters alone.

A UIKit overlay was rejected: it produced a blank game window after transitions
even when GPU preview counters advanced. Visual verification and restoration are
required; the shipped experiment uses a separate AppKit window instead.
