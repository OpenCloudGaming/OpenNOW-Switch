#!/usr/bin/env bash
set -euo pipefail

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITA64="${DEVKITA64:-$DEVKITPRO/devkitA64}"
export PATH="/mingw64/bin:$DEVKITPRO/tools/bin:$DEVKITA64/bin:$DEVKITPRO/portlibs/switch/bin:$PATH"

cd "$(dirname "$0")/.."

cmake -B build/switch -G Ninja
cmake --build build/switch --target SwitchNOW.nro -j"$(nproc)"

printf '\nBuilt: %s\n' "$(pwd)/build/switch/SwitchNOW.nro"
