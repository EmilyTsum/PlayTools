# Testing

Linux/static checks:

* fresh PlayTools clone -> apply -> apply again -> verify -> unapply -> unapply -> clean git status
* no private Metal implementation class names are embedded
* runtime full-class scans occur only during bounded discovery events, never every frame
* no `getBytes:` capture readback

GitHub Actions macOS build checks Xcode compilation, warnings/errors, Catalyst build-version metadata, linked frameworks, and ad-hoc signature verification.

Mac runtime acceptance:

1. Use the latest successful PlayCover nightly/develop build.
2. Install the framework and enable PTMC environment; restart PlayCover/game.
3. `ptmc_control_capture.sh status` should show hooks and actual drawable dimensions.
4. For a patched 120-fps game, compare PTMC `present` to WindowServer's composited rate.
5. Record, stop, then verify with `ffprobe`: HEVC, intended drawable dimensions (target 3840x2160), and ~120 fps timing.
6. Watch `dropped_pool`, `dropped_encoder`, `dropped_late`, thermals, and game frame pacing. Increase ring size only if pool drops are material and memory/thermal budget allows.
