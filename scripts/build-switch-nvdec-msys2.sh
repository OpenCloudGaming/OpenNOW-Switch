#!/usr/bin/env bash
set -euo pipefail

printf 'NVDEC and software fallback are combined in the unified build.\n'
exec bash "$(dirname "$0")/build-switch-msys2.sh"
