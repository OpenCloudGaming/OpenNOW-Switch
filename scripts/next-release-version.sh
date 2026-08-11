#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    printf 'Usage: %s <latest-release-tag>\n' "$0" >&2
    exit 2
fi

latest_tag="$1"
if [[ ! "$latest_tag" =~ ^v([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
    printf 'Latest release tag is not a semantic version: %s\n' "$latest_tag" >&2
    exit 1
fi

printf '%s.%s.%s\n' \
    "$((10#${BASH_REMATCH[1]}))" \
    "$((10#${BASH_REMATCH[2]}))" \
    "$((10#${BASH_REMATCH[3]} + 1))"
