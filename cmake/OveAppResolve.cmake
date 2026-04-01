# OveAppResolve.cmake — Resolve OVE_APP_DIR from app_paths.json.
#
# Supports both two-level (apps/<lang>/<app>) and flat (apps/<app>) layouts.
# Include this file after OVE_DIR is set and before app sources are needed.

if(NOT DEFINED OVE_APP_DIR)
    file(STRINGS "${OVE_DIR}/.config" _app_line REGEX "^CONFIG_OVE_APP_NAME=")
    if(_app_line)
        string(REGEX REPLACE ".*=\"(.*)\"" "\\1" _app_name "${_app_line}")
        # Try app_paths.json first (handles two-level layout)
        set(_app_paths_json "${OVE_DIR}/output/kconfig/app_paths.json")
        if(EXISTS "${_app_paths_json}")
            file(READ "${_app_paths_json}" _app_paths_content)
            string(REGEX MATCH "\"${_app_name}\"[: ]+\"([^\"]+)\"" _match "${_app_paths_content}")
            if(CMAKE_MATCH_1)
                set(OVE_APP_DIR "${CMAKE_MATCH_1}")
            endif()
        endif()
        # Fallback: flat layout
        if(NOT DEFINED OVE_APP_DIR OR NOT EXISTS "${OVE_APP_DIR}")
            set(OVE_APP_DIR "${OVE_DIR}/apps/${_app_name}")
        endif()
    endif()
endif()
