"""绑定 C RSS 同步契约的可执行参考模型。/ Executable model bound to the C RSS contract."""

from __future__ import annotations

import json
import os
import zlib
from dataclasses import dataclass
from pathlib import Path


@dataclass
class Pending:
    entry_id: int
    read: bool | None = None
    bookmarked: bool | None = None
    bookmark_confirmation_pending: bool = False


class CacheModel:
    def __init__(self, metadata_max: int, body_max: int, body_total_max: int, body_single_max: int):
        self.metadata_max = metadata_max
        self.body_max = body_max
        self.body_total_max = body_total_max
        self.body_single_max = body_single_max
        self.sequence = 0
        self.metadata: dict[int, tuple[dict, int]] = {}
        self.bodies: dict[int, tuple[bytes, int]] = {}

    def _touch(self) -> int:
        self.sequence += 1
        return self.sequence

    def put_metadata(self, entry: dict) -> None:
        if entry["id"] not in self.metadata and len(self.metadata) >= self.metadata_max:
            oldest = min(self.metadata, key=lambda key: self.metadata[key][1])
            del self.metadata[oldest]
        self.metadata[entry["id"]] = (dict(entry), self._touch())

    def get_metadata(self, entry_id: int) -> dict | None:
        value = self.metadata.get(entry_id)
        if value is None:
            return None
        entry, _ = value
        self.metadata[entry_id] = (entry, self._touch())
        return entry

    def put_body(self, entry_id: int, body: bytes) -> None:
        if len(body) > self.body_single_max:
            raise ValueError("body limit")
        self.bodies.pop(entry_id, None)
        while self.bodies and (
            len(self.bodies) >= self.body_max
            or sum(len(value[0]) + 1 for value in self.bodies.values()) + len(body) + 1
            > self.body_total_max
        ):
            oldest = min(self.bodies, key=lambda key: self.bodies[key][1])
            del self.bodies[oldest]
        if len(body) + 1 > self.body_total_max:
            raise ValueError("body total limit")
        self.bodies[entry_id] = (body, self._touch())

    def get_body(self, entry_id: int) -> bytes | None:
        value = self.bodies.get(entry_id)
        if value is None:
            return None
        body, _ = value
        self.bodies[entry_id] = (body, self._touch())
        return body

    def save(self, path: Path) -> None:
        payload = json.dumps(
            {
                "metadata": {str(k): v[0] for k, v in self.metadata.items()},
                "bodies": {str(k): v[0].decode() for k, v in self.bodies.items()},
            },
            sort_keys=True,
        ).encode()
        wrapper = json.dumps({"crc32": zlib.crc32(payload), "payload": payload.decode()}).encode()
        backup = path.with_suffix(path.suffix + ".bak")
        if path.exists():
            backup.write_bytes(path.read_bytes())
        path.write_bytes(wrapper)

    @classmethod
    def load(cls, path: Path, limits: tuple[int, int, int, int]) -> "CacheModel":
        for candidate in (path, path.with_suffix(path.suffix + ".bak")):
            try:
                wrapper = json.loads(candidate.read_bytes())
                payload = wrapper["payload"].encode()
                if zlib.crc32(payload) != wrapper["crc32"]:
                    raise ValueError("crc")
                saved = json.loads(payload)
                cache = cls(*limits)
                for entry in saved["metadata"].values():
                    cache.put_metadata(entry)
                for key, body in saved["bodies"].items():
                    cache.put_body(int(key), body.encode())
                return cache
            except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError):
                continue
        raise ValueError("corrupt cache")


class SyncModel:
    def __init__(self, entries: dict[int, dict]):
        self.entries = entries
        self.pending: dict[int, Pending] = {}

    def enqueue(self, operation: Pending) -> None:
        current = self.pending.setdefault(operation.entry_id, Pending(operation.entry_id))
        if operation.read is not None:
            current.read = operation.read
        if operation.bookmarked is not None:
            current.bookmarked = operation.bookmarked

    def apply_read(self, entry_id: int) -> None:
        operation = self.pending[entry_id]
        if operation.read is not None:
            self.entries[entry_id]["status"] = "read" if operation.read else "unread"
            operation.read = None

    def bookmark_attempt(self, entry_id: int, *, applied: bool, timed_out: bool) -> bool:
        operation = self.pending[entry_id]
        if operation.bookmark_confirmation_pending:
            return False
        if self.entries[entry_id]["starred"] == operation.bookmarked:
            operation.bookmarked = None
            return False
        if applied:
            self.entries[entry_id]["starred"] = not self.entries[entry_id]["starred"]
        if timed_out:
            operation.bookmark_confirmation_pending = True
            return True
        self.entries[entry_id]["starred"] = bool(operation.bookmarked)
        operation.bookmarked = None
        return True

    def merge_server(self, server_entries: list[dict]) -> None:
        for entry in server_entries:
            self.entries[entry["id"]] = dict(entry)
            operation = self.pending.get(entry["id"])
            if operation:
                operation.bookmark_confirmation_pending = False

    def compact(self) -> None:
        self.pending = {
            key: value
            for key, value in self.pending.items()
            if value.read is not None or value.bookmarked is not None
        }


def save_pending_atomic(path: Path, pending: list[Pending], *, interrupt_after_rotate: bool = False) -> None:
    payload = json.dumps(
        {
            "magic": "RSP1",
            "version": 1,
            "operations": [operation.__dict__ for operation in pending],
        },
        sort_keys=True,
    ).encode()
    temporary = path.with_suffix(path.suffix + ".tmp")
    backup = path.with_suffix(path.suffix + ".bak")
    temporary.write_bytes(payload)
    json.loads(temporary.read_bytes())
    if path.exists():
        backup.unlink(missing_ok=True)
        os.replace(path, backup)
        if interrupt_after_rotate:
            return
    os.replace(temporary, path)
    backup.unlink(missing_ok=True)


def load_pending(path: Path) -> list[Pending]:
    for candidate in (path, path.with_suffix(path.suffix + ".bak")):
        try:
            saved = json.loads(candidate.read_bytes())
            if saved["magic"] != "RSP1" or saved["version"] != 1:
                raise ValueError("header")
            operations = [Pending(**operation) for operation in saved["operations"]]
            if any(
                operation.entry_id <= 0
                or (operation.read is None and operation.bookmarked is None)
                or (operation.bookmark_confirmation_pending and operation.bookmarked is None)
                for operation in operations
            ):
                raise ValueError("operation")
            if len({operation.entry_id for operation in operations}) != len(operations):
                raise ValueError("duplicate")
            if candidate != path:
                path.unlink(missing_ok=True)
                os.replace(candidate, path)
            return operations
        except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError):
            continue
    raise ValueError("corrupt pending")
