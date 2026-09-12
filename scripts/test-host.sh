#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
bash scripts/test-streaming-host.sh

out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
cxx="${CXX:-g++}"
cxxflags=(-std=c++20 -Wall -Wextra -Werror -Iapp/src -pthread)
if [[ -n "${OPENNOW_SANITIZERS:-}" ]]; then
    cxxflags+=(-g -O1 -fno-omit-frame-pointer "-fsanitize=$OPENNOW_SANITIZERS")
fi

run_cpp() {
    local name="$1"
    shift
    "$cxx" "${cxxflags[@]}" "tests/${name}_test.cpp" "$@" -o "$out/$name"
    "$out/$name"
    printf 'PASS %s\n' "$name"
}

for name in app_launch_mode_policy atomic_file_replace auth_policy catalog_paging_policy cloud_launch_state cover_image_worker \
    community_proxy_policy device_identity_policy game_detail_policy \
    game_grid_navigation home_shortcut_policy library_sort membership_label \
    membership_tier_policy nro_shortcut_policy remote_candidate_policy \
    server_location_policy session_error_policy startup_callback_policy subscription_display \
    ui_refresh_policy ui_text_policy; do
    run_cpp "$name"
done

run_cpp ice_candidate_pair_policy -Iextern/libpeer/src
run_cpp localization app/src/localization.cpp
run_cpp nte_credentials app/src/nte_credentials.cpp
run_cpp play_history_policy app/src/play_history.cpp -ljansson
run_cpp stream_settings_persistence app/src/stream_settings.cpp app/src/localization.cpp -ljansson
run_cpp app_state_session_generation app/src/app_state.cpp
run_cpp auth_client_token_refresh -ffunction-sections -fdata-sections -Wl,--gc-sections \
    app/src/gfn/authentication.cpp app/src/gfn/shared.cpp -ljansson
run_cpp auth_saved_session_refresh -ffunction-sections -fdata-sections -Wl,--gc-sections \
    app/src/gfn/authentication.cpp app/src/gfn/persistence.cpp app/src/gfn/shared.cpp \
    -ljansson -lmbedcrypto
run_cpp catalog_response -ffunction-sections -fdata-sections -Wl,--gc-sections \
    app/src/gfn/shared.cpp -ljansson
run_cpp cover_image_cache -Itests/cover_cache_stubs app/src/cover_image_cache.cpp
run_cpp cached_image_admission -Itests/cover_cache_stubs app/src/cover_image_cache.cpp
run_cpp http_client_error app/src/http_client.cpp -lcurl
run_cpp http_transfer -lcurl
run_cpp http_tls_policy -ffunction-sections -fdata-sections -Wl,--gc-sections app/src/http_client.cpp -lcurl
run_cpp http_tls_policy -D__SWITCH__ -ffunction-sections -fdata-sections -Wl,--gc-sections app/src/http_client.cpp -lcurl
"$cxx" "${cxxflags[@]}" tests/http_client_transfer_integration_test.cpp app/src/http_client.cpp \
    -lcurl -o "$out/http_client_transfer_integration"
python3 tests/http_client_transfer_fixture.py "$out/http_client_transfer_integration"
printf 'PASS http_client_transfer_integration\n'
