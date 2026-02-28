#!/usr/bin/env bash
set -euo pipefail

# Dedicated supervisor for the MCSR cache service.
# This script is intentionally isolated from other apps:
# - Separate PID file
# - Separate log files
# - No broad pkill patterns
# - Optional tunnel modes (off/quick/named)

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT_PATH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"

PID_DIR="${MCSR_PID_DIR:-$ROOT_DIR/.run}"
LOG_DIR="${MCSR_LOG_DIR:-$ROOT_DIR/.logs}"
PID_FILE="$PID_DIR/mcsr-host.pid"
STATE_HOME_DEFAULT="${XDG_STATE_HOME:-$HOME/.local/state}"
KEYS_FILE="${MCSR_KEYS_FILE:-$STATE_HOME_DEFAULT/toolscreen/mcsr-host-keys.env}"
FALLBACK_KEYS_FILE="$PID_DIR/mcsr-host-keys.env"

SUPERVISOR_LOG="$LOG_DIR/mcsr-host.log"
BACKEND_LOG="$LOG_DIR/mcsr-backend.log"
TUNNEL_LOG="$LOG_DIR/mcsr-cloudflared.log"

BACKEND_BIND="${BACKEND_BIND:-127.0.0.1}"
BACKEND_PORT="${BACKEND_PORT:-8787}"
PYTHON_BIN="${PYTHON_BIN:-python3}"

# Set BACKEND_CMD to your actual API server command.
# Default runs the local MCSR caching proxy service.
BACKEND_CMD="${BACKEND_CMD:-$PYTHON_BIN \"$ROOT_DIR/scripts/mcsr_cache_service.py\" --bind $BACKEND_BIND --port $BACKEND_PORT --db \"$ROOT_DIR/data/mcsr_cache_service.db\" --upstream https://mcsrranked.com}"

# Tunnel mode:
#   off   -> no cloudflared process; useful when another app's tunnel already routes to this port
#   quick -> cloudflared quick tunnel (random trycloudflare URL)
#   named -> cloudflared named tunnel using CLOUDFLARED_CONFIG
TUNNEL_MODE="${TUNNEL_MODE:-off}"
CLOUDFLARED_BIN="${CLOUDFLARED_BIN:-cloudflared}"
CLOUDFLARED_CONFIG="${CLOUDFLARED_CONFIG:-$ROOT_DIR/cloudflared/mcsr_config.yml}"
TUNNEL_URL="${TUNNEL_URL:-http://$BACKEND_BIND:$BACKEND_PORT}"
HEALTH_LOCAL_URL="${MCSR_HEALTH_LOCAL_URL:-http://$BACKEND_BIND:$BACKEND_PORT/health}"
HEALTH_RETRIES="${MCSR_HEALTH_RETRIES:-15}"
HEALTH_RETRY_DELAY_S="${MCSR_HEALTH_RETRY_DELAY_S:-1}"
MCSR_CACHE_AUTH_TOKEN="${MCSR_CACHE_AUTH_TOKEN:-}"
MCSR_CACHE_AUTH_HEADER="${MCSR_CACHE_AUTH_HEADER:-x-toolscreen-token}"
MCSR_CACHE_PUBLIC_TOKEN="${MCSR_CACHE_PUBLIC_TOKEN:-}"

load_keys_from_file_if_available() {
  if [[ ! -f "$KEYS_FILE" && -f "$FALLBACK_KEYS_FILE" ]]; then
    KEYS_FILE="$FALLBACK_KEYS_FILE"
  fi

  if [[ ! -f "$KEYS_FILE" ]]; then
    return 0
  fi

  if [[ -n "$MCSR_CACHE_AUTH_TOKEN" && -n "$MCSR_CACHE_PUBLIC_TOKEN" ]]; then
    return 0
  fi

  # shellcheck disable=SC1090
  source "$KEYS_FILE"
  MCSR_CACHE_AUTH_TOKEN="${MCSR_CACHE_AUTH_TOKEN:-}"
  MCSR_CACHE_AUTH_HEADER="${MCSR_CACHE_AUTH_HEADER:-x-toolscreen-token}"
  MCSR_CACHE_PUBLIC_TOKEN="${MCSR_CACHE_PUBLIC_TOKEN:-}"
}

timestamp() {
  date '+%Y-%m-%d %H:%M:%S'
}

