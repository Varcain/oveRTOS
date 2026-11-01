# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/sdk)
foreach(header semphr task queue timers event_groups stream_buffer)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/sdk/${header}.h
        "#include \"FreeRTOS.h\"\n")
endforeach()

foreach(platform stm32 nuttx)
    if(platform STREQUAL "stm32")
        set(adapter ${OVE_ROOT}/drivers/freertos/stm32f7/stm32f7_spi.c)
        set(storage ${OVE_ROOT}/backends/freertos/include)
        set(definitions CONFIG_OVE_RTOS_FREERTOS=1 STM32F746xx=1 OVE_TEST_SPI_STM32=1)
    else()
        set(adapter ${OVE_ROOT}/backends/nuttx/nuttx_spi.c)
        set(storage ${POSIX_CORE_DIR}/include)
        set(definitions CONFIG_OVE_RTOS_POSIX=1)
    endif()
    set(name ove_test_spi_${platform})
    add_library(${name}_adapter OBJECT ${adapter})
    target_compile_definitions(${name}_adapter PRIVATE
        ove_hal_spi_transfer=ove_test_spi_native_transfer)
    add_executable(${name} frames.c ${OVE_SRC}/ove_spi.c
        $<TARGET_OBJECTS:${name}_adapter>)
    foreach(target ${name} ${name}_adapter)
        target_include_directories(${target} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/sdk
            ${CMAKE_CURRENT_BINARY_DIR}/sdk ${storage} ${OVE_INCLUDE}
            ${OVE_ROOT}/backends/common)
        target_compile_definitions(${target} PRIVATE ${definitions})
        if(NOT CMAKE_C_FLAGS MATCHES "-fsanitize")
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined
                -fno-omit-frame-pointer -fno-sanitize-recover=all -g)
        endif()
    endforeach()
    if(platform STREQUAL "nuttx")
        target_compile_definitions(${name}_adapter PRIVATE
            ioctl=ove_test_spi_ioctl pthread_create=ove_test_spi_pthread_create)
    endif()
    target_include_directories(${name} PRIVATE ${cmocka_SOURCE_DIR}/include)
    target_link_libraries(${name} PRIVATE cmocka-static pthread)
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    if(NOT CMAKE_C_FLAGS MATCHES "-fsanitize")
        target_link_options(${name} PRIVATE -fsanitize=address,undefined
            -fno-sanitize-recover=all)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endforeach()
