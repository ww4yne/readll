#!/usr/bin/env python3
"""Validate the public readll web-flashing bundle."""

from __future__ import annotations

import hashlib
import json
import sys
import struct
import re
from pathlib import Path


EXPECTED = {
    "bootloader.bin": (0x0, 0x8000),
    "partitions.bin": (0x8000, 0x10000),
    "firmware.bin": (0x10000, 0x410000),
    "ota_data_initial.bin": (0xD10000, 0xD12000),
}


def check_archived_releases(folder: Path, current_partitions: Path | None = None) -> list[dict]:
    # 历史版本只引用原正式固件，并核对每个完整刷机部件的大小、地址与校验值。
    # Historical choices use original releases; verify every base part's size, address and hash.
    catalog_path = folder / "releases.json"
    if not catalog_path.is_file():
        return []
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    assert catalog.get("schema") == 1 and isinstance(catalog.get("releases"), list)
    versions = set()
    for release in catalog["releases"]:
        version = release["version"]
        assert re.fullmatch(r"\d+\.\d+\.\d+(?:-rc\d+)?", version)
        assert version not in versions, "duplicate archived version"
        versions.add(version)
        assert re.fullmatch(r"[0-9a-f]{40}", release["source_commit"]), "missing release provenance"
        assert release["manifest"] == f"releases/{version}/manifest.json"
        assert release["bin"] == f"readll-update-{version}.bin"
        manifest_path = folder / release["manifest"]
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        assert manifest["version"] == version and manifest.get("new_install_prompt_erase") is True
        assert manifest["readll_ota"]["base_version"] == version
        assert manifest["readll_ota"]["layout"] == "readll-dual-4m-v1" and manifest["readll_ota"]["rollback"] is True
        builds = manifest["builds"]
        assert len(builds) == 1 and builds[0]["chipFamily"] == "ESP32-S3"
        paths = {name: name for name in EXPECTED}
        paths["firmware.bin"] = f"../../{release['bin']}"
        parts = builds[0]["parts"]
        assert len(parts) == len(EXPECTED) and set(release["files"]) == set(EXPECTED)
        seen = set()
        for part in parts:
            name = next((name for name, path in paths.items() if part["path"] == path), None)
            assert name and name not in seen, "unexpected historical part"
            seen.add(name)
            start, end = EXPECTED[name]
            assert part["offset"] == start, "wrong historical flash address"
            target = (manifest_path.parent / part["path"]).resolve()
            assert target.is_relative_to(folder.resolve()), "historical file escapes public bundle"
            data = target.read_bytes()
            assert data and len(data) <= end - start
            expected = release["files"][name]
            assert expected["size"] == len(data) and expected["sha256"] == hashlib.sha256(data).hexdigest(), (
                f"historical bytes changed: {version} {name}")
            if name == "firmware.bin":
                assert data[0] == 0xe9 and struct.unpack_from("<H", data, 12)[0] == 9
                assert struct.unpack_from("<I", data, 32)[0] == 0xabcd5432
                assert data[48:80].split(b"\0")[0].decode() == version
                assert data[80:112].split(b"\0")[0] == b"readll"
            elif name == "partitions.bin":
                assert data == (current_partitions or folder / "partitions.bin").read_bytes(), "historical layout incompatible"
        print(f"Historical USB bundle verified: {version} (original bytes, preserve-data default)")
    assert (folder / "versions.js").is_file(), "missing version selector"
    return catalog["releases"]


