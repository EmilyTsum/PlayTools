#!/usr/bin/env bash
set -euo pipefail
APP=${PLAYCOVER_APP:-/Applications/PlayCover.app}
DEST="$APP/Contents/Frameworks/PlayTools.framework"
if pgrep -x PlayCover >/dev/null 2>&1; then echo "Quit PlayCover first." >&2; exit 3; fi
BACKUP=${1:-$(ls -1dt "$DEST".backup-* 2>/dev/null | head -1 || true)}
[[ -n "$BACKUP" && -d "$BACKUP" ]] || { echo "No PlayTools backup found." >&2; exit 2; }
FAILED="$DEST.failed-$(date +%Y%m%d-%H%M%S)"
[[ -e "$DEST" ]] && mv "$DEST" "$FAILED"
cp -R "$BACKUP" "$DEST"
echo "Restored: $BACKUP -> $DEST"
[[ -d "$FAILED" ]] && echo "Replaced PTMC framework kept at: $FAILED"