log() {
  printf '[%s] %s\n' "$(timestamp)" "$*" | tee -a "$SUPERVISOR_LOG"
}

collect_descendants() {
  local parent="$1"
  local child
  while read -r child; do
    [[ -z "$child" ]] && continue
    echo "$child"
    collect_descendants "$child"
  done < <(pgrep -P "$parent" 2>/dev/null || true)
}

kill_with_children() {
  local root_pid="$1"
  local signal="${2:-TERM}"
  local descendants
  descendants="$(collect_descendants "$root_pid" | sort -u || true)"
  if [[ -n "$descendants" ]]; then
    # shellcheck disable=SC2086
    kill "-$signal" $descendants 2>/dev/null || true
  fi
  kill "-$signal" "$root_pid" 2>/dev/null || true
}

is_running() {
  [[ -f "$PID_FILE" ]] && kill -0 "$(cat "$PID_FILE" 2>/dev/null)" 2>/dev/null
}

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1" >&2
    exit 1
  fi
}

generate_token() {
  require_command "$PYTHON_BIN"
  "$PYTHON_BIN" -c 'import secrets; print(secrets.token_urlsafe(32))'
}

write_keys_file() {
  local target_file="$KEYS_FILE"
  local target_dir
  target_dir="$(dirname "$target_file")"
  if ! mkdir -p "$target_dir" 2>/dev/null; then
    mkdir -p "$PID_DIR"
    target_file="$FALLBACK_KEYS_FILE"
    KEYS_FILE="$target_file"
    echo "Warning: could not create key directory '$target_dir'; falling back to '$KEYS_FILE'."
  fi

  umask 077
  cat >"$target_file" <<EOF
export MCSR_CACHE_AUTH_HEADER='$MCSR_CACHE_AUTH_HEADER'
export MCSR_CACHE_AUTH_TOKEN='$MCSR_CACHE_AUTH_TOKEN'
export MCSR_CACHE_PUBLIC_TOKEN='$MCSR_CACHE_PUBLIC_TOKEN'
EOF
  chmod 600 "$target_file"
  local perms
  perms="$(stat -c '%a' "$target_file" 2>/dev/null || true)"
  if [[ "$perms" != "600" ]]; then
    echo "Warning: could not enforce 600 perms on $target_file (current: ${perms:-unknown})."
    echo "Set MCSR_KEYS_FILE to a Linux-home path (example: $HOME/.local/state/toolscreen/mcsr-host-keys.env)."
  fi
}

rotate_auth_keys() {
  MCSR_CACHE_AUTH_TOKEN="$(generate_token)"
  MCSR_CACHE_PUBLIC_TOKEN="$(generate_token)"
  export MCSR_CACHE_AUTH_TOKEN MCSR_CACHE_PUBLIC_TOKEN MCSR_CACHE_AUTH_HEADER
  write_keys_file
  echo "Rotated MCSR cache auth keys."
  echo "Private/API token: ${MCSR_CACHE_AUTH_TOKEN:0:8}..."
  echo "Public/health token: ${MCSR_CACHE_PUBLIC_TOKEN:0:8}..."
  echo "Keys file: $KEYS_FILE (chmod 600)"
  echo "Load into current shell when needed: source \"$KEYS_FILE\""
}

run_loop() {
  local name="$1"
  local target_log="$2"
  shift 2

  while true; do
    printf '[%s] Starting %s...\n' "$(timestamp)" "$name" >>"$SUPERVISOR_LOG"
    set +e
    "$@" >>"$target_log" 2>&1
    local exit_code=$?
    set -e
    printf '[%s] %s exited with code %s. Restarting in 3s...\n' \
      "$(timestamp)" "$name" "$exit_code" >>"$SUPERVISOR_LOG"
    sleep 3
  done
}

cleanup() {
  if [[ -n "${BACKEND_LOOP_PID:-}" ]] && kill -0 "$BACKEND_LOOP_PID" 2>/dev/null; then
    kill_with_children "$BACKEND_LOOP_PID" TERM
    wait "$BACKEND_LOOP_PID" 2>/dev/null || true
  fi

  if [[ -n "${TUNNEL_LOOP_PID:-}" ]] && kill -0 "$TUNNEL_LOOP_PID" 2>/dev/null; then
    kill_with_children "$TUNNEL_LOOP_PID" TERM
    wait "$TUNNEL_LOOP_PID" 2>/dev/null || true
  fi

  if [[ -f "$PID_FILE" ]] && [[ "$(cat "$PID_FILE" 2>/dev/null)" == "$$" ]]; then
    rm -f "$PID_FILE"
  fi
}

