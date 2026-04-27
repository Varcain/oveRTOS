# Build picolibc in-tree against the configured arm-none-eabi-gcc toolchain.
#
# The arm-gnu-toolchain 15.x distribution ships only newlib (nano.specs,
# rdimon.specs, nosys.specs) — no picolibc.specs, no libpicolibc.a.
# Rather than swapping toolchain (Zephyr SDK / xPack), we vendor picolibc
# (manifest.yaml > libraries.picolibc) and build it once via meson against
# the same arm-none-eabi-gcc the rest of the FreeRTOS build uses.  This
# keeps a single `arm-none-eabi-` toolchain for everything FreeRTOS, with
# unified picolibc semantics matching Zephyr.
#
# Inputs (must be set by caller before include):
#   OVE_DIR                         — repo root (used to locate dl/picolibc-*)
#   PICOLIBC_TAG                    — git tag, mirrors manifest.yaml
#   PICOLIBC_TOOLCHAIN_PREFIX       — e.g. "/path/to/arm-none-eabi-"
#   PICOLIBC_CPU_FLAGS              — "-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16"
#
# Outputs (set in PARENT_SCOPE):
#   OVE_PICOLIBC_PREFIX             — install dir
#   OVE_PICOLIBC_SPECS              — absolute path to picolibc.specs
#
# Build runs at configure time via execute_process(); subsequent configures
# skip when picolibc.specs already exists at the install path.

