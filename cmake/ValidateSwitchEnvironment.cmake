set(errors "")

if(NOT DEFINED DEVKITPRO OR DEVKITPRO STREQUAL "")
    if(DEFINED ENV{DEVKITPRO})
        set(DEVKITPRO "$ENV{DEVKITPRO}")
    elseif(EXISTS "C:/devkitPro")
        set(DEVKITPRO "C:/devkitPro")
    elseif(EXISTS "C:/DEVKITPRO")
        set(DEVKITPRO "C:/DEVKITPRO")
    endif()
endif()

if(NOT DEFINED DEVKITA64 OR DEVKITA64 STREQUAL "")
    if(DEFINED ENV{DEVKITA64})
        set(DEVKITA64 "$ENV{DEVKITA64}")
    elseif(DEFINED DEVKITPRO AND EXISTS "${DEVKITPRO}/devkitA64")
        set(DEVKITA64 "${DEVKITPRO}/devkitA64")
    elseif(EXISTS "C:/devkitPro/devkitA64")
        set(DEVKITA64 "C:/devkitPro/devkitA64")
    elseif(EXISTS "C:/DEVKITPRO/devkitA64")
        set(DEVKITA64 "C:/DEVKITPRO/devkitA64")
    endif()
endif()

if(NOT DEVKITPRO)
    list(APPEND errors "DEVKITPRO is not set")
elseif(NOT EXISTS "${DEVKITPRO}")
    list(APPEND errors "DEVKITPRO does not exist: ${DEVKITPRO}")
endif()

if(NOT DEVKITA64)
    list(APPEND errors "DEVKITA64 is not set")
elseif(NOT EXISTS "${DEVKITA64}")
    list(APPEND errors "DEVKITA64 does not exist: ${DEVKITA64}")
endif()

foreach(tool IN ITEMS nacptool elf2nro)
    if(DEFINED DEVKITPRO AND EXISTS "${DEVKITPRO}/tools/bin/${tool}${CMAKE_EXECUTABLE_SUFFIX}")
        continue()
    endif()
    find_program(found_${tool} ${tool}
        HINTS
            "${DEVKITPRO}/tools/bin"
            "$ENV{DEVKITPRO}/tools/bin"
        NO_CACHE
    )
    if(NOT found_${tool})
        list(APPEND errors "${tool} was not found in DEVKITPRO/tools/bin or PATH")
    endif()
endforeach()

if(errors)
    list(JOIN errors "\n  - " error_text)
    message(FATAL_ERROR "Switch build environment validation failed:\n  - ${error_text}")
endif()

message(STATUS "Switch build environment validation passed")
