param([string]$SettingsPath = "$env:LOCALAPPDATA/Aion2Pipe/query-worker/settings.json")
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security
$root = Split-Path $PSScriptRoot -Parent
$settings = Get-Content -LiteralPath $SettingsPath -Raw | ConvertFrom-Json
if ($settings.clientId -notmatch '^[a-zA-Z0-9_-]{1,64}$') { throw 'Invalid query client identity' }
$secretBytes = [Security.Cryptography.ProtectedData]::Unprotect([Convert]::FromBase64String($settings.passwordDpapi), $null, [Security.Cryptography.DataProtectionScope]::CurrentUser)
try {
    if (!$secretBytes.Length -or $secretBytes.Length -gt 4096) { throw 'Invalid query credential' }
    # Private build input only. Never print credentials or place them in logs.
    $literal = ($secretBytes | ForEach-Object { '\x{0:x2}' -f $_ }) -join ''
    $source = '#pragma once' + "`n" + 'namespace aion::embedded_query {' + "`n" + 'inline constexpr char clientId[] = "' + $settings.clientId + '";' + "`n" + 'inline constexpr char password[] = "' + $literal + '";' + "`n}" + "`n"
    $dir = Join-Path $root 'private'
    $null = New-Item -ItemType Directory -Force -Path $dir
    [IO.File]::WriteAllText((Join-Path $dir 'query_credential.hpp'), $source, [Text.UTF8Encoding]::new($false))
    Write-Output 'Private query credential build input generated.'
} finally { [Array]::Clear($secretBytes, 0, $secretBytes.Length) }
