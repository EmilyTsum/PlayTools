# macOS build and nightly PlayCover

`scripts/build_on_mac.sh` performs the full reproducible path: clone the pinned/current PlayTools upstream, apply/verify the overlay, build the iPhoneOS PlayTools target unsigned, convert the Mach-O build-version platform to Mac Catalyst with `vtool`, ad-hoc sign it, and emit `dist/PlayTools.framework` plus zip artifacts.

The Catalyst minimum is 12.0 by default, matching current PlayCover `develop`; the SDK field is taken from the installed macOS SDK rather than hard-coded to an old Xcode version.

`scripts/ptmc_install_playcover_nightly.sh` uses `gh` to select the latest successful `PlayCover/PlayCover` `Build nightly release` run, downloads its DMG artifact, backs up an existing `/Applications/PlayCover.app`, and installs the nightly. PTMC does not target the stale stable 3.1.0 line.

The final framework is rebundled into the same `Versions/A` deep-framework structure used by current PlayCover `develop`, then ad-hoc signed. Installation keeps backups outside the app bundle under `~/Library/Application Support/PlayTools-MetalCapture/backups`, replaces the nested framework atomically enough for an offline app, and re-signs the outer PlayCover app because replacing a nested signed framework invalidates the original app seal.
