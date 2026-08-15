# Known limitations

* Runtime testing still requires an Apple Silicon Mac with PlayCover and a real game workload. CI validates Apple SDK compilation, not a live CAMetalDrawable stream.
* A mid-recording drawable-size change stops recording safely. Restart capture to create a new session/file for the new size; PTMC does not splice resolution changes into one MOV.
* Initial format support is 8-bit BGRA/BGRA-sRGB -> 8-bit video-range NV12 HEVC Main. HDR, 10-bit P010, ProRes, and non-BGRA drawable formats are future work.
* Conversion is appended to the game's command buffer for ordering correctness. This adds GPU work to the render queue; it avoids CPU/GPU synchronization and a second-queue shared-event dependency, but should still be measured on the M5 Air thermal envelope.
* `PTMC_SPOOF_MAX_FPS` changes only `UIScreen.maximumFramesPerSecond`. A game that hardcodes another pacing API may still remain at 60 fps.

* Installing a modified framework necessarily invalidates upstream PlayCover's original notarized code signature. The installer therefore ad-hoc re-signs the app and removes quarantine metadata; this is a development/custom-build workflow, not a notarized distribution path.
