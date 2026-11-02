# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

include_guard(GLOBAL)

# Install before the simulator's relocatable link as well as the ARM link.
# Keep the native table traversal and architecture-specific initialization.
function(ove_nuttx_guard_cxx_initialization)
    # NuttX also evaluates applications in its earlier Kconfig preapps pass.
    if(NOT TARGET nuttx OR NOT CONFIG_BUILD_FLAT OR NOT CONFIG_HAVE_CXXINITIALIZE)
        return()
    endif()
    if(NOT TARGET c)
        message(FATAL_ERROR "Cannot guard NuttX constructors without the native c target")
    endif()
    get_target_property(_installed c OVE_CXX_INITIALIZATION_GUARDED)
    if(_installed)
        return()
    endif()

    get_target_property(_sources c SOURCES)
    get_target_property(_source_dir c SOURCE_DIR)
    set(_initializers "")
    foreach(_source IN LISTS _sources)
        if(_source MATCHES "(^|/)misc/lib_cxx_initialize\\.c$")
            get_filename_component(_initializer "${_source}" ABSOLUTE
                BASE_DIR "${_source_dir}")
            list(APPEND _initializers "${_initializer}")
        endif()
    endforeach()
    list(LENGTH _initializers _count)
    if(NOT _count EQUAL 1)
        message(FATAL_ERROR "Expected exactly one native NuttX constructor initializer")
    endif()
    list(GET _initializers 0 _initializer)
    get_source_file_property(_definitions "${_initializer}" TARGET_DIRECTORY c
        COMPILE_DEFINITIONS)
    foreach(_definition IN LISTS _definitions)
        if(_definition MATCHES "^lib_cxx_initialize($|=)")
            message(FATAL_ERROR "The native NuttX constructor initializer is already renamed")
        endif()
    endforeach()
    set_property(SOURCE "${_initializer}" TARGET_DIRECTORY c APPEND PROPERTY
        COMPILE_DEFINITIONS "lib_cxx_initialize=ove_nuttx_native_cxx_initialize")
    target_sources(c PRIVATE "${OVE_DIR}/backends/nuttx/nuttx_cxx_initialize.c")
    set_property(TARGET c PROPERTY OVE_CXX_INITIALIZATION_GUARDED TRUE)
endfunction()
