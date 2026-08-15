# Frame pacing

The first diagnostic is PTMC's `present=... fps`, not the Metal HUD's `Composited ... 60 Hz` line.

* `present ~= 120/sec` while WindowServer is 60 Hz proves that the game is producing ~120 pre-compositor frames. Capture/encoder/PTS are then the focus.
* `present ~= 60/sec` means the limit is upstream of texture capture. Check the game's existing 120-fps patch, `UIScreen.maximumFramesPerSecond`, `CADisplayLink` preferred rates/ranges, `CAMetalLayer.displaySyncEnabled`, `presentDrawable:afterMinimumDuration:`, and Unity target-frame-rate logic.

PTMC has two opt-in diagnostics for the second case: `PTMC_DISABLE_DISPLAY_SYNC=1` and `PTMC_SPOOF_MAX_FPS=120`. They are not forced by default because frame pacing is game-dependent.

PTS uses `mach_continuous_time` converted to a 60,000-unit timescale and is monotonically increased if two presents collapse onto the same tick unit. No wall-clock timestamp is used.
