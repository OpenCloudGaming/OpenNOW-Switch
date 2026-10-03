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
"${CXX:-g++}" "${cxxflags[@]}" tests/webrtc_decode_output_accounting_test.cpp \
    app/src/webrtc/media.cpp app/src/webrtc/shared.cpp \
    app/src/stream/ffmpeg/FFmpegVideoDecoder.cpp app/src/stream/ffmpeg/AVFrameHolder.cpp \
    -Wl,--gc-sections,--wrap=avcodec_send_packet,--wrap=avcodec_receive_frame \
    -lavcodec -lavutil -ljansson -o "$out/test"
"$out/test"
