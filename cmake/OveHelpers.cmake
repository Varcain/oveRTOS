# ============================================================================
# OveHelpers.cmake — shared cmake helpers used by multiple RTOS backends
# ============================================================================

# ─── _ove_filter_backend_list(LIST_VAR MOD1 [MOD2 ...]) ──────────
# Remove engine backend source files matching the listed modules from the
# named list variable. Engine files use a _(module).c suffix under a concrete
# engine directory; shared backends/common composition must remain present.
macro(_ove_filter_backend_list LIST_VAR)
    foreach(_mod ${ARGN})
        string(TOUPPER "${_mod}" _MOD_UPPER)
        string(TOLOWER "${_mod}" _mod_lower)
        set(_new_backend "")
        foreach(_bsrc ${${LIST_VAR}})
            get_filename_component(_bname "${_bsrc}" NAME)
            set(_exclude FALSE)
            if("${_bsrc}" MATCHES "/backends/(freertos|nuttx|posix|wasm|zephyr)/")
                if("${_bname}" MATCHES "_(${_mod_lower})\\.c$")
                    set(_exclude TRUE)
                endif()
                if("${_MOD_UPPER}" STREQUAL "BSP"
                   AND "${_bname}" MATCHES "_bsp\\.c$")
                    set(_exclude TRUE)
                endif()
            endif()
            if(NOT _exclude)
                list(APPEND _new_backend "${_bsrc}")
            endif()
        endforeach()
        set(${LIST_VAR} ${_new_backend})
    endforeach()
endmacro()
