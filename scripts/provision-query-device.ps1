param(
    [ValidatePattern('^[a-zA-Z0-9_-]{1,64}$')][string]$ClientId = ([Guid]::NewGuid().ToString('N')),
    [string]$ApiConfig = "$PSScriptRoot/../private/emqx-api.json",
    [switch]$InstallLocal
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security
$settings = Get-Content -LiteralPath $ApiConfig -Raw | ConvertFrom-Json
$secretBytes = [Security.Cryptography.ProtectedData]::Unprotect([Convert]::FromBase64String($settings.appSecretDpapi),$null,[Security.Cryptography.DataProtectionScope]::CurrentUser)
$authorization = 'Basic ' + [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($settings.appId+':'+[Text.Encoding]::UTF8.GetString($secretBytes)))
[Array]::Clear($secretBytes,0,$secretBytes.Length)
$username = 'query-' + $ClientId
$userPath = '/authentication/password_based:built_in_database/users/' + [Uri]::EscapeDataString($username)
try {
    $null = Invoke-WebRequest -UseBasicParsing -Uri ($settings.apiBase+$userPath) -Headers @{Authorization=$authorization} -TimeoutSec 15
    throw 'This device already exists. Existing credentials will not be replaced.'
} catch {
    if ([int]$_.Exception.Response.StatusCode -ne 404) { throw }
}
$random = New-Object byte[] 32
[Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($random)
$password = [Convert]::ToBase64String($random)
$encrypted = [Security.Cryptography.ProtectedData]::Protect([Text.Encoding]::UTF8.GetBytes($password),$null,[Security.Cryptography.DataProtectionScope]::CurrentUser)
$device = @{clientId=$ClientId;username=$username;host='od43e177.ala.cn-shenzhen.emqxsl.cn';websocketPort=8084;passwordDpapi=[Convert]::ToBase64String($encrypted);boot=0}
$directory = Join-Path $PSScriptRoot '../private/query-devices'
$null = New-Item -ItemType Directory -Force -Path $directory
$devicePath = Join-Path $directory ($ClientId+'.json')
if (Test-Path -LiteralPath $devicePath) { throw 'Local device credential already exists; refusing to overwrite.' }
$device | ConvertTo-Json | Set-Content -LiteralPath $devicePath -Encoding UTF8
# Establish narrow permissions before making the credential usable.
$rules = @(
    @{topic="aion2/query-workers/$ClientId/+/state";permission='allow';action='publish';qos=@(1);retain='false'},
    @{topic="aion2/query-workers/$ClientId/+/events";permission='allow';action='publish';qos=@(1);retain='false'},
    @{topic="aion2/query-workers/$ClientId/+/task";permission='allow';action='subscribe';qos=@(1)},
    @{topic='#';permission='deny';action='all'}
)
$acl = ConvertTo-Json -InputObject @(@{username=$username;rules=$rules}) -Depth 6 -Compress
$null = Invoke-WebRequest -UseBasicParsing -Method Post -Uri ($settings.apiBase+'/authorization/sources/built_in_database/rules/users') -Headers @{Authorization=$authorization} -ContentType 'application/json' -Body $acl -TimeoutSec 15
$account = @{user_id=$username;password=$password;is_superuser=$false} | ConvertTo-Json -Compress
$null = Invoke-WebRequest -UseBasicParsing -Method Post -Uri ($settings.apiBase+'/authentication/password_based:built_in_database/users') -Headers @{Authorization=$authorization} -ContentType 'application/json' -Body $account -TimeoutSec 15
$verification = Invoke-RestMethod -Uri ($settings.apiBase+'/authorization/sources/built_in_database/rules/users/'+$username) -Headers @{Authorization=$authorization} -TimeoutSec 15
if ($verification.rules.Count -ne 4) { throw 'Device ACL verification failed' }
if ($InstallLocal) {
    $localDirectory = Join-Path $env:LOCALAPPDATA 'Aion2Pipe/query-worker'
    $localPath = Join-Path $localDirectory 'settings.json'
    if (Test-Path -LiteralPath $localPath) { throw 'Device created; local settings already exist and were not replaced.' }
    $null = New-Item -ItemType Directory -Force -Path $localDirectory
    Copy-Item -LiteralPath $devicePath -Destination $localPath
}
Write-Output "Provisioned device $ClientId with four scoped ACL rules. Password saved locally with DPAPI encryption."
