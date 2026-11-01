# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

function(ove_add_mqtt_lifecycle name zero_heap tls rx_size tx_size)
    add_library(${name}_implementation OBJECT
        ${OVE_ROOT}/backends/common/ove_net_mqtt.c)
    add_executable(${name}
        lifecycle.c
        ${OVE_ROOT}/backends/common/ove_mqtt_topic.c
        $<TARGET_OBJECTS:${name}_implementation>)
    foreach(target ${name} ${name}_implementation)
        target_include_directories(${target} PRIVATE
            ${OVE_INCLUDE} ${OVE_ROOT}/backends/common
            ${CMAKE_CURRENT_SOURCE_DIR}/.. ${POSIX_CORE_DIR}/include)
        target_compile_definitions(${target} PRIVATE
            CONFIG_OVE_NET=1 CONFIG_OVE_NET_MQTT=1
            CONFIG_OVE_NET_MQTT_RX_BUF=${rx_size}
            CONFIG_OVE_NET_MQTT_TX_BUF=${tx_size})
        if(zero_heap)
            target_compile_definitions(${target} PRIVATE CONFIG_OVE_ZERO_HEAP=1)
        else()
            target_compile_options(${target} PRIVATE -UCONFIG_OVE_ZERO_HEAP)
        endif()
        if(tls)
            target_compile_definitions(${target} PRIVATE CONFIG_OVE_NET_TLS=1)
            target_include_directories(${target} SYSTEM PRIVATE
                ${OVE_TEST_MBEDTLS_DIR}/include)
        endif()
        if(NOT CMAKE_C_FLAGS MATCHES "-fsanitize")
            target_compile_options(${target} PRIVATE
                -fsanitize=address,undefined -fno-omit-frame-pointer
                -fno-sanitize-recover=all -g)
        endif()
    endforeach()
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    target_compile_definitions(${name}_implementation PRIVATE
        malloc=ove_test_mqtt_malloc free=ove_test_mqtt_free)
    target_include_directories(${name} PRIVATE ${cmocka_SOURCE_DIR}/include)
    target_link_libraries(${name} PRIVATE cmocka-static)
    if(NOT CMAKE_C_FLAGS MATCHES "-fsanitize")
        target_link_options(${name} PRIVATE
            -fsanitize=address,undefined -fno-sanitize-recover=all)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

ove_add_mqtt_lifecycle(ove_test_mqtt_heap FALSE TRUE 1024 512)
ove_add_mqtt_lifecycle(ove_test_mqtt_zh TRUE TRUE 1024 512)
ove_add_mqtt_lifecycle(ove_test_mqtt_no_tls FALSE FALSE 1024 512)
ove_add_mqtt_lifecycle(ove_test_mqtt_zh_no_tls TRUE FALSE 1024 512)

ove_add_mqtt_lifecycle(ove_test_mqtt_wide FALSE TRUE 1024 196640)
ove_add_mqtt_lifecycle(ove_test_mqtt_varint FALSE TRUE 1024 130)
ove_add_mqtt_lifecycle(ove_test_mqtt_small_rx FALSE TRUE 2 512)
ove_add_mqtt_lifecycle(ove_test_mqtt_zh_small_rx TRUE TRUE 2 512)
