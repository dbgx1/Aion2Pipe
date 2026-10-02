param([ValidateSet('Debug','Release')][string]$Configuration = 'Release', [string]$InstallPrefix = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
& "$PSScriptRoot/bootstrap.ps1"
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmake) { $cmakePath = $cmake.Source } else {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'Install Visual Studio with Desktop development with C++ and CMake tools.' }
    $cmakePath = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
& $cmakePath -S $root -B "$root/build" -A x64
if ($LASTEXITCODE) { throw 'Configure failed' }
& $cmakePath --build "$root/build" --config $Configuration --parallel
if ($LASTEXITCODE) { throw 'Build failed' }
$ctest = Join-Path (Split-Path $cmakePath) 'ctest.exe'
& $ctest --test-dir "$root/build" -C $Configuration --output-on-failure
if ($LASTEXITCODE) { throw 'Tests failed' }
if (!$InstallPrefix) { $InstallPrefix = "$root/dist" }
& $cmakePath --install "$root/build" --config $Configuration --prefix $InstallPrefix
if ($LASTEXITCODE) { throw 'Packaging failed' }
