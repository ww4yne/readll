"""中文：比较已审查的上游基线与远端分支，只报告变化，不修改代码。
English: Compare reviewed upstream baselines with remote branches without changing code.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import subprocess
import sys


@dataclass(frozen=True)
class Source:
    repository: str
    branch: str
    reviewed_commit: str


def load_sources(path: Path) -> list[Source]:
    config = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(config, dict) or config.get("schema") != 1:
        raise ValueError("Unsupported upstream baseline schema")
    entries = config.get("sources")
    if not isinstance(entries, list) or not entries:
        raise ValueError("Upstream sources must be a nonempty list")
    sources = []
    seen = set()
    for entry in entries:
        if not isinstance(entry, dict) or set(entry) != {
            "repository", "branch", "reviewed_commit"
        } or not all(isinstance(value, str) for value in entry.values()):
            raise ValueError("Invalid upstream baseline entry")
        source = Source(**entry)
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9-]*/[A-Za-z0-9][A-Za-z0-9_.-]*", source.repository):
            raise ValueError("Invalid upstream repository")
        if not re.fullmatch(r"[A-Za-z0-9_-]+", source.branch):
            raise ValueError("Invalid upstream branch")
        if not re.fullmatch(r"[0-9a-f]{40}", source.reviewed_commit):
            raise ValueError("Invalid reviewed upstream commit")
        key = (source.repository, source.branch)
        if key in seen:
            raise ValueError("Duplicate upstream source")
        seen.add(key)
        sources.append(source)
    return sources


def remote_head(source: Source) -> str:
    reference = f"refs/heads/{source.branch}"
    result = subprocess.run(
        ["git", "ls-remote", "--exit-code",
         f"https://github.com/{source.repository}.git", reference],
        capture_output=True, text=True, timeout=30, check=True,
        env={**os.environ, "GIT_TERMINAL_PROMPT": "0"},
    )
    fields = result.stdout.strip().split()
    if len(fields) != 2 or fields[1] != reference or not re.fullmatch(r"[0-9a-f]{40}", fields[0]):
        raise ValueError(f"Invalid remote branch response: {source.repository}")
    return fields[0]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, default=Path(__file__).with_name("upstream.json"))
    parser.add_argument("--require-current", action="store_true",
                        help="Fail if any upstream change has not been reviewed")
    args = parser.parse_args(argv)
    try:
        outdated = False
        for source in load_sources(args.baseline):
            latest = remote_head(source)
            if latest == source.reviewed_commit:
                print(f"Upstream current: {source.repository} @ {latest[:12]}")
            else:
                outdated = True
                print(f"WARNING: upstream changed: {source.repository}\n"
                      f"  reviewed {source.reviewed_commit}; remote {latest}\n"
                      f"  https://github.com/{source.repository}/compare/"
                      f"{source.reviewed_commit}...{latest}\n"
                      "  Review and integrate before updating the baseline; no code was merged.")
        return 1 if outdated and args.require_current else 0
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"Upstream check failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
