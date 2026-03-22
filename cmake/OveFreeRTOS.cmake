# ============================================================================
# OveFreeRTOS.cmake — FreeRTOS kernel integration
# ============================================================================
#
# Provides:
#   ove_add_freertos_kernel(port_path)
#     port_path: sub-path under portable/GCC/ (e.g. ARM_CM7/r0p1)
#
# Must be called after ove_setup_project().
# FREERTOS_PATH can be set by the board to override auto-detection.


# ─── ove_add_freertos_kernel(port_path) ──────────────────────────────
# Add FreeRTOS kernel source files and include directories.
macro(ove_add_freertos_kernel _port_path)
    # Auto-detect FreeRTOS location if not explicitly set
    if(NOT DEFINED FREERTOS_PATH)
        if(EXISTS "${OVE_DL_DIR}/FreeRTOS-Kernel/include")
            # Standalone FreeRTOS-Kernel (QEMU boards)
            set(FREERTOS_PATH "${OVE_DL_DIR}/FreeRTOS-Kernel")
        elseif(EXISTS "${OVE_DL_DIR}/STM32CubeF7/Middlewares/Third_Party/FreeRTOS/Source/include")
            # STM32CubeF7 bundled FreeRTOS
            set(FREERTOS_PATH
                "${OVE_DL_DIR}/STM32CubeF7/Middlewares/Third_Party/FreeRTOS/Source")
        else()
            message(FATAL_ERROR
                "FreeRTOS kernel not found in ${OVE_DL_DIR}. "
                "Set FREERTOS_PATH or run download first.")
        endif()
    endif()

    # Kernel core sources
    set(_OVE_FREERTOS_SOURCES
        ${FREERTOS_PATH}/tasks.c
        ${FREERTOS_PATH}/queue.c
        ${FREERTOS_PATH}/list.c
        ${FREERTOS_PATH}/timers.c
        ${FREERTOS_PATH}/event_groups.c
        ${FREERTOS_PATH}/stream_buffer.c
        ${FREERTOS_PATH}/portable/GCC/${_port_path}/port.c
    )
    if(NOT OVE_ZERO_HEAP)
        list(APPEND _OVE_FREERTOS_SOURCES
            ${FREERTOS_PATH}/portable/MemMang/heap_4.c)
    else()
        list(APPEND _OVE_FREERTOS_SOURCES
            ${OVE_DIR}/backends/freertos/freertos_heap_stubs.c)
    endif()

    # FreeRTOS include directories
    include_directories(
        ${FREERTOS_PATH}/include
        ${FREERTOS_PATH}/portable/GCC/${_port_path}
    )

    message(STATUS "  FreeRTOS: ${FREERTOS_PATH}")
    message(STATUS "  Port:     ${_port_path}")
endmacro()
