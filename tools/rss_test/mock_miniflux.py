"""RSS 本机测试使用的 Miniflux 兼容 HTTP 服务。/ Local Miniflux-compatible test service."""

from __future__ import annotations

import json
import socket
import threading
import time
from copy import deepcopy
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit


TOKEN = "device-test-token"


def sample_entries() -> list[dict]:
    entries = []
    for offset in range(6):
        entry_id = 101 + offset
        changed = 1_800_000_000 + offset * 60
        entries.append(
            {
                "id": entry_id,
                "feed_id": 7 + offset % 2,
                "title": f"Entry {entry_id}",
                "url": f"https://feed.example/entries/{entry_id}",
                "author": "readll test",
                "published_at": "2026-10-14T08:00:00Z",
                "changed_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(changed)),
                "status": "unread",
                "starred": False,
                "reading_time": 2,
                "content": f"<p>body {entry_id}</p>",
                "feed": {
                    "title": "Feed A" if offset % 2 == 0 else "Feed B",
                    "category": {"id": 1 + offset % 2, "title": "News"},
                },
            }
        )
    return entries


class MockState:
    def __init__(self) -> None:
        self.user = {"id": 9, "username": "wayne"}
        self.categories = [{"id": 1, "title": "News"}, {"id": 2, "title": "Tech"}]
        self.entries = sample_entries()
        self.requests: list[dict] = []
        self.faults: dict[str, str] = {}
        self.lock = threading.Lock()

    def record(self, method: str, path: str, body: bytes) -> None:
        with self.lock:
            self.requests.append({"method": method, "path": path, "body": body})


class MockMiniflux:
    def __init__(self) -> None:
        self.state = MockState()
        state = self.state

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, _format: str, *_args: object) -> None:
                return

            def _fault(self, path: str) -> str | None:
                return state.faults.get(path) or state.faults.get("*")

            def _json(self, status: int, value: object, *, truncate: bool = False) -> None:
                payload = json.dumps(value, separators=(",", ":")).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload) + (17 if truncate else 0)))
                self.end_headers()
                if truncate:
                    self.wfile.write(payload[: max(1, len(payload) // 2)])
                    self.wfile.flush()
                    self.close_connection = True
                    return
                self.wfile.write(payload)

            def _empty(self, status: int) -> None:
                self.send_response(status)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def _authorized(self) -> bool:
                if self.headers.get("X-Auth-Token") == TOKEN:
                    return True
                self._json(401, {"error": "unauthorized"})
                return False

            def _read_body(self) -> bytes:
                length = int(self.headers.get("Content-Length", "0"))
                return self.rfile.read(length) if length else b""

            def do_GET(self) -> None:
                split = urlsplit(self.path)
                state.record("GET", self.path, b"")
                if not self._authorized():
                    return
                fault = self._fault(split.path)
                if fault == "500":
                    self._json(500, {"error": "injected"})
                    return
                if fault == "oversized":
                    self._json(200, {"padding": "x" * (96 * 1024 + 1)})
                    return
                if fault == "truncated":
                    self._json(200, {"entries": [], "total": 0}, truncate=True)
                    return
                if split.path == "/v1/me":
                    value = deepcopy(state.user)
                elif split.path == "/v1/categories":
                    value = deepcopy(state.categories)
                elif split.path == "/v1/entries":
                    query = parse_qs(split.query)
                    limit = int(query.get("limit", ["20"])[0])
                    changed_after = int(query.get("changed_after", ["0"])[0])
                    after_entry_id = int(query.get("after_entry_id", ["0"])[0])
                    category_id = int(query.get("category_id", ["0"])[0])
                    selected = [
                        deepcopy(entry)
                        for entry in state.entries
                        if entry["id"] > after_entry_id
                        and _epoch(entry["changed_at"]) > changed_after
                        and (not category_id or entry["feed"]["category"]["id"] == category_id)
                    ]
                    value = {"total": len(selected), "entries": selected[:limit]}
                else:
                    self._json(404, {"error": "not found"})
                    return
                if fault == "unknown_fields":
                    if isinstance(value, dict):
                        value["future_field"] = {"nested": [1, 2, 3]}
                    else:
                        for item in value:
                            item["future_field"] = True
                self._json(200, value)

            def do_PUT(self) -> None:
                split = urlsplit(self.path)
                body = self._read_body()
                state.record("PUT", self.path, body)
                if not self._authorized():
                    return
                fault = self._fault(split.path)
                if split.path == "/v1/entries":
                    if fault == "500":
                        self._json(500, {"error": "injected"})
                        return
                    request = json.loads(body)
                    ids = set(request["entry_ids"])
                    for entry in state.entries:
                        if entry["id"] in ids:
                            entry["status"] = request["status"]
                    self._empty(204)
                    return
                if split.path.startswith("/v1/entries/") and split.path.endswith("/bookmark"):
                    entry_id = int(split.path.split("/")[3])
                    for entry in state.entries:
                        if entry["id"] == entry_id:
                            entry["starred"] = not entry["starred"]
                            break
                    if fault == "timeout_after_apply":
                        time.sleep(0.3)
                    if fault == "500":
                        self._json(500, {"error": "injected"})
                    else:
                        try:
                            self._empty(204)
                        except (BrokenPipeError, ConnectionResetError):
                            pass
                    return
                self._json(404, {"error": "not found"})

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    @property
    def base_url(self) -> str:
        host, port = self.server.server_address
        return f"http://{host}:{port}"

    def __enter__(self) -> "MockMiniflux":
        self.thread.start()
        return self

    def __exit__(self, *_exc: object) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


def _epoch(timestamp: str) -> int:
    return int(time.mktime(time.strptime(timestamp, "%Y-%m-%dT%H:%M:%SZ")) - time.timezone)
