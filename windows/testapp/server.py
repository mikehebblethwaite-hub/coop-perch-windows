#!/usr/bin/env python3
"""Private-network static server and request watcher for the Perch benchmark."""

from __future__ import annotations

from collections import defaultdict, deque
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import ipaddress
import json
import math
import os
from pathlib import Path
import re
import threading
import time
from urllib.parse import unquote, urlsplit


PAGES = {
    "/": Path(__file__).with_name("index.html"),
    "/index.html": Path(__file__).with_name("index.html"),
    "/results.html": Path(__file__).with_name("results.html"),
    "/email-results.html": Path(__file__).with_name("email-results.html"),
}
HOST = "127.0.0.1"
PRIVATE_ADDRESS = "10.0.2.15"
PORT = 8765
BIND_HOSTS = (HOST, PRIVATE_ADDRESS)
ALLOWED_HOSTS = frozenset(("localhost", HOST, PRIVATE_ADDRESS))
PROBE_WINDOW_SECONDS = 60
PROBE_LIMIT = 5
BAN_SECONDS = 10 * 60
LOG_WINDOW_SECONDS = 60
LOG_LIMIT = 5
EXEMPT_NETWORKS = tuple(
    ipaddress.ip_network(network)
    for network in ("127.0.0.0/8", "::1/128", "10.0.2.0/24")
)


PROBE_PATTERNS = tuple(
    (re.compile(pattern, re.IGNORECASE), category)
    for pattern, category in (
        (r"union[ +]+(all[ +]+)?select|union%20select|information_schema|sleep\(|benchmark\(|%27%20or|'[ +]+or[ +]+'?1|\bxp_cmdshell", "sql-injection"),
        (r"\.\./\.\./|/etc/passwd|(?:^|/)\.\.(?:/|$)|\\\.\.\\", "path-traversal"),
        (r"<script", "script-injection"),
        (r"base64_decode|(wget|curl)[ +%]", "remote-command"),
        (r"\.env( |\?|\"|$)", "environment-file"),
        (r"wp-login|wp-config|/wp-admin(?:/|$)|/wp-content(?:/|$)|/wp-includes(?:/|$)", "wordpress"),
        (r"phpmyadmin", "phpmyadmin"),
        (r"/\.(git|svn|hg)/", "source-control"),
        (r"/\.(aws|ssh)/", "cloud-or-ssh-keys"),
        (r"/\.vscode/", "editor-settings"),
        (r"\.ds_store", "folder-listing"),
        (r"phpinfo", "phpinfo"),
        (r"\.(zip|tar|tgz|gz|rar|7z|sql|bak|old|swp)( |\?|\"|$)", "backup-or-database-dump"),
        (r" /vpn/|/remote/login|/remote/fgt_lang|/logon/LogonPoint", "vpn-or-citrix-login"),
        (r"/wsfed/|/adfs/", "adfs-login"),
        (r"/geoserver/", "geoserver"),
        (r"/owa/|/ecp/|/autodiscover", "exchange"),
        (r"/actuator", "spring-actuator"),
        (r"/solr/", "solr"),
        (r"/cgi-bin/", "cgi-bin"),
        (r"/boaform/|/HNAP1|/GponForm/", "router-admin"),
        (r"/vendor/phpunit/", "phpunit"),
    )
)


def classify_probe(request_target: str) -> str | None:
    decoded = request_target
    for _ in range(3):
        next_decoded = unquote(decoded)
        if next_decoded == decoded:
            break
        decoded = next_decoded
    if any(ord(char) < 32 or ord(char) == 127 for char in decoded):
        return "control-character"
    text = f"{request_target} {decoded.replace(chr(92), '/')}"
    for pattern, category in PROBE_PATTERNS:
        if pattern.search(text):
            return category
    return None


def is_exempt_source(source_ip: str) -> bool:
    address = ipaddress.ip_address(source_ip)
    if isinstance(address, ipaddress.IPv6Address) and address.ipv4_mapped:
        address = address.ipv4_mapped
    return any(
        address.version == network.version and address in network
        for network in EXEMPT_NETWORKS
    )


