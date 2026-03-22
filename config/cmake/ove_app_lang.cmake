# oveRTOS Application Language Support
#
# Provides ove_apply_app_language(TARGET) — call after add_executable().
# Reads OVE_APP_LANG from ove_config.cmake (already included).
#
# C:    adds APP_SOURCES as-is
# C++:  enables CXX, sets C++20, adds -fno-exceptions -fno-rtti
# Rust: delegates to ove_rust.cmake

function(ove_apply_app_language TARGET)
    if(NOT DEFINED OVE_APP_LANG)
        set(OVE_APP_LANG "c")
    endif()

    if(OVE_APP_LANG STREQUAL "c")
        # Standard C: add sources directly
        target_sources(${TARGET} PRIVATE ${APP_SOURCES})

    elseif(OVE_APP_LANG STREQUAL "cpp")
        # C++: set standard, add flags (CXX enabled via project() declaration)
        set_property(TARGET ${TARGET} PROPERTY CXX_STANDARD 20)
        set_property(TARGET ${TARGET} PROPERTY CXX_STANDARD_REQUIRED ON)
        target_compile_options(${TARGET} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>
        )
        target_sources(${TARGET} PRIVATE ${APP_SOURCES})

    elseif(OVE_APP_LANG STREQUAL "rust")
        # Rust: delegate to cargo-based build
        include(${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ove_rust.cmake)
        ove_build_rust_crate(${TARGET})

    elseif(OVE_APP_LANG STREQUAL "zig")
        # Zig: delegate to zig build-lib
        include(${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ove_zig.cmake)
        ove_build_zig_lib(${TARGET})

    else()
        message(FATAL_ERROR "Unknown OVE_APP_LANG: ${OVE_APP_LANG}")
    endif()
endfunction()
