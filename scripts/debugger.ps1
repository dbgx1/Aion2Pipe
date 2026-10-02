param(
    [string]$Tool = 'GetDebugState',
    [string]$ArgumentsJson = '{}',
    [string]$Config = 'E:\tool\x64dbg2.472\release\x64\mcp_config.json',
    [switch]$ListTools
)
$ErrorActionPreference = 'Stop'
$cfg = Get-Content -LiteralPath $Config | ConvertFrom-Json
if ($cfg.IpAddress -notin @('127.0.0.1','localhost','::1')) { throw 'Only local debugger connections are supported.' }
$request = @{jsonrpc='2.0'; id=1}
if ($ListTools) { $request.method='tools/list'; $request.params=@{} }
else { $request.method='tools/call'; $request.params=@{name=$Tool; arguments=($ArgumentsJson | ConvertFrom-Json)} }
$response = Invoke-RestMethod -Uri ('http://127.0.0.1:'+$cfg.Port+'/') -Method Post -Headers @{Authorization=('Bearer '+$cfg.AuthToken)} -ContentType 'application/json' -Body ($request | ConvertTo-Json -Depth 20 -Compress) -TimeoutSec 40
$response | ConvertTo-Json -Depth 30
