#!/usr/bin/env bash
set -euo pipefail
APP=${PLAYCOVER_APP:-/Applications/PlayCover.app}
DEST="$APP/Contents/Frameworks/PlayTools.framework"
BACKUP_ROOT=${PTMC_BACKUP_DIR:-$HOME/Library/Application Support/PlayTools-MetalCapture/backups}
if pgrep -x PlayCover >/dev/null 2>&1; then echo "Quit PlayCover first." >&2; exit 3; fi
BACKUP=${1:-$(ls -1dt "$BACKUP_ROOT"/PlayTools.framework.backup-* 2>/dev/null | head -1 || true)}
[[ -n "$BACKUP" && -d "$BACKUP" ]] || { echo "No PlayTools backup found under: $BACKUP_ROOT" >&2; exit 2; }
FAILED="$BACKUP_ROOT/PlayTools.framework.failed-$(date +%Y%m%d-%H%M%S)"
[[ -e "$DEST" ]] && ditto "$DEST" "$FAILED"
rm -rf "$DEST"
ditto "$BACKUP" "$DEST"
codesign --force --deep --sign - "$DEST"
codesign --force --deep --sign - "$APP"
xattr -dr com.apple.quarantine "$APP" 2>/dev/null || true
codesign --verify --deep --strict "$APP"
echo "Restored: $BACKUP"
echo "Replaced PTMC framework kept at: $FAILED"
