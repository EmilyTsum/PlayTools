#!/usr/bin/env bash
set -euo pipefail
for k in PTMC_ENABLE PTMC_FPS PTMC_BITRATE PTMC_BUFFERS PTMC_LOG_INTERVAL PTMC_OUTPUT PTMC_OUTPUT_DIR PTMC_DISABLE_DISPLAY_SYNC PTMC_SPOOF_MAX_FPS PTMC_AUTOSTART; do
  launchctl unsetenv "$k" 2>/dev/null || true
done
echo "PTMC launch environment removed. Restart PlayCover/game to return to unhooked mode."
