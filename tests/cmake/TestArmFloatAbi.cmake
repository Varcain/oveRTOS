if(NOT DEFINED OVE_ROOT)
    message(FATAL_ERROR "OVE_ROOT is required")
endif()

include("${OVE_ROOT}/cmake/OveBindingsCommon.cmake")

function(assert_target EXPECTED INPUT ABI)
    set(OVE_RTOS "freertos")
    set(OVE_ARM_FLOAT_ABI "${ABI}")
    _ove_binding_align_arm_float_target(_actual "${INPUT}")
    if(NOT "${_actual}" STREQUAL "${EXPECTED}")
        message(FATAL_ERROR
            "${ABI}: ${INPUT} resolved to ${_actual}, expected ${EXPECTED}")
    endif()
endfunction()

assert_target("thumbv7em-none-eabihf" "thumbv7em-none-eabihf" "hard")
assert_target("thumbv7em-none-eabihf" "thumbv7em-none-eabi" "hard")
assert_target("thumbv7em-none-eabi" "thumbv7em-none-eabihf" "softfp")
assert_target("thumbv7em-none-eabi" "thumbv7em-none-eabi" "softfp")

# Non-FreeRTOS backends own their ABI selection and must remain untouched.
set(OVE_RTOS "zephyr")
set(OVE_ARM_FLOAT_ABI "hard")
_ove_binding_align_arm_float_target(_zephyr "thumbv8m.main-none-eabi")
if(NOT "${_zephyr}" STREQUAL "thumbv8m.main-none-eabi")
    message(FATAL_ERROR "Non-FreeRTOS target was unexpectedly rewritten")
endif()
