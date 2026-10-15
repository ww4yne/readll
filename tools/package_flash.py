#!/usr/bin/env python3
"""中文：从同一次编译生成 WebUSB/USB 与 TF 资产，并写出禁用的设备联网 OTA 状态。/ English: Stage WebUSB/USB and TF assets plus an explicitly disabled device-OTA status."""
from pathlib import Path
from datetime import datetime, timedelta, timezone
import hashlib, json, shutil, sys
from verify_flash_bundle import check
ROOT=Path(__file__).resolve().parents[1]
def package(build: Path, output: Path):
    config=(build/'config/sdkconfig.h').read_text(encoding='utf-8')
    assert '#define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1' in config, 'rollback bootloader required'
    data=(build/'readll.bin').read_bytes()
    assert data[0]==0xe9 and int.from_bytes(data[32:36],'little')==0xabcd5432
    version=data[48:80].split(b'\0')[0].decode('ascii')
    upgrade_name=f'readll-update-{version}.bin'
    # 本地同版本测试只能打到构建目录，已发布文件的下载内容必须固定。
    # Same-version local tests belong in build directories; published downloads stay immutable.
    published=ROOT/'flash'/upgrade_name
    if output.resolve()==(ROOT/'flash').resolve() and published.is_file():
        assert published.read_bytes()==data, (
            'published version already exists with different bytes; '
            'stage local tests in build/, or increment the version for an authorized release')
    manifest=json.loads((ROOT/'flash/manifest.json').read_text(encoding='utf-8'))
    manifest['version']=version
    manifest['name']='readll'
    manifest['updated_at']=datetime.now(timezone(timedelta(hours=8))).isoformat(timespec='seconds')
    manifest['readll_delivery']={'webusb_enabled':True,'tf_enabled':True}
    manifest['readll_ota']={'layout':'readll-dual-4m-v1','base_version':version,
        'rollback':True,'online_enabled':False}
    output.mkdir(parents=True,exist_ok=True)
    parts={'firmware.bin':build/'readll.bin','bootloader.bin':build/'bootloader/bootloader.bin',
           'partitions.bin':build/'partition_table/partition-table.bin','ota_data_initial.bin':build/'ota_data_initial.bin'}
    for name,path in parts.items():shutil.copy2(path,output/name)
    shutil.copy2(build/'readll.bin',output/upgrade_name)
    shutil.copy2(build/'readll.bin',output/'readll-update.bin')
    release={'schema':1,'enabled':False,'project':'readll','version':version,
        'message':'在线升级尚未配置'}
    (output/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    (output/'update.json').write_text(json.dumps(release,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    asset_names=tuple(parts)+('readll-update.bin',upgrade_name)
    assets={'schema':'readll.release-assets.v1','project':'readll','version':version,'files':{
        name:{'size':(output/name).stat().st_size,'sha256':hashlib.sha256((output/name).read_bytes()).hexdigest()}
        for name in asset_names}}
    (output/'release-assets.json').write_text(json.dumps(assets,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    checksums=''.join(f"{assets['files'][name]['sha256']}  {name}\n" for name in sorted(asset_names))
    (output/'SHA256SUMS.txt').write_text(checksums,encoding='ascii')
    check(output, require_site=False)
if __name__=='__main__':package(Path(sys.argv[1]),Path(sys.argv[2]) if len(sys.argv)>2 else ROOT/'flash')
