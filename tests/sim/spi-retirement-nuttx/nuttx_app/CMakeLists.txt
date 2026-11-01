# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
if(NOT CONFIG_EXTERNAL_OVE_TEST)
    return()
endif()
set(_backend ${OVE_DIR}/backends/nuttx)
set(_sources ${CMAKE_CURRENT_SOURCE_DIR}/main.c ${OVE_DIR}/src/ove_spi.c
    ${_backend}/nuttx_spi.c ${_backend}/nuttx_sync.c)
set(_definitions "")
if(OVE_SPI_ZERO_HEAP)
    list(APPEND _definitions CONFIG_OVE_ZERO_HEAP=1)
    list(APPEND _sources ${OVE_DIR}/backends/common/ove_heap_lock.c
        ${_backend}/nuttx_heap_lock.c ${_backend}/nuttx_zh_reserved_heap.c)
endif()
set_source_files_properties(${_backend}/nuttx_spi.c PROPERTIES
    COMPILE_DEFINITIONS "open=spi_probe_open;ioctl=spi_probe_ioctl")
nuttx_add_application(NAME ${CONFIG_EXTERNAL_OVE_TEST_PROGNAME}
    SRCS ${_sources} STACKSIZE ${CONFIG_EXTERNAL_OVE_TEST_STACKSIZE}
    PRIORITY ${CONFIG_EXTERNAL_OVE_TEST_PRIORITY} DEFINITIONS ${_definitions}
    INCLUDE_DIRECTORIES ${_OVE_VARIANT_DIR} ${OVE_DIR}/include
    ${_backend}/include ${OVE_DIR}/backends/common ${OVE_DIR}/tests)
if(OVE_SPI_ZERO_HEAP AND TARGET nuttx)
    target_link_options(nuttx PRIVATE
        -Wl,--wrap=mm_malloc
        -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc
        -Wl,--wrap=zalloc -Wl,--wrap=memalign -Wl,--wrap=free
        -Wl,--wrap=kmm_malloc -Wl,--wrap=kmm_calloc -Wl,--wrap=kmm_realloc
        -Wl,--wrap=kmm_zalloc -Wl,--wrap=kmm_memalign -Wl,--wrap=kmm_free)
endif()