class RequestWatchdog:
    def __init__(self, log_path: Path | None) -> None:
        self.log_path = log_path
        self._probes: dict[str, deque[float]] = defaultdict(deque)
        self._bans: dict[str, float] = {}
        self._logged: dict[str, deque[float]] = defaultdict(deque)
        self._lock = threading.Lock()

    def ban_remaining(self, source_ip: str, now: float | None = None) -> int:
        if is_exempt_source(source_ip):
            return 0
        current = time.monotonic() if now is None else now
        with self._lock:
            expires = self._bans.get(source_ip, 0)
            if expires <= current:
                self._bans.pop(source_ip, None)
                return 0
            return math.ceil(expires - current)

    def record_probe(self, source_ip: str, category: str, now: float | None = None) -> dict[str, object]:
        current = time.monotonic() if now is None else now
        with self._lock:
            if is_exempt_source(source_ip):
                event: dict[str, object] = {
                    "source_ip": source_ip,
                    "category": category,
                    "probe_count_60s": 0,
                    "action": "rejected-exempt-source",
                    "ban_seconds": 0,
                }
            else:
                events = self._probes[source_ip]
                cutoff = current - PROBE_WINDOW_SECONDS
                while events and events[0] <= cutoff:
                    events.popleft()
                events.append(current)
                count = len(events)
                banned = count >= PROBE_LIMIT
                if banned:
                    self._bans[source_ip] = current + BAN_SECONDS
                event = {
                    "source_ip": source_ip,
                    "category": category,
                    "probe_count_60s": count,
                    "action": "banned-10m" if banned else "rejected",
                    "ban_seconds": BAN_SECONDS if banned else 0,
                }
            self._write_event(event, current)
            return event

    def _write_event(self, event: dict[str, object], now: float) -> None:
        if self.log_path is None:
            return
        source_ip = str(event["source_ip"])
        logged = self._logged[source_ip]
        cutoff = now - LOG_WINDOW_SECONDS
        while logged and logged[0] <= cutoff:
            logged.popleft()
        if len(logged) >= LOG_LIMIT:
            return
        logged.append(now)
        record = {
            "time_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            **event,
        }
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        with self.log_path.open("a", encoding="utf-8") as log_file:
            log_file.write(json.dumps(record, separators=(",", ":")) + "\n")


def local_watch_log_path() -> Path:
    local_app_data = os.environ.get("LOCALAPPDATA")
    if not local_app_data:
        raise RuntimeError("LOCALAPPDATA is not available for the local watchdog log")
    return Path(local_app_data) / "Perch" / "testapp" / "watchdog.jsonl"


WATCHDOG = RequestWatchdog(local_watch_log_path())


class Handler(BaseHTTPRequestHandler):
    server_version = "PerchLocalBenchmark/1.0"

    def _valid_host(self) -> bool:
        value = self.headers.get("Host", "").strip().lower()
        host, separator, port = value.rpartition(":")
        if separator:
            if not port.isdigit() or int(port) != self.server.server_address[1]:
                return False
            value = host
        return value in ALLOWED_HOSTS

    def _send(self, status: int, body: bytes, content_type: str, retry_after: int | None = None) -> None:
        self._last_status = status
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        if retry_after is not None:
            self.send_header("Retry-After", str(retry_after))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _dispatch(self) -> None:
        source_ip = self.client_address[0]
        remaining = WATCHDOG.ban_remaining(source_ip)
        if remaining:
            self._send(429, b"temporarily blocked\n", "text/plain; charset=utf-8", remaining)
            return

        category = classify_probe(self.path)
        if category:
            event = WATCHDOG.record_probe(source_ip, category)
            if event["action"] == "banned-10m":
                self._send(429, b"temporarily blocked\n", "text/plain; charset=utf-8", BAN_SECONDS)
            else:
                self._send(403, b"request rejected\n", "text/plain; charset=utf-8")
            return

        if not self._valid_host():
            self._send(403, b"invalid host\n", "text/plain; charset=utf-8")
            return

        if self.command not in ("GET", "HEAD"):
            self.close_connection = True
            self._send(405, b"method not allowed\n", "text/plain; charset=utf-8")
            return

        page_path = PAGES.get(urlsplit(self.path).path)
        if page_path is None:
            self._send(404, b"not found\n", "text/plain; charset=utf-8")
            return
        try:
            page = page_path.read_bytes()
        except OSError:
            self._send(500, b"benchmark page is unavailable\n", "text/plain; charset=utf-8")
            return
        self._send(200, page, "text/html; charset=utf-8")

    def do_GET(self) -> None:
        self._dispatch()

    def do_HEAD(self) -> None:
        self._dispatch()

    def do_POST(self) -> None:
        self._dispatch()

    def do_PUT(self) -> None:
        self._dispatch()

    def do_DELETE(self) -> None:
        self._dispatch()

    def do_PATCH(self) -> None:
        self._dispatch()

    def do_OPTIONS(self) -> None:
        self._dispatch()

    def do_TRACE(self) -> None:
        self._dispatch()

    def log_message(self, format_string: str, *args: object) -> None:
        status = getattr(self, "_last_status", "-")
        print(f"{self.log_date_time_string()} source={self.client_address[0]} status={status}")


def serve() -> None:
    servers = [ThreadingHTTPServer((host, PORT), Handler) for host in BIND_HOSTS]
    threads = [
        threading.Thread(target=server.serve_forever, daemon=True)
        for server in servers
    ]
    try:
        for thread in threads:
            thread.start()
        print(f"Perch benchmark: http://{HOST}:{PORT}/")
        print(f"Private/NAT endpoint: http://{PRIVATE_ADDRESS}:{PORT}/")
        print("NAT sources are exempt from bans; suspicious requests are logged locally.")
        print("Press Ctrl+C to stop.")
        for thread in threads:
            thread.join()
    except KeyboardInterrupt:
        pass
    finally:
        for server in servers:
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    serve()
