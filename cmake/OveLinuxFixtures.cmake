# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.
#
# OveLinuxFixtures.cmake — generate the Linux-personality guest fixtures (the
# Buildroot rootfs CPIO + the uClibc/FDPIC guest test programs) at build time
# from the Buildroot artifacts, so the large byte-array headers are NOT
# committed to git. If the required Buildroot image/toolchain is unavailable the
# build FAILS with an actionable message (per the project decision to keep
# generated blobs out of the tree).

if(COMMAND ove_linux_generate_fixtures)
    return()
endif()

# The Buildroot tree (OVE_BUILDROOT) and the output subdir (OVE_LINUX_ROOTFS_OUTPUT) are CONFIG_*
# (Kconfig), set by the generated ove_config.cmake that the board includes before this — NO local
# path is hardcoded here. A relative OVE_BUILDROOT is resolved against the oveRTOS root below.

# ove_linux_generate_fixtures(<out-dir-var> FIXTURES <names...>)
#   Generates loader_<name>_image.h headers into <build>/ove_linux_fixtures and
#   returns that directory in <out-dir-var>. Supported names:
#     rootfs — embed ${OVE_BUILDROOT}/${OVE_LINUX_ROOTFS_OUTPUT}/images/rootfs.cpio (SYM ove_test_rootfs_cpio)
#   FATAL_ERROR if a required artifact is missing.
function(ove_linux_generate_fixtures out_var)
    cmake_parse_arguments(F "" "" "FIXTURES" ${ARGN})
    if(NOT DEFINED OVE_DIR)
        message(FATAL_ERROR "ove_linux_generate_fixtures: OVE_DIR is not set")
    endif()
    # OVE_BUILDROOT / OVE_LINUX_ROOTFS_OUTPUT come from CONFIG_* via ove_config.cmake. Resolve a
    # relative OVE_BUILDROOT against the oveRTOS root so the "../buildroot" sibling default works
    # with no path baked into the sources.
    if(NOT DEFINED OVE_BUILDROOT OR OVE_BUILDROOT STREQUAL "")
        message(FATAL_ERROR
            "CONFIG_OVE_BUILDROOT is empty — point it at your Buildroot tree (menuconfig or a "
            "defconfig fragment), e.g. CONFIG_OVE_BUILDROOT=\"../buildroot\".")
    endif()
    set(_buildroot "${OVE_BUILDROOT}")
    if(NOT IS_ABSOLUTE "${_buildroot}")
        get_filename_component(_buildroot "${OVE_DIR}/${_buildroot}" ABSOLUTE)
    endif()
    if(NOT DEFINED OVE_LINUX_ROOTFS_OUTPUT OR OVE_LINUX_ROOTFS_OUTPUT STREQUAL "")
        set(OVE_LINUX_ROOTFS_OUTPUT "output")
    endif()
    set(gendir "${CMAKE_BINARY_DIR}/ove_linux_fixtures")
    file(MAKE_DIRECTORY "${gendir}")
    set(embed "${OVE_DIR}/tests/cmake/embed_bin.cmake")

    foreach(fx ${F_FIXTURES})
        if(fx STREQUAL "rootfs")
            set(cpio "${_buildroot}/${OVE_LINUX_ROOTFS_OUTPUT}/images/rootfs.cpio")
            # Re-run CMake configure (and thus re-embed) whenever the cpio changes,
            # so a Buildroot rootfs rebuild is picked up by `ove build` alone.
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${cpio}")
            if(NOT EXISTS "${cpio}")
                message(FATAL_ERROR
                    "Linux-personality fixture: ${cpio} not found.\n"
                    "  Build the Buildroot rootfs first:\n"
                    "    tests/sim/zephyr-linux/regen-rootfs-fixture.sh\n"
                    "  or set -DOVE_LINUX_ROOTFS_OUTPUT=<dir> (default output) / -DOVE_BUILDROOT=<tree>.")
            endif()
            # Zephyr runs the program UNPRIVILEGED: place the cpio in an executable .text
            # subsection so its user threads can execute libc.so's RO text IN-PLACE from the
            # embedded cpio (the kernel's user-RX .text MPU region already covers .text — no
            # separate partition, so no MPU-budget overflow). FreeRTOS/NuttX run privileged and
            # reach .rodata directly, so they keep the default section.
            set(_extra_args)
            if(OVE_RTOS STREQUAL "zephyr")
                set(_extra_args "-DSECTION=.text.ove_rootfs")
            endif()
            execute_process(
                COMMAND ${CMAKE_COMMAND} -DIN=${cpio}
                        -DOUT=${gendir}/loader_rootfs_image.h
                        -DSYM=ove_test_rootfs_cpio ${_extra_args} -P ${embed}
                RESULT_VARIABLE _rc)
            if(NOT _rc EQUAL 0)
                message(FATAL_ERROR "embed ${cpio} failed (${_rc})")
            endif()
        else()
            message(FATAL_ERROR "ove_linux_generate_fixtures: unknown fixture '${fx}'")
        endif()
    endforeach()
    set(${out_var} "${gendir}" PARENT_SCOPE)
endfunction()
