cmake_minimum_required(VERSION 3.20)
project(opennow_ui_layout LANGUAGES C CXX)
get_filename_component(ROOT "../.." ABSOLUTE)
set(PLATFORM_DESKTOP ON)
set(USE_SDL2 ON)
set(USE_SYSTEM_SDL2 ON)
set(USE_GL3 ON)
set(BRLS_RESOURCES_DIR "${ROOT}")
include("${ROOT}/extern/borealis/library/cmake/commonOption.cmake")
include("${ROOT}/extern/borealis/library/cmake/toolchain.cmake")
add_subdirectory("${ROOT}/extern/borealis/library" borealis)
add_executable(ui_layout ui_layout.cpp ui_layout_stubs.cpp
    "${ROOT}/app/src/top_bar_frame.cpp"
    "${ROOT}/app/src/game_browser_header.cpp"
    "${ROOT}/app/src/game_card_view.cpp"
    "${ROOT}/app/src/game_detail_view.cpp"
    "${ROOT}/app/src/app_state.cpp"
    "${ROOT}/app/src/localization.cpp")
target_compile_features(ui_layout PRIVATE cxx_std_20)
target_include_directories(ui_layout PRIVATE "${ROOT}/app/src")
target_link_libraries(ui_layout PRIVATE borealis)
