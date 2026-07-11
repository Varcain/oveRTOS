# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# lxp — the OS-agnostic Linux personality, as a standalone module.
#
# Two ways to consume it:
#   * Standalone:  cmake -S modules/lxp -B build && cmake --build build
#                  (pick a port, e.g. -DLXP_PORT_POSIX=ON for the reference port)
#   * Embedded:    add_subdirectory(modules/lxp) then target_link_libraries(app lxp::lxp)
#                  and set the LXP_ENABLE_* / sizing defines from your own config.

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED PROJECT_NAME)
  project(lxp C)
endif()

# ---- feature options (exposed to a standalone build; the embedded build sets
#      the LXP_ENABLE_* compile-defs directly from its Kconfig instead) --------
option(LXP_ENABLE_NET "Guest sockets (net-ops port required)" OFF)
option(LXP_ENABLE_NETFS "9P remote filesystem (needs NET)" OFF)
option(LXP_ENABLE_NETFS_EXEC "Execute programs off the 9P mount" OFF)
option(LXP_ENABLE_PTY "Pseudo-terminal support" OFF)
option(LXP_ENABLE_DEV "Device layer (/dev)" OFF)
option(LXP_ENABLE_DEV_FB "Framebuffer /dev/fb0 (display-ops port required)" OFF)
option(LXP_ENABLE_DEV_INPUT "Input event device (/dev/input)" OFF)
option(LXP_PORT_POSIX "Build the bundled POSIX reference port" OFF)

# ---- sources: collect whatever exists (the tree is populated incrementally) --
file(GLOB_RECURSE LXP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")

if(LXP_SOURCES)
  add_library(lxp STATIC ${LXP_SOURCES})
  target_include_directories(lxp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_include_directories(lxp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

  foreach(feat NET NETFS NETFS_EXEC PTY DEV DEV_FB DEV_INPUT)
    if(LXP_ENABLE_${feat})
      target_compile_definitions(lxp PUBLIC LXP_ENABLE_${feat}=1)
    endif()
  endforeach()

  if(LXP_PORT_POSIX)
    target_sources(lxp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ports/posix/lxp_port_posix.c")
  endif()
else()
  # Scaffold state: headers only. A header-only interface target so the module
  # is add_subdirectory-able before the sources land.
  add_library(lxp INTERFACE)
  target_include_directories(lxp INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
endif()

add_library(lxp::lxp ALIAS lxp)
