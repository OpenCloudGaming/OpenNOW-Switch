cmake_minimum_required(VERSION 3.20)

if(PLATFORM_SWITCH AND CMAKE_GENERATOR MATCHES "Unix Makefiles")
    set(CMAKE_DEPENDS_USE_COMPILER FALSE CACHE BOOL "Avoid Windows drive-letter depfile paths in devkitPro/MSYS make builds." FORCE)
endif()

project(OpenNOWSwitch LANGUAGES C CXX)

option(PLATFORM_SWITCH "Build for Nintendo Switch with devkitPro/libnx" OFF)
option(OPENNOW_BUILD_TESTS "Build host-side unit tests" ON)
option(OPENNOW_ENABLE_CURL "Build libcurl-backed HTTP client when CURL is available" ON)
option(OPENNOW_REQUIRE_SWITCH_TOOLS "Fail Switch configure when NRO packaging tools are missing" ON)
option(OPENNOW_VALIDATE_NRO "Validate generated OpenNOW.nro and OpenNOW.nacp after packaging" ON)
option(OPENNOW_USE_CUSTOM_UI "Use the custom full-screen OpenNOW Switch UI" ON)
option(OPENNOW_USE_BOREALIS "Use the Borealis native Switch UI when available" ON)
set(OPENNOW_LIBPEER_ROOT "" CACHE PATH "Path to a Switch-built sepfy/libpeer checkout for native WebRTC.")
set(OPENNOW_LIBDATACHANNEL_ROOT "" CACHE PATH "Path to a Switch-built libdatachannel checkout for native WebRTC.")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(OPENNOW_VERSION "0.1.0")
set(OPENNOW_AUTHOR "OpenCloudGaming")
set(OPENNOW_TITLE_ID "054F504E4E4F5700")
set(OPENNOW_DISPLAY_NAME "OpenNOW Switch")
set(OPENNOW_CA_BUNDLE_SOURCE "")
foreach(OPENNOW_CA_BUNDLE_CANDIDATE
    "$ENV{DEVKITPRO}/msys2/usr/ssl/certs/ca-bundle.crt"
    "/usr/ssl/certs/ca-bundle.crt"
)
    if(NOT OPENNOW_CA_BUNDLE_SOURCE AND EXISTS "${OPENNOW_CA_BUNDLE_CANDIDATE}")
        set(OPENNOW_CA_BUNDLE_SOURCE "${OPENNOW_CA_BUNDLE_CANDIDATE}")
    endif()
endforeach()

add_custom_target(validate-switch-env
    COMMAND ${CMAKE_COMMAND}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ValidateSwitchEnvironment.cmake"
    COMMENT "Validating devkitPro Switch build environment"
)

add_library(opennow_switch_core
    src/app/OpenNowSwitchApp.cpp
    src/core/Log.cpp
    src/core/Settings.cpp
    src/gfn/Auth.cpp
    src/gfn/AuthService.cpp
    src/gfn/AuthSessionStore.cpp
    src/gfn/Catalog.cpp
    src/gfn/CloudMatch.cpp
    src/gfn/GfnService.cpp
    src/gfn/InputEncoder.cpp
    src/gfn/OAuthCallbackServer.cpp
    src/gfn/Sdp.cpp
    src/input/ControllerMapper.cpp
    src/gfn/SignalingClient.cpp
    src/media/GfnWebRtcClient.cpp
    src/media/NativeMediaPipeline.cpp
    src/media/VideoFrameStore.cpp
    src/net/CurlHttpClient.cpp
    src/net/MockHttpClient.cpp
    src/net/WssClient.cpp
    src/ui/Dashboard.cpp
    src/util/Json.cpp
)

