param(
 [Parameter(Mandatory=$true)][ValidatePattern('^[a-zA-Z0-9_-]{1,64}$')][string]$ClientId,
 [switch]$RevealPassword
)
$ErrorActionPreference='Stop'
$path=Join-Path $PSScriptRoot ('../private/query-devices/'+$ClientId+'.json')
$device=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json
if($device.clientId -ne $ClientId){throw 'Device identity mismatch'}
if(!$RevealPassword){
 [pscustomobject]@{clientId=$device.clientId;username=$device.username;password='(hidden; use -RevealPassword to display this device password)'}
 return
}
Add-Type -AssemblyName System.Security
$bytes=[Security.Cryptography.ProtectedData]::Unprotect([Convert]::FromBase64String($device.passwordDpapi),$null,[Security.Cryptography.DataProtectionScope]::CurrentUser)
try {
 [pscustomobject]@{clientId=$device.clientId;username=$device.username;password=[Text.Encoding]::UTF8.GetString($bytes)}
} finally {[Array]::Clear($bytes,0,$bytes.Length)}
