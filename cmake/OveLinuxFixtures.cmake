# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.
#
# OveLinuxFixtures.cmake — generate the Linux-personality guest fixtures (the
# Buildroot rootfs CPIO + the uClibc/bFLT guest test programs) at build time
# from the Buildroot artifacts, so the large byte-array headers are NOT
# committed to git. If the required Buildroot image/toolchain is unavailable the
# build FAILS with an actionable message (per the project decision to keep
# generated blobs out of the tree).

if(COMMAND ove_linux_generate_fixtures)
    return()
endif()

set(OVE_BUILDROOT "$ENV{HOME}/projects/private/hIRoic/buildroot" CACHE PATH
    "Buildroot clone providing output/images/rootfs.cpio + the uClibc/bFLT toolchain")

# ove_linux_generate_fixtures(<out-dir-var> FIXTURES <names...>)
#   Generates loader_<name>_image.h headers into <build>/ove_linux_fixtures and
#   returns that directory in <out-dir-var>. Supported names:
#     rootfs — embed ${OVE_BUILDROOT}/output/images/rootfs.cpio (SYM ove_test_rootfs_cpio)
#   FATAL_ERROR if a required artifact is missing.
function(ove_linux_generate_fixtures out_var)
    cmake_parse_arguments(F "" "" "FIXTURES" ${ARGN})
    if(NOT DEFINED OVE_DIR)
        message(FATAL_ERROR "ove_linux_generate_fixtures: OVE_DIR is not set")
    endif()
    set(gendir "${CMAKE_BINARY_DIR}/ove_linux_fixtures")
    file(MAKE_DIRECTORY "${gendir}")
    set(embed "${OVE_DIR}/tests/cmake/embed_bin.cmake")

    foreach(fx ${F_FIXTURES})
        if(fx STREQUAL "rootfs")
            set(cpio "${OVE_BUILDROOT}/output/images/rootfs.cpio")
            # Re-run CMake configure (and thus re-embed) whenever the cpio changes,
            # so a Buildroot rootfs rebuild is picked up by `ove build` alone.
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${cpio}")
            if(NOT EXISTS "${cpio}")
                message(FATAL_ERROR
                    "Linux-personality fixture: ${cpio} not found.\n"
                    "  Build the Buildroot rootfs first:\n"
                    "    tests/sim/zephyr-linux/regen-rootfs-fixture.sh\n"
                    "  or set -DOVE_BUILDROOT=<tree with output/images/rootfs.cpio>.")
            endif()
            execute_process(
                COMMAND ${CMAKE_COMMAND} -DIN=${cpio}
                        -DOUT=${gendir}/loader_rootfs_image.h
                        -DSYM=ove_test_rootfs_cpio -P ${embed}
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
