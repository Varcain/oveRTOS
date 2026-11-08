# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
include_guard(GLOBAL)

# Call before find_package(Zephyr), so its syscall scan sees the private API.
function(ove_zephyr_register_eventgroup_syscalls)
    get_filename_component(_ove_syscall_dir
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../backends/zephyr/syscalls" ABSOLUTE)
    set(_ove_syscall_dirs ${SYSCALL_INCLUDE_DIRS} "${_ove_syscall_dir}")
    list(REMOVE_DUPLICATES _ove_syscall_dirs)
    set(SYSCALL_INCLUDE_DIRS ${_ove_syscall_dirs} PARENT_SCOPE)
endfunction()
