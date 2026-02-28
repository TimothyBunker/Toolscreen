#!/usr/bin/env python3
"""
Lightweight caching proxy for MCSR API endpoints.

Primary use:
- Run locally (127.0.0.1:8787) behind scripts/host_mcsr.sh
- Let Toolscreen query this service first, with automatic fallback to direct API
- Reduce repeated upstream calls with persistent SQLite-backed response cache
"""

from __future__ import annotations

import argparse
import json
import os
import secrets
import sqlite3
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlencode, urlsplit, urlunsplit


def now_epoch() -> int:
    return int(time.time())


def ensure_parent_dir(path: str) -> None:
    parent = os.path.dirname(os.path.abspath(path))
    if parent:
        os.makedirs(parent, exist_ok=True)


class CacheStore:
    def __init__(self, db_path: str) -> None:
        ensure_parent_dir(db_path)
        self.conn = sqlite3.connect(db_path, check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        self.conn.execute("PRAGMA journal_mode=WAL;")
        self.conn.execute("PRAGMA synchronous=NORMAL;")
        self.conn.execute(
            """
            CREATE TABLE IF NOT EXISTS cache_entries (
                key TEXT PRIMARY KEY,
                status_code INTEGER NOT NULL,
                content_type TEXT NOT NULL,
                body BLOB NOT NULL,
                fetched_epoch INTEGER NOT NULL,
                expires_epoch INTEGER NOT NULL
            )
            """
        )
        self.conn.execute(
            """
            CREATE TABLE IF NOT EXISTS meta (
                key TEXT PRIMARY KEY,
                value TEXT NOT NULL
            )
            """
        )
        self.conn.execute(
            "INSERT INTO meta(key, value) VALUES('schema_version', '1') "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value"
        )
        self.conn.commit()
        self.lock = threading.Lock()

    def get(self, key: str) -> dict | None:
        with self.lock:
            row = self.conn.execute(
                "SELECT key, status_code, content_type, body, fetched_epoch, expires_epoch FROM cache_entries WHERE key = ?",
                (key,),
            ).fetchone()
        if row is None:
            return None
        if int(row["expires_epoch"]) <= now_epoch():
            return None
        return {
            "status_code": int(row["status_code"]),
            "content_type": str(row["content_type"]),
            "body": bytes(row["body"]),
            "fetched_epoch": int(row["fetched_epoch"]),
            "expires_epoch": int(row["expires_epoch"]),
        }

    def put(self, key: str, status_code: int, content_type: str, body: bytes, ttl_seconds: int) -> None:
        fetched = now_epoch()
        expires = fetched + max(1, int(ttl_seconds))
        with self.lock:
            self.conn.execute(
                """
                INSERT INTO cache_entries(key, status_code, content_type, body, fetched_epoch, expires_epoch)
                VALUES (?, ?, ?, ?, ?, ?)
                ON CONFLICT(key) DO UPDATE SET
                    status_code=excluded.status_code,
                    content_type=excluded.content_type,
                    body=excluded.body,
                    fetched_epoch=excluded.fetched_epoch,
                    expires_epoch=excluded.expires_epoch
                """,
                (key, int(status_code), content_type, sqlite3.Binary(body), fetched, expires),
            )
            self.conn.commit()

    def stats(self) -> dict:
        with self.lock:
            row = self.conn.execute(
                "SELECT COUNT(*) AS n, COALESCE(MIN(fetched_epoch), 0) AS min_ts, COALESCE(MAX(fetched_epoch), 0) AS max_ts FROM cache_entries"
            ).fetchone()
        return {
            "entry_count": int(row["n"]) if row else 0,
            "min_fetched_epoch": int(row["min_ts"]) if row else 0,
            "max_fetched_epoch": int(row["max_ts"]) if row else 0,
        }


def ttl_for_path(path_and_query: str, status_code: int, default_ttl_s: int) -> int:
    if status_code == 429:
        return 10
    if 500 <= status_code <= 599:
        return 5
    if status_code == 404:
        return 25
    if 400 <= status_code <= 499:
        return 20
    if status_code != 200:
        return max(10, default_ttl_s // 2)

    path_lower = path_and_query.lower()
    if path_lower.startswith("/api/leaderboard") or path_lower.startswith("/api/record-leaderboard"):
        return max(default_ttl_s, 300)
    if path_lower.startswith("/api/matches?page="):
        return min(max(default_ttl_s, 120), 300)
    if "/api/users/" in path_lower and "/matches" in path_lower:
        return min(max(default_ttl_s, 60), 240)
    if path_lower.startswith("/api/users/"):
        return min(max(default_ttl_s, 90), 300)
    if path_lower.startswith("/api/matches/"):
        return min(max(default_ttl_s, 120), 360)
    return default_ttl_s


class McsrCacheProxyHandler(BaseHTTPRequestHandler):
    server_version = "McsrCacheProxy/1.0"

    def log_message(self, fmt: str, *args) -> None:  # noqa: A003
        # Keep logs concise and deterministic.
        sys.stdout.write("[%s] %s\n" % (self.log_date_time_string(), fmt % args))
        sys.stdout.flush()

    @property
    def cache_store(self) -> CacheStore:
        return self.server.cache_store  # type: ignore[attr-defined]

    @property
    def upstream_base(self) -> str:
        return self.server.upstream_base  # type: ignore[attr-defined]

    @property
    def timeout_s(self) -> float:
        return self.server.timeout_s  # type: ignore[attr-defined]

    @property
    def default_ttl_s(self) -> int:
        return self.server.default_ttl_s  # type: ignore[attr-defined]

    @property
    def forward_api_key(self) -> bool:
        return self.server.forward_api_key  # type: ignore[attr-defined]

    @property
    def forced_api_key(self) -> str:
        return self.server.forced_api_key  # type: ignore[attr-defined]

    @property
    def forced_api_key_header(self) -> str:
        return self.server.forced_api_key_header  # type: ignore[attr-defined]

    @property
    def auth_token(self) -> str:
        return self.server.auth_token  # type: ignore[attr-defined]

    @property
    def auth_header(self) -> str:
        return self.server.auth_header  # type: ignore[attr-defined]

    @property
    def public_token(self) -> str:
        return self.server.public_token  # type: ignore[attr-defined]

    def _is_authorized(self) -> bool:
        private_token = str(self.auth_token or "").strip()
        public_token = str(self.public_token or "").strip()
        if not private_token and not public_token:
            return True

        header_name = str(self.auth_header or "x-toolscreen-token").strip()
        candidate_tokens: list[str] = []
        incoming = (self.headers.get(header_name) or "").strip()
        if incoming:
            candidate_tokens.append(incoming)
        auth = (self.headers.get("authorization") or "").strip()
        if auth.lower().startswith("bearer "):
            bearer = auth[7:].strip()
            if bearer:
                candidate_tokens.append(bearer)

        for candidate in candidate_tokens:
            if private_token and secrets.compare_digest(candidate, private_token):
                return True
            if public_token and secrets.compare_digest(candidate, public_token):
                # Public token is intentionally limited to health probes.
                if self.path.startswith("/health"):
                    return True
        return False

    def _send_json(self, status: int, payload: dict, extra_headers: dict | None = None) -> None:
        body = json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        self.send_response(int(status))
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        if extra_headers:
            for key, value in extra_headers.items():
                self.send_header(str(key), str(value))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802
        if not self._is_authorized():
            self._send_json(
                401,
                {"ok": False, "error": "unauthorized"},
                extra_headers={"WWW-Authenticate": "Bearer"},
            )
            return

        parts = urlsplit(self.path)
        path = parts.path or "/"

        if path == "/health":
            stats = self.cache_store.stats()
            self._send_json(
                200,
                {
                    "ok": True,
                    "service": "mcsr-cache-proxy",
                    "upstream": self.upstream_base,
                    "cache": stats,
                    "epoch": now_epoch(),
                },
            )
            return

        if not path.startswith("/api/"):
            self._send_json(404, {"ok": False, "error": "not_found", "path": path})
            return

        cache_key = path
        if parts.query:
            cache_key += "?" + parts.query

        force_refresh = False
        if parts.query:
            # Keep this simple and explicit.
            query_pairs = [p for p in parts.query.split("&") if p]
            force_refresh = any(p == "refresh=1" or p == "force=1" for p in query_pairs)

        if not force_refresh:
            cached = self.cache_store.get(cache_key)
            if cached is not None:
                body = cached["body"]
                self.send_response(cached["status_code"])
                self.send_header("Content-Type", cached["content_type"])
                self.send_header("Content-Length", str(len(body)))
                self.send_header("X-Cache", "HIT")
                self.send_header("X-Cache-Fetched", str(cached["fetched_epoch"]))
                self.end_headers()
                self.wfile.write(body)
                return

        upstream_url = self.upstream_base.rstrip("/") + path
        if parts.query:
            upstream_url += "?" + parts.query

        req = urllib.request.Request(upstream_url, method="GET")
        req.add_header("Accept", "application/json")
        req.add_header("User-Agent", "Toolscreen-MCSR-Cache/1.0")

        if self.forward_api_key:
            incoming_key = (self.headers.get("x-api-key") or "").strip()
            if incoming_key:
                req.add_header("x-api-key", incoming_key)
            incoming_auth = (self.headers.get("authorization") or "").strip()
            if incoming_auth:
                req.add_header("Authorization", incoming_auth)

        if self.forced_api_key:
            req.add_header(self.forced_api_key_header, self.forced_api_key)

        status_code = 0
        content_type = "application/json; charset=utf-8"
        body = b""
        upstream_error = ""

        try:
            with urllib.request.urlopen(req, timeout=self.timeout_s) as resp:
                status_code = int(resp.getcode() or 200)
                content_type = str(resp.headers.get("Content-Type") or content_type)
                body = resp.read()
        except urllib.error.HTTPError as e:
            status_code = int(e.code)
            content_type = str(e.headers.get("Content-Type") or content_type)
            body = e.read()
        except Exception as e:  # noqa: BLE001
            upstream_error = str(e)

        if upstream_error:
            self._send_json(
                502,
                {"ok": False, "error": "upstream_unreachable", "detail": upstream_error, "upstream": upstream_url},
                extra_headers={"X-Cache": "MISS"},
            )
            return

        if not body:
            body = b'{"status":"error","data":{"error":"empty response"}}'
            if status_code == 200:
                status_code = 502

        ttl = ttl_for_path(cache_key, status_code, self.default_ttl_s)
        self.cache_store.put(cache_key, status_code, content_type, body, ttl)

        self.send_response(status_code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-Cache", "MISS")
        self.send_header("X-Cache-TTL", str(ttl))
        self.end_headers()
        self.wfile.write(body)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run an MCSR API caching proxy service.")
    parser.add_argument("--bind", default=os.environ.get("BACKEND_BIND", "127.0.0.1"), help="Bind address")
    parser.add_argument("--port", type=int, default=int(os.environ.get("BACKEND_PORT", "8787")), help="Bind port")
    parser.add_argument(
        "--db",
        default=os.environ.get("MCSR_CACHE_DB", "data/mcsr_cache_service.db"),
        help="SQLite database path for response cache",
    )
    parser.add_argument(
        "--upstream",
        default=os.environ.get("MCSR_UPSTREAM_BASE", "https://mcsrranked.com"),
        help="Upstream base URL (default: https://mcsrranked.com)",
    )
    parser.add_argument("--timeout", type=float, default=float(os.environ.get("MCSR_UPSTREAM_TIMEOUT_S", "6.0")))
    parser.add_argument("--ttl", type=int, default=int(os.environ.get("MCSR_DEFAULT_TTL_S", "120")))
    parser.add_argument(
        "--forward-api-key",
        action="store_true",
        default=True,
        help="Forward x-api-key/authorization headers from incoming request (default: on)",
    )
    parser.add_argument(
        "--no-forward-api-key",
        dest="forward_api_key",
        action="store_false",
        help="Disable forwarding incoming API key headers",
    )
    parser.add_argument(
        "--api-key",
        default=os.environ.get("MCSR_API_KEY", "").strip(),
        help="Optional fixed upstream API key to inject",
    )
    parser.add_argument(
        "--api-key-header",
        default=os.environ.get("MCSR_API_KEY_HEADER", "x-api-key").strip(),
        help="Header used for fixed --api-key value",
    )
    parser.add_argument(
        "--auth-token",
        default=os.environ.get("MCSR_CACHE_AUTH_TOKEN", "").strip(),
        help="Optional shared token required by this cache service (header or Authorization: Bearer)",
    )
    parser.add_argument(
        "--auth-header",
        default=os.environ.get("MCSR_CACHE_AUTH_HEADER", "x-toolscreen-token").strip(),
        help="Header name used for --auth-token (default: x-toolscreen-token)",
    )
    parser.add_argument(
        "--public-token",
        default=os.environ.get("MCSR_CACHE_PUBLIC_TOKEN", "").strip(),
        help="Optional public token that can only access /health",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.port <= 0 or args.port > 65535:
        print(f"invalid port: {args.port}", file=sys.stderr)
        return 2

    cache_store = CacheStore(args.db)
    server = ThreadingHTTPServer((args.bind, args.port), McsrCacheProxyHandler)
    server.cache_store = cache_store  # type: ignore[attr-defined]
    server.upstream_base = str(args.upstream).rstrip("/")  # type: ignore[attr-defined]
    server.timeout_s = max(0.5, float(args.timeout))  # type: ignore[attr-defined]
    server.default_ttl_s = max(5, int(args.ttl))  # type: ignore[attr-defined]
    server.forward_api_key = bool(args.forward_api_key)  # type: ignore[attr-defined]
    server.forced_api_key = str(args.api_key).strip()  # type: ignore[attr-defined]
    server.forced_api_key_header = str(args.api_key_header).strip() or "x-api-key"  # type: ignore[attr-defined]
    server.auth_token = str(args.auth_token).strip()  # type: ignore[attr-defined]
    server.auth_header = str(args.auth_header).strip() or "x-toolscreen-token"  # type: ignore[attr-defined]
    server.public_token = str(args.public_token).strip()  # type: ignore[attr-defined]

    print(
        f"[mcsr-cache] serving on http://{args.bind}:{args.port} "
        f"upstream={server.upstream_base} db={os.path.abspath(args.db)} ttl={server.default_ttl_s}s "
        f"auth={'on' if bool(server.auth_token) else 'off'} public_health_token={'on' if bool(server.public_token) else 'off'}",
        flush=True,
    )
    try:
        server.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
