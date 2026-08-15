#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
INC="$ROOT/PlayTools/MetalCapture/PTMetalCapture.inc"
[[ $(grep -c 'objc_getClassList' "$INC") -eq 2 ]]
! grep -nE 'getBytes:|AGX|IOGPU' "$INC"
grep -q 'ptmc_class_implements_selector' "$INC"
grep -q 'for (Class cls = object_getClass(self); cls != Nil; cls = class_getSuperclass(cls))' "$INC"
grep -q '_dyld_register_func_for_add_image' "$INC"
grep -q 'dispatch_async(session.encoderQueue' "$INC"
grep -q 'kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange' "$INC"
grep -q 'RequireHardwareAcceleratedVideoEncoder' "$INC"
grep -q '#include "MetalCapture/PTMetalCapture.inc"' "$ROOT/PlayTools/PlayLoader.m"
! grep -q 'ptmc_next_drawable_original' "$INC"
echo 'PTMC static invariants: PASS'
