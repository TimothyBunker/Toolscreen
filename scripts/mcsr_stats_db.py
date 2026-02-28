#!/usr/bin/env python3
"""
Build and maintain a local SQLite DB for MCSR analytics.

Use this to:
- harvest usernames at scale (`sync-usernames`)
- sync per-user profiles/matches/splits (`sync-user`, `bulk-sync`)
- compare split timings against global averages (`compare-splits`)
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import socket
import sqlite3
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Dict, Iterable, List, Optional, Tuple


def utc_now_iso() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def clamp(v: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, v))


def safe_int(v, default: int = 0) -> int:
    try:
        if v is None:
            return default
        return int(v)
    except Exception:
        return default


def first_dict(container: dict, keys: Iterable[str]) -> dict:
    for key in keys:
        candidate = container.get(key)
        if isinstance(candidate, dict):
            return candidate
    return {}


def ranked_count(obj: dict, key: str) -> int:
    bucket = obj.get(key)
    if isinstance(bucket, dict):
        value = bucket.get("ranked")
        if isinstance(value, (int, float)):
            return int(value)
    if isinstance(bucket, (int, float)):
        return int(bucket)
    return 0


def extract_forfeit_rate_percent(data_obj: dict) -> Optional[float]:
    def read_rate(container: dict) -> Optional[float]:
        if not isinstance(container, dict):
            return None
        for key in ("forfeitRate", "forfeitRatePercent", "ffRate"):
            value = container.get(key)
            if isinstance(value, dict):
                if isinstance(value.get("ranked"), (int, float)):
                    value = value["ranked"]
                elif isinstance(value.get("all"), (int, float)):
                    value = value["all"]
                elif isinstance(value.get("value"), (int, float)):
                    value = value["value"]
                else:
                    value = None
            if isinstance(value, (int, float)):
                rate = float(value)
                if 0.0 <= rate <= 1.0:
                    rate *= 100.0
                return clamp(rate, 0.0, 100.0)
        return None

    stats = data_obj.get("statistics") if isinstance(data_obj.get("statistics"), dict) else {}
    overall = first_dict(stats, ("all", "allTime", "overall", "global", "lifetime"))
    season = stats.get("season") if isinstance(stats.get("season"), dict) else {}

    for source in (overall, data_obj, stats, season):
        rate = read_rate(source)
        if rate is not None:
            return rate

    wins = ranked_count(overall, "wins")
    losses = ranked_count(overall, "loses") or ranked_count(overall, "losses")
    ffs = ranked_count(overall, "ffs")
    total = max(0, wins + losses)
    if total > 0:
        return clamp((100.0 * ffs) / total, 0.0, 100.0)
    return None


def extract_profile_metrics(data_obj: dict) -> dict:
    stats = data_obj.get("statistics") if isinstance(data_obj.get("statistics"), dict) else {}
    season = stats.get("season") if isinstance(stats.get("season"), dict) else {}
    overall = first_dict(stats, ("all", "allTime", "overall", "global", "lifetime"))

    best_time_ms = 0
    best_ws = 0
    avg_ms = 0
    avg_priority = -1

    def read_avg(container: dict) -> int:
        if not isinstance(container, dict):
            return 0
        for key in ("averageTime", "avgTime"):
            value = container.get(key)
            if isinstance(value, dict):
                if isinstance(value.get("ranked"), (int, float)):
                    return safe_int(value.get("ranked"), 0)
                if isinstance(value.get("all"), (int, float)):
                    return safe_int(value.get("all"), 0)
                if isinstance(value.get("value"), (int, float)):
                    return safe_int(value.get("value"), 0)
                continue
            if isinstance(value, (int, float)):
                return safe_int(value, 0)
        return 0

    def consider_avg(ms: int, priority: int) -> None:
        nonlocal avg_ms, avg_priority
        if ms <= 0:
            return
        if priority > avg_priority:
            avg_priority = priority
            avg_ms = ms

    achievements = data_obj.get("achievements")
    display = achievements.get("display") if isinstance(achievements, dict) else []
    if isinstance(display, list):
        for row in display:
            if not isinstance(row, dict):
                continue
            aid = str(row.get("id", "")).strip().lower()
            value = safe_int(row.get("value"), 0)
            if aid == "besttime":
                best_time_ms = max(best_time_ms, value)
            elif aid == "highestwinstreak":
                best_ws = max(best_ws, value)
            elif aid in ("averagetime", "avgtime"):
                consider_avg(value, 260)

    consider_avg(read_avg(overall), 320)
    consider_avg(read_avg(data_obj), 220)
    consider_avg(read_avg(season), 120)
    consider_avg(read_avg(stats), 80)

    return {
        "season_wins": ranked_count(season, "wins"),
        "season_losses": ranked_count(season, "loses") or ranked_count(season, "losses"),
        "season_completions": ranked_count(season, "completions"),
        "season_points": ranked_count(season, "points"),
        "best_time_ms": best_time_ms,
        "average_time_ms": avg_ms,
        "best_winstreak": best_ws,
        "forfeit_rate_percent": extract_forfeit_rate_percent(data_obj),
    }


class ApiClient:
    def __init__(
        self,
        base_url: str,
        timeout_s: float,
        api_key: str,
        api_key_header: str,
        min_interval_s: float,
        max_retries: int = 4,
    ) -> None:
        self.base_url = base_url.rstrip("/")
        self.timeout_s = timeout_s
        self.api_key = api_key.strip()
        self.api_key_header = (api_key_header or "x-api-key").strip()
        self.min_interval_s = max(0.0, min_interval_s)
        self.max_retries = max(0, max_retries)
        self._last_request_time = 0.0

    def _sleep_for_rate_limit(self) -> None:
        if self.min_interval_s <= 0.0:
            return
        now = time.time()
        elapsed = now - self._last_request_time
        if elapsed < self.min_interval_s:
            time.sleep(self.min_interval_s - elapsed)

    def get(self, path: str) -> dict:
        url = self.base_url + path
        attempt = 0
        while True:
            attempt += 1
            self._sleep_for_rate_limit()

            req = urllib.request.Request(url, method="GET")
            req.add_header("Accept", "application/json")
            if self.api_key:
                req.add_header(self.api_key_header, self.api_key)

            try:
                with urllib.request.urlopen(req, timeout=self.timeout_s) as resp:
                    self._last_request_time = time.time()
                    body = resp.read().decode("utf-8", errors="replace")
                    payload = json.loads(body)
                    return payload
            except urllib.error.HTTPError as e:
                self._last_request_time = time.time()
                body = e.read().decode("utf-8", errors="replace")
                retry_after = e.headers.get("retry-after")
                wait_s = 0.0
                if retry_after:
                    try:
                        wait_s = float(retry_after)
                    except Exception:
                        wait_s = 0.0

                if e.code == 429 and attempt <= self.max_retries + 1:
                    sleep_s = max(wait_s, 5.0)
                    print(f"[429] rate limited on {path}, waiting {sleep_s:.1f}s...", file=sys.stderr)
                    time.sleep(sleep_s)
                    continue
                if 500 <= e.code < 600 and attempt <= self.max_retries + 1:
                    sleep_s = min(20.0, 0.8 * (2 ** (attempt - 1)))
                    print(f"[{e.code}] server error on {path}, retrying in {sleep_s:.1f}s...", file=sys.stderr)
                    time.sleep(sleep_s)
                    continue
                raise RuntimeError(f"HTTP {e.code} {path}: {body[:240]}") from None
            except urllib.error.URLError as e:
                if attempt <= self.max_retries + 1:
                    sleep_s = min(20.0, 0.8 * (2 ** (attempt - 1)))
                    print(f"[net] {path}: {e}. retrying in {sleep_s:.1f}s...", file=sys.stderr)
                    time.sleep(sleep_s)
                    continue
                raise RuntimeError(f"Network failure for {path}: {e}") from None
            except (TimeoutError, socket.timeout) as e:
                if attempt <= self.max_retries + 1:
                    sleep_s = min(20.0, 0.8 * (2 ** (attempt - 1)))
                    print(f"[timeout] {path}: {e}. retrying in {sleep_s:.1f}s...", file=sys.stderr)
                    time.sleep(sleep_s)
                    continue
                raise RuntimeError(f"Timeout for {path}: {e}") from None


def open_db(path: str, readonly: bool = False) -> sqlite3.Connection:
    if readonly:
        abs_path = os.path.abspath(path)
        uri = f"file:{abs_path}?mode=ro"
        conn = sqlite3.connect(uri, uri=True)
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA query_only=ON;")
        return conn

    conn = sqlite3.connect(path)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL;")
    conn.execute("PRAGMA synchronous=NORMAL;")
    conn.execute("PRAGMA foreign_keys=ON;")
    return conn


def init_db(conn: sqlite3.Connection) -> None:
    conn.executescript(
        """
        CREATE TABLE IF NOT EXISTS meta (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS known_usernames (
            username TEXT PRIMARY KEY,
            source TEXT NOT NULL,
            first_seen_utc TEXT NOT NULL,
            last_seen_utc TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS users (
            uuid TEXT PRIMARY KEY,
            nickname TEXT NOT NULL UNIQUE,
            country TEXT,
            elo_rate INTEGER,
            elo_rank INTEGER,
            peak_elo INTEGER,
            updated_utc TEXT NOT NULL,
            raw_json TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS user_snapshots (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            nickname TEXT NOT NULL,
            uuid TEXT,
            elo_rate INTEGER,
            elo_rank INTEGER,
            season_wins INTEGER,
            season_losses INTEGER,
            season_completions INTEGER,
            season_points INTEGER,
            best_time_ms INTEGER,
            average_time_ms INTEGER,
            forfeit_rate_percent REAL,
            best_win_streak INTEGER,
            polled_utc TEXT NOT NULL,
            raw_json TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS matches (
            match_id TEXT PRIMARY KEY,
            type INTEGER,
            season INTEGER,
            category TEXT,
            game_mode TEXT,
            date_epoch INTEGER,
            forfeited INTEGER NOT NULL DEFAULT 0,
            result_uuid TEXT,
            result_name TEXT,
            result_time_ms INTEGER,
            raw_json TEXT NOT NULL,
            updated_utc TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS match_players (
            match_id TEXT NOT NULL,
            player_uuid TEXT,
            player_name TEXT,
            elo_rate INTEGER,
            elo_delta INTEGER,
            elo_after INTEGER,
            PRIMARY KEY (match_id, player_uuid, player_name)
        );

        CREATE TABLE IF NOT EXISTS user_matches (
            tracked_nickname TEXT NOT NULL,
            match_id TEXT NOT NULL,
            opponent_name TEXT,
            outcome TEXT NOT NULL,
            result_time_ms INTEGER,
            forfeited INTEGER NOT NULL DEFAULT 0,
            page_index INTEGER NOT NULL DEFAULT 0,
            category TEXT,
            game_mode TEXT,
            date_epoch INTEGER,
            PRIMARY KEY (tracked_nickname, match_id)
        );

        CREATE TABLE IF NOT EXISTS match_splits (
            match_id TEXT NOT NULL,
            player_uuid TEXT NOT NULL,
            split_type INTEGER NOT NULL,
            time_ms INTEGER NOT NULL,
            PRIMARY KEY (match_id, player_uuid, split_type)
        );
        """
    )
    conn.execute(
        "INSERT INTO meta(key, value) VALUES('schema_version', '1') "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value"
    )
    conn.commit()


def upsert_known_username(conn: sqlite3.Connection, username: str, source: str) -> bool:
    username = (username or "").strip()
    if not username:
        return False
    now = utc_now_iso()
    row = conn.execute("SELECT username FROM known_usernames WHERE username = ?", (username,)).fetchone()
    if row:
        conn.execute(
            "UPDATE known_usernames SET last_seen_utc = ?, source = ? WHERE username = ?",
            (now, source, username),
        )
        return False
    conn.execute(
        "INSERT INTO known_usernames(username, source, first_seen_utc, last_seen_utc) VALUES(?, ?, ?, ?)",
        (username, source, now, now),
    )
    return True


def extract_player_entries(match_obj: dict) -> List[Tuple[str, str]]:
    out: List[Tuple[str, str]] = []
    players = match_obj.get("players")
    if not isinstance(players, list):
        return out
    for p in players:
        if not isinstance(p, dict):
            continue
        uuid = str(p.get("uuid") or "").strip()
        nickname = str(p.get("nickname") or p.get("mc_name") or p.get("name") or "").strip()
        if not nickname and isinstance(p.get("user"), dict):
            u = p["user"]
            nickname = str(u.get("nickname") or u.get("mc_name") or u.get("name") or "").strip()
        out.append((uuid, nickname))
    return out


def sync_usernames(
    conn: sqlite3.Connection,
    api: ApiClient,
    max_match_pages: int,
    match_page_start: int = 0,
    commit_every_pages: int = 25,
) -> None:
    added = 0
    seen = 0
    start_page = max(0, int(match_page_start))
    pages_limit = max(1, int(max_match_pages))
    commit_every = max(1, int(commit_every_pages))

    lb = api.get("/api/leaderboard")
    lb_users = (lb.get("data") or {}).get("users") if isinstance(lb.get("data"), dict) else []
    if isinstance(lb_users, list):
        for row in lb_users:
            if not isinstance(row, dict):
                continue
            nick = str(row.get("nickname") or "").strip()
            if not nick:
                continue
            seen += 1
            if upsert_known_username(conn, nick, "leaderboard"):
                added += 1

    rec = api.get("/api/record-leaderboard")
    rec_rows = rec.get("data")
    if isinstance(rec_rows, list):
        for row in rec_rows:
            if not isinstance(row, dict):
                continue
            user = row.get("user") if isinstance(row.get("user"), dict) else {}
            nick = str(user.get("nickname") or "").strip()
            if not nick:
                continue
            seen += 1
            if upsert_known_username(conn, nick, "record-leaderboard"):
                added += 1

    pages_scanned = 0
    last_page = start_page - 1
    for page in range(start_page, start_page + pages_limit):
        try:
            payload = api.get(f"/api/matches?page={page}")
        except Exception as e:
            print(f"[sync-usernames] page {page} failed: {e}. stopping page scan.", file=sys.stderr)
            break
        rows = payload.get("data")
        if not isinstance(rows, list) or not rows:
            break
        pages_scanned += 1
        last_page = page
        for match in rows:
            if not isinstance(match, dict):
                continue
            for _, nick in extract_player_entries(match):
                if not nick:
                    continue
                seen += 1
                if upsert_known_username(conn, nick, "matches-feed"):
                    added += 1
        if (pages_scanned % commit_every) == 0:
            conn.commit()
    conn.commit()
    print(
        f"[sync-usernames] processed={seen} new={added} "
        f"match_pages_scanned={pages_scanned} start_page={start_page} last_page={last_page}"
    )


def _match_result_outcome(match_obj: dict, tracked_uuid: str, tracked_nick: str) -> str:
    result = match_obj.get("result") if isinstance(match_obj.get("result"), dict) else {}
    result_uuid = str(result.get("uuid") or "").strip().lower()
    if not result_uuid:
        return "DRAW"
    if tracked_uuid and result_uuid == tracked_uuid.lower():
        return "WON"
    if tracked_nick:
        winner_name = str(result.get("nickname") or result.get("name") or "").strip()
        if winner_name and winner_name.lower() == tracked_nick.lower():
            return "WON"
    return "LOST"


def _opponent_name(match_obj: dict, tracked_uuid: str, tracked_nick: str) -> str:
    players = extract_player_entries(match_obj)
    if not players:
        return "Unknown"
    for puuid, pname in players:
        if tracked_uuid and puuid and puuid.lower() == tracked_uuid.lower():
            continue
        if tracked_nick and pname and pname.lower() == tracked_nick.lower():
            continue
        if pname:
            return pname
    return "Unknown"


def _upsert_match(conn: sqlite3.Connection, match_obj: dict) -> str:
    match_id = str(match_obj.get("id") or "").strip()
    if not match_id:
        return ""
    result = match_obj.get("result") if isinstance(match_obj.get("result"), dict) else {}
    conn.execute(
        """
        INSERT INTO matches(
            match_id, type, season, category, game_mode, date_epoch, forfeited,
            result_uuid, result_name, result_time_ms, raw_json, updated_utc
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(match_id) DO UPDATE SET
            type=excluded.type,
            season=excluded.season,
            category=excluded.category,
            game_mode=excluded.game_mode,
            date_epoch=excluded.date_epoch,
            forfeited=excluded.forfeited,
            result_uuid=excluded.result_uuid,
            result_name=excluded.result_name,
            result_time_ms=excluded.result_time_ms,
            raw_json=excluded.raw_json,
            updated_utc=excluded.updated_utc
        """,
        (
            match_id,
            safe_int(match_obj.get("type")),
            safe_int(match_obj.get("season")),
            str(match_obj.get("category") or ""),
            str(match_obj.get("gameMode") or ""),
            safe_int(match_obj.get("date")),
            1 if bool(match_obj.get("forfeited")) else 0,
            str(result.get("uuid") or ""),
            str(result.get("nickname") or result.get("name") or ""),
            safe_int(result.get("time")),
            json.dumps(match_obj, separators=(",", ":"), ensure_ascii=False),
            utc_now_iso(),
        ),
    )
    return match_id


def _upsert_match_players(conn: sqlite3.Connection, match_id: str, match_obj: dict) -> None:
    for p in match_obj.get("players", []):
        if not isinstance(p, dict):
            continue
        user = p.get("user") if isinstance(p.get("user"), dict) else {}
        puid = str(p.get("uuid") or user.get("uuid") or "").strip()
        pname = str(p.get("nickname") or p.get("mc_name") or p.get("name") or user.get("nickname") or "").strip()
        elo_after = safe_int(p.get("eloRate"), 0)
        if not puid and not pname:
            continue
        conn.execute(
            """
            INSERT OR REPLACE INTO match_players(
                match_id, player_uuid, player_name, elo_rate, elo_delta, elo_after
            ) VALUES (?, ?, ?, ?, ?, ?)
            """,
            (
                match_id,
                puid,
                pname,
                safe_int(p.get("eloRate"), 0),
                safe_int(p.get("change"), 0),
                elo_after,
            ),
        )
        if pname:
            upsert_known_username(conn, pname, "match-players")


def _upsert_user_match_row(
    conn: sqlite3.Connection,
    tracked_nick: str,
    match_id: str,
    match_obj: dict,
    tracked_uuid: str,
    page_index: int,
) -> None:
    result = match_obj.get("result") if isinstance(match_obj.get("result"), dict) else {}
    conn.execute(
        """
        INSERT OR REPLACE INTO user_matches(
            tracked_nickname, match_id, opponent_name, outcome, result_time_ms, forfeited,
            page_index, category, game_mode, date_epoch
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        (
            tracked_nick,
            match_id,
            _opponent_name(match_obj, tracked_uuid, tracked_nick),
            _match_result_outcome(match_obj, tracked_uuid, tracked_nick),
            safe_int(result.get("time"), 0),
            1 if bool(match_obj.get("forfeited")) else 0,
            page_index,
            str(match_obj.get("category") or ""),
            str(match_obj.get("gameMode") or ""),
            safe_int(match_obj.get("date"), 0),
        ),
    )


def _store_match_splits(conn: sqlite3.Connection, match_id: str, detail_obj: dict) -> int:
    stored = 0
    timelines = detail_obj.get("timelines")
    if not isinstance(timelines, list):
        return 0
    for row in timelines:
        if not isinstance(row, dict):
            continue
        player_uuid = str(row.get("uuid") or "").strip()
        split_type = safe_int(row.get("type"), -1)
        split_time = safe_int(row.get("time"), -1)
        if not player_uuid or split_type < 0 or split_time < 0:
            continue
        conn.execute(
            """
            INSERT OR REPLACE INTO match_splits(match_id, player_uuid, split_type, time_ms)
            VALUES (?, ?, ?, ?)
            """,
            (match_id, player_uuid, split_type, split_time),
        )
        stored += 1
    return stored


def sync_user(conn: sqlite3.Connection, api: ApiClient, identifier: str, pages: int, fetch_details: bool) -> None:
    ident_enc = urllib.parse.quote(identifier.strip(), safe="")
    profile_payload = api.get(f"/api/users/{ident_enc}")
    profile_data = profile_payload.get("data")
    if not isinstance(profile_data, dict):
        raise RuntimeError(f"profile not found for '{identifier}'")

    nickname = str(profile_data.get("nickname") or identifier).strip()
    uuid = str(profile_data.get("uuid") or "").strip()
    if nickname:
        upsert_known_username(conn, nickname, "profile")

    metrics = extract_profile_metrics(profile_data)
    conn.execute(
        """
        INSERT INTO users(uuid, nickname, country, elo_rate, elo_rank, peak_elo, updated_utc, raw_json)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(uuid) DO UPDATE SET
            nickname=excluded.nickname,
            country=excluded.country,
            elo_rate=excluded.elo_rate,
            elo_rank=excluded.elo_rank,
            peak_elo=excluded.peak_elo,
            updated_utc=excluded.updated_utc,
            raw_json=excluded.raw_json
        """,
        (
            uuid or f"nick:{nickname}",
            nickname,
            str(profile_data.get("country") or ""),
            safe_int(profile_data.get("eloRate"), 0),
            safe_int(profile_data.get("eloRank"), 0),
            safe_int(profile_data.get("peakElo") or profile_data.get("eloPeak"), safe_int(profile_data.get("eloRate"), 0)),
            utc_now_iso(),
            json.dumps(profile_data, separators=(",", ":"), ensure_ascii=False),
        ),
    )
    conn.execute(
        """
        INSERT INTO user_snapshots(
            nickname, uuid, elo_rate, elo_rank, season_wins, season_losses, season_completions,
            season_points, best_time_ms, average_time_ms, forfeit_rate_percent, best_win_streak,
            polled_utc, raw_json
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        (
            nickname,
            uuid,
            safe_int(profile_data.get("eloRate"), 0),
            safe_int(profile_data.get("eloRank"), 0),
            metrics["season_wins"],
            metrics["season_losses"],
            metrics["season_completions"],
            metrics["season_points"],
            metrics["best_time_ms"],
            metrics["average_time_ms"],
            metrics["forfeit_rate_percent"],
            metrics["best_winstreak"],
            utc_now_iso(),
            json.dumps(profile_data, separators=(",", ":"), ensure_ascii=False),
        ),
    )

    new_match_ids: List[str] = []
    total_matches = 0
    for page in range(max(1, pages)):
        payload = api.get(f"/api/users/{ident_enc}/matches?page={page}")
        rows = payload.get("data")
        if not isinstance(rows, list) or not rows:
            break
        for match in rows:
            if not isinstance(match, dict):
                continue
            match_id = _upsert_match(conn, match)
            if not match_id:
                continue
            _upsert_match_players(conn, match_id, match)
            _upsert_user_match_row(conn, nickname, match_id, match, uuid, page)
            total_matches += 1

            exists = conn.execute("SELECT 1 FROM match_splits WHERE match_id = ? LIMIT 1", (match_id,)).fetchone()
            if exists is None:
                new_match_ids.append(match_id)

    # Derive "completion avg" from tracked user's own completed wins in fetched pages.
    completion_avg_row = conn.execute(
        """
        SELECT AVG(result_time_ms) AS avg_ms
        FROM user_matches
        WHERE tracked_nickname = ?
          AND outcome = 'WON'
          AND forfeited = 0
          AND result_time_ms > 0
          AND page_index >= 0
        """,
        (nickname,),
    ).fetchone()
    if completion_avg_row is not None and completion_avg_row["avg_ms"] is not None:
        completion_avg_ms = int(round(float(completion_avg_row["avg_ms"])))
        conn.execute(
            """
            UPDATE user_snapshots
            SET average_time_ms = COALESCE(NULLIF(average_time_ms, 0), ?)
            WHERE id = (SELECT MAX(id) FROM user_snapshots WHERE nickname = ?)
            """,
            (completion_avg_ms, nickname),
        )

    split_rows = 0
    if fetch_details:
        for mid in new_match_ids:
            detail_payload = api.get(f"/api/matches/{urllib.parse.quote(mid, safe='')}")
            detail_data = detail_payload.get("data")
            if not isinstance(detail_data, dict):
                continue
            split_rows += _store_match_splits(conn, mid, detail_data)

    conn.commit()
    print(
        f"[sync-user] user={nickname} pages={max(1, pages)} matches={total_matches} "
        f"new_detail_matches={len(new_match_ids)} split_rows={split_rows}"
    )


def bulk_sync(conn: sqlite3.Connection, api: ApiClient, limit: int, pages: int, fetch_details: bool) -> None:
    rows = conn.execute(
        "SELECT username FROM known_usernames ORDER BY last_seen_utc DESC, username ASC LIMIT ?",
        (max(1, limit),),
    ).fetchall()
    if not rows:
        print("[bulk-sync] no known usernames yet. run sync-usernames first.")
        return
    for idx, row in enumerate(rows, start=1):
        username = str(row["username"])
        print(f"[bulk-sync] ({idx}/{len(rows)}) {username}")
        try:
            sync_user(conn, api, username, pages=pages, fetch_details=fetch_details)
        except Exception as e:
            print(f"[bulk-sync] failed for {username}: {e}", file=sys.stderr)


def compare_splits(conn: sqlite3.Connection, nickname: str) -> None:
    user_row = conn.execute(
        "SELECT uuid, nickname FROM users WHERE LOWER(nickname) = LOWER(?) ORDER BY updated_utc DESC LIMIT 1",
        (nickname,),
    ).fetchone()
    if user_row is None:
        raise RuntimeError(f"user '{nickname}' not found in DB")
    uuid = str(user_row["uuid"])
    display_nick = str(user_row["nickname"])

    user_stats = conn.execute(
        """
        SELECT split_type, AVG(time_ms) AS avg_ms, COUNT(*) AS n
        FROM match_splits
        WHERE player_uuid = ?
        GROUP BY split_type
        ORDER BY split_type
        """,
        (uuid,),
    ).fetchall()
    global_stats = conn.execute(
        """
        SELECT split_type, AVG(time_ms) AS avg_ms, COUNT(*) AS n
        FROM match_splits
        WHERE player_uuid <> ?
        GROUP BY split_type
        ORDER BY split_type
        """,
        (uuid,),
    ).fetchall()

    global_by_type = {int(r["split_type"]): (float(r["avg_ms"]), int(r["n"])) for r in global_stats}
    print(f"Split comparison for {display_nick} ({uuid})")
    print("type | user_avg_ms | global_avg_ms | delta_ms | user_n | global_n")
    print("-----+-------------+---------------+----------+--------+---------")
    for r in user_stats:
        st = int(r["split_type"])
        u_avg = float(r["avg_ms"])
        u_n = int(r["n"])
        g_avg, g_n = global_by_type.get(st, (0.0, 0))
        delta = u_avg - g_avg if g_n > 0 else 0.0
        print(f"{st:>4} | {u_avg:>11.1f} | {g_avg:>13.1f} | {delta:>8.1f} | {u_n:>6} | {g_n:>7}")


def list_known_users(conn: sqlite3.Connection, limit: int) -> None:
    rows = conn.execute(
        "SELECT username, source, last_seen_utc FROM known_usernames ORDER BY last_seen_utc DESC, username ASC LIMIT ?",
        (max(1, limit),),
    ).fetchall()
    print(f"known usernames: {len(rows)} shown")
    for row in rows:
        print(f"- {row['username']} ({row['source']}, last_seen={row['last_seen_utc']})")


def count_known_usernames(conn: sqlite3.Connection) -> int:
    row = conn.execute("SELECT COUNT(*) AS c FROM known_usernames").fetchone()
    if row is None:
        return 0
    return int(row["c"])


def count_username_index_file(path: str) -> int:
    if not os.path.isfile(path):
        return 0
    count = 0
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.strip():
                count += 1
    return count


def export_username_index(conn: sqlite3.Connection, output_path: str) -> int:
    rows = conn.execute(
        "SELECT username FROM known_usernames ORDER BY LOWER(username) ASC, username ASC"
    ).fetchall()
    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    with open(output_path, "w", encoding="utf-8", newline="\n") as f:
        for row in rows:
            username = str(row["username"] or "").strip()
            if not username:
                continue
            f.write(username + "\n")
    return len(rows)


def _write_username_index_meta(meta_path: str, count: int, max_match_pages: int) -> None:
    payload = {
        "generated_utc": utc_now_iso(),
        "username_count": int(max(0, count)),
        "max_match_pages": int(max(1, max_match_pages)),
    }
    os.makedirs(os.path.dirname(meta_path) or ".", exist_ok=True)
    with open(meta_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(payload, f, indent=2, sort_keys=True)
        f.write("\n")


def build_username_index(
    conn: sqlite3.Connection,
    api: ApiClient,
    output_path: str,
    max_match_pages: int,
    match_page_start: int,
    commit_every_pages: int,
    refresh_days: float,
    force: bool,
) -> None:
    output_path = os.path.normpath(output_path)
    previous_count = count_username_index_file(output_path)
    refresh_s = max(0.0, float(refresh_days) * 24.0 * 60.0 * 60.0)
    if (not force) and os.path.isfile(output_path) and refresh_s > 0.0:
        age_s = time.time() - os.path.getmtime(output_path)
        if age_s >= 0.0 and age_s < refresh_s:
            remaining_h = max(0.0, (refresh_s - age_s) / 3600.0)
            print(
                f"[name-index] skip: {output_path} is fresh "
                f"({age_s/3600.0:.1f}h old, refresh window {refresh_days:.1f}d, ~{remaining_h:.1f}h remaining)"
            )
            return

    sync_usernames(
        conn,
        api,
        max_match_pages=max(1, max_match_pages),
        match_page_start=max(0, int(match_page_start)),
        commit_every_pages=max(1, int(commit_every_pages)),
    )
    count = export_username_index(conn, output_path=output_path)
    meta_path = output_path + ".meta.json"
    _write_username_index_meta(meta_path, count=count, max_match_pages=max_match_pages)
    print(f"[name-index] wrote {count} usernames -> {output_path}")
    delta = count - previous_count
    print(f"[name-index] delta vs previous file: {delta:+d}")
    print(f"[name-index] meta -> {meta_path}")


def export_username_index_only(
    conn: sqlite3.Connection,
    output_path: str,
    max_match_pages: int,
) -> None:
    output_path = os.path.normpath(output_path)
    previous_count = count_username_index_file(output_path)
    count = export_username_index(conn, output_path=output_path)
    meta_path = output_path + ".meta.json"
    _write_username_index_meta(meta_path, count=count, max_match_pages=max(1, max_match_pages))
    print(f"[name-index-export] wrote {count} usernames -> {output_path}")
    delta = count - previous_count
    print(f"[name-index-export] delta vs previous file: {delta:+d}")
    print(f"[name-index-export] meta -> {meta_path}")


def print_name_index_stats(conn: sqlite3.Connection, index_path: str) -> None:
    index_path = os.path.normpath(index_path)
    db_count = count_known_usernames(conn)
    file_count = count_username_index_file(index_path)
    print(f"[name-index-stats] db known_usernames: {db_count}")
    print(f"[name-index-stats] file usernames: {file_count}")
    print(f"[name-index-stats] file-db delta: {file_count - db_count:+d}")
    if os.path.isfile(index_path):
        age_s = max(0.0, time.time() - os.path.getmtime(index_path))
        print(f"[name-index-stats] file age: {age_s/3600.0:.1f}h")
    meta_path = index_path + ".meta.json"
    if os.path.isfile(meta_path):
        try:
            with open(meta_path, "r", encoding="utf-8") as f:
                meta = json.load(f)
            generated = str(meta.get("generated_utc") or "").strip()
            username_count = int(meta.get("username_count") or 0)
            max_pages = int(meta.get("max_match_pages") or 0)
            print(f"[name-index-stats] meta generated_utc: {generated}")
            print(f"[name-index-stats] meta username_count: {username_count}")
            print(f"[name-index-stats] meta max_match_pages: {max_pages}")
        except Exception as e:  # noqa: BLE001
            print(f"[name-index-stats] meta parse error: {e}", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description="MCSR local stats DB utility")
    parser.add_argument("--db", default="data/mcsr_stats.db", help="SQLite DB path")
    parser.add_argument("--base-url", default="https://mcsrranked.com", help="MCSR API base URL")
    parser.add_argument("--timeout", type=float, default=10.0, help="HTTP timeout (seconds)")
    parser.add_argument("--api-key", default=os.environ.get("MCSR_API_KEY", ""), help="Optional API key")
    parser.add_argument(
        "--api-key-header",
        default=os.environ.get("MCSR_API_KEY_HEADER", "x-api-key"),
        help="API key header name",
    )
    parser.add_argument(
        "--min-interval-ms",
        type=int,
        default=1300,
        help="Minimum delay between API calls (ms). Keep >=1200 without API key.",
    )
    parser.add_argument("--max-retries", type=int, default=6, help="HTTP retry attempts for network/timeout/5xx/429")

    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("init", help="Initialize DB schema")

    p_sync_names = sub.add_parser("sync-usernames", help="Harvest usernames from leaderboard + matches feed")
    p_sync_names.add_argument("--max-match-pages", type=int, default=120, help="Max /api/matches pages to scan")
    p_sync_names.add_argument("--match-page-start", type=int, default=0, help="Start page for /api/matches scan")
    p_sync_names.add_argument("--commit-every-pages", type=int, default=25, help="Commit DB every N scanned match pages")

    p_sync_user = sub.add_parser("sync-user", help="Sync one user's profile/matches/details")
    p_sync_user.add_argument("identifier", help="Nickname or UUID")
    p_sync_user.add_argument("--pages", type=int, default=2, help="Number of /users/{id}/matches pages")
    p_sync_user.add_argument("--no-details", action="store_true", help="Skip /api/matches/{id} detail fetch")

    p_bulk = sub.add_parser("bulk-sync", help="Sync many users from known_usernames table")
    p_bulk.add_argument("--limit", type=int, default=200, help="How many known usernames to sync")
    p_bulk.add_argument("--pages", type=int, default=1, help="Match pages per user")
    p_bulk.add_argument("--no-details", action="store_true", help="Skip /api/matches/{id} detail fetch")

    p_cmp = sub.add_parser("compare-splits", help="Compare a user's split times vs global")
    p_cmp.add_argument("nickname", help="User nickname already present in DB")

    p_list = sub.add_parser("list-users", help="List known harvested usernames")
    p_list.add_argument("--limit", type=int, default=50, help="Rows to show")

    p_name_index = sub.add_parser("name-index", help="Build fast plaintext username cache for search/autocomplete")
    p_name_index.add_argument("--output", default="mcsr_username_index.txt", help="Output txt file (1 username per line)")
    p_name_index.add_argument("--max-match-pages", type=int, default=120, help="Max /api/matches pages to scan")
    p_name_index.add_argument("--match-page-start", type=int, default=0, help="Start page for /api/matches scan")
    p_name_index.add_argument("--commit-every-pages", type=int, default=25, help="Commit DB every N scanned match pages")
    p_name_index.add_argument("--refresh-days", type=float, default=7.0, help="Skip rebuild if output is newer than this many days")
    p_name_index.add_argument("--force", action="store_true", help="Force rebuild even if cache is still fresh")

    p_export_index = sub.add_parser("export-name-index", help="Export username index from current DB only (no API calls)")
    p_export_index.add_argument("--output", default="mcsr_username_index.txt", help="Output txt file (1 username per line)")
    p_export_index.add_argument("--max-match-pages", type=int, default=120, help="Metadata only: last/expected match pages")

    p_index_stats = sub.add_parser("name-index-stats", help="Show username index counts from DB + file")
    p_index_stats.add_argument("--input", default="mcsr_username_index.txt", help="Index txt file path")

    args = parser.parse_args()
    os.makedirs(os.path.dirname(args.db) or ".", exist_ok=True)

    read_only_cmds = {"list-users", "compare-splits", "export-name-index", "name-index-stats"}
    readonly = args.cmd in read_only_cmds

    conn = open_db(args.db, readonly=readonly)
    try:
        if not readonly:
            init_db(conn)
        if args.cmd == "init":
            print(f"initialized {args.db}")
            return 0

        api = ApiClient(
            base_url=args.base_url,
            timeout_s=args.timeout,
            api_key=args.api_key,
            api_key_header=args.api_key_header,
            min_interval_s=max(0.0, args.min_interval_ms / 1000.0),
            max_retries=max(0, int(args.max_retries)),
        )

        if args.cmd == "sync-usernames":
            sync_usernames(
                conn,
                api,
                max_match_pages=max(1, args.max_match_pages),
                match_page_start=max(0, int(args.match_page_start)),
                commit_every_pages=max(1, int(args.commit_every_pages)),
            )
            return 0
        if args.cmd == "sync-user":
            sync_user(
                conn,
                api,
                identifier=args.identifier,
                pages=max(1, args.pages),
                fetch_details=not args.no_details,
            )
            return 0
        if args.cmd == "bulk-sync":
            bulk_sync(
                conn,
                api,
                limit=max(1, args.limit),
                pages=max(1, args.pages),
                fetch_details=not args.no_details,
            )
            return 0
        if args.cmd == "compare-splits":
            compare_splits(conn, nickname=args.nickname)
            return 0
        if args.cmd == "list-users":
            list_known_users(conn, limit=max(1, args.limit))
            return 0
        if args.cmd == "name-index":
            build_username_index(
                conn,
                api,
                output_path=args.output,
                max_match_pages=max(1, args.max_match_pages),
                match_page_start=max(0, int(args.match_page_start)),
                commit_every_pages=max(1, int(args.commit_every_pages)),
                refresh_days=max(0.0, args.refresh_days),
                force=bool(args.force),
            )
            return 0
        if args.cmd == "export-name-index":
            export_username_index_only(
                conn,
                output_path=args.output,
                max_match_pages=max(1, args.max_match_pages),
            )
            return 0
        if args.cmd == "name-index-stats":
            print_name_index_stats(
                conn,
                index_path=args.input,
            )
            return 0
    finally:
        conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
