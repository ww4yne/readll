"""中文：验证联网升级禁用提示与 TF 本地升级入口。
English: Verify the disabled online-update notice and retained local TF path.
SPDX-License-Identifier: Apache-2.0
"""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
backend = (root / "main/ota_online.c").read_text(encoding="utf-8")
header = (root / "main/ota_online.h").read_text(encoding="utf-8")
ui = (root / "main/apps/app_device_settings.c").read_text(encoding="utf-8")
local = (root / "main/ota_update.h").read_text(encoding="utf-8")

assert '#define READLL_OTA_ONLINE_CONFIGURED 0' in header
assert "在线升级尚未配置" in backend
assert "esp_http_client" not in backend
assert "readll_online_check()" in ui
assert "readll_online_download()" in ui
assert "readll_online_get_status(&s_update)" in ui
assert "READLL_OTA_UPDATE_PATH" in ui
assert "readll-update.bin" in local

print("PASS: UI receives an explicit not-configured result; online backend has no HTTP path; TF update remains")
