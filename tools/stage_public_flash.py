#!/usr/bin/env python3
"""Stage only the files intended for the public HTTPS flashing site."""

from __future__ import annotations

from html.parser import HTMLParser
from pathlib import Path
import shutil
import json
import re
import sys
from urllib.parse import urlparse

from verify_flash_bundle import check
from release_assets import hydrate


ROOT = Path(__file__).resolve().parents[1]
FLASH = ROOT / "flash"


class LocalLinks(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.links: list[str] = []

    def handle_starttag(self, _tag: str, attrs: list[tuple[str, str | None]]) -> None:
        for key, value in attrs:
            if key in {"href", "src", "manifest"} and value:
                self.links.append(value)


def stage(output: Path, package_dir: Path | None = None) -> None:
    package_dir = package_dir or (FLASH if (FLASH / "firmware.bin").is_file() else ROOT / "build/readll-package")
    if not (package_dir / "firmware.bin").is_file():
        raise FileNotFoundError("readll package not found; run tools/package_flash.py first or pass --package-dir")
    output.mkdir(parents=True, exist_ok=True)
    for name in ("index.html", "notices.html", "manual.html", "site.css", "site.js", "installer-bridge.js",
                 "versions.js", "toc-preview.png"):
        if (FLASH / name).is_file(): shutil.copy2(FLASH / name, output / name)
    for name in ("manifest.json", "firmware.bin", "bootloader.bin", "partitions.bin",
                 "ota_data_initial.bin", "readll-update.bin", "update.json",
                 "release-assets.json", "SHA256SUMS.txt"):
        shutil.copy2(package_dir / name, output / name)
    # 官网仅发布展示资源与用户说明；内部构建及调试记录不进入网站。
    # Publish website assets and the user manual, excluding private build and debug files.
    shutil.copytree(FLASH / "assets", output / "assets", dirs_exist_ok=True)
    # 中文：只复制仓库中的公开说明书（正文、检索索引、图解、PDF）。
    # English: Copy the curated public manual, never its local rendering/debug workspace.
    shutil.copytree(FLASH / "manual", output / "manual", dirs_exist_ok=True)
    version = json.loads((package_dir / "manifest.json").read_text(encoding="utf-8"))["version"]
    versioned = package_dir / f"readll-update-{version}.bin"
    if versioned.is_file(): shutil.copy2(versioned, output / versioned.name)
    # 可选发布源清单只补充已校验的本地历史资产，不与成品哈希清单共用 schema。
    # The optional source catalog adds verified local history without sharing the product-hash schema.
    hydrate(FLASH, output)
    # 只发布当前 readll 本地构建的版本化 TF 包，不从旧仓库下载历史产物。
    # Publish only versioned TF packages from the current local readll build; never fetch legacy artifacts.
    for upgrade in package_dir.glob("readll-update-*.bin"):
        name = re.fullmatch(r"readll-update-(\d+\.\d+\.\d+(?:-rc\d+)?)\.bin", upgrade.name)
        if not name: continue
        with upgrade.open("rb") as source:
            header = source.read(112)
        assert header[48:80].split(b"\0")[0].decode("ascii") == name[1], "historical upgrade version mismatch"
        assert header[80:112].split(b"\0")[0] == b"readll", "historical upgrade project mismatch"
        shutil.copy2(upgrade, output / upgrade.name)
    shutil.copytree(FLASH / "vendor/web", output / "vendor/web", dirs_exist_ok=True)
    shutil.copy2(FLASH / "vendor/LICENSE", output / "vendor/LICENSE")
    licenses = {
        FLASH / "licenses/LUCIDE-ISC.txt": "LUCIDE-ISC.txt",
        ROOT / "components/jpegdec/LICENSE": "JPEGDEC-APACHE-2.0.txt",
        ROOT / "licenses/LGPL-3.0.txt": "LGPL-3.0.txt",
        ROOT / "licenses/GPL-3.0.txt": "GPL-3.0.txt",
        ROOT / "components/read_pico_search/LICENSE.pypinyin": "PYPINYIN-MIT.txt",
        ROOT / "main/assets/OFL-Noto.txt": "OFL-Noto.txt",
        ROOT / "licenses/CrossMux-MIT.txt": "CROSSMUX-MIT.txt",
        ROOT / "licenses/FreeInk-SDK-MIT.txt": "FREEINK-SDK-MIT.txt",
    }
    (output / "licenses").mkdir(exist_ok=True)
    for source, name in licenses.items():
        shutil.copy2(source, output / "licenses" / name)
    shutil.copy2(ROOT / "LICENSE", output / "LICENSE")
    (output / ".nojekyll").touch()

    for page in ("index.html", "notices.html", "manual.html", "manual/index.html"):
        links = LocalLinks()
        links.feed((output / page).read_text(encoding="utf-8"))
        for link in links.links:
            parsed = urlparse(link)
            if parsed.scheme or parsed.netloc or not parsed.path:
                continue
            target = (output / Path(page).parent / parsed.path).resolve()
            if target.is_dir(): target = target / 'index.html'
            assert target.is_relative_to(output.resolve()) and target.is_file(), (
                f"broken local link in {page}: {link}")
    check(output)
    files = list(output.rglob("*"))
    count = sum(path.is_file() for path in files)
    size = sum(path.stat().st_size for path in files if path.is_file())
    print(f"Public site: {count} files, {size} bytes")


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('output', nargs='?', type=Path, default=ROOT / '_site')
    parser.add_argument('--package-dir', type=Path)
    args = parser.parse_args()
    stage(args.output, args.package_dir)
