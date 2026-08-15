# Updating PlayTools upstream

1. Fetch `PlayCover/PlayTools` and inspect current `master`/release notes.
2. Run `scripts/apply_patch.py <fresh-upstream>`. It fails loudly if the `PlayLoader.m` anchor changed rather than guessing a patch location.
3. Run the patch idempotency test and `scripts/build_on_mac.sh` (or CI with `PTMC_UPSTREAM_REF=master`).
4. Review compiler warnings, runtime-hook method signatures, PlayLoader initialization changes, and linked system frameworks.
5. After a successful macOS CI build, update `UPSTREAM_COMMIT` to the exact tested SHA and document material changes.

The overlay deliberately avoids modifying `project.pbxproj`; system-framework linker flags are supplied by the build script. This keeps rebases small.
