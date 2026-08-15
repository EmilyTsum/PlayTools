#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FRAMEWORK=${PTMC_FRAMEWORK:-$ROOT/dist/PlayTools.framework}
DEST=${PLAYCOVER_APP:-/Applications/PlayCover.app}/Contents/Frameworks/PlayTools.framework
[[ -d "$FRAMEWORK" ]] || { echo "framework not found: $FRAMEWORK" >&2; exit 2; }
[[ -d "${PLAYCOVER_APP:-/Applications/PlayCover.app}" ]] || { echo "PlayCover.app not found" >&2; exit 2; }
if pgrep -x PlayCover >/dev/null 2>&1; then
  echo "PlayCover is running. Quit it before installing to avoid replacing a loaded framework." >&2
  exit 3
fi
STAMP=$(date +%Y%m%d-%H%M%S)
BACKUP="$DEST.backup-$STAMP"
if [[ -e "$DEST" ]]; then
  cp -R "$DEST" "$BACKUP"
  echo "Backup: $BACKUP"
fi
TMP="$DEST.ptmc-new-$$"
rm -rf "$TMP"
cp -R "$FRAMEWORK" "$TMP"
mv "$TMP" "$DEST"
codesign --verify --deep --strict "$DEST"
echo "Installed PTMC PlayTools into PlayCover nightly path: $DEST"
