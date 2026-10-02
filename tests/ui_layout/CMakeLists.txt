cmake_minimum_required(VERSION 3.20)
project(opennow_ui_layout LANGUAGES C CXX)
option(OPENNOW_UI_SANITIZERS "Instrument the native harness and Borealis with ASan/UBSan" OFF)
if(OPENNOW_UI_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()
get_filename_component(ROOT "../.." ABSOLUTE)
set(PLATFORM_DESKTOP ON)
set(USE_SDL2 ON)
set(USE_SYSTEM_SDL2 ON)
set(USE_GL3 ON)
set(BRLS_RESOURCES_DIR "${ROOT}")
include("${ROOT}/extern/borealis/library/cmake/commonOption.cmake")
include("${ROOT}/extern/borealis/library/cmake/toolchain.cmake")
add_subdirectory("${ROOT}/extern/borealis/library" borealis)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JANSSON REQUIRED IMPORTED_TARGET jansson)
add_executable(ui_layout ui_layout.cpp ui_layout_stubs.cpp
    "${ROOT}/app/src/library_tab.cpp"
    "${ROOT}/app/src/library_row_view.cpp"
    "${ROOT}/app/src/catalog_tab.cpp"
    "${ROOT}/app/src/settings_tab.cpp"
    "${ROOT}/app/src/settings_tab_pages.cpp"
    "${ROOT}/app/src/settings_tab_actions.cpp"
    "${ROOT}/app/src/settings_tab_account.cpp"
    "${ROOT}/app/src/providers_tab.cpp"
    "${ROOT}/app/src/qr_login_dialog.cpp"
    "${ROOT}/app/src/qrcodegen.c"
    "${ROOT}/app/src/ui_action_guard.cpp"
    "${ROOT}/app/src/ui_helpers.cpp"
    "${ROOT}/app/src/queue_view.cpp"
    "${ROOT}/app/src/stream_overlay_view.cpp"
    "${ROOT}/app/src/ui_theme.cpp"
    "${ROOT}/app/src/stream_settings.cpp"
    "${ROOT}/app/src/stream_diagnostics.cpp"
    "${ROOT}/app/src/top_bar_frame.cpp"
    "${ROOT}/app/src/game_browser_header.cpp"
    "${ROOT}/app/src/game_card_view.cpp"
    "${ROOT}/app/src/game_detail_view.cpp"
    "${ROOT}/app/src/app_state.cpp"
    "${ROOT}/app/src/localization.cpp")
target_compile_features(ui_layout PRIVATE cxx_std_20)
target_include_directories(ui_layout PRIVATE "${ROOT}/app/src")
target_link_libraries(ui_layout PRIVATE borealis PkgConfig::JANSSON)
target_compile_definitions(ui_layout PRIVATE OPENNOW_UI_SOURCE_ROOT="${ROOT}")
target_link_options(ui_layout PRIVATE "-Wl,--wrap=_ZN4brls11Application6notifyERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE")
