param(
    [string]$Version = "0.0.6",
    [string]$BuildDirectory = "switch",
    [string]$Flavor = ""
)

$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
$nro = Join-Path $repo "build\$BuildDirectory\SwitchNOW.nro"
if (-not (Test-Path -LiteralPath $nro)) {
    throw "Missing build artifact: $nro. Run .\build-switch.ps1 first."
}

$dist = Join-Path $repo "dist"
$flavorSuffix = if ([string]::IsNullOrWhiteSpace($Flavor)) { "" } else { "-$Flavor" }
$packageName = "SwitchNOW-$Version$flavorSuffix"
$packageRoot = Join-Path $dist $packageName
$switchDir = Join-Path $packageRoot "switch\SwitchNOW"
$versionedNro = Join-Path $dist "$packageName.nro"
$zipPath = Join-Path $dist "$packageName.zip"

$repoResolved = (Resolve-Path -LiteralPath $repo).Path
if (Test-Path -LiteralPath $packageRoot) {
    $packageResolved = (Resolve-Path -LiteralPath $packageRoot).Path
    if (-not $packageResolved.StartsWith($repoResolved, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean package path outside repo: $packageResolved"
    }
    Remove-Item -LiteralPath $packageRoot -Recurse -Force
}

if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}

if (Test-Path -LiteralPath $versionedNro) {
    Remove-Item -LiteralPath $versionedNro -Force
}

New-Item -ItemType Directory -Force -Path $switchDir | Out-Null

Copy-Item -LiteralPath $nro -Destination (Join-Path $switchDir "SwitchNOW.nro") -Force
Copy-Item -LiteralPath $nro -Destination $versionedNro -Force
Copy-Item -LiteralPath (Join-Path $repo "resources\icon\icon.jpg") -Destination (Join-Path $switchDir "icon.jpg") -Force
Copy-Item -LiteralPath (Join-Path $repo "resources\icon\icon.png") -Destination (Join-Path $switchDir "icon.png") -Force
Copy-Item -LiteralPath (Join-Path $repo "README.md") -Destination (Join-Path $packageRoot "README.md") -Force

$manifest = @"
SwitchNOW $Version

Install:
1. Copy the switch folder from this package to the root of the SD card.
2. Launch Homebrew Menu with title override/application mode.
3. Start SwitchNOW from hbmenu.

Included:
- switch/SwitchNOW/SwitchNOW.nro
- switch/SwitchNOW/icon.jpg
- switch/SwitchNOW/icon.png
- README.md
"@

Set-Content -LiteralPath (Join-Path $packageRoot "INSTALL.txt") -Value $manifest -Encoding ASCII

Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($packageRoot, $zipPath)

Write-Host "Packaged: $zipPath"
Write-Host "Versioned NRO: $versionedNro"
