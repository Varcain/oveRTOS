# CMake toolchain file for ARM Cortex-M7 (STM32F7)
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake ..

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# Toolchain path resolution (priority order):
# 1. OVE_TOOLCHAIN_DIR from build system (downloaded or custom)
# 2. ARM_TOOLCHAIN env var (set in .env)
# 3. Fall back to PATH
if(OVE_TOOLCHAIN_DIR AND EXISTS "${OVE_TOOLCHAIN_DIR}/bin/arm-none-eabi-gcc")
    set(TOOLCHAIN_PREFIX "${OVE_TOOLCHAIN_DIR}/bin/arm-none-eabi-")
elseif(DEFINED ENV{ARM_TOOLCHAIN})
    set(TOOLCHAIN_PREFIX "$ENV{ARM_TOOLCHAIN}/bin/arm-none-eabi-")
else()
    set(TOOLCHAIN_PREFIX "arm-none-eabi-")
endif()

set(CMAKE_C_COMPILER "${TOOLCHAIN_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_PREFIX}g++")
set(CMAKE_ASM_COMPILER "${TOOLCHAIN_PREFIX}gcc")
set(CMAKE_AR "${TOOLCHAIN_PREFIX}ar")
set(CMAKE_OBJCOPY "${TOOLCHAIN_PREFIX}objcopy")
set(CMAKE_OBJDUMP "${TOOLCHAIN_PREFIX}objdump")
set(CMAKE_SIZE "${TOOLCHAIN_PREFIX}size")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# CPU-specific flags for STM32F746 (Cortex-M7 with FPU)
set(CPU_FLAGS "-march=armv7e-m -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16")

set(CMAKE_C_FLAGS_INIT "${CPU_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${CPU_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${CPU_FLAGS}")

set(CMAKE_EXE_LINKER_FLAGS_INIT "${CPU_FLAGS} -specs=nosys.specs")

# Search paths
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
