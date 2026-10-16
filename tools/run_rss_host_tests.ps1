param(
    [string]$CJsonPath = ''
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$python = (Get-Command py -ErrorAction Stop).Source
$work = Join-Path $repo 'build\rss-tests'
$generated = Join-Path $work 'generated-fixtures'

New-Item -ItemType Directory -Force -Path $generated | Out-Null
$env:PYTHONDONTWRITEBYTECODE = '1'
$env:RSS_TEST_WORKDIR = Join-Path $work 'work'

& $python -3.12 (Join-Path $repo 'tools\rss_test\generate_parser_fixtures.py') $generated
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$cc = Get-Command cc -ErrorAction SilentlyContinue
$cjson = Join-Path $repo 'managed_components\espressif__cjson\cJSON'
if ($CJsonPath) { $cjson = (Resolve-Path $CJsonPath).Path }
elseif (!(Test-Path (Join-Path $cjson 'cJSON.c'))) {
    $cjson = Join-Path $work 'cjson'
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vcvars = ''
if (!$cc -and (Test-Path $vswhere)) {
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vs) { $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat' }
}
$hasMsvc = $vcvars -and (Test-Path $vcvars)
if (($cc -or $hasMsvc) -and (Test-Path (Join-Path $cjson 'cJSON.c'))) {
    $parserExe = Join-Path $work 'rss-parser-fixtures.exe'
    $coreExe = Join-Path $work 'rss-on-demand.exe'
    $common = @(
        (Join-Path $repo 'main\rss\rss_json.c'),
        (Join-Path $repo 'main\rss\rss_model.c'),
        (Join-Path $cjson 'cJSON.c')
    )
    function Build-HostTest($Target, $Sources) {
        if ($cc) {
            & $cc.Source -std=c11 -Wall -Wextra -Werror `
                "-I$(Join-Path $repo 'main\rss')" "-I$cjson" `
                "-I$(Join-Path $repo 'tools\rss_test\stubs')" `
                @Sources -lm -o $Target
        }
        else {
            $quoted = ($Sources | ForEach-Object { "`"$_`"" }) -join ' '
            $line = "call `"$vcvars`" >nul && cd /d `"$work`" && cl /nologo /std:c11 /W3 /WX /wd4996 " +
                "/I`"$(Join-Path $repo 'main\rss')`" /I`"$cjson`" " +
                "/I`"$(Join-Path $repo 'tools\rss_test\stubs')`" $quoted /Fe:`"$Target`""
            & $env:ComSpec /c $line
        }
        if ($LASTEXITCODE -ne 0) { throw "RSS production C build failed: $Target" }
    }
    Build-HostTest $parserExe (@((Join-Path $repo 'tools\rss_test\rss_parser_fixture_host_test.c')) + $common)
    & $parserExe (Join-Path $repo 'tools\rss_test\fixtures') $generated
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Build-HostTest $coreExe (@(
        (Join-Path $repo 'tools\rss_test\rss_on_demand_host_test.c'),
        (Join-Path $repo 'main\rss\rss_client.c'),
        (Join-Path $repo 'main\rss\rss_cache.c'),
        (Join-Path $repo 'main\rss\rss_sync.c')
    ) + $common)
    & $coreExe (Join-Path $generated 'generated_entry.json') `
        (Join-Path $work 'metadata.cache') (Join-Path $work 'legacy.cache')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
else {
    Write-Host 'RSS production C tests: SKIP (cc/MSVC or cJSON unavailable); provide -CJsonPath for cJSON 1.7.19.'
}

Push-Location $repo
try {
    & $python -3.12 -m unittest -v tools.rss_test.test_rss
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
finally {
    Pop-Location
}

Write-Host 'RSS host tests PASS: mock Miniflux, model/fixture contracts, cache/sync/security checks.'
