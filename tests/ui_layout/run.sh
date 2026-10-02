#!/usr/bin/env bash
set -euo pipefail
binary="$(realpath "$1")"
output="$(realpath -m "$2")"
python3 "$(dirname "$(realpath "$0")")/font_chain_test.py"
mkdir -p "$output"
shift 2
include_extra=1
if [[ "${1:-}" == --screens-only ]]; then
    include_extra=0
    shift
    printf 'INFO Running named screens only; guest/empty cases run in the separate main matrix.\n'
fi
if (( $# )); then scenarios=("$@"); else scenarios=(library store settings detail queue overlay); fi
status=0
capture_case() {
    local language="$1" width="$2" account="$3" scenario="$4" name="$5"
    rm -f "$output/$name.ppm" "$output/$name.png" "$output/$name.ppm.queue.ppm" "$output/$name.ppm.queue.png" \
        "$output/$name.ppm.failure.ppm" "$output/$name.ppm.failure.png" "$output/$name.ppm.restored.ppm" "$output/$name.ppm.restored.png"
    rm -f "$output/$name.ppm".settings-*.ppm "$output/$name.ppm".settings-*.png
    if xvfb-run -a -s '-screen 0 1920x1080x24' "$binary" "$language" "$account" \
        "$output/$name.ppm" "$width" "$scenario" > "$output/$name.log" 2>&1; then
        printf 'PASS %s\n' "$name"
    else
        printf 'FAIL %s (see %s.log)\n' "$name" "$name"
        status=1
    fi
    if [[ -f "$output/$name.ppm" ]]; then convert "$output/$name.ppm" "$output/$name.png"; fi
    if [[ -f "$output/$name.ppm.queue.ppm" ]]; then convert "$output/$name.ppm.queue.ppm" "$output/$name.ppm.queue.png"; fi
    if [[ -f "$output/$name.ppm.failure.ppm" ]]; then convert "$output/$name.ppm.failure.ppm" "$output/$name.ppm.failure.png"; fi
    if [[ -f "$output/$name.ppm.restored.ppm" ]]; then convert "$output/$name.ppm.restored.ppm" "$output/$name.ppm.restored.png"; fi
    find "$output" -maxdepth 1 -name "$name.ppm.settings-*.ppm" -print0 |
        while IFS= read -r -d '' path; do convert "$path" "${path%.ppm}.png"; done
}
for width in 1280 1920; do
    for language in en zh-CN es ru it fr pl uk; do
        for scenario in "${scenarios[@]}"; do
            capture_case "$language" "$width" 'A player with a very long account display name' "$scenario" "$language-$width-$scenario"
        done
    done
    if (( include_extra )); then
        capture_case en "$width" guest library "guest-$width"
        capture_case en "$width" Player empty "empty-$width"
        capture_case en "$width" guest store "guest-store-$width"
        capture_case en "$width" Player empty-store "empty-store-$width"
    fi
done
exit "$status"
