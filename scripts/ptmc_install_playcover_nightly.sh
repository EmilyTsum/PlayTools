#!/usr/bin/env bash
set -euo pipefail
command -v gh >/dev/null || { echo "gh is required" >&2; exit 2; }
command -v hdiutil >/dev/null || { echo "macOS required" >&2; exit 2; }
RUN=$(gh run list -R PlayCover/PlayCover --workflow 'Build nightly release' --status success --limit 1 --json databaseId,headSha,createdAt --jq '.[0]')
ID=$(jq -r .databaseId <<<"$RUN"); SHA=$(jq -r .headSha <<<"$RUN")
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
gh run download "$ID" -R PlayCover/PlayCover -D "$TMP"
DMG=$(find "$TMP" -name '*.dmg' -print -quit)
[[ -n "$DMG" ]] || { echo "nightly DMG artifact not found" >&2; exit 1; }
MOUNT=$(hdiutil attach -nobrowse -readonly "$DMG" | awk '/\/Volumes\//{sub(/^.*\/Volumes\//,"/Volumes/");print;exit}')
trap 'hdiutil detach "$MOUNT" >/dev/null 2>&1 || true; rm -rf "$TMP"' EXIT
APP=$(find "$MOUNT" -maxdepth 2 -name 'PlayCover.app' -print -quit)
[[ -n "$APP" ]] || { echo "PlayCover.app missing in nightly DMG" >&2; exit 1; }
if pgrep -x PlayCover >/dev/null 2>&1; then echo "Quit PlayCover first." >&2; exit 3; fi
if [[ -d /Applications/PlayCover.app ]]; then mv /Applications/PlayCover.app "/Applications/PlayCover.app.backup-$(date +%Y%m%d-%H%M%S)"; fi
cp -R "$APP" /Applications/PlayCover.app
xattr -dr com.apple.quarantine /Applications/PlayCover.app 2>/dev/null || true
echo "Installed latest successful PlayCover nightly: run=$ID commit=$SHA"
