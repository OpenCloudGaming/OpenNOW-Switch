#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
for size in 64x32 32x64; do
    ffmpeg -v error -f lavfi -i "color=c=gray:s=$size:r=60" -frames:v 20 \
        -c:v libx264 -threads 1 -preset ultrafast -tune zerolatency \
        -x264-params keyint=1:bframes=0 -f h264 "$out/$size.h264"
done
cxxflags=(-std=c++20 -Wall -Wextra -Werror -pthread -g -O1
    -fno-omit-frame-pointer -Iapp/src -Itests/stream_stubs)
if [[ -n "${OPENNOW_SANITIZERS:-}" ]]; then
    cxxflags+=("-fsanitize=$OPENNOW_SANITIZERS")
fi
"${CXX:-g++}" "${cxxflags[@]}" tests/ffmpeg_decode_integration_test.cpp \
    app/src/stream/ffmpeg/FFmpegVideoDecoder.cpp app/src/stream/ffmpeg/AVFrameHolder.cpp \
    -lavcodec -lavutil -o "$out/test"
"$out/test" "$out/64x32.h264" "$out/32x64.h264"
