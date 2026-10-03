#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
cxxflags=(-std=c++20 -Wall -Wextra -Werror -pthread -g -O1
    -fno-omit-frame-pointer -ffunction-sections -fdata-sections
    -Iapp/src -Itests/stream_stubs -Iextern/libpeer/src
    -Iextern/libpeer/third_party/mbedtls/include
    -Iextern/borealis/library/include/borealis/extern/nanovg)
if [[ -n "${OPENNOW_SANITIZERS:-}" ]]; then
    cxxflags+=("-fsanitize=$OPENNOW_SANITIZERS")
fi
"${CXX:-g++}" "${cxxflags[@]}" tests/audio_video_underrun_test.cpp \
    app/src/webrtc/media.cpp app/src/stream/audio/AudioPipeline.cpp \
    app/src/stream/ffmpeg/AVFrameHolder.cpp \
    -Wl,--gc-sections -lavcodec -lavutil -ljansson -o "$out/test"
"$out/test"