function(ove_build_picolibc)
    if(NOT DEFINED OVE_DIR)
        message(FATAL_ERROR "ove_build_picolibc: OVE_DIR not set")
    endif()
    if(NOT DEFINED PICOLIBC_TAG)
        message(FATAL_ERROR "ove_build_picolibc: PICOLIBC_TAG not set")
    endif()
    if(NOT DEFINED PICOLIBC_TOOLCHAIN_PREFIX)
        message(FATAL_ERROR
            "ove_build_picolibc: PICOLIBC_TOOLCHAIN_PREFIX not set")
    endif()
    if(NOT DEFINED PICOLIBC_CPU_FLAGS)
        message(FATAL_ERROR "ove_build_picolibc: PICOLIBC_CPU_FLAGS not set")
    endif()

    # Workspace-shared install dir — keyed on tag + cpu so re-targeting
    # different cores doesn't cross-contaminate.  Hash kept short for
    # readability; collisions are not a security concern (build artifacts).
    string(SHA1 _hash "${PICOLIBC_TAG}|${PICOLIBC_CPU_FLAGS}")
    string(SUBSTRING "${_hash}" 0 8 _hash)
    set(_install "${OVE_DIR}/output/picolibc-install/${PICOLIBC_TAG}-${_hash}")
    set(_specs "${_install}/lib/picolibc.specs")
    set(_build "${OVE_DIR}/output/picolibc-build/${PICOLIBC_TAG}-${_hash}")

    set(OVE_PICOLIBC_PREFIX "${_install}" PARENT_SCOPE)
    set(OVE_PICOLIBC_SPECS "${_specs}" PARENT_SCOPE)

    if(EXISTS "${_specs}")
        message(STATUS "picolibc: already built at ${_install}")
        return()
    endif()

    # Locate source.  `ove download` lands picolibc under
    # dl/picolibc-<tag>/, with a workspace-local symlink dl/picolibc.
    set(_src "${OVE_DIR}/dl/picolibc-${PICOLIBC_TAG}")
    if(NOT EXISTS "${_src}/meson.build")
        # Try the unhashed symlink as a fallback.
        set(_src "${OVE_DIR}/dl/picolibc")
    endif()
    if(NOT EXISTS "${_src}/meson.build")
        message(FATAL_ERROR
            "picolibc source not found.  Run `ove download` (or `make download`) "
            "to fetch picolibc-${PICOLIBC_TAG} into dl/.")
    endif()

    find_program(_meson meson REQUIRED)
    find_program(_ninja ninja REQUIRED)

    file(MAKE_DIRECTORY "${_build}")

    # Convert space-separated CPU flags into a meson string list.
    string(REPLACE " " ";" _cpu_list "${PICOLIBC_CPU_FLAGS}")
    set(_cpu_quoted "")
    foreach(_f IN LISTS _cpu_list)
        if(NOT _f STREQUAL "")
            list(APPEND _cpu_quoted "'${_f}'")
        endif()
    endforeach()
    string(JOIN ", " _cpu_meson ${_cpu_quoted})

    # Cross file.  `-nostdlib` on the c entry is required by older meson
    # versions for the basic compiler probe (matches picolibc's stock
    # scripts/cross-arm-none-eabi.txt).
    set(_cross "${_build}/cross-ove.txt")
    file(WRITE "${_cross}"
        "[binaries]\n"
        "c = ['${PICOLIBC_TOOLCHAIN_PREFIX}gcc', '-nostdlib']\n"
        "ar = '${PICOLIBC_TOOLCHAIN_PREFIX}ar'\n"
        "as = '${PICOLIBC_TOOLCHAIN_PREFIX}as'\n"
        "nm = '${PICOLIBC_TOOLCHAIN_PREFIX}nm'\n"
        "strip = '${PICOLIBC_TOOLCHAIN_PREFIX}strip'\n"
        "\n"
        "[host_machine]\n"
        "system = 'none'\n"
        "cpu_family = 'arm'\n"
        "cpu = 'arm'\n"
        "endian = 'little'\n"
        "\n"
        "[properties]\n"
        "skip_sanity_check = true\n"
        "default_flash_addr = '0x00000000'\n"
        "default_flash_size = '0x00400000'\n"
        "default_ram_addr   = '0x20000000'\n"
        "default_ram_size   = '0x00200000'\n"
        "\n"
        "[built-in options]\n"
        "c_args = [${_cpu_meson}]\n"
        "c_link_args = [${_cpu_meson}]\n"
    )

    # Meson setup.
    #
    # Picolibc options chosen to match Zephyr's CONFIG_PICOLIBC_IO_LONG_LONG
    # and the FreeRTOS-side picolibc-freertos.h TLS glue:
    #   format-default=long-long  → %lld/%llu always supported
    #   tinystdio=true (default)  → minimal FILE-based stdio
    #   newlib-nano-malloc=true   → size-optimised malloc impl
    #   thread-local-storage=true → enables __thread (errno per task)
    #   tls-model=local-exec      → static binary TLS, no dynamic relocations
    #   multilib=false            → build for the one CPU variant we target
    #   atomic-ungetc=false       → avoids dependence on libatomic
    #   posix-console=false       → board provides _write() via syscalls.c
    message(STATUS "picolibc: configuring meson for ${PICOLIBC_TAG}")
    execute_process(
        COMMAND "${_meson}" setup
                "${_build}" "${_src}"
                "--cross-file=${_cross}"
                "--buildtype=minsize"
                "--prefix=${_install}"
                "--libdir=lib"
                "--includedir=include"
                "-Dspecsdir=${_install}/lib"
                "-Dmultilib=false"
                "-Dtinystdio=true"
                "-Dformat-default=long-long"
                "-Dnewlib-nano-malloc=true"
                "-Dthread-local-storage=true"
                "-Dtls-model=local-exec"
                "-Datomic-ungetc=false"
                "-Dposix-console=false"
                "-Dtests=false"
                "--reconfigure"
        RESULT_VARIABLE _setup_rc
        OUTPUT_VARIABLE _setup_out
        ERROR_VARIABLE _setup_err)
    if(NOT _setup_rc EQUAL 0)
        message(FATAL_ERROR
            "picolibc meson setup failed (rc=${_setup_rc}):\n"
            "${_setup_out}\n${_setup_err}")
    endif()

    message(STATUS "picolibc: compiling")
    execute_process(
        COMMAND "${_meson}" compile -C "${_build}"
        RESULT_VARIABLE _build_rc
        OUTPUT_VARIABLE _build_out
        ERROR_VARIABLE _build_err)
    if(NOT _build_rc EQUAL 0)
        message(FATAL_ERROR
            "picolibc meson compile failed (rc=${_build_rc}):\n"
            "${_build_out}\n${_build_err}")
    endif()

    message(STATUS "picolibc: installing to ${_install}")
    execute_process(
        COMMAND "${_meson}" install -C "${_build}"
        RESULT_VARIABLE _install_rc
        OUTPUT_VARIABLE _install_out
        ERROR_VARIABLE _install_err)
    if(NOT _install_rc EQUAL 0)
        message(FATAL_ERROR
            "picolibc meson install failed (rc=${_install_rc}):\n"
            "${_install_out}\n${_install_err}")
    endif()

    if(NOT EXISTS "${_specs}")
        message(FATAL_ERROR
            "picolibc install completed but ${_specs} not found")
    endif()
    message(STATUS "picolibc: ready (${_specs})")
endfunction()
