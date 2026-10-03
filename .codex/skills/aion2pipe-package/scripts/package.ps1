param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Package')]
    [ValidatePattern('^[0-9A-Za-z][0-9A-Za-z._-]{0,63}$')]
    [string]$Version,
    [Parameter(Mandatory = $true, ParameterSetName = 'Check')]
    [switch]$CheckOnly
)

$ErrorActionPreference = 'Stop'
$skillDir = Split-Path $PSScriptRoot -Parent
$root = Split-Path (Split-Path (Split-Path $skillDir -Parent) -Parent) -Parent
. (Join-Path $PSScriptRoot 'git-guard.ps1')
$gitBefore = Get-PackageGitSnapshot -Root $root
if ($CheckOnly) {
    [pscustomobject]@{ GitHygienePassed = $true; VisibleFiles = $gitBefore.Count; BuildStarted = $false } | ConvertTo-Json
    return
}
$releaseRoot = Join-Path $root 'release'
$packageStartedAt = [DateTimeOffset]::UtcNow.ToOffset([TimeSpan]::FromHours(8))
$packageTimestamp = $packageStartedAt.ToString('yyyyMMdd-HHmmss', [Globalization.CultureInfo]::InvariantCulture)
$name = "Aion2Pipe-$Version-$packageTimestamp-UTC8-win64-portable"
$stage = Join-Path $releaseRoot $name
$zip = Join-Path $releaseRoot "$name.zip"

if (Test-Path -LiteralPath $stage) { throw "Release directory already exists: $stage" }
if (Test-Path -LiteralPath $zip) { throw "Release archive already exists: $zip" }

New-Item -ItemType Directory -Path $releaseRoot -Force | Out-Null
& (Join-Path $root 'scripts\build-chat-bridge.ps1')
if ($LASTEXITCODE) { throw 'Chat bridge build or tests failed.' }
$ChatBridgeSource = Join-Path $root 'chat_bridge\dist\Aion2ChatBridge'
& (Join-Path $root 'scripts\build.ps1') -Configuration Release -InstallPrefix $stage
if ($LASTEXITCODE) { throw 'Build, tests, or install failed.' }

Copy-Item -LiteralPath $ChatBridgeSource -Destination (Join-Path $stage 'chat_bridge') -Recurse

# A development chat bundle may contain machine-specific runtime remnants.
# They are never distributable, even when the executable itself is reusable.
Get-ChildItem -LiteralPath (Join-Path $stage 'chat_bridge') -Recurse -File |
    Where-Object { $_.Extension -in '.log','.pcap','.cap','.a2session','.jsonl' } |
    Remove-Item -Force
$mqttConfig = Join-Path $root 'private\chat_bridge\mqtt.private.local.json'
if (Test-Path -LiteralPath $mqttConfig) {
    Copy-Item -LiteralPath $mqttConfig -Destination (Join-Path $stage 'chat_bridge\mqtt.private.local.json')
}

$quickStart = @'
Aion2Pipe 便携版

使用顺序：
1. 完全退出旧版 Aion2Pipe 和游戏。
2. 先启动加速器。
3. 右键“以管理员身份运行”Aion2Pipe.exe。
4. 再启动游戏，必须建立全新的连接。

本版本仅使用虚拟网卡/系统路由模式，不使用 Clash 或 7897。
程序不识别加速器品牌、进程或动态中继端口。
请在加速器中开启虚拟网卡/TUN 模式，所有外连沿 Windows 当前路由运行。

不要同时运行两个 Aion2Pipe。程序目录可放在任意本地磁盘路径。
'@
Set-Content -LiteralPath (Join-Path $stage '使用说明.txt') -Value $quickStart -Encoding UTF8

$forbidden = Get-ChildItem -LiteralPath $stage -Recurse -File |
    Where-Object { $_.Extension -in '.json','.ini','.yaml','.yml','.txt','.md','.bat','.cmd','.ps1' } |
    Select-String -SimpleMatch 'E:\project\aion2','E:\project\Aion2Pipe' -ErrorAction SilentlyContinue
if ($forbidden) { throw 'Development paths were found in the staged release.' }
$privateArtifacts = Get-ChildItem -LiteralPath $stage -Recurse -File |
    Where-Object { $_.Extension -in '.log','.pcap','.cap','.a2session','.jsonl' }
if ($privateArtifacts) { throw 'Runtime logs or capture artifacts were found in the staged release.' }

[pscustomobject]@{
    version = $Version
    packageName = $name
    packagingStartedAt = $packageStartedAt.ToString('o')
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stage 'build-info.json') -Encoding UTF8

$files = Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName
$manifest = foreach ($file in $files) {
    $relative = $file.FullName.Substring($stage.Length + 1).Replace('\','/')
    [pscustomobject]@{ path = $relative; size = $file.Length }
}
$manifest | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $stage 'manifest.json') -Encoding UTF8

$hashLines = Get-ChildItem -LiteralPath $stage -Recurse -File |
    Where-Object Name -ne 'SHA256SUMS.txt' |
    Sort-Object FullName |
    ForEach-Object {
        $relative = $_.FullName.Substring($stage.Length + 1).Replace('\','/')
        $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        "$hash  $relative"
    }
$hashLines | Set-Content -LiteralPath (Join-Path $stage 'SHA256SUMS.txt') -Encoding ASCII

Assert-PackageGitSnapshot -Root $root -Before $gitBefore
Compress-Archive -LiteralPath $stage -DestinationPath $zip -CompressionLevel Optimal
$exeHash = (Get-FileHash -LiteralPath (Join-Path $stage 'Aion2Pipe.exe') -Algorithm SHA256).Hash
$zipHash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
Assert-PackageGitSnapshot -Root $root -Before $gitBefore
[pscustomobject]@{
    Version = $Version
    PackagingStartedAt = $packageStartedAt.ToString('o')
    Directory = $stage
    Archive = $zip
    Aion2PipeSHA256 = $exeHash
    ArchiveSHA256 = $zipHash
    GitVisibleFilesUnchanged = $true
    Files = (Get-ChildItem -LiteralPath $stage -Recurse -File).Count
} | ConvertTo-Json -Depth 3
