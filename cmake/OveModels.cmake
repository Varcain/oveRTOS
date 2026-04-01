# OveModels.cmake — Generate C arrays from .tflite model files.
#
# Call ove_generate_models(TARGET) after add_executable() when
# CONFIG_OVE_INFER is enabled.  Generates .c/.h files from all
# .tflite files found under ${OVE_DIR}/models/ and compiles them
# as an OBJECT library linked into the firmware.

function(ove_generate_models TARGET)
    set(MODEL_DIR "${OVE_DIR}/models")
    set(GEN_DIR "${CMAKE_BINARY_DIR}/generated_models")

    # Skip if no models directory exists
    if(NOT EXISTS "${MODEL_DIR}")
        return()
    endif()

    # Find all .tflite files
    file(GLOB_RECURSE TFLITE_FILES "${MODEL_DIR}/*.tflite")
    if(NOT TFLITE_FILES)
        return()
    endif()

    # Run converter at configure time
    find_package(Python3 QUIET COMPONENTS Interpreter)
    set(_PYTHON "python3")
    if(Python3_FOUND)
        set(_PYTHON "${Python3_EXECUTABLE}")
    endif()

    execute_process(
        COMMAND ${_PYTHON} "${MODEL_DIR}/convert.py"
                --model-dir "${MODEL_DIR}"
                --output-dir "${GEN_DIR}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
    )
    if(_rc)
        message(WARNING "[ove] Model conversion failed (rc=${_rc})")
        return()
    endif()
    message(STATUS "[ove] ${_out}")

    # Compile generated .c files
    file(GLOB MODEL_SOURCES "${GEN_DIR}/*.c")
    if(NOT MODEL_SOURCES)
        return()
    endif()

    add_library(ove_models OBJECT ${MODEL_SOURCES})
    target_include_directories(ove_models PUBLIC "${GEN_DIR}")

    # Inherit cross-compilation settings from main target
    if(OVE_RTOS STREQUAL "zephyr" AND TARGET zephyr_interface)
        target_link_libraries(ove_models PRIVATE zephyr_interface)
    endif()

    # Link into firmware
    if(OVE_RTOS STREQUAL "zephyr" AND COMMAND zephyr_link_libraries)
        zephyr_link_libraries(ove_models)
    else()
        target_link_libraries(${TARGET} PRIVATE ove_models)
    endif()

    # Add generated headers to the main target's include path
    target_include_directories(${TARGET} PRIVATE "${GEN_DIR}")
endfunction()
