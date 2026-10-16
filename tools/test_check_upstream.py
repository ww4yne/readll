"""中文：验证上游变化、网络失败与发布门禁，不访问真实网络。
English: Verify upstream changes, network errors and publication gates without network access.
"""
from __future__ import annotations

from contextlib import redirect_stderr, redirect_stdout
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from tools.check_upstream import Source, load_sources, main, remote_head


ROOT = Path(__file__).resolve().parents[1]
SOURCE = Source("example/reader", "main", "a" * 40)


class UpstreamCheckTests(unittest.TestCase):
    def test_reviewed_sources_are_pinned(self) -> None:
        sources = load_sources(ROOT / "tools/upstream.json")
        self.assertEqual({source.repository for source in sources},
                         {"wegooo-cell/read-pico-reader", "MindReset/read_pico_firmware"})

    def test_checks_are_wired_into_every_build_and_publication(self) -> None:
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("add_custom_target(readll_upstream_check ALL", cmake)
        self.assertIn("add_dependencies(${PROJECT_NAME}.elf readll_upstream_check)", cmake)
        self.assertIn("if(READLL_SKIP_UPSTREAM_CHECK AND READLL_REQUIRE_CURRENT_UPSTREAM)", cmake)
        self.assertIn('message(FATAL_ERROR "Required upstream review cannot be skipped")', cmake)
        wrapper = (ROOT / "tools/build_firmware.ps1").read_text(encoding="utf-8")
        self.assertIn("[switch]$SkipUpstreamCheck", wrapper)
        workflow = (ROOT / ".github/workflows/pages.yml").read_text(encoding="utf-8")
        self.assertIn("READLL_REQUIRE_CURRENT_UPSTREAM=ON", workflow)

    def test_lookup_is_read_only_and_bounded(self) -> None:
        result = subprocess.CompletedProcess([], 0, f"{'b' * 40}\trefs/heads/main\n", "")
        with patch("tools.check_upstream.subprocess.run", return_value=result) as run:
            self.assertEqual(remote_head(SOURCE), "b" * 40)
        args, kwargs = run.call_args
        self.assertEqual(args[0][:3], ["git", "ls-remote", "--exit-code"])
        self.assertEqual(kwargs["timeout"], 30)
        self.assertTrue(kwargs["check"])
        self.assertEqual(kwargs["env"]["GIT_TERMINAL_PROMPT"], "0")

    def test_invalid_remote_response_fails(self) -> None:
        for output in ("", "not-a-sha refs/heads/main", f"{'b' * 40} refs/heads/other"):
            with self.subTest(output=output):
                result = subprocess.CompletedProcess([], 0, output, "")
                with patch("tools.check_upstream.subprocess.run", return_value=result):
                    with self.assertRaises(ValueError):
                        remote_head(SOURCE)

    def test_current_and_changed_release_gates(self) -> None:
        for latest, strict, expected in (("a" * 40, False, 0), ("a" * 40, True, 0),
                                         ("b" * 40, False, 0), ("b" * 40, True, 1)):
            with self.subTest(latest=latest, strict=strict):
                output = io.StringIO()
                with patch("tools.check_upstream.load_sources", return_value=[SOURCE]), \
                     patch("tools.check_upstream.remote_head", return_value=latest), \
                     redirect_stdout(output):
                    self.assertEqual(main(["--require-current"] if strict else []), expected)
                if latest != SOURCE.reviewed_commit:
                    self.assertIn("WARNING: upstream changed", output.getvalue())
                    self.assertIn(f"compare/{SOURCE.reviewed_commit}...{latest}", output.getvalue())
                    self.assertIn("no code was merged", output.getvalue())
                else:
                    self.assertIn("Upstream current", output.getvalue())

    def test_network_errors_are_not_success(self) -> None:
        for error in (OSError("git missing"), subprocess.TimeoutExpired("git", 30),
                      subprocess.CalledProcessError(128, "git")):
            with self.subTest(error=error), \
                 patch("tools.check_upstream.load_sources", return_value=[SOURCE]), \
                 patch("tools.check_upstream.remote_head", side_effect=error), \
                 redirect_stderr(io.StringIO()) as output:
                self.assertEqual(main([]), 2)
                self.assertIn("Upstream check failed", output.getvalue())

    def test_malformed_baselines_are_rejected(self) -> None:
        work = ROOT / "build/upstream-check-tests"
        work.mkdir(parents=True, exist_ok=True)
        entry = {"repository": SOURCE.repository, "branch": SOURCE.branch,
                 "reviewed_commit": SOURCE.reviewed_commit}
        configs = ({}, {"schema": 1, "sources": []},
                   {"schema": 1, "sources": [entry, entry]},
                   {"schema": 1, "sources": [{**entry, "reviewed_commit": "invalid"}]},
                   {"schema": 1, "sources": [{**entry, "repository": "../invalid"}]},
                   {"schema": 1, "sources": [{**entry, "branch": "--upload-pack=bad"}]})
        with tempfile.TemporaryDirectory(dir=work) as folder:
            path = Path(folder) / "baseline.json"
            for config in configs:
                with self.subTest(config=config):
                    path.write_text(json.dumps(config), encoding="utf-8")
                    with self.assertRaises(ValueError):
                        load_sources(path)


if __name__ == "__main__":
    unittest.main()
