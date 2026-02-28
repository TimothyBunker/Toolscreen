#!/usr/bin/env bash
# Safe diagnostics runner for MCSR cache + split analytics.
# Does not exit your interactive shell on errors.
set -u

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR" || exit 1

LOG_DIR="$ROOT_DIR/.logs"
mkdir -p "$LOG_DIR"
STAMP="$(date +%Y%m%d_%H%M%S)"
OUT_FILE="$LOG_DIR/mcsr_validate_${STAMP}.log"
TMP_BODY="/tmp/mcsr_probe_body_${STAMP}.txt"

exec > >(tee -a "$OUT_FILE") 2>&1

echo "=== MCSR Validation ==="
echo "timestamp: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "root: $ROOT_DIR"
echo

run_cmd() {
  local desc="$1"
  shift
  echo "--- $desc ---"
  "$@"
  local rc=$?
  echo "[exit=$rc]"
  echo
  return 0
}

get_keys_file() {
  local from_show
  from_show="$(./mcsr_host.sh show-keys 2>/dev/null | awk -F': ' '/^Keys file:/ {print $2; exit}')"
  if [[ -n "$from_show" && -f "$from_show" ]]; then
    echo "$from_show"
    return 0
  fi
  if [[ -f "$ROOT_DIR/.run/mcsr-host-keys.env" ]]; then
    echo "$ROOT_DIR/.run/mcsr-host-keys.env"
    return 0
  fi
  echo ""
  return 1
}

probe() {
  local label="$1"
  local token="$2"
  local url="$3"
  local code

  if [[ -n "$token" ]]; then
    code="$(curl -sS -o "$TMP_BODY" -w '%{http_code}' -H "${MCSR_CACHE_AUTH_HEADER}: ${token}" "$url" || echo "000")"
  else
    code="$(curl -sS -o "$TMP_BODY" -w '%{http_code}' "$url" || echo "000")"
  fi

  echo "$label => $code ($url)"
  if [[ -f "$TMP_BODY" ]]; then
    head -c 180 "$TMP_BODY"
    echo
  else
    echo "(no response body)"
  fi
  echo
}

KEYS_FILE="$(get_keys_file || true)"
if [[ -n "$KEYS_FILE" ]]; then
  # shellcheck disable=SC1090
  source "$KEYS_FILE"
  echo "keys_file: $KEYS_FILE"
else
  echo "keys_file: (not found)"
fi

MCSR_CACHE_AUTH_HEADER="${MCSR_CACHE_AUTH_HEADER:-x-toolscreen-token}"
MCSR_CACHE_AUTH_TOKEN="${MCSR_CACHE_AUTH_TOKEN:-}"
MCSR_CACHE_PUBLIC_TOKEN="${MCSR_CACHE_PUBLIC_TOKEN:-}"
MCSR_CACHE_SERVER_URL="${MCSR_CACHE_SERVER_URL:-}"

echo "auth_header: $MCSR_CACHE_AUTH_HEADER"
if [[ -n "$MCSR_CACHE_AUTH_TOKEN" ]]; then
  echo "private_token: ${MCSR_CACHE_AUTH_TOKEN:0:8}..."
else
  echo "private_token: (unset)"
fi
if [[ -n "$MCSR_CACHE_PUBLIC_TOKEN" ]]; then
  echo "public_token: ${MCSR_CACHE_PUBLIC_TOKEN:0:8}..."
else
  echo "public_token: (unset)"
fi
if [[ -n "$MCSR_CACHE_SERVER_URL" ]]; then
  echo "public_base: $MCSR_CACHE_SERVER_URL"
else
  echo "public_base: (unset)"
fi
echo

run_cmd "Host Status" ./mcsr_host.sh status
run_cmd "Host Health" ./mcsr_host.sh health

echo "=== Endpoints (Local) ==="
probe "local health private" "$MCSR_CACHE_AUTH_TOKEN" "http://127.0.0.1:8787/health"
probe "local health public" "$MCSR_CACHE_PUBLIC_TOKEN" "http://127.0.0.1:8787/health"
probe "local user private" "$MCSR_CACHE_AUTH_TOKEN" "http://127.0.0.1:8787/api/users/SaladIsBoned"
probe "local user public (expect 401)" "$MCSR_CACHE_PUBLIC_TOKEN" "http://127.0.0.1:8787/api/users/SaladIsBoned"
probe "local user none (expect 401)" "" "http://127.0.0.1:8787/api/users/SaladIsBoned"

if [[ -n "$MCSR_CACHE_SERVER_URL" ]]; then
  BASE="${MCSR_CACHE_SERVER_URL%/}"
  echo "=== Endpoints (Public) ==="
  probe "public health public" "$MCSR_CACHE_PUBLIC_TOKEN" "${BASE}/health"
  probe "public user private" "$MCSR_CACHE_AUTH_TOKEN" "${BASE}/api/users/SaladIsBoned"
  probe "public user public (expect 401)" "$MCSR_CACHE_PUBLIC_TOKEN" "${BASE}/api/users/SaladIsBoned"
fi

echo "=== DB Counts ==="
python3 - <<'PY'
import sqlite3
conn = sqlite3.connect("data/mcsr_stats.db")
cur = conn.cursor()
queries = [
    ("users", "select count(*) from users"),
    ("matches", "select count(*) from matches"),
    ("match_players", "select count(*) from match_players"),
    ("match_splits", "select count(*) from match_splits"),
    ("known_usernames", "select count(*) from known_usernames"),
]
for name, q in queries:
    try:
        n = cur.execute(q).fetchone()[0]
    except Exception as e:
        n = f"ERR {e}"
    print(f"{name}={n}")
PY
echo

echo "=== Split Analytics (Before) ==="
python3 scripts/mcsr_stats_db.py --db data/mcsr_stats.db compare-splits SaladIsBoned || true
echo

SPLIT_ROWS="$(python3 - <<'PY'
import sqlite3
conn = sqlite3.connect("data/mcsr_stats.db")
try:
    n = conn.execute("select count(*) from match_splits").fetchone()[0]
except Exception:
    n = 0
print(n)
PY
)"

if [[ "$SPLIT_ROWS" == "0" ]]; then
  echo "=== Split rows are 0; attempting targeted detail sync via local cache server ==="
  SYNC_ARGS=(
    --db data/mcsr_stats.db
    --base-url http://127.0.0.1:8787
    --timeout 20
  )
  if [[ -n "$MCSR_CACHE_AUTH_TOKEN" ]]; then
    # Use --arg=value form so argparse treats leading '-' in token as a value, not a flag.
    SYNC_ARGS+=("--api-key=${MCSR_CACHE_AUTH_TOKEN}")
    SYNC_ARGS+=("--api-key-header=${MCSR_CACHE_AUTH_HEADER}")
  fi
  python3 scripts/mcsr_stats_db.py \
    "${SYNC_ARGS[@]}" \
    sync-user SaladIsBoned --pages 3 || true
  echo
  echo "=== Split Analytics (After) ==="
  python3 scripts/mcsr_stats_db.py --db data/mcsr_stats.db compare-splits SaladIsBoned || true
  echo
fi

echo "=== Done ==="
echo "report_file: $OUT_FILE"
echo "Paste that file's content here."
