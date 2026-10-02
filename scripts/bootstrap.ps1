$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$deps = Join-Path $root 'third_party'
New-Item -ItemType Directory -Force -Path $deps | Out-Null
$packages = @(
    @{ Name='sqlite'; Folder='sqlite-amalgamation-3530400'; Url='https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip'; Hash='1E71DDF93849C6A6ECF58B827C0692073D2DD7EE40196158068F7B29F422E87D' },
    @{ Name='imgui'; Folder='imgui-1.91.9b'; Url='https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.zip'; Hash='FD37507C8476A6D14CC7C4B352401F31BCBD0F0D995D35390811E968C466F46E' },
    @{ Name='windivert'; Folder='WinDivert-2.2.2-A'; Url='https://github.com/basil00/WinDivert/releases/download/v2.2.2/WinDivert-2.2.2-A.zip'; Hash='63CB41763BB4B20F600B6DE04E991A9C2BE73279E317D4D82F237B150C5F3F15' }
    @{ Name='miniz'; Folder='miniz-3.0.2'; Url='https://github.com/richgel999/miniz/archive/refs/tags/3.0.2.zip'; Hash='1B4FAC7144A64D8D26C4642C25BD5B237599071EAF7C12FC6EAFF294AF31E931' },
    @{ Name='json'; Folder='json-3.11.3'; Url='https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.zip'; Hash='04022B05D806EB5FF73023C280B68697D12B93E1B7267A0B22A1A39EC7578069' }
)
foreach ($package in $packages) {
    $zip = Join-Path $deps ($package.Name + '.zip')
    if (!(Test-Path -LiteralPath $zip)) {
        & curl.exe -L --fail --retry 2 -o $zip $package.Url
        if ($LASTEXITCODE -ne 0) { throw "Download failed: $($package.Name)" }
    }
    if ((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash -ne $package.Hash) { throw "SHA256 mismatch: $zip" }
    if (!(Test-Path -LiteralPath (Join-Path $deps $package.Folder))) { Expand-Archive -LiteralPath $zip -DestinationPath $deps }
}
Write-Host 'Dependencies verified and ready.'
