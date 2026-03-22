# CMake script to parse and display memory usage with percentages
# Usage: cmake -DELF_FILE=<path> -DSIZE_TOOL=<tool> -DFLASH_SIZE=<bytes> -DRAM_SIZE=<bytes> -P print_size.cmake

# Run size tool and capture output
execute_process(
    COMMAND ${SIZE_TOOL} --format=berkeley ${ELF_FILE}
    OUTPUT_VARIABLE SIZE_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Parse the output (berkeley format: "text data bss dec hex filename")
# Example output:
#    text    data     bss     dec     hex filename
#  12345    678     901   14024    36c8 main.elf
# Extract all numbers from the output (should be on the data line)
string(REGEX MATCHALL "[0-9]+" NUMBERS ${SIZE_OUTPUT})
list(LENGTH NUMBERS NUM_COUNT)
if(NUM_COUNT GREATER_EQUAL 3)
    # The first three numbers are: text, data, bss
    list(GET NUMBERS 0 TEXT_SIZE)
    list(GET NUMBERS 1 DATA_SIZE)
    list(GET NUMBERS 2 BSS_SIZE)
else()
    message(FATAL_ERROR "Failed to parse size output. Expected at least 3 numbers, got ${NUM_COUNT}.\nOutput: ${SIZE_OUTPUT}")
endif()

# Calculate totals
math(EXPR FLASH_USED "${TEXT_SIZE} + ${DATA_SIZE}")
math(EXPR RAM_USED "${DATA_SIZE} + ${BSS_SIZE}")

# Calculate percentages
math(EXPR FLASH_PERCENT "${FLASH_USED} * 100 / ${FLASH_SIZE}")
math(EXPR RAM_PERCENT "${RAM_USED} * 100 / ${RAM_SIZE}")

# Format sizes for display
if(FLASH_USED GREATER 1023)
    math(EXPR FLASH_USED_KB "${FLASH_USED} / 1024")
    set(FLASH_USED_STR "${FLASH_USED_KB}KB (${FLASH_USED} bytes)")
else()
    set(FLASH_USED_STR "${FLASH_USED} bytes")
endif()

if(FLASH_SIZE GREATER 1023)
    math(EXPR FLASH_SIZE_KB "${FLASH_SIZE} / 1024")
    set(FLASH_SIZE_STR "${FLASH_SIZE_KB}KB (${FLASH_SIZE} bytes)")
else()
    set(FLASH_SIZE_STR "${FLASH_SIZE} bytes")
endif()

if(RAM_USED GREATER 1023)
    math(EXPR RAM_USED_KB "${RAM_USED} / 1024")
    set(RAM_USED_STR "${RAM_USED_KB}KB (${RAM_USED} bytes)")
else()
    set(RAM_USED_STR "${RAM_USED} bytes")
endif()

if(RAM_SIZE GREATER 1023)
    math(EXPR RAM_SIZE_KB "${RAM_SIZE} / 1024")
    set(RAM_SIZE_STR "${RAM_SIZE_KB}KB (${RAM_SIZE} bytes)")
else()
    set(RAM_SIZE_STR "${RAM_SIZE} bytes")
endif()

# Display formatted output
message(STATUS "")
message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
message(STATUS "Memory Usage Summary")
message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
message(STATUS "FLASH:  ${FLASH_USED_STR} / ${FLASH_SIZE_STR}  (${FLASH_PERCENT}%)")
message(STATUS "RAM:    ${RAM_USED_STR} / ${RAM_SIZE_STR}  (${RAM_PERCENT}%)")
message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
message(STATUS "")
message(STATUS "Breakdown:")
message(STATUS "  Text (code):     ${TEXT_SIZE} bytes")
message(STATUS "  Data (init):    ${DATA_SIZE} bytes")
message(STATUS "  BSS (uninit):   ${BSS_SIZE} bytes")
message(STATUS "")
