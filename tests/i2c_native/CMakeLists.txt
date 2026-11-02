# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
foreach(mode heap zero)
    set(name ove_test_i2c_front_${mode})
    add_library(${name}_front OBJECT ${OVE_SRC}/ove_i2c.c)
    target_compile_definitions(${name}_front PRIVATE ove_mutex_lock=i2c_test_lock)
    add_executable(${name} frontend.c ${POSIX_CORE_DIR}/posix_sync.c
        $<TARGET_OBJECTS:${name}_front>)
    foreach(target ${name} ${name}_front)
        target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${POSIX_CORE_DIR}/include ${OVE_INCLUDE} ${OVE_ROOT}/backends/common)
        if(mode STREQUAL "zero")
            target_compile_definitions(${target} PRIVATE CONFIG_OVE_ZERO_HEAP=1)
        endif()
        if(NOT CMAKE_C_FLAGS MATCHES "-fsanitize")
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined
                -fno-omit-frame-pointer -fno-sanitize-recover=all -g)
        endif()
    endforeach()
    target_include_directories(${name} PRIVATE ${cmocka_SOURCE_DIR}/include)
    target_link_libraries(${name} PRIVATE cmocka-static pthread)
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    if(NOT CMAKE_C_FLAGS MATCHES "-fsanitize")
        target_link_options(${name} PRIVATE -fsanitize=address,undefined
            -fno-sanitize-recover=all)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endforeach()

add_library(ove_test_i2c_nuttx_adapter OBJECT ${OVE_ROOT}/backends/nuttx/nuttx_i2c.c)
target_compile_definitions(ove_test_i2c_nuttx_adapter PRIVATE
    open=i2c_test_open close=i2c_test_close ioctl=i2c_test_ioctl
    pthread_create=i2c_test_pthread_create)
add_executable(ove_test_i2c_nuttx nuttx.c ${OVE_SRC}/ove_i2c.c
    ${POSIX_CORE_DIR}/posix_sync.c $<TARGET_OBJECTS:ove_test_i2c_nuttx_adapter>)
foreach(target ove_test_i2c_nuttx ove_test_i2c_nuttx_adapter)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/sdk ${POSIX_CORE_DIR}/include
        ${OVE_INCLUDE} ${OVE_ROOT}/backends/common)
    if(NOT CMAKE_C_FLAGS MATCHES "-fsanitize")
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined
            -fno-omit-frame-pointer -fno-sanitize-recover=all -g)
    endif()
endforeach()
target_include_directories(ove_test_i2c_nuttx PRIVATE ${cmocka_SOURCE_DIR}/include)
target_link_libraries(ove_test_i2c_nuttx PRIVATE cmocka-static pthread)
set_target_properties(ove_test_i2c_nuttx PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
if(NOT CMAKE_C_FLAGS MATCHES "-fsanitize")
    target_link_options(ove_test_i2c_nuttx PRIVATE -fsanitize=address,undefined
        -fno-sanitize-recover=all)
endif()
add_test(NAME ove_test_i2c_nuttx COMMAND ove_test_i2c_nuttx)
