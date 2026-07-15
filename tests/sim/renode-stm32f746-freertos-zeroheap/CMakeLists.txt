# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.
#
# Renode STM32F746 test target — zero-heap mode.
#
# Mirrors tests/sim/freertos-qemu-zeroheap/CMakeLists.txt but links
# against the real STM32F7 HAL (from dl/STM32CubeF7-*) + the FreeRTOS
# ARM_CM7/r0p1 port + the STM32F746NGHx linker script.  Renode's
# stm32f7_discovery-bb platform implements the CPU, NVIC, SysTick,
# USARTs, timers, DMA, GPIO, RCC, SPI, I2C, LTDC; peripherals that
# Renode does not model (IWDG, SAI) are either avoided or graceful-fail.
#
# The toolchain file is the QEMU one (same cortex-m7 + semihosting).
# We can't use the board's own shim because that disables semihosting
# for hardware deployment; Renode needs it on.

cmake_minimum_required(VERSION 3.14)

# OVE_HW: when ON (passed on the cmake command line), the firmware is
# being built for real STM32F746G-Discovery silicon instead of Renode.
# That switches the toolchain shim from the QEMU/Renode one (rdimon.specs
# → semihosting libc) to the board's own (nosys.specs → no host libc),
# and later in the file picks usart1_io.c over semihosting_io.c.
# The toolchain file is read before project(), which is why we resolve
# OVE_HW here rather than via option() further down.
if(OVE_HW)
    set(CMAKE_TOOLCHAIN_FILE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../boards/stm32f746g-discovery/freertos/cmake/arm-none-eabi.cmake
        CACHE FILEPATH "ARM toolchain file")
else()
    set(CMAKE_TOOLCHAIN_FILE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../boards/qemu-mps2-an500/cmake/arm-none-eabi.cmake
        CACHE FILEPATH "ARM toolchain file")
endif()

project(ove_test_renode_stm32f746_freertos_zeroheap C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function
                    -ffunction-sections -fdata-sections)

include(FetchContent)

