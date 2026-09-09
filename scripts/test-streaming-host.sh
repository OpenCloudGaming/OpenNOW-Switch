#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

cxx="${CXX:-g++}"
cc="${CC:-cc}"
cxxflags=(-std=c++20 -Wall -Wextra -Werror -Iapp/src -pthread)
cflags=(-std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Wno-sign-compare -Iextern/libpeer/src)
sections=(-ffunction-sections -fdata-sections -Wl,--gc-sections)

run_cpp() {
    local name="$1"
    shift
    "$cxx" "${cxxflags[@]}" "tests/${name}_test.cpp" "$@" -o "$out/$name"
    "$out/$name"
    printf 'PASS %s\n' "$name"
}

run_c() {
    local name="$1"
    shift
    "$cc" "${cflags[@]}" "tests/${name}_test.c" "$@" -o "$out/$name"
    "$out/$name"
    printf 'PASS %s\n' "$name"
}

for name in audio_latency_policy audio_rtp_utils controller_assignment_policy \
    controller_delivery_policy controller_layout decode_queue_policy \
    keyboard_input_policy keyboard_shortcut_controls network_loop_policy \
    network_quality_policy stream_end_policy stream_overlay_policy \
    stream_settings_policy software_yuv_upload touch_mapping video_frame_timing \
    video_quality_policy websocket_write_queue; do
    run_cpp "$name"
done

run_cpp network_monitor app/src/network_monitor.cpp app/src/network_utils.cpp
run_cpp diagnostic_writer app/src/webrtc/diagnostic_writer.cpp
run_cpp websocket_client -DFMT_HEADER_ONLY -isystem extern/ffmpeg/include \
    -isystem extern/borealis/library/include -isystem extern/borealis/library/lib/extern/fmt/include \
    app/src/WebSocketClient.cpp
run_cpp av_frame_queue -Itests/stream_stubs app/src/stream/ffmpeg/AVFrameHolder.cpp -lavcodec -lavutil
run_cpp audio_pipeline -Itests/stream_stubs -Iextern/libpeer/src app/src/stream/audio/AudioPipeline.cpp
run_cpp nvst_sdp app/src/webrtc/nvst_sdp.cpp
run_cpp cloud_session_protocol "${sections[@]}" app/src/gfn/cloud_session_protocol.cpp app/src/gfn/shared.cpp -ljansson
run_cpp stream_bitrate_pipeline "${sections[@]}" app/src/stream_settings.cpp app/src/localization.cpp \
    app/src/gfn/cloud_session_protocol.cpp app/src/gfn/shared.cpp app/src/webrtc/nvst_sdp.cpp -ljansson
run_c rtp_h264_assembly extern/libpeer/src/rtp.c
run_c rtp_reorder extern/libpeer/src/rtp.c
run_c rtcp_nack extern/libpeer/src/rtcp.c
run_c rtcp_receiver_report extern/libpeer/src/rtcp.c
run_c peer_rtcp_receive "${sections[@]}" -Wno-unused-but-set-variable \
    -Iextern/libpeer/third_party/mbedtls/include extern/libpeer/src/rtcp.c
run_c agent_socket_poll "${sections[@]}"
