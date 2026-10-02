param([switch]$SkipTests)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$bridge = Join-Path $root 'chat_bridge'
$source = Join-Path $bridge 'source'
$venv = Join-Path $bridge '.venv-local'
$python = Join-Path $venv 'Scripts\python.exe'
$localBasePython = Join-Path $bridge '.python\python.exe'

if (-not (Test-Path -LiteralPath $python)) {
    if (Test-Path -LiteralPath $localBasePython) {
        & $localBasePython -m venv $venv
    } else {
        & py -3 -m venv $venv
    }
    if ($LASTEXITCODE) { throw 'Unable to create the chat bridge Python environment.' }
}

& $python -m pip install --disable-pip-version-check -r (Join-Path $source 'requirements.txt') -r (Join-Path $bridge 'requirements-build.txt')
if ($LASTEXITCODE) { throw 'Unable to install chat bridge build dependencies.' }

if (-not $SkipTests) {
    Push-Location $source
    try {
        & $python (Join-Path $source 'test_client_optional.py') (Join-Path $source 'mitm_ws_message_monitor.py')
        if ($LASTEXITCODE) { throw 'Chat bridge optional payload test failed.' }
        $testArgs = @('-m','unittest','test_account_switch','test_faction','test_login_ipc','test_relay_recovery','test_relay_reliability','test_game_server_status')
        & $python $testArgs
        if ($LASTEXITCODE) { throw 'Chat bridge tests failed.' }
    } finally {
        Pop-Location
    }
}

& $python -m PyInstaller --noconfirm --clean --distpath (Join-Path $bridge 'dist') --workpath (Join-Path $bridge 'build') (Join-Path $bridge 'Aion2ChatBridge.spec')
if ($LASTEXITCODE) { throw 'Chat bridge packaging failed.' }

$output = Join-Path $bridge 'dist\Aion2ChatBridge\Aion2ChatBridge.exe'
if (-not (Test-Path -LiteralPath $output)) { throw "Chat bridge output is missing: $output" }
Get-Item -LiteralPath $output | Select-Object FullName,Length,LastWriteTime
