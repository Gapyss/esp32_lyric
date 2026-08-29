#!/usr/bin/env bash
#
# Start / stop / inspect the ESP8266 lyrics daemon, and refuse to start it wrong.
#
# The daemon is run by hand rather than by launchd, which is a deliberate choice
# -- but it means three mistakes are always one keystroke away, and all three
# produce the same useless symptom: a panel stuck on "waiting for lyrics" with
# nothing anywhere to say why.
#
#   1. A second daemon started on top of a running one. Port 8766 is already
#      bound, so the newcomer dies and you spend the next ten minutes debugging
#      a process that was never the one talking to the board.
#   2. A daemon started without --insecure. It comes up clean, the board's
#      socket connects, and not one frame ever renders: this board has no heap
#      for the SEC2 record layer and cannot complete a proto=2 handshake.
#   3. A board that is powered off, on another network, or unreachable. From the
#      Mac side that is indistinguishable from a broken daemon.
#
# So: guard on the port, force --insecure, and say what the board itself thinks
# before and after. Usage:
#
#   tools/lyrics.sh [start] [extra daemon args...]   start it (backgrounded)
#   tools/lyrics.sh start -f                         start it in the foreground
#   tools/lyrics.sh status                           what is the chain doing?
#   tools/lyrics.sh stop                             stop it

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DAEMON="$HERE/../daemon/lyrics_display_daemon.py"
BOARD_NAME="${G4PYS_LYRICS_BOARD_NAME:-clawdmeter.local}"
BOARD_PORT="${G4PYS_LYRICS_BOARD_PORT:-8766}"
EXT_PORT="${G4PYS_LYRICS_EXTENSION_PORT:-8765}"
LOG="${G4PYS_LYRICS_LOG:-$HOME/Library/Logs/g4pys-lyrics-display.log}"
PY="${G4PYS_LYRICS_PYTHON:-python3}"

if [ -t 1 ]; then B=$'\033[1m'; DIM=$'\033[2m'; OK=$'\033[32m'; BAD=$'\033[31m'; WARN=$'\033[33m'; R=$'\033[0m'
else B=""; DIM=""; OK=""; BAD=""; WARN=""; R=""; fi

say()  { printf '%s\n' "$*"; }
good() { printf '  %s+%s %s\n' "$OK"  "$R" "$*"; }
bad()  { printf '  %s!%s %s\n' "$BAD" "$R" "$*"; }
warn() { printf '  %s~%s %s\n' "$WARN" "$R" "$*"; }
note() { printf '    %s%s%s\n' "$DIM" "$*" "$R"; }

# --- the one honest answer to "is the daemon up?" ----------------------------
# Keyed off the listening socket, not a process name. `pgrep -f 'a\|b'` looks
# like it works and does not: pgrep matches with ERE, where \| is a literal
# pipe, so the pattern silently matches nothing. It reported "none running"
# against a live daemon while this script was being written. The bound port is
# the thing we actually care about anyway -- it is what the board dials.
daemon_pid() {
  lsof -nP -iTCP:"$BOARD_PORT" -sTCP:LISTEN -t 2>/dev/null | head -1
}

# Fast, IPv4-only, and never asks for AAAA. getaddrinfo on a .local name whose
# host publishes no AAAA record waits out the full mDNS IPv6 timeout -- 5s here
# -- before returning the A record it already had.
resolve_board() {
  "$PY" -c "import socket,sys
try: sys.stdout.write(socket.gethostbyname(sys.argv[1]))
except OSError: sys.exit(1)" "$BOARD_NAME" 2>/dev/null
}

