if(NOT DEFINED SELFTEST_DIR OR SELFTEST_DIR STREQUAL "")
    message(FATAL_ERROR "SELFTEST_DIR was not provided")
endif()

if(NOT DEFINED VALIDATOR_PATH OR VALIDATOR_PATH STREQUAL "")
    message(FATAL_ERROR "VALIDATOR_PATH was not provided")
endif()

file(MAKE_DIRECTORY "${SELFTEST_DIR}")

set(test_nro "${SELFTEST_DIR}/valid-fixture.nro")
set(test_nacp "${SELFTEST_DIR}/valid-fixture.nacp")

string(REPEAT "A" 16 nro_prefix)
string(REPEAT "B" 256 nro_padding)
file(WRITE "${test_nro}" "${nro_prefix}NRO0${nro_padding}")

string(REPEAT "C" 17000 nacp_padding)
file(WRITE "${test_nacp}" "OpenNOW Switch OpenCloudGaming ${nacp_padding}")

set(NRO_PATH "${test_nro}")
set(NACP_PATH "${test_nacp}")
set(EXPECTED_TITLE "OpenNOW Switch")
set(EXPECTED_AUTHOR "OpenCloudGaming")

include("${VALIDATOR_PATH}")
