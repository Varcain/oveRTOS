# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.
#
# OveZeroOverheadAudit.cmake — link-time symbol audit for the
# "zero runtime overhead / zero cost abstraction" claim.
#
# Adds a POST_BUILD step that runs `scripts/zero_overhead_audit.py`
# against the linked ELF and fails the build if any of the following
# appear:
#
#   - C++ vtable / typeinfo symbols (matches `_ZTV*` / `_ZTI*` / `_ZTS*`
#     mangled or `vtable for *` / `typeinfo for *` demangled).  oveRTOS
#     bindings claim no virtual dispatch — any hit is a regression.
#
#   - Rust trait-object (`<dyn …>::vtable`) entries.  The bindings are
#     no_std with no public dyn Trait API; any hit is a regression.
#
#   - ove_* symbols of data type (D/B/R).  All ove_* APIs must be code
#     symbols (T) or undefined externals (U) resolved by the backend at
#     link time — a `D`/`B`/`R` ove_* symbol indicates a function-pointer
#     dispatch table contradicting the "compile-time backend dispatch,
#     no function pointers or vtables" claim from README.md.
#
# The audit also enumerates the ove_* text symbols actually present in
# the ELF and writes them to <output_dir>/<target>.txt for review.
#
# Modeled directly on OveZeroHeapAudit.cmake — same _ove_pick_nm() helper,
# same `add_custom_command(OUTPUT … stamp)` hook for use across CMake
# subscope boundaries (Zephyr's nested final-link scope).
#
# Usage:
#
#   include(${OVE_DIR}/cmake/OveZeroOverheadAudit.cmake)
#   ove_assert_no_dispatch_overhead(<target>
#       BINDING <c|cpp|rust|zig>
#       [ELF_PATH <path>]
#       [OUTPUT_DIR <dir>])
#
# If ELF_PATH is omitted the macro uses $<TARGET_FILE:<target>> (works
# for plain CMake executables).  Pass an explicit path for Zephyr where
# the final ELF lives in a subscope CMakeLists.txt cannot attach
# POST_BUILD steps to; the macro then uses a file-level dependency via
# add_custom_command(OUTPUT) + add_custom_target(ALL) to hook into the
# build graph from any scope.
#
# OUTPUT_DIR defaults to ${CMAKE_BINARY_DIR}/audit/symbols.
#
# Reuses _ove_pick_nm() and _ove_pick_cppfilt() helpers — declared in
# OveZeroHeapAudit.cmake when that file is included alongside this one,
# otherwise the local fallbacks below are used (the helpers test for
# prior definition before redeclaring).

if(NOT COMMAND _ove_pick_nm)
    function(_ove_pick_nm out_var)
        if(CMAKE_NM)
            set(${out_var} "${CMAKE_NM}" PARENT_SCOPE)
            return()
        endif()
        if(CMAKE_C_COMPILER MATCHES "^(.+)-gcc$")
            set(${out_var} "${CMAKE_MATCH_1}-nm" PARENT_SCOPE)
            return()
        endif()
        set(${out_var} "nm" PARENT_SCOPE)
    endfunction()
endif()

if(NOT COMMAND _ove_pick_cppfilt)
    function(_ove_pick_cppfilt out_var)
        # Match the toolchain prefix when cross-compiling so we use the
        # right c++filt for the binary's mangling conventions (matters
        # rarely but matters for hosted Rust v0 mangling support).
        if(CMAKE_C_COMPILER MATCHES "^(.+)-gcc$")
            set(${out_var} "${CMAKE_MATCH_1}-c++filt" PARENT_SCOPE)
            return()
        endif()
        set(${out_var} "c++filt" PARENT_SCOPE)
    endfunction()
endif()

if(NOT COMMAND _ove_pick_python3)
    function(_ove_pick_python3 out_var)
        if(Python3_EXECUTABLE)
            set(${out_var} "${Python3_EXECUTABLE}" PARENT_SCOPE)
            return()
        endif()
        find_package(Python3 QUIET COMPONENTS Interpreter)
        if(Python3_EXECUTABLE)
            set(${out_var} "${Python3_EXECUTABLE}" PARENT_SCOPE)
            return()
        endif()
        find_program(_ove_python3_fallback python3)
        if(_ove_python3_fallback)
            set(${out_var} "${_ove_python3_fallback}" PARENT_SCOPE)
            return()
        endif()
        set(${out_var} "python3" PARENT_SCOPE)
    endfunction()
endif()