# Print the board's own view of itself. Sets BOARD_LYR as a side effect so the
# caller can decide whether the chain is actually up.
BOARD_LYR=""
board_report() {
  local ip="$1" json
  json="$(curl -s --max-time 4 "http://$ip/usage.json" 2>/dev/null)"
  if [ -z "$json" ]; then
    bad "board at $ip did not answer /usage.json"
    note "powered off, on another network, or wedged -- power-cycle it"
    BOARD_LYR=""
    return 1
  fi
  local line
  line="$("$PY" - "$json" <<'PYEOF'
import json, sys
try:
    d = json.loads(sys.argv[1])
except Exception:
    print("?|unparseable /usage.json")
    raise SystemExit
lyr = {0: "idle (no Mac IP known)", 1: "connecting", 2: "streaming"}.get(d.get("lyr"), "?")
up = int(d.get("up") or 0)
line = "wifi %s (%s dBm) - %s - heap %dk - up %dh%02dm - boot %s" % (
    d.get("ssid", "?"), d.get("rssi", "?"), lyr,
    int(d.get("heap") or 0) // 1024, up // 3600, (up % 3600) // 60,
    d.get("rst", "?"))
# Watchdog state. Absent on firmware older than the watchdog, hence the
# .get guards -- status has to keep working against a board not yet reflashed.
down = int(d.get("wifidown") or 0)
drops = int(d.get("wifidrops") or 0)
if down:
    line += " - WIFI DOWN %ds (watchdog reconnecting)" % down
if drops:
    line += " - %d wifi drop%s since boot" % (drops, "" if drops == 1 else "s")
if d.get("mdnsok") == 0:
    line += " - mDNS refresh FAILED (.local may be dark; use --announce-url http://<ip>)"
print("%s|%s" % (d.get("lyr", "?"), line))
PYEOF
)"
  BOARD_LYR="${line%%|*}"
  case "$BOARD_LYR" in
    2) good "board ${line#*|}" ;;
    1) warn "board ${line#*|}" ;;
    *) bad  "board ${line#*|}" ;;
  esac
}

cmd_status() {
  say "${B}daemon${R}"
  local pid; pid="$(daemon_pid)"
  if [ -n "$pid" ]; then
    local args; args="$(ps -o command= -p "$pid" 2>/dev/null)"
    good "listening on :$BOARD_PORT (pid $pid)"
    case "$args" in
      *--insecure*) : ;;
      *) bad "running WITHOUT --insecure -- the board's socket will connect and never render"
         note "stop it and start it with this script" ;;
    esac
    if lsof -nP -iTCP:"$EXT_PORT" -sTCP:LISTEN -t >/dev/null 2>&1; then
      local n; n="$(lsof -nP -iTCP:"$EXT_PORT" -sTCP:ESTABLISHED -t 2>/dev/null | sort -u | wc -l | tr -d ' ')"
      if [ "$n" -gt 0 ]; then good "browser extension attached on :$EXT_PORT"
      else warn "nothing attached on :$EXT_PORT -- open music.youtube.com"; fi
    fi
  else
    bad "nothing listening on :$BOARD_PORT -- the daemon is not running"
  fi

  say ""
  say "${B}board${R}"
  local ip; ip="$(resolve_board)"
  if [ -z "$ip" ]; then
    bad "cannot resolve $BOARD_NAME"
    note "mDNS is quiet: the board may be off, or on a network that blocks multicast"
    return 1
  fi
  good "$BOARD_NAME resolves to $ip"
  board_report "$ip" || return 1
  [ "$BOARD_LYR" = "2" ]
}

cmd_stop() {
  local pid; pid="$(daemon_pid)"
  if [ -z "$pid" ]; then say "nothing listening on :$BOARD_PORT -- already stopped."; return 0; fi
  # Never kill on the port alone. The README's dual-daemon setup leaves this
  # board on 8766 and moves the ESP32 daemon to 8767 -- start that one on its
  # defaults by mistake and an unguarded stop takes down the other board's
  # daemon without a word. Match on the script name, not a path: a hand-started
  # daemon shows up with a relative one.
  local args; args="$(ps -o command= -p "$pid" 2>/dev/null)"
  case "$args" in
    *lyrics_display_daemon.py*) : ;;
    *) bad "pid $pid holds :$BOARD_PORT but is not a lyrics daemon -- refusing to kill it"
       note "$args"
       return 1 ;;
  esac
  kill "$pid" 2>/dev/null
  for _ in $(seq 1 20); do
    [ -z "$(daemon_pid)" ] && { say "stopped (was pid $pid)."; return 0; }
    sleep 0.25
  done
  kill -9 "$pid" 2>/dev/null
  say "force-stopped (was pid $pid)."
}

