$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
$installer = Join-Path $repo "tools\opennow-forwarder-installer"

Push-Location $installer
try {
    & pnpm install --frozen-lockfile
    if ($LASTEXITCODE -ne 0) {
        throw "pnpm install failed with exit code $LASTEXITCODE"
    }
    & pnpm run typecheck
    if ($LASTEXITCODE -ne 0) {
        throw "Forwarder installer typecheck failed with exit code $LASTEXITCODE"
    }
    & pnpm run nro
    if ($LASTEXITCODE -ne 0) {
        throw "Forwarder installer build failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}

Write-Host "Built: $installer\opennow-forwarder-installer.nro"
