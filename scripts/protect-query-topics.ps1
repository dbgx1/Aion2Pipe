param([string]$ApiConfig="$PSScriptRoot/../private/emqx-api.json")
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Security
$settings=Get-Content -LiteralPath $ApiConfig -Raw|ConvertFrom-Json
$bytes=[Security.Cryptography.ProtectedData]::Unprotect([Convert]::FromBase64String($settings.appSecretDpapi),$null,[Security.Cryptography.DataProtectionScope]::CurrentUser)
$headers=@{Authorization=('Basic '+[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($settings.appId+':'+[Text.Encoding]::UTF8.GetString($bytes))))}
[Array]::Clear($bytes,0,$bytes.Length)
$url=$settings.apiBase+'/authorization/sources/built_in_database/rules/all'
$existing=Invoke-RestMethod $url -Headers $headers -TimeoutSec 15
$backup=Join-Path $PSScriptRoot ('../private/emqx-global-acl-'+(Get-Date -Format yyyyMMddHHmmss)+'.json')
$existing|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $backup -Encoding UTF8
# Device-specific allow rules take precedence. Other accounts cannot send tasks,
# impersonate worker events, or publish presence results. Preserve unrelated ACLs.
$required=@(
 @{topic='aion2/query-workers/#';permission='deny';action='all'},
 @{topic='aion2/presence/aion2web/results/#';permission='deny';action='publish'}
)
$rules=@($required)
foreach($rule in $existing.rules){
 if(!($required|Where-Object {$_.topic -eq $rule.topic -and $_.action -eq $rule.action -and $_.permission -eq $rule.permission})){$rules+=$rule}
}
$body=@{rules=$rules}|ConvertTo-Json -Depth 12 -Compress
$null=Invoke-WebRequest -Method Post -Uri $url -Headers $headers -ContentType application/json -Body $body -TimeoutSec 15
$actual=Invoke-RestMethod $url -Headers $headers -TimeoutSec 15
foreach($rule in $required){if(!($actual.rules|Where-Object {$_.topic -eq $rule.topic -and $_.permission -eq 'deny' -and $_.action -eq $rule.action})){throw 'Query topic ACL verification failed'}}
'Query topic global denies installed; previous ACL saved locally. Verify device access and HTTP publication before release.'