run_supervisor() {
  mkdir -p "$PID_DIR" "$LOG_DIR"
  touch "$SUPERVISOR_LOG" "$BACKEND_LOG" "$TUNNEL_LOG"

  if is_running; then
    echo "MCSR host already running (pid $(cat "$PID_FILE"))." >&2
    exit 1
  fi

  require_command bash
  require_command "$PYTHON_BIN"

  case "$TUNNEL_MODE" in
    off)
      ;;
    quick)
      require_command "$CLOUDFLARED_BIN"
      ;;
    named)
      require_command "$CLOUDFLARED_BIN"
      if [[ ! -f "$CLOUDFLARED_CONFIG" ]]; then
        echo "Cloudflared config not found at $CLOUDFLARED_CONFIG" >&2
        exit 1
      fi
      ;;
    *)
      echo "Invalid TUNNEL_MODE: $TUNNEL_MODE (use off|quick|named)" >&2
      exit 1
      ;;
  esac

  echo "$$" >"$PID_FILE"
  trap cleanup EXIT INT TERM

  run_loop "mcsr-backend" "$BACKEND_LOG" \
    env PYTHONUNBUFFERED=1 BACKEND_BIND="$BACKEND_BIND" BACKEND_PORT="$BACKEND_PORT" \
    bash -lc "$BACKEND_CMD" &
  BACKEND_LOOP_PID=$!

  if [[ "$TUNNEL_MODE" == "quick" ]]; then
    run_loop "cloudflared-quick" "$TUNNEL_LOG" \
      "$CLOUDFLARED_BIN" tunnel --url "$TUNNEL_URL" &
    TUNNEL_LOOP_PID=$!
  elif [[ "$TUNNEL_MODE" == "named" ]]; then
    run_loop "cloudflared-named" "$TUNNEL_LOG" \
      "$CLOUDFLARED_BIN" tunnel --config "$CLOUDFLARED_CONFIG" run &
    TUNNEL_LOOP_PID=$!
  fi

  log "MCSR host started."
  log "Backend bind: $BACKEND_BIND:$BACKEND_PORT"
  log "Backend command: $BACKEND_CMD"
  log "Tunnel mode: $TUNNEL_MODE"
  if [[ "$TUNNEL_MODE" == "quick" ]]; then
    log "Quick tunnel URL target: $TUNNEL_URL"
  elif [[ "$TUNNEL_MODE" == "named" ]]; then
    log "Named tunnel config: $CLOUDFLARED_CONFIG"
  else
    log "No tunnel process started by this script."
  fi
  log "Logs: $LOG_DIR"

  if [[ -n "${TUNNEL_LOOP_PID:-}" ]]; then
    wait "$BACKEND_LOOP_PID" "$TUNNEL_LOOP_PID"
  else
    wait "$BACKEND_LOOP_PID"
  fi
}

start_daemon() {
  mkdir -p "$PID_DIR" "$LOG_DIR"
  touch "$SUPERVISOR_LOG"

  if is_running; then
    echo "Already running (pid $(cat "$PID_FILE"))."
    return 0
  fi

  nohup "$SCRIPT_PATH" run >>"$SUPERVISOR_LOG" 2>&1 &
  local daemon_pid=$!
  sleep 1

  if kill -0 "$daemon_pid" 2>/dev/null; then
    echo "Started MCSR host supervisor (pid $daemon_pid)."
    echo "Use: $SCRIPT_PATH status"
  else
    echo "Failed to start MCSR host supervisor. Check $SUPERVISOR_LOG" >&2
    exit 1
  fi
}

stop_daemon() {
  if [[ ! -f "$PID_FILE" ]]; then
    echo "Not running."
    return 0
  fi

  local pid
  pid="$(cat "$PID_FILE")"
  if ! kill -0 "$pid" 2>/dev/null; then
    rm -f "$PID_FILE"
    echo "Not running (stale pid file removed)."
    return 0
  fi

  kill "$pid"
  for _ in {1..20}; do
    if ! kill -0 "$pid" 2>/dev/null; then
      rm -f "$PID_FILE"
      echo "Stopped."
      return 0
    fi
    sleep 0.5
  done

  kill_with_children "$pid" KILL
  rm -f "$PID_FILE"
  echo "Force stopped."
}

