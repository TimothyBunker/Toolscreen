#!/usr/bin/env python3
import argparse
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone


def now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def request_json(base_url: str, path: str, timeout_s: float, api_key: str, api_key_header: str):
    url = base_url.rstrip("/") + path
    req = urllib.request.Request(url, method="GET")
    req.add_header("Accept", "application/json")
    if api_key:
        req.add_header(api_key_header, api_key)

    try:
        with urllib.request.urlopen(req, timeout=timeout_s) as resp:
            status = resp.getcode()
            body = resp.read().decode("utf-8", errors="replace")
            headers = dict(resp.headers.items())
            return status, headers, body, None
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace")
        headers = dict(e.headers.items()) if e.headers else {}
        return e.code, headers, body, None
    except Exception as e:  # noqa: BLE001
        return 0, {}, "", str(e)


def print_response(label: str, status: int, headers: dict, body: str, err: str, max_body_chars: int):
    print(f"[{now_iso()}] {label}")
    if err:
        print(f"  error: {err}")
        return

    print(f"  status: {status}")
    for key in ("x-ratelimit-limit", "x-ratelimit-remaining", "x-ratelimit-reset", "retry-after"):
        if key in {k.lower(): v for k, v in headers.items()}:
            lowered = {k.lower(): v for k, v in headers.items()}
            print(f"  {key}: {lowered[key]}")

    preview = body.strip().replace("\n", " ")
    if max_body_chars > 0 and len(preview) > max_body_chars:
        preview = preview[:max_body_chars] + "..."
    print(f"  body: {preview}")


def parse_json(body: str):
    try:
        return json.loads(body)
    except Exception:  # noqa: BLE001
        return None


def _as_number(value):
    if isinstance(value, (int, float)):
        return float(value)
    return None


def _ranked_count(section) -> int:
    if not isinstance(section, dict):
        return 0
    ranked = section.get("ranked")
    if isinstance(ranked, (int, float)):
        return int(ranked)
    return 0


def _extract_profile_metrics(data: dict) -> dict:
    metrics = {
        "season_wins": 0,
        "season_losses": 0,
        "season_points": 0,
        "best_time_ms": 0,
        "avg_time_ms": 0,
        "best_winstreak": 0,
        "forfeit_rate_percent": None,
    }
    if not isinstance(data, dict):
        return metrics

    stats = data.get("statistics") if isinstance(data.get("statistics"), dict) else {}
    season = stats.get("season") if isinstance(stats.get("season"), dict) else {}

    metrics["season_wins"] = _ranked_count(season.get("wins"))
    metrics["season_losses"] = _ranked_count(season.get("loses") or season.get("losses"))
    metrics["season_points"] = _ranked_count(season.get("points"))

    overall = None
    for key in ("all", "allTime", "overall", "global", "lifetime"):
        candidate = stats.get(key)
        if isinstance(candidate, dict):
            overall = candidate
            break

    def try_forfeit_rate(container):
        if not isinstance(container, dict):
            return None
        for key in ("forfeitRate", "forfeitRatePercent", "ffRate"):
            value = _as_number(container.get(key))
            if value is None:
                continue
            if 0.0 <= value <= 1.0:
                value *= 100.0
            return max(0.0, min(100.0, value))
        return None

    for source in (data, season, overall, stats):
        rate = try_forfeit_rate(source)
        if rate is not None:
            metrics["forfeit_rate_percent"] = rate
            break

    if metrics["forfeit_rate_percent"] is None and isinstance(overall, dict):
        wins = _ranked_count(overall.get("wins"))
        losses = _ranked_count(overall.get("loses") or overall.get("losses"))
        ffs = _ranked_count(overall.get("ffs"))
        total = wins + losses
        if total > 0:
            metrics["forfeit_rate_percent"] = (100.0 * float(ffs)) / float(total)

    avg_time = None
    for source in (data, season, overall, stats):
        if not isinstance(source, dict):
            continue
        avg_time = source.get("averageTime")
        if isinstance(avg_time, (int, float)):
            metrics["avg_time_ms"] = int(avg_time)
            break

    achievements = data.get("achievements") if isinstance(data.get("achievements"), dict) else {}
    display = achievements.get("display") if isinstance(achievements.get("display"), list) else []
    for row in display:
        if not isinstance(row, dict):
            continue
        aid = str(row.get("id", "")).strip().lower()
        value = row.get("value")
        if not isinstance(value, (int, float)):
            continue
        ival = int(value)
        if aid == "besttime":
            metrics["best_time_ms"] = max(metrics["best_time_ms"], ival)
        elif aid == "highestwinstreak":
            metrics["best_winstreak"] = max(metrics["best_winstreak"], ival)
        elif aid in ("averagetime", "avgtime") and metrics["avg_time_ms"] <= 0:
            metrics["avg_time_ms"] = max(0, ival)

    return metrics