# --- Fetch FreeRTOS-Kernel (same version as board CMakeLists pulls via manifest) ---
FetchContent_Declare(
    freertos_kernel
    GIT_REPOSITORY https://github.com/FreeRTOS/FreeRTOS-Kernel.git
    GIT_TAG        V11.2.0
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(freertos_kernel)
if(NOT freertos_kernel_POPULATED)
    FetchContent_Populate(freertos_kernel)
endif()

# --- Fetch CMocka (bare-metal-safe compile, mirrors QEMU sim) ---
FetchContent_Declare(
    cmocka
    GIT_REPOSITORY https://gitlab.com/cmocka/cmocka.git
    GIT_TAG        cmocka-1.1.7
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(cmocka)
if(NOT cmocka_POPULATED)
    FetchContent_Populate(cmocka)
endif()
add_library(cmocka-static STATIC ${cmocka_SOURCE_DIR}/src/cmocka.c)
target_include_directories(cmocka-static PUBLIC ${cmocka_SOURCE_DIR}/include)
target_include_directories(cmocka-static PRIVATE ${cmocka_SOURCE_DIR}/src)
target_compile_definitions(cmocka-static PRIVATE
    HAVE_STRINGS_H HAVE_INTTYPES_H HAVE_MALLOC_H
    HAVE_SIGNAL_H
    HAVE_SETJMP_H HAVE_STDARG_H HAVE_STDDEF_H HAVE_STDINT_H
    HAVE_STDIO_H HAVE_STDLIB_H HAVE_STRING_H
    HAVE_CALLOC HAVE_EXIT HAVE_FREE HAVE_LONGJMP HAVE_MALLOC
    HAVE_MEMCPY HAVE_MEMSET HAVE_SETJMP HAVE_STRCMP HAVE_SPRINTF
    HAVE_VSNPRINTF HAVE_FPRINTF HAVE_PRINTF
)
target_compile_options(cmocka-static PRIVATE
    -Wno-unused-parameter -Wno-implicit-function-declaration -Wno-int-conversion)

# --- Paths ---
set(OVE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(OVE_INCLUDE ${OVE_ROOT}/include)
set(OVE_SRC ${OVE_ROOT}/src)
set(FREERTOS_BACKEND ${OVE_ROOT}/backends/freertos)
set(TESTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(STUB_DIR ${TESTS_ROOT}/backends/stub)
set(SUITE_DIR ${TESTS_ROOT}/suites)
include(${TESTS_ROOT}/cmake/OveTest.cmake)
set(BOARD_DIR ${OVE_ROOT}/boards/stm32f746g-discovery)
set(FREERTOS_SRC ${freertos_kernel_SOURCE_DIR})
set(FREERTOS_PORT ${FREERTOS_SRC}/portable/GCC/ARM_CM7/r0p1)

# STM32CubeF7 path (downloaded to dl/ by main ove setup; glob to pick up
# the current snapshot hash).
file(GLOB _CUBE_F7_CANDIDATES "${OVE_ROOT}/dl/STM32CubeF7-*")
if(NOT _CUBE_F7_CANDIDATES)
    message(FATAL_ERROR
        "STM32CubeF7 not found under ${OVE_ROOT}/dl/. "
        "Run 'make download' (top-level) or any 'make stm32f746g-discovery.freertos.<app>' target first.")
endif()
list(GET _CUBE_F7_CANDIDATES 0 STM32CUBE_PATH)
message(STATUS "STM32CubeF7: ${STM32CUBE_PATH}")

set(STM32_HAL_DIR  ${STM32CUBE_PATH}/Drivers/STM32F7xx_HAL_Driver)
set(STM32_CMSIS_DIR ${STM32CUBE_PATH}/Drivers/CMSIS/Device/ST/STM32F7xx)
set(CMSIS_CORE_DIR ${STM32CUBE_PATH}/Drivers/CMSIS/Include)

# --- CMSIS startup + system.  rdimon.specs (from the toolchain shim)
#     provides _write/_read/_lseek/_close etc. pointing at semihosting,
#     so we deliberately omit the board's syscalls.c which targets real
#     hardware USART output via serial_write. ---
set(STARTUP_SOURCES
    ${STM32_CMSIS_DIR}/Source/Templates/gcc/startup_stm32f746xx.s
    ${STM32_CMSIS_DIR}/Source/Templates/system_stm32f7xx.c
)

# --- HAL driver sources (all non-template; Renode accepts register writes
#      to unmodeled peripherals with warnings, not crashes). ---
file(GLOB HAL_SOURCES CONFIGURE_DEPENDS "${STM32_HAL_DIR}/Src/*.c")
list(FILTER HAL_SOURCES EXCLUDE REGEX ".*_template\\.c$")

# --- FreeRTOS kernel sources ---
set(FREERTOS_KERNEL_SOURCES
    ${FREERTOS_SRC}/tasks.c
    ${FREERTOS_SRC}/queue.c
    ${FREERTOS_SRC}/list.c
    ${FREERTOS_SRC}/timers.c
    ${FREERTOS_SRC}/event_groups.c
    ${FREERTOS_SRC}/stream_buffer.c
    ${FREERTOS_BACKEND}/freertos_heap_stubs.c
    ${FREERTOS_PORT}/port.c
)

# --- Pure FreeRTOS backend modules.
#     Includes freertos_watchdog.c because the STM32F7-gated
#     `struct ove_watchdog` embeds IWDG_HandleTypeDef — using the stub
#     would mismatch the layout ove_storage_freertos.h declares.  Renode
#     doesn't model IWDG, so any actual register access emits a warning
#     rather than a fatal error; tests that need IWDG behaviour skip via
#     OVE_RENODE_STM32F746 gating (e.g. test_storage_bounds tolerates
#     OVE_ERR_NOT_SUPPORTED from ove_watchdog_init already). ---
set(FREERTOS_BACKEND_SOURCES
    ${FREERTOS_BACKEND}/freertos_thread.c
    # Heap-lock wrap chain (zero-heap only).  Common impl in
    # backends/common/ove_heap_lock.c; freertos extras here.
    ${OVE_ROOT}/backends/common/ove_heap_lock.c
    ${FREERTOS_BACKEND}/freertos_heap_lock.c
    ${FREERTOS_BACKEND}/freertos_sync.c
    ${FREERTOS_BACKEND}/freertos_queue.c
    ${FREERTOS_BACKEND}/freertos_timer.c
    ${FREERTOS_BACKEND}/freertos_eventgroup.c
    ${FREERTOS_BACKEND}/freertos_stream.c
    ${FREERTOS_BACKEND}/freertos_workqueue.c
    ${FREERTOS_BACKEND}/freertos_watchdog.c
    # Async runtime substrate (CONFIG_OVE_ASYNC).
    ${FREERTOS_BACKEND}/freertos_irq.c
    # Real HAL-backed drivers — exercised against Renode's modelled
    # STM32 peripherals instead of tests/backends/stub/* surrogates.
    ${FREERTOS_BACKEND}/freertos_gpio.c
    # freertos_shell.c is pure logic (no HAL dependency) — swapping it
    # in exercises the real ove_shell_* API implementation.
    ${FREERTOS_BACKEND}/freertos_shell.c
    # Networking — full lwIP stack on top of stm32f7_eth.c (added in
    # STM32F7_DRIVER_SOURCES below).  Renode's SynopsysEthernetMAC +
    # EthernetPhysicalLayer back the wire; the test only verifies
    # bring-up + IP assignment, no external traffic required.
    ${FREERTOS_BACKEND}/freertos_net.c
    ${FREERTOS_BACKEND}/lwip_sys_arch.c
    # FS / NVS deferred — see tests/suites/test_renode_stm32_periph.c
    # comment block.  Renode 1.16's STM32FSDMMC model doesn't accept the
    # full register-write set STM32CubeF7's BSP_SD emits during HAL_SD_Init
    # (notably DTEN/DBLOCKSIZE writes flag as "Unhandled bits"), so
    # f_open against an SdCardFromFile-attached image hangs in the
    # data-transfer state.  Stay on stub_nvs.c until either Renode's
    # SDMMC model matures or we ship a slim diskio that bypasses BSP_SD.
    # freertos_time.c is NOT linked in: its ove_time_delay_us busy-waits
    # on DWT->CYCCNT, which Renode 1.16's Cortex-M model doesn't advance.
    # stub_time.c provides functionally-equivalent ove_time_get_us /
    # _get_ns (FreeRTOS tick count — same rate as HAL_GetTick) and a
    # scheduler-friendly delay_us built on vTaskDelay, so API-level
    # coverage is preserved.
    #
    # Not swapped:
    # - freertos_board.c    depends on bsp.h + lv_port_disp.h (board BSP + LVGL)
    # - freertos_nvs.c      depends on ove_fs.h (FatFS + SDMMC driver)
    # - freertos_lvgl.c     depends on lvgl.h + LCD port driver
    # All three pull in infrastructure outside this target's scope;
    # revisit if/when a board-agnostic BSP / FS / LVGL layer appears.
)

# --- Core modules ---
set(CORE_SOURCES
    ${OVE_SRC}/ove_board.c
    ${OVE_SRC}/ove_gpio.c
    ${OVE_SRC}/ove_led.c
    ${OVE_SRC}/ove_app.c
    # Hardware peripheral dispatchers — paired with the STM32F7 drivers
    # below to drive Renode's modelled peripherals.
    ${OVE_SRC}/ove_i2c.c
    ${OVE_SRC}/ove_spi.c
    ${OVE_SRC}/ove_uart.c
)

# STM32F7 peripheral drivers — implement the ove_hal_<periph>_*
# contracts on top of HAL_<PERIPH>_*.  Renode 1.16's STM32F7_I2C /
# STM32SPI / STM32F7_USART models accept HAL transactions, so these
# can run unmodified.
set(STM32F7_DRIVER_SOURCES
    ${OVE_ROOT}/drivers/freertos/stm32f7/stm32f7_i2c.c
    ${OVE_ROOT}/drivers/freertos/stm32f7/stm32f7_spi.c
    ${OVE_ROOT}/drivers/freertos/stm32f7/stm32f7_uart.c
    # Ethernet — implements lwIP's ethernetif_init / ethernetif_input on
    # top of HAL_ETH.  Tested against Renode's SynopsysEthernetMAC.
    ${OVE_ROOT}/drivers/freertos/stm32f7/stm32f7_eth.c
)


# --- Stubs for subsystems that don't run under Renode on this target.
#      Notably: stub_watchdog is used because Renode does NOT model IWDG.
#      Using the real freertos_watchdog.c against an unmodeled peripheral
#      would turn every HAL_IWDG_Init into a warning stream.  Canary
#      coverage for the watchdog layout still comes from test_storage_bounds
#      (that test exercises memory layout, not IWDG register access). ---
set(STUB_SOURCES
    ${OVE_ROOT}/backends/common/ove_audio_graph.c
    ${OVE_ROOT}/backends/common/ove_audio_nodes.c
    ${STUB_DIR}/stub_bsp.c
    ${STUB_DIR}/stub_lvgl.c
    # stub_gpio removed — freertos_gpio.c + HAL_GPIO_* now drive the
    # STM32_GPIOPort peripherals Renode models.
    ${STUB_DIR}/stub_board.c
    ${STUB_DIR}/stub_time.c
    ${STUB_DIR}/stub_console.c
    # stub_shell removed — freertos_shell.c backs ove_shell_* directly.
    ${STUB_DIR}/stub_nvs.c
)

# --- Test suites ---
ove_test_common_suite_sources(SUITE_SOURCES ${SUITE_DIR})

# --- lwIP static library ---
# Keep this inline (rather than via cmake/OveLwIP.cmake) — that helper
# expects production-board variables (OVE_GEN_DIR, BOARD_DIR/src,
# _OVE_FREERTOS_INCLUDE_DIRS) that don't fit the test-sim layout.
file(GLOB _CUBE_LWIP_CANDIDATES "${OVE_ROOT}/dl/lwip-*")
if(NOT _CUBE_LWIP_CANDIDATES)
    message(FATAL_ERROR
        "lwIP not found under ${OVE_ROOT}/dl/. Run 'make download'.")
endif()
list(GET _CUBE_LWIP_CANDIDATES 0 LWIP_PATH)
message(STATUS "lwIP: ${LWIP_PATH}")
file(GLOB LWIP_CORE_SRC CONFIGURE_DEPENDS
    "${LWIP_PATH}/src/core/*.c"
    "${LWIP_PATH}/src/core/ipv4/*.c"
    "${LWIP_PATH}/src/api/*.c"
    "${LWIP_PATH}/src/netif/ethernet.c"
)
add_library(ove_lwip STATIC
    ${LWIP_CORE_SRC}
    ${FREERTOS_BACKEND}/lwip_sys_arch.c
)
target_include_directories(ove_lwip PUBLIC
    ${LWIP_PATH}/src/include
    ${FREERTOS_BACKEND}/lwip_port
    # lwipopts.h — reuse the production board's, no need to duplicate
    # for the test target.
    ${BOARD_DIR}/freertos/src
)
target_include_directories(ove_lwip PRIVATE
    ${FREERTOS_SRC}/include
    ${FREERTOS_PORT}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OVE_INCLUDE}
)
target_compile_options(ove_lwip PRIVATE -w)
set_target_properties(ove_lwip PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)

# Drop lwip_sys_arch.c from FREERTOS_BACKEND_SOURCES — it's already in
# the ove_lwip library above.  Build it in only one place.
list(REMOVE_ITEM FREERTOS_BACKEND_SOURCES ${FREERTOS_BACKEND}/lwip_sys_arch.c)

# --- stdio backend selection ---
# OVE_HW (set above project() because it gates the toolchain choice)
# also picks which stdio shim gets linked into the firmware:
#   ON  → usart1_io.c   (HAL_UART_Transmit on PA9/PB7 USART1)
#   OFF → semihosting_io.c (Renode SemihostingUart via SYS_WRITEC)
if(OVE_HW)
    set(STDIO_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/usart1_io.c)
else()
    set(STDIO_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/semihosting_io.c)
endif()

# --- Executable ---
add_executable(ove_test_renode_stm32f746_freertos_zeroheap
    ${CMAKE_CURRENT_SOURCE_DIR}/sim_main.c
    # OVE_HW gates which stdio backend gets linked.  See the option block
    # above; semihosting_io.c works around Renode 1.16's missing
    # SYS_WRITE, usart1_io.c writes to the real PA9/PB7 USART1 pins.
    ${STDIO_SOURCE}
    ${STARTUP_SOURCES}
    ${HAL_SOURCES}
    ${FREERTOS_KERNEL_SOURCES}
    ${FREERTOS_BACKEND_SOURCES}
    ${CORE_SOURCES}
    ${STM32F7_DRIVER_SOURCES}
    ${STUB_SOURCES}
    ${SUITE_SOURCES}
)

target_compile_definitions(ove_test_renode_stm32f746_freertos_zeroheap PRIVATE
    OVE_RENODE_STM32F746
    # OVE_HW propagates the CMake option to C code so sim_main.c can
    # skip the post-summary semihosting_exit() trap (which on real
    # silicon would raise a HardFault and panic).
    $<$<BOOL:${OVE_HW}>:OVE_HW>
    # OVE_QEMU_ARM unlocks the bare-metal ARM branch of stub_time.c
    # (FreeRTOS tick-count-based time) and matches how the QEMU sim
    # composes the same stubs.  The name is legacy; the branch is really
    # "bare-metal ARM with FreeRTOS and semihosting", which also fits us.
    OVE_QEMU_ARM
    STM32F746xx
    USE_HAL_DRIVER
    ARM_MATH_CM7
    # The shared stm32f7xx_hal_conf.h disables SPI / TIM by default;
    # the production board enables only what its app uses.  Our test
    # firmware uses both (SPI driver for ove_spi tests, TIM for
    # potential ISR timing tests), so unlock them at the consumer level
    # without touching the shared conf.
    HAL_SPI_MODULE_ENABLED
    HAL_TIM_MODULE_ENABLED
    HAL_ETH_MODULE_ENABLED
)

target_include_directories(ove_test_renode_stm32f746_freertos_zeroheap PRIVATE
    # FreeRTOSConfig.h + ove_config.h live here
    ${CMAKE_CURRENT_SOURCE_DIR}
    # FreeRTOS kernel + port headers
    ${FREERTOS_SRC}/include
    ${FREERTOS_PORT}
    # oveRTOS public API
    ${OVE_INCLUDE}
    # FreeRTOS backend storage headers (ove_storage_freertos.h — holds
    # the STM32F7-gated `struct ove_watchdog`)
    ${FREERTOS_BACKEND}/include
    # backends/common (ove_backend_common.h)
    ${OVE_ROOT}/backends/common
    # Test framework
    ${TESTS_ROOT}
    # STM32 HAL + CMSIS
    ${STM32_HAL_DIR}/Inc
    ${STM32_CMSIS_DIR}/Include
    ${CMSIS_CORE_DIR}
    # stm32f7xx_hal_conf.h (picks which HAL modules are compiled in)
    ${OVE_ROOT}/drivers/freertos/stm32f7
    # STM32F7 syscalls needs main.h / serial_wrapper.h from the board
    ${BOARD_DIR}/freertos/inc
    # lwIP headers — needed by freertos_net.c + stm32f7_eth.c
    ${LWIP_PATH}/src/include
    ${FREERTOS_BACKEND}/lwip_port
    # Production board's lwipopts.h
    ${BOARD_DIR}/freertos/src
)

target_link_libraries(ove_test_renode_stm32f746_freertos_zeroheap PRIVATE
    cmocka-static
    ove_lwip
    m
)

# Linker script — reuse the board's STM32F746 flash layout.  Override
# __heap_size to give picolibc _sbrk an arena: zero-heap mode has no
# pvPortMalloc, so cmocka's malloc() lands in the picolibc nano-malloc
# path that's backed by _sbrk over `.heap`.  64 KB matches the working
# pre-cleanup configuration (was [_end, _estack - 8KB) ≈ 47–64 KB
# depending on BSS); production zero-heap apps don't run cmocka and
# are expected to avoid libc malloc entirely (audited by
# backends/common/ove_heap_lock.c).
set(LINKER_SCRIPT ${BOARD_DIR}/freertos/STM32F746NGHx_FLASH.ld)
target_link_options(ove_test_renode_stm32f746_freertos_zeroheap PRIVATE
    -T${LINKER_SCRIPT}
    -Wl,--defsym=__heap_size=0x10000
    # Shrink the boot stack back to 512 B — the linker script PROVIDEs 4 KB
    # to fit Rust/Zig app_main, but CMocka test binaries don't run app code
    # on MSP, so 512 B is sufficient and recovers 3.5 KB of RAM for the
    # picolibc heap arena and per-task stacks.
    -Wl,--defsym=__stack_size=0x200
    -Wl,--gc-sections
    # rdimon spec from the QEMU toolchain shim routes stdio to semihosting.
)

# Give the output a .elf suffix so Renode's LoadELF recognises it as an
# ELF by extension (not strictly required but matches QEMU convention).
set_target_properties(ove_test_renode_stm32f746_freertos_zeroheap PROPERTIES
    SUFFIX ".elf"
)

# FreeRTOS + HAL generate warnings; suppress for third-party sources.
set_source_files_properties(${FREERTOS_KERNEL_SOURCES} PROPERTIES
    COMPILE_FLAGS "-Wno-missing-field-initializers -Wno-cast-function-type -Wno-sign-compare"
)
set_source_files_properties(${HAL_SOURCES} PROPERTIES
    COMPILE_FLAGS "-Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type -Wno-sign-compare -Wno-unused-variable -Wno-unused-but-set-variable -Wno-type-limits -Wno-stringop-overflow"
)
set_source_files_properties(${STM32_CMSIS_DIR}/Source/Templates/system_stm32f7xx.c PROPERTIES
    COMPILE_FLAGS "-Wno-unused-parameter"
)

# Zero-heap link-time wrap + audit.  --wrap routes libc malloc through
# backends/common/ove_heap_lock.c's __wrap_* trampolines so the trap
# test exercises the real wrap chain (not just direct calls).  STRICT
# adds __HeapBase/__HeapLimit (linker script heap region) to the
# forbidden list.  Picolibc-nano-malloc — unlike newlib — doesn't
# instantiate a malloc-table data symbol that would survive the audit
# (was __malloc_av_ in newlib; picolibc's nano-malloc only defines
# __malloc_free_list which dead-strips when malloc is unreachable).
include(${OVE_ROOT}/cmake/OveZeroHeapAudit.cmake)
ove_apply_zero_heap_wrap(ove_test_renode_stm32f746_freertos_zeroheap)
ove_zero_heap_assert_no_kernel_alloc(ove_test_renode_stm32f746_freertos_zeroheap
    RTOS FREERTOS STRICT)

# Dispatch-overhead symbol audit + NO_PANIC_SYMBOLS on the zeroheap
# test ELF. Symbol audit only on test scaffolding (see the heap variant
# for hotpath-disasm rationale).
include(${OVE_ROOT}/cmake/OveZeroOverheadAudit.cmake)
ove_assert_no_dispatch_overhead(ove_test_renode_stm32f746_freertos_zeroheap
    BINDING c NO_PANIC_SYMBOLS)