status_daemon() {
  if is_running; then
    echo "Running (pid $(cat "$PID_FILE"))."
  else
    echo "Not running."
  fi
  echo "Backend: $BACKEND_BIND:$BACKEND_PORT"
  echo "Tunnel mode: $TUNNEL_MODE"
  echo "Logs: $LOG_DIR"
}

clean_orphans() {
  if [[ -f "$PID_FILE" ]]; then
    local pid
    pid="$(cat "$PID_FILE" 2>/dev/null || true)"
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
      kill_with_children "$pid" TERM
      sleep 1
      if kill -0 "$pid" 2>/dev/null; then
        kill_with_children "$pid" KILL
      fi
    fi
    rm -f "$PID_FILE"
  fi
  echo "Cleaned MCSR host supervisor state."
}

show_logs() {
  mkdir -p "$LOG_DIR"
  touch "$SUPERVISOR_LOG" "$BACKEND_LOG" "$TUNNEL_LOG"

  local mode="${1:-all}"
  local lines="${2:-120}"
  if ! [[ "$lines" =~ ^[0-9]+$ ]]; then
    echo "Invalid lines value: $lines (must be numeric)" >&2
    exit 1
  fi

  case "$mode" in
    all)
      tail -n "$lines" -f "$BACKEND_LOG" "$TUNNEL_LOG" "$SUPERVISOR_LOG"
      ;;
    backend)
      tail -n "$lines" -f "$BACKEND_LOG"
      ;;
    tunnel|cloudflared)
      tail -n "$lines" -f "$TUNNEL_LOG"
      ;;
    supervisor|host)
      tail -n "$lines" -f "$SUPERVISOR_LOG"
      ;;
    *)
      echo "Unknown logs mode: $mode (use all|backend|tunnel|supervisor)" >&2
      exit 1
      ;;
  esac
}

check_url_health() {
  local url="$1"
  local label="$2"
  local retries="$3"
  local delay_s="$4"
  local token="${5:-}"
  local body
  local i
  local -a curl_args

  require_command curl

  curl_args=(-fsS --max-time 4)
  if [[ -n "$token" ]]; then
    curl_args+=(-H "$MCSR_CACHE_AUTH_HEADER: $token")
  fi

  for ((i=1; i<=retries; i++)); do
    body="$(curl "${curl_args[@]}" "$url" 2>/dev/null || true)"
    if [[ -n "$body" ]]; then
      if [[ "$body" == *'"ok":true'* || "$body" == *'"ok": true'* ]]; then
        echo "$label health OK: $url"
        return 0
      fi
      # If endpoint responds but not the expected JSON shape, still report response for debugging.
      echo "$label health responded but unexpected body: $url"
      echo "$body" | head -c 180
      echo
      return 1
    fi
    sleep "$delay_s"
  done

  echo "$label health FAILED after ${retries} attempts: $url" >&2
  return 1
}

health_checks() {
  local local_ok=0
  local public_ok=0

  if ! [[ "$HEALTH_RETRIES" =~ ^[0-9]+$ ]]; then
    echo "Invalid MCSR_HEALTH_RETRIES: $HEALTH_RETRIES (must be numeric)" >&2
    return 1
  fi

  if ! check_url_health "$HEALTH_LOCAL_URL" "Local backend" "$HEALTH_RETRIES" "$HEALTH_RETRY_DELAY_S" "$MCSR_CACHE_AUTH_TOKEN"; then
    local_ok=1
  fi

  local public_base="${MCSR_CACHE_SERVER_URL:-}"
  if [[ -n "$public_base" ]]; then
    local public_health_url
    if [[ "$public_base" == */health ]]; then
      public_health_url="$public_base"
    else
      public_health_url="${public_base%/}/health"
    fi
    local public_token="${MCSR_CACHE_PUBLIC_TOKEN:-$MCSR_CACHE_AUTH_TOKEN}"
    if ! check_url_health "$public_health_url" "Public cache URL" "3" "1" "$public_token"; then
      public_ok=1
      echo "Public URL health is optional for local runs. Set/verify MCSR_CACHE_SERVER_URL if needed." >&2
    fi
  else
    echo "Public cache URL check skipped (MCSR_CACHE_SERVER_URL not set)."
  fi

  if [[ "$local_ok" -ne 0 ]]; then
    return 1
  fi
  if [[ "$public_ok" -ne 0 ]]; then
    return 2
  fi
  return 0
}

