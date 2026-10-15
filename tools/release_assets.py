#!/usr/bin/env python3
"""Copy explicitly provided local release assets with bounded sizes and SHA-256 checks.
中文：未配置远端发布仓库；只接受源码目录中已提供并校验的本地文件。
English: No remote release repository is configured; accept only verified local files.
"""
from pathlib import Path
import hashlib
import json
import re
import shutil

def hydrate(source: Path, output: Path) -> None:
    catalog = source / 'release-sources.json'
    if not catalog.is_file():
        return
    release = json.loads(catalog.read_text(encoding='utf-8'))
    assert release.get('schema') == 'readll.release-sources.v1'
    assert re.fullmatch(r'v\d+\.\d+\.\d+(?:-rc\d+)?', release['tag'])
    assert release['tag'] == 'v' + json.loads((source / 'manifest.json').read_text(encoding='utf-8'))['version']
    _hydrate_files(source, output, release)
    # 中文：旧固定网址继续从原 Release 取字节，不能被当前版本别名覆盖。
    # English: Keep immutable older URLs backed by their original Release, not current aliases.
    seen = set(release['files'])
    for historical in release.get('historical', []):
        assert re.fullmatch(r'v\d+\.\d+\.\d+(?:-rc\d+)?', historical['tag'])
        expected = 'readll-update-' + historical['tag'][1:] + '.bin'
        assert set(historical['files']) == {expected} and expected not in seen
        seen.add(expected)
        _hydrate_files(source, output, historical)

def _hydrate_files(source: Path, output: Path, release: dict) -> None:
    fetched = {}
    for name, info in release['files'].items():
        assert re.fullmatch(r'[A-Za-z0-9_.-]+\.bin', name)
        asset = info['asset']
        assert re.fullmatch(r'[A-Za-z0-9_.-]+\.bin', asset)
        assert re.fullmatch(r'[a-f0-9]{64}', info['sha256']) and 0 < info['size'] <= 0x400000
        target = output / name
        local = source / name
        if local.is_file() and local.stat().st_size == info['size'] and hashlib.sha256(local.read_bytes()).hexdigest() == info['sha256']:
            shutil.copy2(local, target)
            fetched[asset] = target
            continue
        if asset in fetched:
            cached = fetched[asset]
            assert cached.stat().st_size == info['size'] and hashlib.sha256(cached.read_bytes()).hexdigest() == info['sha256'], 'Release alias checksum mismatch'
            shutil.copy2(cached, target)
            continue
        raise FileNotFoundError(f"local release asset is not configured: {asset}")
