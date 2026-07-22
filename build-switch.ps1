$ErrorActionPreference = "Stop"

$bashCandidates = @(
    "C:\msys64\usr\bin\bash.exe",
    "C:\devkitPro\msys2\usr\bin\bash.exe"
)
$bash = $bashCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $bash) {
    throw "MSYS2 bash was not found. Tried: $($bashCandidates -join ', ')"
}

$repo = $PSScriptRoot -replace "\\", "/"
if ($repo -match "^([A-Za-z]):/(.*)$") {
    $repo = "/$($matches[1].ToLower())/$($matches[2])"
}

& $bash -lc "cd '$repo' && bash scripts/build-switch-msys2.sh"