reset_daemon() {
  stop_daemon
  clean_orphans
}

startup_sequence() {
  if is_running; then
    echo "Already running; keeping current auth keys."
  else
    rotate_auth_keys
  fi
  start_daemon
  status_daemon
  health_checks
}

restart_sequence() {
  reset_daemon
  startup_sequence
}

usage() {
  cat <<EOF
Usage: $(basename "$0") <start|stop|status|clean|reset|stopclean|health|rotate-keys|show-keys|startup|restart|logs|run>

Commands:
  start   Run backend (and optional cloudflared) in background with auto-restart loops
  stop    Stop the background supervisor
  status  Show whether services are running
  clean   Remove stale pid/child state for this script only
  reset   stop + clean (common "hard stop" flow)
  stopclean Alias of reset (stop + clean)
  health  Run backend/public health checks
  rotate-keys Generate new private/public tokens and write $KEYS_FILE
  show-keys Show keys file location and shell source command
  startup start + status + health checks
  restart reset + startup
  logs    Tail logs (default: all, 120 lines)
  run     Run in foreground (for debugging)

Examples:
  $(basename "$0") start
  $(basename "$0") startup
  $(basename "$0") restart
  TUNNEL_MODE=quick $(basename "$0") start
  TUNNEL_MODE=named CLOUDFLARED_CONFIG=/path/to/mcsr.yml $(basename "$0") start
  BACKEND_CMD="python3 -m uvicorn api:app --host 127.0.0.1 --port 8787" $(basename "$0") start

Environment:
  BACKEND_BIND       Default: 127.0.0.1
  BACKEND_PORT       Default: 8787
  PYTHON_BIN         Default: python3
  BACKEND_CMD        Default: python3 scripts/mcsr_cache_service.py --bind \$BACKEND_BIND --port \$BACKEND_PORT ...
  TUNNEL_MODE        Default: off (off|quick|named)
  CLOUDFLARED_BIN    Default: cloudflared
  CLOUDFLARED_CONFIG Default: \$ROOT_DIR/cloudflared/mcsr_config.yml
  TUNNEL_URL         Default: http://\$BACKEND_BIND:\$BACKEND_PORT
  MCSR_CACHE_SERVER_URL Optional public URL for external health check (ex: https://mcsr.workingledger.com)
  MCSR_HEALTH_LOCAL_URL Override local health URL (default: http://\$BACKEND_BIND:\$BACKEND_PORT/health)
  MCSR_HEALTH_RETRIES  Default: 15
  MCSR_HEALTH_RETRY_DELAY_S Default: 1
  MCSR_CACHE_AUTH_TOKEN Optional shared auth token for cache service
  MCSR_CACHE_AUTH_HEADER Optional auth header name (default: x-toolscreen-token)
  MCSR_CACHE_PUBLIC_TOKEN Optional public token (used for public /health checks)
  MCSR_KEYS_FILE Optional auth-key env file path (default: \$XDG_STATE_HOME/toolscreen/mcsr-host-keys.env or ~/.local/state/toolscreen/mcsr-host-keys.env)
EOF
}

command="${1:-}"
if [[ $# -gt 0 ]]; then
  shift
fi
load_keys_from_file_if_available
case "$command" in
  start)
    start_daemon
    ;;
  stop)
    stop_daemon
    ;;
  status)
    status_daemon
    ;;
  clean)
    clean_orphans
    ;;
  reset)
    reset_daemon
    ;;
  stopclean)
    reset_daemon
    ;;
  health)
    health_checks
    ;;
  rotate-keys)
    rotate_auth_keys
    ;;
  show-keys)
    echo "Keys file: $KEYS_FILE"
    if [[ -f "$KEYS_FILE" ]]; then
      ls -l "$KEYS_FILE"
    else
      echo "No keys file yet. Run: $(basename "$0") rotate-keys"
    fi
    echo "Load keys into this shell: source \"$KEYS_FILE\""
    ;;
  startup)
    startup_sequence
    ;;
  restart)
    restart_sequence
    ;;
  up)
    startup_sequence
    ;;
  logs)
    show_logs "$@"
    ;;
  run)
    run_supervisor
    ;;
  *)
    usage
    exit 1
    ;;
esac
