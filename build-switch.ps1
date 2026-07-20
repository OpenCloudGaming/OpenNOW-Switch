$ErrorActionPreference = "Stop"

$bash = "C:\msys64\usr\bin\bash.exe"
if (-not (Test-Path $bash)) {
    throw "MSYS2 bash was not found at $bash"
}

$repo = $PSScriptRoot -replace "\\", "/"
if ($repo -match "^([A-Za-z]):/(.*)$") {
    $repo = "/$($matches[1].ToLower())/$($matches[2])"
}

& $bash -lc "cd '$repo' && bash scripts/build-switch-msys2.sh"