# ─── ove_dump_hotpaths(<target> BINDING <c|cpp|rust|zig> ...) ──────
#
# Run scripts/dump_hotpaths.py POST_BUILD on <target>, comparing the
# disassembly of selected bench-case run trampolines against the
# golden file at tests/audit/hotpath_expected.yaml.  Companion to
# ove_assert_no_dispatch_overhead — the audit proves no dispatch table
# *exists*; this proves the wrapper layer compiles to the expected
# inlined call sequence.
#
# Soft fail (warning only) on instruction-count overruns.  Hard fail
# on unexpected callees or forbidden demangled-name patterns.
function(ove_dump_hotpaths target)
    set(options STRICT)
    set(oneValueArgs ELF_PATH BINDING TARGET_KEY CONFIG OUTPUT_DIR)
    set(multiValueArgs)
    cmake_parse_arguments(_OVE_HP "${options}" "${oneValueArgs}"
                          "${multiValueArgs}" ${ARGN})

    if(NOT _OVE_HP_BINDING)
        message(FATAL_ERROR "ove_dump_hotpaths: BINDING required")
    endif()

    _ove_pick_python3(_py)
    _ove_pick_nm(_nm)
    _ove_pick_cppfilt(_cppfilt)
    # objdump is normally exposed by CMake as CMAKE_OBJDUMP; fall back
    # to the toolchain prefix derivation if not.  The audit must run
    # the cross objdump on cross-compiled ELFs (host objdump can't
    # decode ARM/Thumb/Cortex-M opcodes).
    if(CMAKE_OBJDUMP)
        set(_objdump "${CMAKE_OBJDUMP}")
    elseif(CMAKE_C_COMPILER MATCHES "^(.+)-gcc$")
        set(_objdump "${CMAKE_MATCH_1}-objdump")
    else()
        set(_objdump "objdump")
    endif()

    if(NOT DEFINED OVE_DIR)
        get_filename_component(_ove_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    else()
        set(_ove_root "${OVE_DIR}")
    endif()
    set(_hp_script "${_ove_root}/scripts/dump_hotpaths.py")

    if(NOT _OVE_HP_CONFIG)
        set(_OVE_HP_CONFIG "${_ove_root}/tests/audit/hotpath_expected.yaml")
    endif()
    if(NOT _OVE_HP_OUTPUT_DIR)
        set(_OVE_HP_OUTPUT_DIR "${CMAKE_BINARY_DIR}/audit/disasm")
    endif()

    set(_strict_flag)
    if(_OVE_HP_STRICT)
        set(_strict_flag "--strict")
    endif()

    set(_target_key_arg)
    if(_OVE_HP_TARGET_KEY)
        set(_target_key_arg --target "${_OVE_HP_TARGET_KEY}")
    endif()

    if(_OVE_HP_ELF_PATH)
        # File-level dependency for cross-subscope use (e.g. NuttX,
        # Zephyr — final ELF is built by a sibling cmake project that
        # this directory cannot attach POST_BUILD steps to).  Same
        # pattern as ove_assert_no_dispatch_overhead's ELF_PATH branch.
        set(_elf "${_OVE_HP_ELF_PATH}")
        set(_stamp_dir "${CMAKE_CURRENT_BINARY_DIR}/hotpath_dump")
        set(_stamp "${_stamp_dir}/${target}.stamp")
        file(MAKE_DIRECTORY "${_stamp_dir}")
        add_custom_command(OUTPUT "${_stamp}"
            DEPENDS "${_elf}" "${_hp_script}"
            COMMAND ${CMAKE_COMMAND} -E echo
                    "[hotpath dump] checking ${_elf} (binding=${_OVE_HP_BINDING})"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_OVE_HP_OUTPUT_DIR}"
            COMMAND ${_py} "${_hp_script}"
                    --elf "${_elf}"
                    --binding "${_OVE_HP_BINDING}"
                    --config "${_OVE_HP_CONFIG}"
                    --output-dir "${_OVE_HP_OUTPUT_DIR}"
                    --nm "${_nm}"
                    --objdump "${_objdump}"
                    --cppfilt "${_cppfilt}"
                    ${_target_key_arg}
                    ${_strict_flag}
            COMMAND ${CMAKE_COMMAND} -E touch "${_stamp}"
            VERBATIM
            COMMENT "Dumping ${target} hotpath disassembly"
        )
        add_custom_target(${target}_hotpath_dump ALL DEPENDS "${_stamp}")
    else()
        set(_elf "$<TARGET_FILE:${target}>")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo
                    "[hotpath dump] checking ${_elf} (binding=${_OVE_HP_BINDING})"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_OVE_HP_OUTPUT_DIR}"
            COMMAND ${_py} "${_hp_script}"
                    --elf "${_elf}"
                    --binding "${_OVE_HP_BINDING}"
                    --config "${_OVE_HP_CONFIG}"
                    --output-dir "${_OVE_HP_OUTPUT_DIR}"
                    --nm "${_nm}"
                    --objdump "${_objdump}"
                    --cppfilt "${_cppfilt}"
                    ${_target_key_arg}
                    ${_strict_flag}
            VERBATIM
            COMMENT "Dumping ${target} hotpath disassembly"
        )
    endif()
