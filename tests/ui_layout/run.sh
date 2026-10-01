#!/usr/bin/env bash
set -euo pipefail
binary="$(realpath "$1")"
output="$(realpath -m "$2")"
mkdir -p "$output"
for width in 1280 1920; do
    for language in en zh-CN es ru it fr pl uk; do
        name="${language}-${width}"
        xvfb-run -a -s '-screen 0 1920x1080x24' "$binary" "$language" \
            'A player with a very long account display name' \
            "$output/$name.ppm" "$width" > "$output/$name.log" 2>&1
        printf 'PASS %s\n' "$name"
    done
    xvfb-run -a -s '-screen 0 1920x1080x24' "$binary" en guest \
        "$output/guest-$width.ppm" "$width" > "$output/guest-$width.log" 2>&1
    printf 'PASS guest-%s\n' "$width"
    xvfb-run -a -s '-screen 0 1920x1080x24' "$binary" fr Player \
        "$output/detail-$width.ppm" "$width" detail > "$output/detail-$width.log" 2>&1
    printf 'PASS detail-%s\n' "$width"
done
