if(NOT DEFINED NRO_PATH OR NRO_PATH STREQUAL "")
    message(FATAL_ERROR "NRO_PATH was not provided")
endif()

if(NOT DEFINED NACP_PATH OR NACP_PATH STREQUAL "")
    message(FATAL_ERROR "NACP_PATH was not provided")
endif()

if(NOT DEFINED EXPECTED_TITLE OR EXPECTED_TITLE STREQUAL "")
    message(FATAL_ERROR "EXPECTED_TITLE was not provided")
endif()

if(NOT DEFINED EXPECTED_AUTHOR OR EXPECTED_AUTHOR STREQUAL "")
    message(FATAL_ERROR "EXPECTED_AUTHOR was not provided")
endif()

string(REPLACE "\\ " " " EXPECTED_TITLE "${EXPECTED_TITLE}")
string(REPLACE "\\ " " " EXPECTED_AUTHOR "${EXPECTED_AUTHOR}")

if(NOT EXISTS "${NRO_PATH}")
    message(FATAL_ERROR "NRO file was not created: ${NRO_PATH}")
endif()

if(NOT EXISTS "${NACP_PATH}")
    message(FATAL_ERROR "NACP file was not created: ${NACP_PATH}")
endif()

file(SIZE "${NRO_PATH}" nro_size)
file(SIZE "${NACP_PATH}" nacp_size)

if(nro_size LESS 256)
    message(FATAL_ERROR "NRO file is too small to be valid: ${NRO_PATH} (${nro_size} bytes)")
endif()

if(nacp_size LESS 16384)
    message(FATAL_ERROR "NACP file is too small to be valid: ${NACP_PATH} (${nacp_size} bytes)")
endif()

file(READ "${NRO_PATH}" nro_header HEX LIMIT 64)
string(FIND "${nro_header}" "4e524f30" nro_magic_at)
if(nro_magic_at LESS 0)
    message(FATAL_ERROR "NRO magic NRO0 was not found in the first 64 bytes of ${NRO_PATH}")
endif()

file(READ "${NACP_PATH}" nacp_text HEX LIMIT 1024)
string(HEX "${EXPECTED_TITLE}" expected_title_hex)
string(TOLOWER "${expected_title_hex}" expected_title_hex)
string(FIND "${nacp_text}" "${expected_title_hex}" title_at)
if(title_at LESS 0)
    message(FATAL_ERROR "NACP title '${EXPECTED_TITLE}' was not found in ${NACP_PATH}")
endif()

string(HEX "${EXPECTED_AUTHOR}" expected_author_hex)
string(TOLOWER "${expected_author_hex}" expected_author_hex)
string(FIND "${nacp_text}" "${expected_author_hex}" author_at)
if(author_at LESS 0)
    message(FATAL_ERROR "NACP author '${EXPECTED_AUTHOR}' was not found in ${NACP_PATH}")
endif()

message(STATUS "NRO validation passed: ${NRO_PATH} (${nro_size} bytes), ${NACP_PATH} (${nacp_size} bytes)")
