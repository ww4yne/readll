from __future__ import annotations

import http.client
import hashlib
import json
import os
import re
import shutil
import socket
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

from tools.rss_test.contract_model import (
    CacheModel,
    Pending,
    SyncModel,
    load_pending,
    save_pending_atomic,
)
from tools.rss_test.mock_miniflux import MockMiniflux, TOKEN, _epoch
from tools.release_assets import hydrate


ROOT = Path(__file__).resolve().parents[2]
WORK = Path(os.environ.get("RSS_TEST_WORKDIR", ROOT / "build/rss-tests/work"))
FIXTURES = ROOT / "tools/rss_test/fixtures"


def request(base: str, path: str, *, method: str = "GET", body: object | None = None,
            token: str = TOKEN, timeout: float = 2.0) -> tuple[int, bytes]:
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(
        base + path,
        data=data,
        method=method,
        headers={"X-Auth-Token": token, "Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            return response.status, response.read()
    except urllib.error.HTTPError as error:
        return error.code, error.read()


class MockMinifluxTests(unittest.TestCase):
    def test_me_categories_unknown_fields_and_auth_errors(self) -> None:
        with MockMiniflux() as mock:
            status, body = request(mock.base_url, "/v1/me")
            self.assertEqual((status, json.loads(body)["username"]), (200, "wayne"))
            mock.state.faults["/v1/categories"] = "unknown_fields"
            status, body = request(mock.base_url, "/v1/categories")
            self.assertEqual(status, 200)
            self.assertTrue(json.loads(body)[0]["future_field"])
            self.assertEqual(request(mock.base_url, "/v1/me", token="wrong")[0], 401)
            mock.state.faults["/v1/me"] = "500"
            self.assertEqual(request(mock.base_url, "/v1/me")[0], 500)

    def test_entries_pagination_changed_after_and_category(self) -> None:
        with MockMiniflux() as mock:
            path = "/v1/entries?order=id&direction=asc&limit=2&changed_after=0"
            first = json.loads(request(mock.base_url, path)[1])
            self.assertEqual([entry["id"] for entry in first["entries"]], [101, 102])
            self.assertEqual(first["entries"][0]["feed"]["title"], "Feed A")
            second = json.loads(request(mock.base_url, path + "&after_entry_id=102")[1])
            self.assertEqual([entry["id"] for entry in second["entries"]], [103, 104])
            cutoff = _epoch(mock.state.entries[2]["changed_at"])
            changed = json.loads(request(mock.base_url, path.replace("changed_after=0", f"changed_after={cutoff}"))[1])
            self.assertEqual([entry["id"] for entry in changed["entries"]], [104, 105])
            category = json.loads(request(mock.base_url, path + "&category_id=2")[1])
            self.assertEqual([entry["id"] for entry in category["entries"]], [102, 104])

    def test_batch_read_unread_and_bookmark_toggle(self) -> None:
        with MockMiniflux() as mock:
            self.assertEqual(
                request(mock.base_url, "/v1/entries", method="PUT",
                        body={"entry_ids": [101, 103], "status": "read"})[0],
                204,
            )
            self.assertEqual([mock.state.entries[index]["status"] for index in (0, 2)], ["read", "read"])
            request(mock.base_url, "/v1/entries", method="PUT",
                    body={"entry_ids": [101], "status": "unread"})
            self.assertEqual(mock.state.entries[0]["status"], "unread")
            self.assertEqual(request(mock.base_url, "/v1/entries/101/bookmark", method="PUT")[0], 204)
            self.assertTrue(mock.state.entries[0]["starred"])

    def test_oversized_truncated_and_unknown_entry_payloads(self) -> None:
        with MockMiniflux() as mock:
            mock.state.faults["/v1/entries"] = "oversized"
            status, body = request(mock.base_url, "/v1/entries")
            self.assertEqual(status, 200)
            self.assertGreater(len(body), 96 * 1024)
            mock.state.faults["/v1/entries"] = "unknown_fields"
            body = request(mock.base_url, "/v1/entries?limit=1")[1]
            self.assertIn("future_field", json.loads(body))
            mock.state.faults["/v1/entries"] = "truncated"
            with self.assertRaises((http.client.IncompleteRead, urllib.error.URLError)):
                request(mock.base_url, "/v1/entries")


class ModelContractTests(unittest.TestCase):
    def setUp(self) -> None:
        shutil.rmtree(WORK, ignore_errors=True)
        WORK.mkdir(parents=True)

    def test_cache_limits_lru_and_corrupt_backup_recovery(self) -> None:
        limits = (3, 2, 12, 8)
        cache = CacheModel(*limits)
        for entry_id in (1, 2, 3):
            cache.put_metadata({"id": entry_id, "status": "unread", "starred": False})
        cache.get_metadata(1)
        cache.put_metadata({"id": 4, "status": "unread", "starred": False})
        self.assertEqual(set(cache.metadata), {1, 3, 4})
        cache.put_body(1, b"aaa")
        cache.put_body(2, b"bbb")
        cache.get_body(1)
        cache.put_body(3, b"ccc")
        self.assertEqual(set(cache.bodies), {1, 3})
        with self.assertRaises(ValueError):
            cache.put_body(4, b"123456789")
        path = WORK / "cache.bin"
        cache.save(path)
        cache.put_metadata({"id": 5, "status": "read", "starred": True})
        cache.save(path)
        path.write_bytes(b"corrupt")
        recovered = CacheModel.load(path, limits)
        self.assertNotIn(5, recovered.metadata)
        self.assertEqual(set(recovered.metadata), {1, 3, 4})

    def test_pending_then_server_conflict_and_multi_device_overlap(self) -> None:
        model = SyncModel({101: {"id": 101, "status": "unread", "starred": False}})
        model.enqueue(Pending(101, read=True))
        model.apply_read(101)
        self.assertEqual(model.entries[101]["status"], "read")
        model.merge_server([{"id": 101, "status": "unread", "starred": False}])
        self.assertEqual(model.entries[101]["status"], "unread")
        model.merge_server([
            {"id": 101, "status": "read", "starred": False},
            {"id": 102, "status": "unread", "starred": True},
        ])
        self.assertEqual(set(model.entries), {101, 102})
        self.assertTrue(model.entries[102]["starred"])

    def test_duplicate_read_timeout_and_231_bookmark_no_blind_replay(self) -> None:
        model = SyncModel({101: {"id": 101, "status": "unread", "starred": False}})
        model.enqueue(Pending(101, read=True))
        model.enqueue(Pending(101, read=True))
        self.assertEqual(len(model.pending), 1)
        model.apply_read(101)
        model.apply_read(101)
        self.assertEqual(model.entries[101]["status"], "read")
        model.enqueue(Pending(101, bookmarked=True))
        self.assertTrue(model.bookmark_attempt(101, applied=True, timed_out=True))
        self.assertTrue(model.entries[101]["starred"])
        self.assertFalse(model.bookmark_attempt(101, applied=False, timed_out=False))
        model.merge_server([{"id": 101, "status": "read", "starred": True}])
        self.assertFalse(model.bookmark_attempt(101, applied=False, timed_out=False))
        model.compact()
        self.assertFalse(model.pending)

    def test_live_timeout_applies_toggle_once(self) -> None:
        with MockMiniflux() as mock:
            mock.state.faults["/v1/entries/101/bookmark"] = "timeout_after_apply"
            with self.assertRaises((socket.timeout, TimeoutError, urllib.error.URLError)):
                request(mock.base_url, "/v1/entries/101/bookmark", method="PUT", timeout=0.05)
            time.sleep(0.35)
            self.assertTrue(mock.state.entries[0]["starred"])
            toggles = [item for item in mock.state.requests if item["path"].endswith("/bookmark")]
            self.assertEqual(len(toggles), 1)

    def test_bookmark_confirmation_survives_restart_before_server_query(self) -> None:
        path = WORK / "rss.pending"
        operation = Pending(101, bookmarked=True, bookmark_confirmation_pending=True)
        save_pending_atomic(path, [operation])
        restored = load_pending(path)
        model = SyncModel({101: {"id": 101, "status": "unread", "starred": False}})
        model.pending = {item.entry_id: item for item in restored}
        self.assertFalse(model.bookmark_attempt(101, applied=False, timed_out=False))
        self.assertFalse(model.entries[101]["starred"])
        model.merge_server([{"id": 101, "status": "unread", "starred": True}])
        self.assertFalse(model.bookmark_attempt(101, applied=False, timed_out=False))
        model.compact()
        self.assertFalse(model.pending)

    def test_pending_atomic_rotation_recovers_backup(self) -> None:
        path = WORK / "rss.pending"
        original = Pending(101, read=True)
        replacement = Pending(101, bookmarked=True, bookmark_confirmation_pending=True)
        save_pending_atomic(path, [original])
        save_pending_atomic(path, [replacement], interrupt_after_rotate=True)
        restored = load_pending(path)
        self.assertEqual(restored, [original])
        self.assertTrue(path.exists())
        self.assertFalse(path.with_suffix(path.suffix + ".bak").exists())
        save_pending_atomic(path, [replacement])
        path.write_bytes(b"corrupt")
        backup = path.with_suffix(path.suffix + ".bak")
        save_pending_atomic(backup, [original])
        restored = load_pending(path)
        self.assertEqual(restored, [original])


class StaticAndFixtureContractTests(unittest.TestCase):
    def test_c_bounds_match_documented_contract(self) -> None:
        model_h = (ROOT / "main/rss/rss_model.h").read_text(encoding="utf-8")
        cache_h = (ROOT / "main/rss/rss_cache.h").read_text(encoding="utf-8")
        self.assertRegex(model_h, r"#define RSS_PAGE_ENTRY_MAX 20\b")
        self.assertRegex(model_h, r"#define RSS_ENTRY_BODY_MAX \(24U \* 1024U\)")
        self.assertRegex(cache_h, r"#define RSS_CACHE_METADATA_MAX 128\b")
        self.assertRegex(cache_h, r"#define RSS_CACHE_BODY_MAX 32\b")
        self.assertRegex(cache_h, r"#define RSS_CACHE_BODY_TOTAL_MAX \(256U \* 1024U\)")
        self.assertRegex(model_h, r"#define RSS_AUTH_TOKEN_MAX 256\b")

    def test_project_and_pending_persistence_contracts(self) -> None:
        project = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        app = (ROOT / "main/apps/app_rss.c").read_text(encoding="utf-8")
        sync_h = (ROOT / "main/rss/rss_sync.h").read_text(encoding="utf-8")
        self.assertRegex(project, r"(?m)^project\(readll\)$")
        self.assertIn('RSS_PENDING_FILE ".tmp"', app)
        self.assertIn('RSS_PENDING_FILE ".bak"', app)
        self.assertIn("pending_read_file(RSS_PENDING_FILE", app)
        self.assertIn("pending_read_file(backup", app)
        self.assertIn("rss_sync_restore_operation", app)
        self.assertIn("operation_revision", sync_h)
        self.assertNotIn("pending_before != s_sync.operation_count", app)

    def test_unread_queue_and_shared_layout_contract(self) -> None:
        app = (ROOT / "main/apps/app_rss.c").read_text(encoding="utf-8")
        model = (ROOT / "main/rss/rss_model.h").read_text(encoding="utf-8")
        parser = (ROOT / "main/rss/rss_json.c").read_text(encoding="utf-8")
        preferences = (ROOT / "main/rss/rss_preferences.c").read_text(encoding="utf-8")
        self.assertRegex(app, r"#define RSS_LIST_ROWS 12\b")
        self.assertIn("mark_page_through", app)
        self.assertIn("mark_current_page_read", app)
        self.assertIn("book_layout_build_blocks", app)
        self.assertIn("book_layout_draw_page", app)
        self.assertIn("book_layout_set_images_visible(false)", app)
        self.assertIn("draw_article_progress", app)
        self.assertIn("RSS_PROGRESS_HEIGHT 4", app)
        self.assertIn('未读文章 (%u)', app)
        self.assertNotIn("RSS_VIEW_HOME", app)
        self.assertNotIn("RSS_FILTER_CATEGORY", app)
        self.assertNotIn("RSS_FILTER_STARRED", app)
        self.assertIn("feed_title[RSS_FEED_TITLE_MAX]", model)
        self.assertIn('"title", entry->feed_title', parser)
        self.assertIn(".px = compact ? 20 : 22", preferences)
        self.assertIn(".line_percent = compact ? 112 : 120", preferences)
        self.assertIn(".margin_px = 10", preferences)

    def test_readll_brand_package_and_disabled_ota_contracts(self) -> None:
        project = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        package = (ROOT / "tools/package_flash.py").read_text(encoding="utf-8")
        stage = (ROOT / "tools/stage_public_flash.py").read_text(encoding="utf-8")
        ota_c = (ROOT / "main/ota_online.c").read_text(encoding="utf-8")
        ota_h = (ROOT / "main/ota_online.h").read_text(encoding="utf-8")
        web = (ROOT / "flash/index.html").read_text(encoding="utf-8")
        manifest = json.loads((ROOT / "flash/manifest.json").read_text(encoding="utf-8"))
        feed = json.loads((ROOT / "flash/update.json").read_text(encoding="utf-8"))
        self.assertRegex(project, r"(?m)^project\(readll\)$")
        self.assertIn("READLL_PROJECT_VER", project)
        self.assertIn("build/'readll.bin'", package)
        self.assertIn("readll-update.bin", package)
        self.assertIn("release-assets.json", package)
        self.assertIn("readll.release-assets.v1", package)
        self.assertIn("SHA256SUMS.txt", package)
        self.assertEqual(manifest["name"], "readll")
        self.assertEqual(manifest["readll_delivery"], {"webusb_enabled": True, "tf_enabled": True})
        self.assertFalse(manifest["readll_ota"]["online_enabled"])
        self.assertEqual(feed, {"schema": 1, "enabled": False, "project": "readll",
                                "version": manifest["version"], "message": "在线升级尚未配置"})
        self.assertIn("#define READLL_OTA_ONLINE_CONFIGURED 0", ota_h)
        self.assertIn("在线升级尚未配置", ota_c)
        self.assertNotIn("esp_http_client", ota_c)
        self.assertIn("<esp-web-install-button", web)
        self.assertIn('manifest="./manifest.json"', web)
        self.assertIn('"vendor/web"', stage)
        self.assertIn('"manifest.json"', stage)
        self.assertIn('"firmware.bin"', stage)
        self.assertIn('"readll-update.bin"', stage)
        self.assertIn('"release-assets.json"', stage)
        self.assertIn("hydrate(FLASH, output)", stage)
        self.assertIn('"SHA256SUMS.txt"', stage)
        legacy_brand = "".join(chr(code) for code in (107, 105, 105, 107, 111))
        forbidden = (legacy_brand + "read", legacy_brand + " read",
                     legacy_brand + "-read", "read-pico-" + "rss")
        roots = [ROOT / name for name in ("README.md", "README.zh-CN.md", "README.ja-JP.md", "AGENTS.md")]
        roots += list((ROOT / "docs").glob("*.md"))
        roots += list((ROOT / "flash").glob("*.html"))
        for path in roots:
            text = path.read_text(encoding="utf-8").lower()
            for value in forbidden:
                self.assertNotIn(value, text, f"{value} remains in {path.relative_to(ROOT)}")

    def test_pages_deployment_contract(self) -> None:
        workflow = (ROOT / ".github/workflows/pages.yml").read_text(encoding="utf-8")
        required = (
            "workflow_dispatch:",
            "pages: write",
            "id-token: write",
            "actions/configure-pages@v5",
            "espressif/idf:v6.1",
            "tools/package_flash.py build/pages build/pages-package",
            "tools/stage_public_flash.py _site --package-dir build/pages-package",
            "tools/verify_flash_bundle.py _site",
            "actions/upload-pages-artifact@v4",
            "actions/deploy-pages@v4",
        )
        for value in required:
            self.assertIn(value, workflow)
        ordered = (
            "tools/package_flash.py build/pages build/pages-package",
            "tools/stage_public_flash.py _site --package-dir build/pages-package",
            "tools/verify_flash_bundle.py _site",
            "actions/upload-pages-artifact@v4",
            "actions/deploy-pages@v4",
        )
        positions = [workflow.index(value) for value in ordered]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("push:\n    branches: [main]", workflow)
        self.assertNotIn("wegooo-cell", workflow)
        legacy_brand = "".join(chr(code) for code in
                               (107, 105, 105, 107, 111, 114, 101, 97, 100))
        self.assertNotIn(legacy_brand, workflow)

    def test_release_manifest_schemas_do_not_conflict(self) -> None:
        package = (ROOT / "tools/package_flash.py").read_text(encoding="utf-8")
        hydrator = (ROOT / "tools/release_assets.py").read_text(encoding="utf-8")
        self.assertIn("'schema':'readll.release-assets.v1'", package)
        self.assertIn("source / 'release-sources.json'", hydrator)
        self.assertIn("'readll.release-sources.v1'", hydrator)
        self.assertNotIn("source / 'release-assets.json'", hydrator)

        source = WORK / "release-source-schema"
        output = WORK / "release-source-output"
        shutil.rmtree(source, ignore_errors=True)
        shutil.rmtree(output, ignore_errors=True)
        source.mkdir(parents=True)
        output.mkdir(parents=True)
        name = "readll-update-0.3.3-rc1.bin"
        data = b"verified local release source"
        (source / name).write_bytes(data)
        (source / "manifest.json").write_text(
            json.dumps({"version": "0.3.3-rc1"}), encoding="utf-8")
        (source / "release-sources.json").write_text(json.dumps({
            "schema": "readll.release-sources.v1",
            "tag": "v0.3.3-rc1",
            "files": {
                name: {
                    "asset": name,
                    "size": len(data),
                    "sha256": hashlib.sha256(data).hexdigest(),
                },
            },
        }), encoding="utf-8")
        hydrate(source, output)
        self.assertEqual((output / name).read_bytes(), data)

    def test_usb_product_descriptor_uses_readll(self) -> None:
        expected = 'CONFIG_TINYUSB_DESC_PRODUCT_STRING="readll SD"'
        defaults = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")
        ci = (ROOT / "sdkconfig.ci").read_text(encoding="utf-8")
        self.assertIn(expected, defaults)
        self.assertIn(expected, ci)
        self.assertIn("CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y", ci)
        self.assertNotIn('CONFIG_TINYUSB_DESC_PRODUCT_STRING="Read Pico', defaults)
        self.assertNotIn('CONFIG_TINYUSB_DESC_PRODUCT_STRING="Read Pico', ci)

    def test_parser_fixture_manifest_is_reusable(self) -> None:
        cases = json.loads((FIXTURES / "parser_cases.json").read_text(encoding="utf-8"))
        self.assertEqual({case["parser"] for case in cases},
                         {"rss_json_parse_user", "rss_json_parse_categories", "rss_json_parse_entries"})
        for case in cases:
            if not case.get("generated"):
                payload = (FIXTURES / case["file"]).read_bytes()
                self.assertTrue(payload)
        truncated = (FIXTURES / "entries_truncated.json").read_text(encoding="utf-8")
        with self.assertRaises(json.JSONDecodeError):
            json.loads(truncated)
        unknown = json.loads((FIXTURES / "entries_unknown.json").read_text(encoding="utf-8"))
        self.assertIn("future_top", unknown)
        generated = WORK.parent / "generated-fixtures/generated_entries_oversized.json"
        self.assertTrue(generated.exists())
        self.assertGreater(len(generated.read_bytes()), 24 * 1024)

    def test_no_secret_logging_or_get_json_echo_in_rss_core(self) -> None:
        sources = "\n".join(
            path.read_text(encoding="utf-8")
            for path in (ROOT / "main/rss").glob("*.[ch]")
        )
        self.assertNotRegex(sources, r"\b(?:ESP_LOG[A-Z]|printf|fprintf|puts)\s*\(")
        client = (ROOT / "main/rss/rss_client.c").read_text(encoding="utf-8")
        self.assertIn(".disable_auto_redirect = true", client)
        self.assertIn('"X-Auth-Token"', client)
        self.assertNotRegex(client, r"cJSON_(?:Add|Print)")
        transfer = (ROOT / "components/read_pico_transfer/read_pico_transfer.c").read_text(encoding="utf-8")
        status_body = re.search(
            r"static esp_err_t miniflux_status_response\(.*?\n\}",
            transfer,
            re.DOTALL,
        )
        self.assertIsNotNone(status_body)
        self.assertNotIn('"api_token"', status_body.group(0))

    def test_bookmark_timeout_requires_server_confirmation(self) -> None:
        header = (ROOT / "main/rss/rss_sync.h").read_text(encoding="utf-8")
        source = (ROOT / "main/rss/rss_sync.c").read_text(encoding="utf-8")
        self.assertIn("bookmark_confirmation_pending", header)
        self.assertIn("RSS_CLIENT_TRANSPORT", source)
        self.assertIn("RSS_CLIENT_INCOMPLETE", source)
        self.assertRegex(source, r"!sync->operations\[i\]\.bookmark_confirmation_pending")
        self.assertIn("confirm_bookmark_state(sync, entry->id);", source)


if __name__ == "__main__":
    unittest.main(verbosity=2)
