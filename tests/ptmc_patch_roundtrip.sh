#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
git clone -q --filter=blob:none https://github.com/PlayCover/PlayTools.git "$TMP/upstream"
git -C "$TMP/upstream" checkout -q "$(cat "$ROOT/PTMC_UPSTREAM_COMMIT")"
[[ -z "$(git -C "$TMP/upstream" status --porcelain=v1)" ]]
python3 "$ROOT/scripts/ptmc_apply_patch.py" "$TMP/upstream"
python3 "$ROOT/scripts/ptmc_apply_patch.py" "$TMP/upstream"
python3 "$ROOT/scripts/ptmc_apply_patch.py" --check "$TMP/upstream"
python3 "$ROOT/scripts/ptmc_unapply_patch.py" "$TMP/upstream"
python3 "$ROOT/scripts/ptmc_unapply_patch.py" "$TMP/upstream"
[[ -z "$(git -C "$TMP/upstream" status --porcelain=v1)" ]]
echo 'PTMC patch roundtrip: PASS'