def _format_duration_ms(ms: int) -> str:
    if not isinstance(ms, int) or ms <= 0:
        return "--:--.--"
    total_seconds = ms // 1000
    minutes = total_seconds // 60
    seconds = total_seconds % 60
    centis = (ms % 1000) // 10
    return f"{minutes:02d}:{seconds:02d}.{centis:02d}"


def main():
    parser = argparse.ArgumentParser(description="Test MCSR Ranked API endpoints out-of-game.")
    parser.add_argument("identifier", help="MCSR username or UUID")
    parser.add_argument(
        "--base-url",
        default="auto",
        help="API base URL (use 'auto' to try mcsrranked.com then api.mcsrranked.com)",
    )
    parser.add_argument("--timeout", type=float, default=8.0, help="HTTP timeout in seconds")
    parser.add_argument("--api-key", default="", help="Optional API key value")
    parser.add_argument("--api-key-header", default="x-api-key", help="Header name for API key")
    parser.add_argument("--max-body-chars", type=int, default=300, help="Max preview chars for response body")
    parser.add_argument("--skip-match-detail", action="store_true", help="Skip GET /api/matches/{id}")
    args = parser.parse_args()

    api_key = args.api_key.strip() or os.environ.get("MCSR_API_KEY", "").strip()
    api_key_header = (args.api_key_header.strip() or os.environ.get("MCSR_API_KEY_HEADER", "x-api-key")).strip()

    if args.base_url.strip().lower() == "auto":
        base_urls = ["https://mcsrranked.com", "https://api.mcsrranked.com"]
    else:
        base_urls = [args.base_url]

    ident_enc = urllib.parse.quote(args.identifier, safe="")
    users_path = f"/api/users/{ident_enc}"
    matches_path = f"/api/users/{ident_enc}/matches?page=0"
    saw_any_http_response = False

    for base_url in base_urls:
        print(f"\n=== Base URL: {base_url} ===")

        status, headers, body, err = request_json(base_url, users_path, args.timeout, api_key, api_key_header)
        print_response(f"GET {users_path}", status, headers, body, err, args.max_body_chars)
        if err:
            continue
        saw_any_http_response = True
        user_json = parse_json(body)
        if status != 200 or not isinstance(user_json, dict):
            if status in (400, 404):
                print("  note: not-found response for this host/identifier")
            continue

        data = user_json.get("data") if isinstance(user_json.get("data"), dict) else {}
        nickname = data.get("nickname")
        uuid = data.get("uuid")
        print(f"  parsed.nickname: {nickname}")
        print(f"  parsed.uuid: {uuid}")
        metrics = _extract_profile_metrics(data)
        season_games = metrics["season_wins"] + metrics["season_losses"]
        season_wr = (100.0 * metrics["season_wins"] / season_games) if season_games > 0 else 0.0
        ff_rate = metrics["forfeit_rate_percent"]
        ff_str = "--" if ff_rate is None else f"{ff_rate:.1f}%"
        print(
            "  parsed.profile:"
            f" WR {season_wr:.1f}%"
            f" | PB {_format_duration_ms(metrics['best_time_ms'])}"
            f" | WS {metrics['best_winstreak']}"
            f" | Avg {_format_duration_ms(metrics['avg_time_ms'])}"
            f" | FF {ff_str}"
            f" | Points {metrics['season_points']}"
        )

        status, headers, body, err = request_json(base_url, matches_path, args.timeout, api_key, api_key_header)
        print_response(f"GET {matches_path}", status, headers, body, err, args.max_body_chars)
        if err or status != 200:
            continue

        matches_json = parse_json(body)
        if not isinstance(matches_json, dict):
            continue
        data_arr = matches_json.get("data")
        if not isinstance(data_arr, list) or not data_arr:
            print("  parsed.matches: empty")
            return 0

        first_match = data_arr[0] if isinstance(data_arr[0], dict) else {}
        match_id = first_match.get("id")
        print(f"  parsed.firstMatchId: {match_id}")
        if not match_id:
            return 0
        if args.skip_match_detail:
            print(f"\nSUCCESS host={base_url} user={nickname or args.identifier}")
            return 0

        match_enc = urllib.parse.quote(str(match_id), safe="")
        match_path = f"/api/matches/{match_enc}"
        status, headers, body, err = request_json(base_url, match_path, args.timeout, api_key, api_key_header)
        print_response(f"GET {match_path}", status, headers, body, err, args.max_body_chars)
        if err:
            return 1
        print(f"\nSUCCESS host={base_url} user={nickname or args.identifier}")
        return 0

    if not saw_any_http_response:
        print("\nFAILED: no HTTP response from any host (network/DNS issue).")
        return 2
    print("\nFAILED: identifier not found on tested hosts.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