cmd_start() {
  local foreground=0 extra=() take_port=0
  for a in "$@"; do
    # A --board-port passed through to the daemon moves the port this script
    # must guard on; without this the guard checks 8766, sees the other daemon,
    # and refuses a start that would not actually have collided.
    if [ "$take_port" = "1" ]; then BOARD_PORT="$a"; take_port=0; extra+=("$a"); continue; fi
    case "$a" in
      -f|--foreground) foreground=1 ;;
      --board-port) take_port=1; extra+=("$a") ;;
      --board-port=*) BOARD_PORT="${a#*=}"; extra+=("$a") ;;
      *) extra+=("$a") ;;
    esac
  done

  # Guard first: everything below assumes we are the only daemon.
  local pid; pid="$(daemon_pid)"
  if [ -n "$pid" ]; then
    bad "already running: pid $pid holds :$BOARD_PORT"
    note "$(ps -o command= -p "$pid" 2>/dev/null)"
    note "use 'tools/lyrics.sh status', or 'stop' first"
    return 1
  fi
  if [ ! -f "$DAEMON" ]; then bad "daemon not found at $DAEMON"; return 1; fi

  say "${B}pre-flight${R}"
  local ip; ip="$(resolve_board)"
  if [ -n "$ip" ]; then
    good "$BOARD_NAME resolves to $ip"
    board_report "$ip"
  else
    warn "cannot resolve $BOARD_NAME -- starting anyway, the announcer keeps retrying"
    note "if the board has a fixed address, pass --announce-url http://<ip>"
  fi

  # --insecure is not optional for this board and is applied whether or not the
  # caller remembered it; passing it twice is harmless (argparse store_true).
  say ""
  say "${B}starting${R}"
  if [ "$foreground" = "1" ]; then
    note "foreground: Ctrl-C stops the daemon and the board falls back within 8s"
    exec env PYTHONUNBUFFERED=1 "$PY" "$DAEMON" serve --insecure "${extra[@]+"${extra[@]}"}"
  fi

  mkdir -p "$(dirname "$LOG")"
  {
    printf '\n===== %s : lyrics.sh start =====\n' "$(date '+%Y-%m-%d %H:%M:%S')"
  } >> "$LOG"
  # PYTHONUNBUFFERED is load-bearing, not tidiness: stdout to a file is block-
  # buffered, so without it the daemon's "board announce reached ..." and "board
  # connected" lines sit in a 8 KB buffer and the log -- the only record of why
  # a run failed -- reads as empty for the whole session.
  nohup env PYTHONUNBUFFERED=1 "$PY" "$DAEMON" serve --insecure "${extra[@]+"${extra[@]}"}" >> "$LOG" 2>&1 &
  local child=$!
  disown 2>/dev/null

  # Did it survive its own startup? A traceback here (bad font, locked DB, a
  # port already taken on :8765) otherwise scrolls past into the log unseen.
  local up=""
  for _ in $(seq 1 20); do
    sleep 0.25
    if ! kill -0 "$child" 2>/dev/null; then
      bad "daemon exited during startup"
      note "last lines of $LOG:"
      tail -n 12 "$LOG" | sed 's/^/    /'
      return 1
    fi
    [ -n "$(daemon_pid)" ] && { up=1; break; }
  done
  if [ -z "$up" ]; then
    bad "daemon is alive but never bound :$BOARD_PORT"
    tail -n 12 "$LOG" | sed 's/^/    /'
    return 1
  fi
  good "daemon up (pid $child), logging to $LOG"

  # The verdict that matters is not "the process started" -- it is "the panel is
  # showing frames". The board dials on its own LYR_RETRY_MS (8s) cadence after
  # the first knock lands, so give it a couple of those before reporting.
  if [ -z "$ip" ]; then
    warn "board was unreachable at pre-flight; not waiting for it"
    return 0
  fi
  say ""
  say "${B}waiting for the board${R}"
  for _ in $(seq 1 12); do
    sleep 2
    board_report "$ip" >/dev/null 2>&1
    if [ "$BOARD_LYR" = "2" ]; then
      board_report "$ip"
      say ""
      say "  ${OK}${B}lyrics are on the panel.${R}"
      return 0
    fi
  done
  board_report "$ip"
  say ""
  warn "board has not reached streaming after ~24s"
  note "check $LOG for 'board announce reached' and 'board connected'"
  return 1
}

case "${1:-start}" in
  status) shift; cmd_status ;;
  stop)   shift; cmd_stop ;;
  start)  shift; cmd_start "$@" ;;
  *)      cmd_start "$@" ;;
esac
