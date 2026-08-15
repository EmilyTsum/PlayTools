#!/usr/bin/env bash
set -euo pipefail
cmd=${1:-status}
command -v notifyutil >/dev/null || { echo "notifyutil is required (macOS)" >&2; exit 2; }
case "$cmd" in
  start) notifyutil -p io.playcover.ptmc.start; echo "PTMC start notification sent" ;;
  stop) notifyutil -p io.playcover.ptmc.stop; echo "PTMC stop notification sent" ;;
  status)
    notifyutil -p io.playcover.ptmc.status
    echo "PTMC status requested. Recent logs:"
    log show --last 5s --style compact --predicate 'eventMessage CONTAINS "[PTMC]"' 2>/dev/null | tail -30 || true ;;
  *) echo "usage: $0 {start|stop|status}" >&2; exit 2 ;;
esac
