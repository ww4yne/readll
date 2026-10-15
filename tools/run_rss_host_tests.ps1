param()

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
if ($cc -and (Test-Path (Join-Path $cjson 'cJSON.c'))) {
    $parserExe = Join-Path $work 'rss-parser-fixtures.exe'
    & $cc.Source -std=c11 -Wall -Wextra -Werror `
        "-I$(Join-Path $repo 'main\rss')" "-I$cjson" `
        (Join-Path $repo 'tools\rss_test\rss_parser_fixture_host_test.c') `
        (Join-Path $repo 'main\rss\rss_json.c') `
        (Join-Path $repo 'main\rss\rss_model.c') `
        (Join-Path $cjson 'cJSON.c') -lm -o $parserExe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $parserExe (Join-Path $repo 'tools\rss_test\fixtures') $generated
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
else {
    Write-Host 'RSS production C parser: SKIP (host cc or managed cJSON unavailable); reusable fixtures and contract model still run.'
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