endfunction()


function(ove_assert_no_dispatch_overhead target)
    set(options NO_PANIC_SYMBOLS)
    set(oneValueArgs ELF_PATH BINDING OUTPUT_DIR)
    set(multiValueArgs)
    cmake_parse_arguments(_OVE_ZO "${options}" "${oneValueArgs}"
                          "${multiValueArgs}" ${ARGN})

    if(NOT _OVE_ZO_BINDING)
        message(FATAL_ERROR
            "ove_assert_no_dispatch_overhead: BINDING required "
            "(one of c, cpp, rust, zig)")
    endif()
    if(NOT _OVE_ZO_BINDING MATCHES "^(c|cpp|rust|zig)$")
        message(FATAL_ERROR
            "ove_assert_no_dispatch_overhead: BINDING must be c, cpp, rust, "
            "or zig (got '${_OVE_ZO_BINDING}')")
    endif()

    # NO_PANIC_SYMBOLS — pass --no-panic-symbols to the script. Forbids
    # Rust core::panicking::*, core::fmt::Arguments::new_v1, Zig
    # std.builtin.default_panic, and __zig_probe_stack symbols. Use on
    # zero-heap builds where panic-formatting machinery would pull in
    # allocating fmt paths.
    set(_no_panic_flag)
    if(_OVE_ZO_NO_PANIC_SYMBOLS)
        set(_no_panic_flag "--no-panic-symbols")
    endif()

    _ove_pick_nm(_nm)
    _ove_pick_cppfilt(_cppfilt)
    _ove_pick_python3(_py)

    # OVE_DIR is set by every project that consumes oveRTOS; fall back to
    # walking up from this file's location for unusual setups.
    if(NOT DEFINED OVE_DIR)
        get_filename_component(_ove_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    else()
        set(_ove_root "${OVE_DIR}")
    endif()
    set(_audit_script "${_ove_root}/scripts/zero_overhead_audit.py")

    if(NOT _OVE_ZO_OUTPUT_DIR)
        set(_OVE_ZO_OUTPUT_DIR "${CMAKE_BINARY_DIR}/audit/symbols")
    endif()

    set(_stamp_dir "${CMAKE_CURRENT_BINARY_DIR}/zero_overhead_audit")
    set(_stamp "${_stamp_dir}/${target}.stamp")
    file(MAKE_DIRECTORY "${_stamp_dir}")

    if(_OVE_ZO_ELF_PATH)
        # File-level dependency for cross-subscope use (Zephyr's nested
        # final link).  See OveZeroHeapAudit.cmake for the rationale.
        set(_elf "${_OVE_ZO_ELF_PATH}")
        add_custom_command(OUTPUT "${_stamp}"
            DEPENDS "${_elf}" "${_audit_script}"
            COMMAND ${CMAKE_COMMAND} -E echo
                    "[zero-overhead audit] checking ${_elf} (binding=${_OVE_ZO_BINDING})"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_OVE_ZO_OUTPUT_DIR}"
            COMMAND ${_py} "${_audit_script}"
                    --elf "${_elf}"
                    --binding "${_OVE_ZO_BINDING}"
                    --target "${target}"
                    --nm "${_nm}"
                    --cppfilt "${_cppfilt}"
                    --output-dir "${_OVE_ZO_OUTPUT_DIR}"
                    ${_no_panic_flag}
            COMMAND ${CMAKE_COMMAND} -E touch "${_stamp}"
            VERBATIM
            COMMENT "Auditing ${_elf} for forbidden zero-overhead symbols"
        )
        add_custom_target(${target}_zero_overhead_audit ALL DEPENDS "${_stamp}")
    else()
        # Plain CMake executable — POST_BUILD on the target is fine.
        set(_elf "$<TARGET_FILE:${target}>")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo
                    "[zero-overhead audit] checking ${_elf} (binding=${_OVE_ZO_BINDING})"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_OVE_ZO_OUTPUT_DIR}"
            COMMAND ${_py} "${_audit_script}"
                    --elf "${_elf}"
                    --binding "${_OVE_ZO_BINDING}"
                    --target "${target}"
                    --nm "${_nm}"
                    --cppfilt "${_cppfilt}"
                    --output-dir "${_OVE_ZO_OUTPUT_DIR}"
                    ${_no_panic_flag}
            VERBATIM
            COMMENT "Auditing ${target} for forbidden zero-overhead symbols"
        )
    endif()
endfunction()