def check(folder: Path, require_site: bool = True) -> None:
    manifest = json.loads((folder / "manifest.json").read_text(encoding="utf-8"))
    assert manifest.get("new_install_prompt_erase") is True, "flasher must preserve user data"
    assert manifest.get("readll_delivery") == {"webusb_enabled": True, "tf_enabled": True}
    builds = manifest.get("builds")
    assert isinstance(builds, list) and len(builds) == 1
    assert builds[0].get("chipFamily") == "ESP32-S3"
    parts = builds[0].get("parts")
    assert isinstance(parts, list) and len(parts) == len(EXPECTED)
    seen: set[str] = set()
    for part in parts:
        name = part.get("path")
        assert name in EXPECTED and name not in seen, f"unexpected flash part: {name}"
        seen.add(name)
        start, end = EXPECTED[name]
        assert part.get("offset") == start, f"wrong offset for {name}"
        data = (folder / name).read_bytes()
        assert data and len(data) <= end - start, f"invalid size for {name}"
        print(f"{name}: {len(data)} bytes, SHA256 {hashlib.sha256(data).hexdigest()}")
    assert seen == set(EXPECTED)
    # 本地升级门禁：实际双槽分区、应用描述与 TF 包必须一致。
    # Local-update gate: real slots, app description and TF package must agree.
    entries = {}
    table = (folder / "partitions.bin").read_bytes()
    for offset in range(0, len(table) - 31, 32):
        magic, kind, subtype, address, size, label, _ = struct.unpack_from("<HBBII16sI", table, offset)
        if magic != 0x50aa: break
        entries[label.split(b"\0")[0].decode()] = (kind, subtype, address, size)
    assert entries.get("ota_0") == (0, 16, 0x10000, 0x400000), "base OTA slot changed"
    assert entries.get("ota_1") == (0, 17, 0x910000, 0x400000), "inactive OTA slot changed"
    assert entries.get("otadata") == (1, 0, 0xd10000, 0x2000), "OTA selection metadata changed"
    assert entries.get("nvs") == (1, 2, 0x9000, 0x5000), "user settings partition changed"
    assert entries.get("storage") == (1, 129, 0x410000, 0x500000), "user storage partition changed"
    image = (folder / "firmware.bin").read_bytes()
    assert image[0] == 0xe9 and struct.unpack_from("<H", image, 12)[0] == 9, "wrong target chip"
    assert struct.unpack_from("<I", image, 32)[0] == 0xabcd5432, "missing app description"
    version = image[48:80].split(b"\0")[0].decode("ascii")
    project = image[80:112].split(b"\0")[0].decode("ascii")
    assert version == manifest["version"] and project == "readll", "base version mismatch"
    assert re.fullmatch(r"\d+\.\d+\.\d+(?:-rc\d+)?", version), "invalid release version"
    capability = manifest.get("readll_ota", {})
    assert capability.get("layout") == "readll-dual-4m-v1"
    assert capability.get("base_version") == version and capability.get("rollback") is True
    assert capability.get("online_enabled") is False
    feed = json.loads((folder / "update.json").read_text(encoding="utf-8"))
    upgrade_name = f"readll-update-{version}.bin"
    assert feed == {"schema": 1, "enabled": False, "project": project, "version": version,
                    "message": "在线升级尚未配置"}
    assert (folder / "readll-update.bin").read_bytes() == image
    assert (folder / upgrade_name).read_bytes() == image
    assets = json.loads((folder / "release-assets.json").read_text(encoding="utf-8"))
    assert assets["schema"] == "readll.release-assets.v1"
    assert assets["project"] == project and assets["version"] == version
    expected_assets = set(EXPECTED) | {"readll-update.bin", upgrade_name}
    assert set(assets["files"]) == expected_assets
    for name, metadata in assets["files"].items():
        data = (folder / name).read_bytes()
        assert metadata == {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    checksum_lines = (folder / "SHA256SUMS.txt").read_text(encoding="ascii").splitlines()
    assert set(checksum_lines) == {
        f"{assets['files'][name]['sha256']}  {name}" for name in expected_assets
    }
    print(f"WebUSB/USB/TF release assets compatible; device online OTA disabled: {version}, {capability['layout']}")
    if require_site:
        index = (folder / "index.html").read_text(encoding="utf-8")
        assert '<esp-web-install-button' in index and 'manifest="./manifest.json"' in index
        assert (folder / "vendor/web/install-button.js").is_file()
        assert (folder / "licenses/LUCIDE-ISC.txt").is_file()
        check_archived_releases(folder)


if __name__ == "__main__":
    check(Path(sys.argv[1]) if len(sys.argv) > 1 else Path("flash"))
