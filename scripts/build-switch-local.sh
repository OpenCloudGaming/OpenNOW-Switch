#!/usr/bin/env bash
# Local Switch (.nro) build for macOS + devkitPro.
# Usage: scripts/build-switch-local.sh [SRC_TREE] [BUILD_DIR]
# Env:   DEVKITPRO, CMAKE, MBED_PY (python with jinja2+jsonschema for mbedTLS gen).
# Builds extern deps first (populates dist/lib), then SwitchNOW.nro. Additive:
# touches no tracked source; the mbedTLS pre-gen guards older GEN_FILES=OFF trees.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${1:-$(cd "$SCRIPT_DIR/.." && pwd)}"
BUILD="${2:-$SRC/build/switch}"

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITA64="${DEVKITA64:-$DEVKITPRO/devkitA64}"
export PATH="$DEVKITPRO/tools/bin:$DEVKITA64/bin:$DEVKITPRO/portlibs/switch/bin:$PATH"

TOOLS_VENV="$HOME/.opennow-switch-tools/cmake3-venv"
CMAKE="${CMAKE:-$( [ -x "$TOOLS_VENV/bin/cmake" ] && echo "$TOOLS_VENV/bin/cmake" || echo cmake )}"
MBED_PY="${MBED_PY:-$( [ -x "$TOOLS_VENV/bin/python" ] && echo "$TOOLS_VENV/bin/python" || echo python3 )}"

# Activate the tools venv so CMake's FindPython3 selects a python that carries
# the mbedTLS generator deps (jsonschema, jinja2). As of upstream #16 the mbedTLS
# ExternalProject builds with -DGEN_FILES=ON, i.e. it REGENERATES its sources
# in-build with whatever python CMake finds; the host/system python often lacks
# those modules. Exporting VIRTUAL_ENV makes FindPython3 prefer this venv.
if [ -d "$TOOLS_VENV" ]; then
  export VIRTUAL_ENV="$TOOLS_VENV"
  export PATH="$TOOLS_VENV/bin:$PATH"
fi

MB="$SRC/extern/libpeer/third_party/mbedtls"
JOBS="$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu || echo 4 )"

echo "== build-switch-local =="
echo "  SRC     : $SRC"
echo "  BUILD   : $BUILD"
echo "  CMAKE   : $CMAKE ($("$CMAKE" --version | head -1))"
echo "  MBED_PY : $MBED_PY"

# --- Step 0: sanity ---------------------------------------------------------
[ -f "$SRC/CMakeLists.txt" ] || { echo "ERROR: $SRC is not the repo root"; exit 1; }
"$MBED_PY" -c 'import jinja2, jsonschema' 2>/dev/null || {
  echo "ERROR: $MBED_PY lacks jinja2/jsonschema (needed by mbedTLS driver-wrapper generator)."
  echo "       Install them into an isolated env, e.g.:"
  echo "         python3 -m venv $TOOLS_VENV && $TOOLS_VENV/bin/pip install jinja2 jsonschema"
  echo "       or set MBED_PY to a python that has them."
  exit 1
}

# --- Step 1: pre-generate mbedTLS sources (GEN_FILES=OFF expects them) -------
gen() { # <output-basename> <command...>
  local out="$MB/library/$1"; shift
  if [ -f "$out" ]; then echo "  mbedtls: $1 already present, skipping"; return; fi
  echo "  mbedtls: generating $1"
  ( cd "$MB" && "$@" )
}
gen error.c                        perl scripts/generate_errors.pl include/mbedtls scripts/data_files "$MB/library/error.c"
gen version_features.c             perl scripts/generate_features.pl include/mbedtls scripts/data_files "$MB/library/version_features.c"
gen ssl_debug_helpers_generated.c  "$MBED_PY" scripts/generate_ssl_debug_helpers.py --mbedtls-root . "$MB/library"
gen psa_crypto_driver_wrappers.c   "$MBED_PY" scripts/generate_driver_wrappers.py "$MB/library"

# --- Step 2: configure ------------------------------------------------------
echo "== configure =="
"$CMAKE" -S "$SRC" -B "$BUILD" -G Ninja

# --- Step 3: build extern deps FIRST (populate dist/lib), then the NRO -------
echo "== build extern deps (cjson mbedtls srtp2 usrsctp) =="
"$CMAKE" --build "$BUILD" --target cjson mbedtls srtp2 usrsctp -j"$JOBS"

echo "== build SwitchNOW.nro =="
"$CMAKE" --build "$BUILD" --target SwitchNOW.nro -j"$JOBS"

NRO="$(find "$BUILD" -name 'SwitchNOW.nro' | head -1)"
[ -n "$NRO" ] || NRO="$(find "$BUILD" -maxdepth 1 -name '*.nro' | head -1)"
echo
if [ -n "$NRO" ]; then echo "Built: $NRO ($(du -h "$NRO" | cut -f1))"; else echo "ERROR: no .nro produced"; exit 1; fi
