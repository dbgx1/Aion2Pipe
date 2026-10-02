# Dot-source from package.ps1. Read-only checks: never modify the Git index.
function Get-PackageGitSnapshot {
    param([Parameter(Mandatory = $true)][string]$Root)
    $resolvedRoot = (Resolve-Path -LiteralPath $Root).Path
    $gitRoot = & git -C $resolvedRoot rev-parse --show-toplevel 2>$null
    if ($LASTEXITCODE -ne 0 -or [IO.Path]::GetFullPath($gitRoot) -ne $resolvedRoot) {
        throw 'Packaging requires a Git repository at the project root.'
    }
    $outputDirs = @('build','release','artifacts','third_party','chat_bridge/.python','chat_bridge/.venv','chat_bridge/.venv-local','chat_bridge/build','chat_bridge/dist','private')
    foreach ($dir in $outputDirs) {
        & git -C $resolvedRoot check-ignore -q -- "$dir/.packaging-ignore-check"
        if ($LASTEXITCODE -ne 0) { throw "Output directory must be ignored before packaging: $dir" }
        $directoryPath = Join-Path $resolvedRoot $dir
        # Output trees must not redirect writes outside the checkout.
        while ($directoryPath -ne $resolvedRoot) {
            if ((Test-Path -LiteralPath $directoryPath) -and ((Get-Item -LiteralPath $directoryPath -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw "Packaging output path contains a link: $dir"
            }
            $directoryPath = Split-Path $directoryPath -Parent
        }
    }
    $paths = @(& git -C $resolvedRoot -c core.quotepath=false ls-files --cached --others --exclude-standard)
    if ($LASTEXITCODE -ne 0) { throw 'Unable to list Git-visible files.' }
    $snapshot = @{}
    # Explicit publication exception authorized by the owner on 2026-10-03.
    $publishedFile = Join-Path $resolvedRoot '.codex/published-private-files.txt'
    $published = @()
    if (Test-Path -LiteralPath $publishedFile) {
        $published = @(Get-Content -LiteralPath $publishedFile | Where-Object {
            $_ -match '^private/' -and $_ -notmatch '(^|/)\.\.(/|$)'
        })
    }
    foreach ($path in $paths | Sort-Object -Unique) {
        if ($path -match '^(build(?:-[^/]+)?|dist(?:-[^/]+)?|release|artifacts|third_party|private|Aion2Pipe-[^/]+-win64)/' -or
            $path -match '^chat_bridge/(\.python|\.venv(?:-local)?|build|dist)/' -or
            $path -match '(?i)(\.(exe|dll|sys|zip|7z|pdb|obj|ilk|pyc|pyo|bin|dmp|log|pcap|pcapng|a2session|a2cs|sqlite3?|db)|\.private\.local\.json|\.log\..+|\.db-(wal|shm))$' -or
            $path -match '(?i)(^|/)(__pycache__|\.pytest_cache|\.vs)/' -or
            ($path -match '(?i)(^|/)\.env(?:\.[^/]+)?$' -and $path -notmatch '(?i)(^|/)\.env\.example$')) {
            if ($path -notin $published) { throw "Generated or private file is visible to Git: $path" }
        }
        $fullPath = Join-Path $resolvedRoot $path
        $snapshot[$path] = if (Test-Path -LiteralPath $fullPath -PathType Leaf) {
            (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256).Hash
        } else { '<missing>' }
    }
    return ,$snapshot
}

function Assert-PackageGitSnapshot {
    param([Parameter(Mandatory = $true)][string]$Root, [Parameter(Mandatory = $true)][hashtable]$Before)
    $after = Get-PackageGitSnapshot -Root $Root
    $changed = @(@($Before.Keys) + @($after.Keys) | Sort-Object -Unique | Where-Object {
        -not $Before.ContainsKey($_) -or -not $after.ContainsKey($_) -or $Before[$_] -ne $after[$_]
    })
    if ($changed.Count) {
        throw ('Packaging changed Git-visible files; preserve and inspect these files: ' + ($changed -join ', '))
    }
}
