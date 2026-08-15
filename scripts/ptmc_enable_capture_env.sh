#!/usr/bin/env bash
set -euo pipefail
FPS=${PTMC_FPS:-120}; BITRATE=${PTMC_BITRATE:-120000000}; BUFFERS=${PTMC_BUFFERS:-6}; LOG_INTERVAL=${PTMC_LOG_INTERVAL:-1}
launchctl setenv PTMC_ENABLE 1
launchctl setenv PTMC_FPS "$FPS"
launchctl setenv PTMC_BITRATE "$BITRATE"
launchctl setenv PTMC_BUFFERS "$BUFFERS"
launchctl setenv PTMC_LOG_INTERVAL "$LOG_INTERVAL"
[[ -n ${PTMC_OUTPUT:-} ]] && launchctl setenv PTMC_OUTPUT "$PTMC_OUTPUT" || launchctl unsetenv PTMC_OUTPUT 2>/dev/null || true
[[ ${PTMC_DISABLE_DISPLAY_SYNC:-0} == 1 ]] && launchctl setenv PTMC_DISABLE_DISPLAY_SYNC 1 || launchctl setenv PTMC_DISABLE_DISPLAY_SYNC 0
[[ -n ${PTMC_SPOOF_MAX_FPS:-} ]] && launchctl setenv PTMC_SPOOF_MAX_FPS "$PTMC_SPOOF_MAX_FPS" || launchctl unsetenv PTMC_SPOOF_MAX_FPS 2>/dev/null || true
echo "PTMC environment enabled: fps=$FPS bitrate=$BITRATE buffers=$BUFFERS"
echo "Restart PlayCover/game so launchctl environment is inherited, then use control_capture.sh start."
