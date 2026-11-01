# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.14)
project(ove_string_tests C)
get_filename_component(OVE_DIR "../.." ABSOLUTE)
set(OVE_DL_DIR "${OVE_DIR}/dl")
option(OVE_ZERO_HEAP "Use static native storage and TLS arena" OFF)
include(${OVE_DIR}/cmake/OveMbedtls.cmake)
ove_build_mbedtls()
file(READ "${OVE_DIR}/tests/ove_config.h" _config)
string(REGEX REPLACE "#define CONFIG_OVE_LINUX[^\n]*\n" "" _config "${_config}")
string(REPLACE "#define CONFIG_OVE_LVGL 1" "" _config "${_config}")
set(_extra "#define CONFIG_OVE_NET 1\n#define CONFIG_OVE_NET_TLS 1\n#define CONFIG_OVE_NET_SNTP 1\n#define CONFIG_OVE_NET_HTTPD_MAX_BODY 1024\n#define CONFIG_OVE_NET_HTTPD 1\n#define CONFIG_OVE_NET_HTTPD_WS 1\n#define CONFIG_OVE_NET_TLS_HEAP_SIZE 262144\n")
if(OVE_ZERO_HEAP)
    string(APPEND _extra "#define CONFIG_OVE_ZERO_HEAP 1\n")
endif()
string(REPLACE "#endif /* OVE_CONFIG_H */" "${_extra}#endif /* OVE_CONFIG_H */" _config "${_config}")
file(WRITE "${CMAKE_BINARY_DIR}/ove_config.h" "${_config}")
set(OVE_STUB_CONFIG_DIR "${CMAKE_BINARY_DIR}" CACHE PATH "" FORCE)
set(OVE_STUB_NVS_SOURCE "${OVE_DIR}/backends/common/ove_nvs_fs.c" CACHE FILEPATH "" FORCE)
add_subdirectory(${OVE_DIR}/tests/rust/stub_cmake stub)
get_target_property(_stub_sources ove_stub SOURCES)
list(FILTER _stub_sources EXCLUDE REGEX "(stub_lvgl|ove_lvgl_text)\\.c$")
set_property(TARGET ove_stub PROPERTY SOURCES "${_stub_sources}")
set_target_properties(ove_stub PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_sources(ove_stub PRIVATE
    fixture.c
    ${OVE_DIR}/tests/host/posix/posix_net.c
    ${OVE_DIR}/src/ove_net.c
    ${OVE_DIR}/backends/common/ove_net_tls.c
    ${OVE_DIR}/backends/common/ove_net_httpd.c
    ${OVE_DIR}/backends/common/ove_net_httpd_ws.c
    ${OVE_DIR}/backends/common/ove_net_httpd_routes.c
    ${OVE_DIR}/backends/common/ove_sha1.c
    ${OVE_DIR}/backends/common/ove_base64.c)
target_compile_definitions(ove_stub PRIVATE OVE_NVS_FS_DIR="/tmp/ove-nvs-rust-tests"
    OVE_TEST_TLS_PKI_DIR="${OVE_DIR}/tests/net_tls/pki")
target_link_libraries(ove_stub PRIVATE ove_mbedtls)
target_link_libraries(_ove_sizes PRIVATE ove_mbedtls)
