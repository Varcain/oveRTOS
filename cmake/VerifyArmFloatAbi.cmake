# Verify that a linked ARM ELF advertises the configured procedure-call ABI.
# A hard-float image must carry Tag_ABI_VFP_args; a softfp image must not.

if(NOT DEFINED ELF_FILE OR NOT EXISTS "${ELF_FILE}")
    message(FATAL_ERROR "ELF_FILE does not name an existing firmware image")
endif()
if(NOT DEFINED READELF OR READELF STREQUAL "")
    message(FATAL_ERROR "READELF must name the readelf executable")
endif()
if(NOT EXPECTED_ABI STREQUAL "hard" AND
   NOT EXPECTED_ABI STREQUAL "softfp")
    message(FATAL_ERROR
        "EXPECTED_ABI must be 'hard' or 'softfp', got '${EXPECTED_ABI}'")
endif()

execute_process(
    COMMAND "${READELF}" -A "${ELF_FILE}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _attributes
    ERROR_VARIABLE _error)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
        "readelf failed for ${ELF_FILE} (rc=${_rc}): ${_error}")
endif()

set(_vfp_args FALSE)
if(_attributes MATCHES "Tag_ABI_VFP_args:[^\n]*VFP registers")
    set(_vfp_args TRUE)
endif()

if(EXPECTED_ABI STREQUAL "hard" AND NOT _vfp_args)
    message(FATAL_ERROR
        "${ELF_FILE} is configured hard-float but does not advertise VFP "
        "argument passing; a soft/mixed archive may have contaminated the link")
elseif(EXPECTED_ABI STREQUAL "softfp" AND _vfp_args)
    message(FATAL_ERROR
        "${ELF_FILE} is configured softfp but advertises VFP argument "
        "passing; a hard-float archive contaminated the link")
endif()

message(STATUS
    "Verified ${EXPECTED_ABI} calling convention in ${ELF_FILE}")