target_include_directories(opennow_switch_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(opennow_switch_core
    PUBLIC
        OPENNOW_SWITCH_VERSION="${OPENNOW_VERSION}"
)

if(MSVC)
    target_compile_options(opennow_switch_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(opennow_switch_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_link_libraries(opennow_switch_core PUBLIC ws2_32)
endif()

if(OPENNOW_ENABLE_CURL)
    find_package(CURL QUIET)
    if(CURL_FOUND)
        target_link_libraries(opennow_switch_core PUBLIC CURL::libcurl)
        target_compile_definitions(opennow_switch_core PUBLIC OPENNOW_HAS_CURL=1)
    else()
        message(STATUS "libcurl not found; live HTTPS client disabled for this build")
    endif()
endif()

add_executable(opennow-switch
    src/main.cpp
    src/platform/MockPlatform.cpp
)

target_link_libraries(opennow-switch PRIVATE opennow_switch_core)

if(PLATFORM_SWITCH)
    if(OPENNOW_REQUIRE_SWITCH_TOOLS)
        include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ValidateSwitchEnvironment.cmake")
    endif()

    target_sources(opennow-switch PRIVATE
        src/platform/SwitchPlatform.cpp
        src/net/MbedtlsTimingShim.cpp
    )
    target_compile_definitions(opennow_switch_core PUBLIC OPENNOW_PLATFORM_SWITCH=1)
    if(OPENNOW_CA_BUNDLE_SOURCE)
        target_compile_definitions(opennow_switch_core PUBLIC OPENNOW_SWITCH_CA_BUNDLE="romfs:/certs/ca-bundle.crt")
    endif()
    target_compile_definitions(opennow-switch PRIVATE OPENNOW_PLATFORM_SWITCH=1)
    target_link_libraries(opennow-switch PRIVATE nx m)
    if(EXISTS "$ENV{DEVKITPRO}/portlibs/switch/include/libavcodec/avcodec.h"
       AND EXISTS "$ENV{DEVKITPRO}/portlibs/switch/lib/libavcodec.a"
       AND EXISTS "$ENV{DEVKITPRO}/portlibs/switch/lib/libswscale.a"
       AND EXISTS "$ENV{DEVKITPRO}/portlibs/switch/lib/libswresample.a"
       AND EXISTS "$ENV{DEVKITPRO}/portlibs/switch/lib/libavutil.a")
        target_compile_definitions(opennow_switch_core PUBLIC
            OPENNOW_HAS_FFMPEG_DECODER=1
            OPENNOW_HAS_FFMPEG_NVTEGRA=1
            OPENNOW_HAS_NATIVE_VIDEO_RENDERER=1
            OPENNOW_HAS_NATIVE_AUDIO=1
        )
        target_include_directories(opennow_switch_core PUBLIC "$ENV{DEVKITPRO}/portlibs/switch/include")
        target_link_directories(opennow-switch PRIVATE "$ENV{DEVKITPRO}/portlibs/switch/lib")
        target_link_libraries(opennow-switch PRIVATE avcodec swscale swresample avutil dav1d bz2 z)
        message(STATUS "Switch FFmpeg decode/audio sinks enabled.")
    else()
        message(WARNING "Switch FFmpeg libraries were not found; native video/audio decode will not be compiled.")
    endif()

    if(NOT OPENNOW_LIBDATACHANNEL_ROOT AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../_research/libdatachannel/build-switch-vendored-mbedtls/libdatachannel.a")
        get_filename_component(OPENNOW_LIBDATACHANNEL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../_research/libdatachannel" ABSOLUTE)
    endif()
    if(OPENNOW_LIBDATACHANNEL_ROOT)
        set(OPENNOW_LIBDATACHANNEL_BUILD "${OPENNOW_LIBDATACHANNEL_ROOT}/build-switch-vendored-mbedtls")
        set(OPENNOW_LIBDATACHANNEL_LIB "${OPENNOW_LIBDATACHANNEL_BUILD}/libdatachannel.a")
        set(OPENNOW_LIBDATACHANNEL_JUICE "${OPENNOW_LIBDATACHANNEL_BUILD}/deps/libjuice/libjuice.a")
        set(OPENNOW_LIBDATACHANNEL_SRTP "${OPENNOW_LIBDATACHANNEL_BUILD}/deps/libsrtp/libsrtp2.a")
        set(OPENNOW_LIBDATACHANNEL_USRSCTP "${OPENNOW_LIBDATACHANNEL_BUILD}/deps/usrsctp/usrsctplib/libusrsctp.a")
    endif()

    if(NOT OPENNOW_LIBPEER_ROOT AND NOT OPENNOW_LIBDATACHANNEL_ROOT AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../_research/libpeer/build-switch/src/libpeer.a")
        get_filename_component(OPENNOW_LIBPEER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../_research/libpeer" ABSOLUTE)
    endif()
    if(OPENNOW_LIBPEER_ROOT)
        set(OPENNOW_LIBPEER_BUILD "${OPENNOW_LIBPEER_ROOT}/build-switch")
        set(OPENNOW_LIBPEER_DIST "${OPENNOW_LIBPEER_BUILD}/dist")
        set(OPENNOW_LIBPEER_LIB "${OPENNOW_LIBPEER_BUILD}/src/libpeer.a")
    elseif(OPENNOW_LIBDATACHANNEL_ROOT AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../_research/libpeer/build-switch/dist/lib/libmbedtls.a")
        get_filename_component(OPENNOW_LIBPEER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../_research/libpeer" ABSOLUTE)
        set(OPENNOW_LIBPEER_BUILD "${OPENNOW_LIBPEER_ROOT}/build-switch")
        set(OPENNOW_LIBPEER_DIST "${OPENNOW_LIBPEER_BUILD}/dist")
    endif()

    if(OPENNOW_LIBPEER_ROOT AND EXISTS "${OPENNOW_LIBPEER_DIST}/include/mbedtls/ssl.h")
        target_compile_definitions(opennow_switch_core PUBLIC OPENNOW_HAS_MBEDTLS_TLS=1)
        target_include_directories(opennow_switch_core PUBLIC "${OPENNOW_LIBPEER_DIST}/include")
        target_link_directories(opennow-switch PRIVATE "$ENV{DEVKITPRO}/portlibs/switch/lib")
    elseif(EXISTS "$ENV{DEVKITPRO}/portlibs/switch/include/mbedtls/ssl.h")
        target_compile_definitions(opennow_switch_core PUBLIC OPENNOW_HAS_MBEDTLS_TLS=1)
        target_include_directories(opennow_switch_core PUBLIC "$ENV{DEVKITPRO}/portlibs/switch/include")
        target_link_directories(opennow-switch PRIVATE "$ENV{DEVKITPRO}/portlibs/switch/lib")
        target_link_libraries(opennow-switch PRIVATE mbedtls mbedx509 mbedcrypto)
    else()
        message(WARNING "switch-mbedtls is not installed; native WSS signaling will not be compiled.")
    endif()

    if(OPENNOW_LIBDATACHANNEL_ROOT
       AND EXISTS "${OPENNOW_LIBDATACHANNEL_LIB}"
       AND EXISTS "${OPENNOW_LIBDATACHANNEL_JUICE}"
       AND EXISTS "${OPENNOW_LIBDATACHANNEL_SRTP}"
       AND EXISTS "${OPENNOW_LIBDATACHANNEL_USRSCTP}"
       AND EXISTS "${OPENNOW_LIBPEER_DIST}/lib/libmbedtls.a")
        target_compile_definitions(opennow_switch_core PUBLIC
            OPENNOW_HAS_LIBDATACHANNEL=1
            OPENNOW_HAS_NATIVE_ICE=1
            OPENNOW_HAS_NATIVE_DTLS_SRTP=1
            OPENNOW_HAS_NATIVE_RTP=1
            RTC_STATIC
            RTC_ENABLE_WEBSOCKET=0
            RTC_ENABLE_MEDIA=1
            RTC_SYSTEM_JUICE=0
            RTC_SYSTEM_SRTP=0
            USE_MBEDTLS=1
            USE_NICE=0
            JUICE_STATIC
        )
        target_include_directories(opennow_switch_core PUBLIC
            "${OPENNOW_LIBDATACHANNEL_ROOT}/include"
            "${OPENNOW_LIBPEER_ROOT}/third_party/mbedtls/include"
            "${OPENNOW_LIBPEER_DIST}/include"
        )
        target_link_libraries(opennow-switch PRIVATE
            "${OPENNOW_LIBDATACHANNEL_LIB}"
            "${OPENNOW_LIBDATACHANNEL_JUICE}"
            "${OPENNOW_LIBDATACHANNEL_SRTP}"
            "${OPENNOW_LIBDATACHANNEL_USRSCTP}"
            "${OPENNOW_LIBPEER_DIST}/lib/libmbedtls.a"
            "${OPENNOW_LIBPEER_DIST}/lib/libmbedx509.a"
            "${OPENNOW_LIBPEER_DIST}/lib/libmbedcrypto.a"
        )
        message(STATUS "Native WebRTC enabled with libdatachannel: ${OPENNOW_LIBDATACHANNEL_ROOT}")
    elseif(OPENNOW_LIBDATACHANNEL_ROOT)
        message(WARNING "OPENNOW_LIBDATACHANNEL_ROOT is set but required Switch static libraries were not found; native WebRTC remains disabled.")
    elseif(OPENNOW_LIBPEER_ROOT)
        if(EXISTS "${OPENNOW_LIBPEER_LIB}"
           AND EXISTS "${OPENNOW_LIBPEER_DIST}/lib/libusrsctp.a"
           AND EXISTS "${OPENNOW_LIBPEER_DIST}/lib/libsrtp2.a"
           AND EXISTS "${OPENNOW_LIBPEER_DIST}/lib/libmbedtls.a")
            target_compile_definitions(opennow_switch_core PUBLIC
                OPENNOW_HAS_LIBPEER=1
                OPENNOW_HAS_NATIVE_ICE=1
                OPENNOW_HAS_NATIVE_DTLS_SRTP=1
                OPENNOW_HAS_NATIVE_RTP=1
            )
            target_include_directories(opennow_switch_core PUBLIC
                "${OPENNOW_LIBPEER_ROOT}/src"
                "${OPENNOW_LIBPEER_ROOT}/include"
                "${OPENNOW_LIBPEER_DIST}/include"
                "${OPENNOW_LIBPEER_DIST}/include/cjson"
            )
            target_link_libraries(opennow-switch PRIVATE
                "${OPENNOW_LIBPEER_LIB}"
                "${OPENNOW_LIBPEER_DIST}/lib/libusrsctp.a"
                "${OPENNOW_LIBPEER_DIST}/lib/libsrtp2.a"
                "${OPENNOW_LIBPEER_DIST}/lib/libmbedtls.a"
                "${OPENNOW_LIBPEER_DIST}/lib/libmbedx509.a"
                "${OPENNOW_LIBPEER_DIST}/lib/libmbedcrypto.a"
                "${OPENNOW_LIBPEER_DIST}/lib/libcjson.a"
            )
            message(STATUS "Native WebRTC enabled with libpeer: ${OPENNOW_LIBPEER_ROOT}")
        else()
            message(WARNING "OPENNOW_LIBPEER_ROOT is set but required Switch static libraries were not found; native WebRTC remains disabled.")
        endif()
    endif()

    set(OPENNOW_ROMFS_PATH "")
    set(OPENNOW_BOREALIS_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/extern/borealis/library")
    if(OPENNOW_USE_CUSTOM_UI AND EXISTS "${OPENNOW_BOREALIS_LIBRARY}/include/borealis/extern/nanovg/nanovg.h")
        target_sources(opennow-switch PRIVATE
            src/ui/CustomUiApp.cpp
            "${OPENNOW_BOREALIS_LIBRARY}/lib/extern/nanovg/nanovg.c"
        )
        target_compile_definitions(opennow-switch PRIVATE OPENNOW_USE_CUSTOM_UI=1 USE_GLES2=1)
        target_include_directories(opennow-switch PRIVATE
            "${OPENNOW_BOREALIS_LIBRARY}/include/borealis/extern/nanovg"
            "$ENV{DEVKITPRO}/portlibs/switch/include"
        )
        target_link_directories(opennow-switch PRIVATE "$ENV{DEVKITPRO}/portlibs/switch/lib")
        target_link_libraries(opennow-switch PRIVATE glfw3 EGL GLESv2 glapi drm_nouveau z)
        set(OPENNOW_ROMFS_PATH "${CMAKE_CURRENT_BINARY_DIR}/resources")
    elseif(OPENNOW_USE_BOREALIS AND EXISTS "${OPENNOW_BOREALIS_LIBRARY}/CMakeLists.txt")
        set(PLATFORM_SWITCH ON CACHE BOOL "" FORCE)
        set(USE_SYSTEM_FMT OFF CACHE BOOL "" FORCE)
        set(FMT_OS OFF CACHE BOOL "" FORCE)
        set(USE_SYSTEM_TINYXML2 OFF CACHE BOOL "" FORCE)
        set(USE_SYSTEM_TWEENY OFF CACHE BOOL "" FORCE)
        set(USE_SDL2 OFF CACHE BOOL "" FORCE)
        set(USE_GLFW OFF CACHE BOOL "" FORCE)
        set(USE_DEKO3D ON CACHE BOOL "" FORCE)
        set(USE_SYSTEM_GLFW OFF CACHE BOOL "" FORCE)
        set(BOREALIS_USE_DEKO3D ON CACHE BOOL "" FORCE)
        set(BRLS_UNITY_BUILD OFF CACHE BOOL "" FORCE)
        set(BRLS_RESOURCES_DIR "${CMAKE_CURRENT_BINARY_DIR}/resources" CACHE PATH "" FORCE)
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
        add_subdirectory("${OPENNOW_BOREALIS_LIBRARY}" "${CMAKE_CURRENT_BINARY_DIR}/borealis" EXCLUDE_FROM_ALL)
        target_include_directories(borealis PUBLIC "$ENV{DEVKITPRO}/portlibs/switch/include")
        target_link_directories(borealis PUBLIC "$ENV{DEVKITPRO}/portlibs/switch/lib")

        target_sources(opennow-switch PRIVATE
            src/ui/BorealisApp.cpp
            "${OPENNOW_BOREALIS_LIBRARY}/lib/platforms/switch/switch_wrapper.c"
        )
        target_compile_definitions(opennow-switch PRIVATE OPENNOW_USE_BOREALIS=1)
        target_link_libraries(opennow-switch PRIVATE borealis)
        set(OPENNOW_ROMFS_PATH "${CMAKE_CURRENT_BINARY_DIR}/resources")
    elseif(OPENNOW_USE_BOREALIS)
        message(WARNING "Borealis was requested but extern/borealis/library was not found; falling back to console UI.")
        target_sources(opennow-switch PRIVATE src/ui/SwitchSafeApp.cpp)
    else()
        target_sources(opennow-switch PRIVATE src/ui/SwitchSafeApp.cpp)
    endif()

    if(OPENNOW_CA_BUNDLE_SOURCE AND NOT OPENNOW_ROMFS_PATH)
        set(OPENNOW_ROMFS_PATH "${CMAKE_CURRENT_BINARY_DIR}/resources")
    endif()

    find_program(NX_NACPTOOL_EXE nacptool HINTS "$ENV{DEVKITPRO}/tools/bin")
    find_program(NX_ELF2NRO_EXE elf2nro HINTS "$ENV{DEVKITPRO}/tools/bin")
    find_program(NX_UAM_EXE uam HINTS "$ENV{DEVKITPRO}/tools/bin")

    if(NX_NACPTOOL_EXE AND NX_ELF2NRO_EXE)
        set(OPENNOW_NACP_PATH "${CMAKE_CURRENT_BINARY_DIR}/OpenNOW.nacp")
        set(OPENNOW_NRO_PATH "${CMAKE_CURRENT_BINARY_DIR}/OpenNOW.nro")
        set(OPENNOW_BOREALIS_SHADER_COMMANDS "")
        if(OPENNOW_ROMFS_PATH AND OPENNOW_USE_BOREALIS AND NOT OPENNOW_USE_CUSTOM_UI)
            if(NX_UAM_EXE)
                set(OPENNOW_BOREALIS_SHADER_PATH "${OPENNOW_BOREALIS_LIBRARY}/lib/extern/nanovg/deko3d/shaders")
                set(OPENNOW_BOREALIS_SHADER_COMMANDS
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${OPENNOW_ROMFS_PATH}/shaders"
                    COMMAND ${NX_UAM_EXE} -s vert
                        -o "${OPENNOW_ROMFS_PATH}/shaders/fill_vsh.dksh"
                        "${OPENNOW_BOREALIS_SHADER_PATH}/fill_vsh.glsl"
                    COMMAND ${NX_UAM_EXE} -s frag
                        -o "${OPENNOW_ROMFS_PATH}/shaders/fill_aa_fsh.dksh"
                        "${OPENNOW_BOREALIS_SHADER_PATH}/fill_aa_fsh.glsl"
                    COMMAND ${NX_UAM_EXE} -s frag
                        -o "${OPENNOW_ROMFS_PATH}/shaders/fill_fsh.dksh"
                        "${OPENNOW_BOREALIS_SHADER_PATH}/fill_fsh.glsl"
                )
            elseif(OPENNOW_REQUIRE_SWITCH_TOOLS)
                message(FATAL_ERROR "uam not found. Install devkitPro tools so Borealis Deko3D shaders can be packaged.")
            else()
                message(WARNING "uam not found. Borealis Deko3D shader files will not be packaged.")
            endif()
        endif()
        set(OPENNOW_VALIDATE_NRO_COMMAND "")
        if(OPENNOW_VALIDATE_NRO)
            set(OPENNOW_VALIDATE_NRO_COMMAND
                COMMAND ${CMAKE_COMMAND}
                    -DNRO_PATH:STRING="${OPENNOW_NRO_PATH}"
                    -DNACP_PATH:STRING="${OPENNOW_NACP_PATH}"
                    -DEXPECTED_TITLE:STRING="${OPENNOW_DISPLAY_NAME}"
                    -DEXPECTED_AUTHOR:STRING="${OPENNOW_AUTHOR}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ValidateNro.cmake"
            )
        endif()
        if(OPENNOW_ROMFS_PATH)
            set(OPENNOW_CA_BUNDLE_COMMANDS "")
            if(OPENNOW_CA_BUNDLE_SOURCE)
                set(OPENNOW_CA_BUNDLE_COMMANDS
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${OPENNOW_ROMFS_PATH}/certs"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${OPENNOW_CA_BUNDLE_SOURCE}"
                        "${OPENNOW_ROMFS_PATH}/certs/ca-bundle.crt"
                )
            else()
                message(WARNING "CA bundle not found; Switch HTTPS certificate verification may fail until certs are packaged.")
            endif()
            add_custom_target(OpenNOW.nro ALL
                DEPENDS opennow-switch
                COMMAND ${CMAKE_COMMAND} -E make_directory "${OPENNOW_ROMFS_PATH}"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_CURRENT_SOURCE_DIR}/extern/borealis/resources"
                    "${OPENNOW_ROMFS_PATH}"
                ${OPENNOW_CA_BUNDLE_COMMANDS}
                ${OPENNOW_BOREALIS_SHADER_COMMANDS}
                COMMAND ${NX_NACPTOOL_EXE}
                    --create "${OPENNOW_DISPLAY_NAME}" "${OPENNOW_AUTHOR}" "${OPENNOW_VERSION}"
                    "${OPENNOW_NACP_PATH}" --titleid=${OPENNOW_TITLE_ID}
                COMMAND ${NX_ELF2NRO_EXE}
                    $<TARGET_FILE:opennow-switch> "${OPENNOW_NRO_PATH}"
                    --nacp="${OPENNOW_NACP_PATH}"
                    --romfsdir="${OPENNOW_ROMFS_PATH}"
                ${OPENNOW_VALIDATE_NRO_COMMAND}
                BYPRODUCTS "${OPENNOW_NRO_PATH}" "${OPENNOW_NACP_PATH}"
                COMMENT "Packaging OpenNOW.nro"
            )
        else()
            add_custom_target(OpenNOW.nro ALL
                DEPENDS opennow-switch
                COMMAND ${NX_NACPTOOL_EXE}
                    --create "${OPENNOW_DISPLAY_NAME}" "${OPENNOW_AUTHOR}" "${OPENNOW_VERSION}"
                    "${OPENNOW_NACP_PATH}" --titleid=${OPENNOW_TITLE_ID}
                COMMAND ${NX_ELF2NRO_EXE}
                    $<TARGET_FILE:opennow-switch> "${OPENNOW_NRO_PATH}"
                    --nacp="${OPENNOW_NACP_PATH}"
                ${OPENNOW_VALIDATE_NRO_COMMAND}
                BYPRODUCTS "${OPENNOW_NRO_PATH}" "${OPENNOW_NACP_PATH}"
                COMMENT "Packaging OpenNOW.nro"
            )
        endif()
    else()
        if(OPENNOW_REQUIRE_SWITCH_TOOLS)
            message(FATAL_ERROR "nacptool/elf2nro not found. Install devkitPro tools or set OPENNOW_REQUIRE_SWITCH_TOOLS=OFF.")
        else()
            message(WARNING "nacptool/elf2nro not found. The ELF will build, but OpenNOW.nro will not be packaged.")
        endif()
    endif()
endif()

if(OPENNOW_BUILD_TESTS AND NOT PLATFORM_SWITCH)
    enable_testing()
    add_executable(opennow-switch-input-tests
        tests/InputEncoderTests.cpp
    )
    target_link_libraries(opennow-switch-input-tests PRIVATE opennow_switch_core)
    add_test(NAME opennow-switch-input-encoder COMMAND opennow-switch-input-tests)

    add_executable(opennow-switch-cloudmatch-tests
        tests/CloudMatchTests.cpp
    )
    target_link_libraries(opennow-switch-cloudmatch-tests PRIVATE opennow_switch_core)
    add_test(NAME opennow-switch-cloudmatch COMMAND opennow-switch-cloudmatch-tests)

    add_executable(opennow-switch-auth-tests
        tests/AuthTests.cpp
    )
    target_link_libraries(opennow-switch-auth-tests PRIVATE opennow_switch_core)
    add_test(NAME opennow-switch-auth COMMAND opennow-switch-auth-tests)

    add_executable(opennow-switch-sdp-tests
        tests/SdpTests.cpp
    )
    target_link_libraries(opennow-switch-sdp-tests PRIVATE opennow_switch_core)
    add_test(NAME opennow-switch-sdp COMMAND opennow-switch-sdp-tests)

    add_executable(opennow-switch-controller-tests
        tests/ControllerMapperTests.cpp
    )
    target_link_libraries(opennow-switch-controller-tests PRIVATE opennow_switch_core)
    add_test(NAME opennow-switch-controller COMMAND opennow-switch-controller-tests)

    add_executable(opennow-switch-json-tests
        tests/JsonTests.cpp
    )
    target_link_libraries(opennow-switch-json-tests PRIVATE opennow_switch_core)
    add_test(NAME opennow-switch-json COMMAND opennow-switch-json-tests)

    add_executable(opennow-switch-log-tests
        tests/LogTests.cpp
    )
    target_link_libraries(opennow-switch-log-tests PRIVATE opennow_switch_core)
    add_test(NAME opennow-switch-log COMMAND opennow-switch-log-tests)

    add_executable(opennow-switch-dashboard-tests
        tests/DashboardTests.cpp
    )
    target_link_libraries(opennow-switch-dashboard-tests PRIVATE opennow_switch_core)
    add_test(NAME opennow-switch-dashboard COMMAND opennow-switch-dashboard-tests)

    add_test(NAME opennow-switch-nro-validator
        COMMAND ${CMAKE_COMMAND}
            -DSELFTEST_DIR:STRING=${CMAKE_CURRENT_BINARY_DIR}/nro-validator-selftest
            -DVALIDATOR_PATH:STRING=${CMAKE_CURRENT_SOURCE_DIR}/cmake/ValidateNro.cmake
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ValidateNroSelfTest.cmake
    )
endif()
