param(
    [string]$Version = "1.0.0"
)

$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
$workspace = Split-Path -Parent $repo
$sourceOut = Join-Path $workspace "SwitchNOW-main"
$releaseOut = Join-Path $workspace "SwitchNOW-release"
$nro = Join-Path $repo "build\switch\SwitchNOW.nro"

if (-not (Test-Path -LiteralPath $nro)) {
    throw "Missing $nro. Run .\build-switch.ps1 first."
}

function Reset-OutputDirectory([string]$Path) {
    $workspaceResolved = (Resolve-Path -LiteralPath $workspace).Path
    if (Test-Path -LiteralPath $Path) {
        $resolved = (Resolve-Path -LiteralPath $Path).Path
        if (-not $resolved.StartsWith($workspaceResolved, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to clean output outside workspace: $resolved"
        }
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
}

function Copy-CleanTree([string]$Source, [string]$Destination) {
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    & robocopy $Source $Destination /E /NFL /NDL /NJH /NJS /NP `
        /XD .git .github build dist .cache __pycache__ node_modules pokemon xml `
        /XF *.log *.tmp *.bak *.user *.suo *.pdb *.ilk *.exe *.o *.obj `
            auth_accounts.json auth_session.json native_credentials.json `
            stream_settings.json nte.txt first_frame.ppm demo_icon.jpg `
            opennow_logo.png opennow_switch_icon.jpg tiles.png | Out-Null
    if ($LASTEXITCODE -gt 7) {
        throw "robocopy failed for $Source with exit code $LASTEXITCODE"
    }
}

Reset-OutputDirectory $sourceOut
Reset-OutputDirectory $releaseOut

foreach ($directory in @("app", "extern", "resources", "scripts", "tests")) {
    Copy-CleanTree (Join-Path $repo $directory) (Join-Path $sourceOut $directory)
}

foreach ($file in @("CMakeLists.txt", "build-switch.ps1", "README.md")) {
    Copy-Item -LiteralPath (Join-Path $repo $file) -Destination (Join-Path $sourceOut $file) -Force
}

Copy-Item -LiteralPath $nro -Destination (Join-Path $releaseOut "SwitchNOW.nro") -Force
Copy-Item -LiteralPath (Join-Path $repo "resources\icon\icon.jpg") -Destination $releaseOut -Force
Copy-Item -LiteralPath (Join-Path $repo "resources\icon\icon.png") -Destination $releaseOut -Force
Copy-Item -LiteralPath (Join-Path $repo "README.md") -Destination $releaseOut -Force

$hash = Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $releaseOut "SwitchNOW.nro")
@(
    "SwitchNOW $Version",
    "SwitchNOW.nro SHA256: $($hash.Hash.ToLowerInvariant())"
) | Set-Content -LiteralPath (Join-Path $releaseOut "SHA256.txt") -Encoding ASCII

Write-Host "Prepared source:  $sourceOut"
Write-Host "Prepared release: $releaseOut"
