#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
INC="$ROOT/PlayTools/MetalCapture/PTMetalCapture.inc"
[[ $(grep -c 'objc_getClassList' "$INC") -eq 2 ]]
! grep -nE 'getBytes:|AGX|IOGPU' "$INC"
grep -q 'ptmc_class_implements_selector' "$INC"
grep -q 'kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange' "$INC"
grep -q 'RequireHardwareAcceleratedVideoEncoder' "$INC"
grep -q '#include "MetalCapture/PTMetalCapture.inc"' "$ROOT/PlayTools/PlayLoader.m"
echo 'PTMC static invariants: PASS'
