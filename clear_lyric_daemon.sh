#!/usr/bin/env bash
set -euo pipefail

patterns=(
  "[l]yrics_display_daemon.py"
  "[l]yric_daemon"
)

db_pattern="[s]qlite3 .*/lyrics-display\\.sqlite3"

stop_pattern() {
  local pattern="$1"
  local label="$2"
  local pids

  pids="$(pgrep -f "$pattern" || true)"
  if [[ -z "$pids" ]]; then
    echo "No $label process found."
    return
  fi

  echo "Stopping $label PID(s): $pids"
  kill $pids || true

  sleep 2

  pids="$(pgrep -f "$pattern" || true)"
  if [[ -n "$pids" ]]; then
    echo "Force stopping $label PID(s): $pids"
    kill -9 $pids || true
  fi
}

for pattern in "${patterns[@]}"; do
  stop_pattern "$pattern" "lyric daemon"
done

stop_pattern "$db_pattern" "lyrics sqlite session"

echo "Remaining matching processes:"
pgrep -af "[l]yric|[l]yrics_display_daemon|[s]qlite3 .*/lyrics-display\\.sqlite3" || echo "None"
