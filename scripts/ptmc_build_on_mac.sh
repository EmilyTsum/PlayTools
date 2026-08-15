#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
[[ $(uname -s) == Darwin ]] || { echo "ptmc_build_on_mac.sh requires macOS" >&2; exit 2; }
for cmd in git xcodebuild xcrun codesign ditto; do command -v "$cmd" >/dev/null || { echo "missing: $cmd" >&2; exit 2; }; done
WORK=${PTMC_BUILD_DIR:-$ROOT/.ptmc-build}
PRODUCTS="$WORK/Products"
OBJ_AK="$WORK/Intermediates-AK"
OBJ_PT="$WORK/Intermediates-PT"
DIST="$ROOT/dist"
rm -rf "$WORK" "$DIST"
mkdir -p "$WORK/bin" "$DIST"
if ! command -v swiftlint >/dev/null 2>&1; then
  printf '#!/bin/sh\nexit 0\n' > "$WORK/bin/swiftlint"
  chmod +x "$WORK/bin/swiftlint"
  export PATH="$WORK/bin:$PATH"
fi
COMMON_SIGN=(CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="")
# AKInterface imports AppKit and must be built as macOS, even though PlayTools itself is iPhoneOS.
xcodebuild \
  -project "$ROOT/PlayTools.xcodeproj" \
  -target AKInterface \
  -configuration Release \
  -sdk macosx \
  SYMROOT="$PRODUCTS" OBJROOT="$OBJ_AK" \
  "${COMMON_SIGN[@]}" \
  build | tee "$WORK/xcodebuild-akinterface.log"
AK=$(find "$PRODUCTS" -type d -name AKInterface.bundle -print -quit)
[[ -n "$AK" ]] || { echo "AKInterface.bundle not found after macOS build" >&2; exit 1; }
mkdir -p "$PRODUCTS/Release-iphoneos"
rm -rf "$PRODUCTS/Release-iphoneos/AKInterface.bundle"
cp -R "$AK" "$PRODUCTS/Release-iphoneos/AKInterface.bundle"
LDFLAGS='$(inherited) -framework Metal -framework QuartzCore -framework CoreVideo -framework CoreMedia -framework VideoToolbox -framework AVFoundation'
xcodebuild \
  -project "$ROOT/PlayTools.xcodeproj" \
  -target PlayTools \
  -configuration Release \
  -sdk iphoneos \
  SYMROOT="$PRODUCTS" OBJROOT="$OBJ_PT" \
  "${COMMON_SIGN[@]}" \
  OTHER_LDFLAGS="$LDFLAGS" \
  build | tee "$WORK/xcodebuild-playtools.log"
FRAMEWORK=$(find "$PRODUCTS" -path '*Release-iphoneos/PlayTools.framework' -type d -print -quit)
[[ -n "$FRAMEWORK" ]] || { echo "PlayTools.framework not found" >&2; exit 1; }
cp -R "$FRAMEWORK" "$DIST/PlayTools.framework"
BIN="$DIST/PlayTools.framework/PlayTools"
MACOS_SDK=$(xcrun --sdk macosx --show-sdk-version)
MIN_CATALYST=${PTMC_CATALYST_MIN:-12.0}
TMPBIN="$BIN.vtool"
xcrun vtool -set-build-version maccatalyst "$MIN_CATALYST" "$MACOS_SDK" -replace -output "$TMPBIN" "$BIN"
mv "$TMPBIN" "$BIN"
codesign --force --deep --sign - "$DIST/PlayTools.framework"
{
  echo "fork_commit=$(git -C "$ROOT" rev-parse HEAD)"
  echo "upstream_commit=$(git -C "$ROOT" rev-parse upstream/master 2>/dev/null || true)"
  echo "macos_sdk=$MACOS_SDK"
  echo "catalyst_min=$MIN_CATALYST"
  xcrun vtool -show-build "$BIN"
  codesign -dv --verbose=2 "$DIST/PlayTools.framework" 2>&1
} | tee "$DIST/BUILD_INFO.txt"
VERSION=${PTMC_VERSION:-0.1.0}
PACKAGE="$WORK/package"
mkdir -p "$PACKAGE/scripts"
cp -R "$DIST/PlayTools.framework" "$PACKAGE/"
for f in ptmc_control_capture.sh ptmc_enable_capture_env.sh ptmc_disable_capture_env.sh ptmc_install_into_playcover.sh ptmc_rollback_playcover.sh ptmc_install_playcover_nightly.sh; do
  cp "$ROOT/scripts/$f" "$PACKAGE/scripts/$f"
done
cp "$DIST/BUILD_INFO.txt" "$PACKAGE/"
ditto -c -k --sequesterRsrc --keepParent "$DIST/PlayTools.framework" "$DIST/PlayTools-MetalCapture.framework.zip"
(cd "$PACKAGE" && ditto -c -k --sequesterRsrc . "$DIST/playtools-metal-capture-${VERSION}.zip")
echo "Built: $DIST/PlayTools.framework"
