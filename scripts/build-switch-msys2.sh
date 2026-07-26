#!/usr/bin/env bash
set -euo pipefail

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITA64="${DEVKITA64:-$DEVKITPRO/devkitA64}"
export PATH="/mingw64/bin:$DEVKITPRO/tools/bin:$DEVKITA64/bin:$DEVKITPRO/portlibs/switch/bin:$PATH"

cd "$(dirname "$0")/.."

detect_build_jobs() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    elif command -v sysctl >/dev/null 2>&1; then
        sysctl -n hw.ncpu
    else
        printf '4\n'
    fi
}

opennow_build_jobs="${OPENNOW_BUILD_JOBS:-$(detect_build_jobs)}"

cmake -B build/switch -G Ninja
cmake --build build/switch --target SwitchNOW.nro -j"$opennow_build_jobs"

printf '\nBuilt: %s\n' "$(pwd)/build/switch/SwitchNOW.nro"
