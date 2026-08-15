#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FRAMEWORK=${PTMC_FRAMEWORK:-$ROOT/dist/PlayTools.framework}
APP=${PLAYCOVER_APP:-/Applications/PlayCover.app}
DEST="$APP/Contents/Frameworks/PlayTools.framework"
BACKUP_ROOT=${PTMC_BACKUP_DIR:-$HOME/Library/Application Support/PlayTools-MetalCapture/backups}
[[ -d "$FRAMEWORK" ]] || { echo "framework not found: $FRAMEWORK" >&2; exit 2; }
[[ -d "$APP" ]] || { echo "PlayCover.app not found: $APP" >&2; exit 2; }
if pgrep -x PlayCover >/dev/null 2>&1; then
  echo "PlayCover is running. Quit it before installing." >&2
  exit 3
fi
STAMP=$(date +%Y%m%d-%H%M%S)
mkdir -p "$BACKUP_ROOT"
if [[ -e "$DEST" ]]; then
  BACKUP="$BACKUP_ROOT/PlayTools.framework.backup-$STAMP"
  ditto "$DEST" "$BACKUP"
  echo "Backup: $BACKUP"
fi
TMP="$APP/Contents/Frameworks/.PlayTools.framework.ptmc-new-$$"
rm -rf "$TMP"
ditto "$FRAMEWORK" "$TMP"
rm -rf "$DEST"
mv "$TMP" "$DEST"
codesign --force --deep --sign - "$DEST"
codesign --force --deep --sign - "$APP"
xattr -dr com.apple.quarantine "$APP" 2>/dev/null || true
codesign --verify --deep --strict "$APP"
echo "Installed PTMC PlayTools: $DEST"
