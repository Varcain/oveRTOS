# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_arena_model_dir "${CMAKE_BINARY_DIR}/arena-models")
set(_arena_model_files
 "${_arena_model_dir}/g_audio_preprocessor_int8_model_data.c"
 "${_arena_model_dir}/g_audio_preprocessor_int8_model_data.h"
 "${_arena_model_dir}/g_micro_speech_quantized_model_data.c"
 "${_arena_model_dir}/g_micro_speech_quantized_model_data.h")
add_custom_command(OUTPUT ${_arena_model_files}
 COMMAND ${Python3_EXECUTABLE} "${OVE_DIR}/models/convert.py"
         --model-dir "${OVE_DIR}/models/keyword" --output-dir "${_arena_model_dir}"
 DEPENDS "${OVE_DIR}/models/convert.py"
         "${OVE_DIR}/models/keyword/audio_preprocessor_int8.tflite"
         "${OVE_DIR}/models/keyword/micro_speech_quantized.tflite"
 VERBATIM)
add_library(infer_arena_data STATIC ${_arena_model_files})
add_executable(infer_arena_allocator "${OVE_DIR}/tests/infer_arena/allocator.cc")
add_executable(infer_arena_models "${OVE_DIR}/tests/infer_arena/models.cc")
target_link_libraries(infer_arena_models PRIVATE infer_arena_data)
add_dependencies(infer_arena_models infer_arena_data)
foreach(_arena_target infer_arena_allocator infer_arena_models)
 target_compile_definitions(${_arena_target} PRIVATE TF_LITE_STATIC_MEMORY)
 target_compile_options(${_arena_target} PRIVATE -fno-rtti -fno-exceptions -fno-threadsafe-statics)
 target_include_directories(${_arena_target} BEFORE PRIVATE
     ${CMAKE_BINARY_DIR} ${OVE_DIR}/tests ${OVE_DIR}/tests/host/posix/include
     ${OVE_DIR}/backends/common "${_arena_model_dir}")
 target_link_libraries(${_arena_target} PRIVATE ove_tflm m pthread)
 target_link_options(${_arena_target} PRIVATE "-Wl,-Map,${CMAKE_BINARY_DIR}/${_arena_target}.map")
endforeach()
target_link_options(infer_arena_models PRIVATE -Wl,--wrap=malloc -Wl,--wrap=free)
unset(_arena_model_dir)
unset(_arena_model_files)
unset(_arena_target)
add_executable(infer_arena_frontiers "${OVE_DIR}/tests/infer_arena/frontiers.cc")
target_compile_definitions(infer_arena_frontiers PRIVATE TF_LITE_STATIC_MEMORY)
target_compile_options(infer_arena_frontiers PRIVATE -fno-rtti -fno-exceptions -fno-threadsafe-statics)
target_include_directories(infer_arena_frontiers BEFORE PRIVATE ${CMAKE_BINARY_DIR} ${OVE_DIR}/tests ${OVE_DIR}/tests/host/posix/include ${OVE_DIR}/backends/common)
target_link_libraries(infer_arena_frontiers PRIVATE ove_tflm m pthread)
target_link_options(infer_arena_frontiers PRIVATE "-Wl,-Map,${CMAKE_BINARY_DIR}/infer_arena_frontiers.map")

option(OVE_ARENA_MUTATIONS "Build bounded inference regression variants" OFF)
if(OVE_ARENA_MUTATIONS)
 set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
     "${OVE_DIR}/tests/infer_arena/mutations.py"
     "${OVE_DIR}/backends/common/ove_infer.cc"
     "${OVE_DIR}/backends/common/tflm/ove_tflm_arena.cc")
 execute_process(COMMAND ${Python3_EXECUTABLE}
     "${OVE_DIR}/tests/infer_arena/mutations.py" "${OVE_DIR}"
     "${CMAKE_BINARY_DIR}/arena-mutations" COMMAND_ERROR_IS_FATAL ANY)
 include("${CMAKE_BINARY_DIR}/arena-mutations/Targets.cmake")
endif()
add_executable(infer_arena_cpp "${OVE_DIR}/tests/infer_arena/cpp_query.cpp")
set_target_properties(infer_arena_cpp PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_compile_definitions(infer_arena_cpp PRIVATE TF_LITE_STATIC_MEMORY)
target_compile_options(infer_arena_cpp PRIVATE -fno-rtti -fno-exceptions -fno-threadsafe-statics)
target_include_directories(infer_arena_cpp BEFORE PRIVATE ${CMAKE_BINARY_DIR} ${OVE_DIR}/bindings/cpp ${OVE_DIR}/tests ${OVE_DIR}/tests/host/posix/include)
target_link_libraries(infer_arena_cpp PRIVATE ove_tflm m pthread)
target_link_options(infer_arena_cpp PRIVATE "-Wl,-Map,${CMAKE_BINARY_DIR}/infer_arena_cpp.map")
