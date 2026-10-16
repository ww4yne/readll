"""在指定构建目录生成大型解析 fixture。/ Generate large fixtures in the build directory."""

from __future__ import annotations

import json
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: generate_parser_fixtures.py OUTPUT_DIR", file=sys.stderr)
        return 2
    output = Path(sys.argv[1])
    output.mkdir(parents=True, exist_ok=True)
    entry = {
        "id": 101,
        "feed_id": 7,
        "title": "Entry 101",
        "url": "https://feed.example/entries/101",
        "author": "test",
        "published_at": "2026-10-14T08:00:00Z",
        "changed_at": "2026-10-14T08:01:00Z",
        "status": "unread",
        "starred": False,
        "reading_time": 2,
        "content": "x" * (24 * 1024 + 1),
        "feed": {"title": "Fixture Feed", "category": {"id": 1, "title": "News"}},
    }
    (output / "generated_entries_oversized.json").write_text(
        json.dumps({"total": 1, "entries": [entry]}, separators=(",", ":")),
        encoding="utf-8",
    )
    entry["content"] = "<p>selected body</p>"
    (output / "generated_entry.json").write_text(json.dumps(entry), encoding="utf-8")
    print(f"generated parser fixtures: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
