#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

header_version="$(sed -n 's/^#define OPENNOW_APP_VERSION "\([^"]*\)"$/\1/p' app/src/app_version.hpp)"
cmake_version="$(sed -n 's/set(OPENNOW_VERSION "\([^"]*\)" CACHE.*/\1/p' CMakeLists.txt)"

if [[ -z "$header_version" || -z "$cmake_version" ]]; then
    printf 'Unable to read the application version from app_version.hpp and CMakeLists.txt.\n' >&2
    exit 1
fi

if [[ "$header_version" != "$cmake_version" ]]; then
    printf 'Version mismatch: app_version.hpp has %s, CMakeLists.txt has %s.\n' \
        "$header_version" "$cmake_version" >&2
    exit 1
fi

printf '%s\n' "$header_version"
