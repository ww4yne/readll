# 中文：在已激活的 ESP-IDF v6.1 环境中构建，隔离 CI 与真机配置，不执行刷写。
# English: Build in an activated ESP-IDF v6.1 shell, isolating CI/board configs without flashing.
param(
    [ValidateSet('ci', 'board')]
    [string]$Profile = 'ci',
    [switch]$SkipUpstreamCheck
)

$ErrorActionPreference = 'Stop'
$env:PYTHONUTF8 = '1'
$repo = Split-Path -Parent $PSScriptRoot
$idf = Get-Command idf.py -ErrorAction Stop
$version = & $idf --version
if ($LASTEXITCODE -ne 0 -or "$version" -notmatch '^ESP-IDF v6\.1(?:\s|$)') {
    throw "Requires the ESP-IDF v6.1 release; active version: $version"
}
Write-Host $version

$build = Join-Path $repo "build\$Profile"
$defaults = if ($Profile -eq 'ci') { 'sdkconfig.ci' } else { 'sdkconfig.defaults' }
$skipCheck = if ($SkipUpstreamCheck) { 'ON' } else { 'OFF' }
Push-Location $repo
try {
    & $idf -B $build "-DSDKCONFIG=$(Join-Path $build 'sdkconfig')" `
        "-DSDKCONFIG_DEFAULTS=$(Join-Path $repo $defaults)" `
        "-DREADLL_SKIP_UPSTREAM_CHECK=$skipCheck" -DIDF_TARGET=esp32s3 build size
    if ($LASTEXITCODE -ne 0) { throw "Firmware $Profile build failed (exit $LASTEXITCODE)." }
}
finally {
    Pop-Location
}
