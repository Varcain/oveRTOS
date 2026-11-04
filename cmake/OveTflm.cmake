# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

# ove_build_tflm() — compile TensorFlow Lite Micro with oveRTOS platform port.
# TFLM is compiled unmodified at the manifest revision. See
# docs-site/docs/internals/inference-runtime.md for the trust and arena contracts.
#
# Expects:
#   OVE_DL_DIR       — download directory containing tflite-micro/
#   OVE_DIR          — oveRTOS root directory
#   OVE_GEN_DIR      — generated config header directory (ove_config.h)
#   OVE_INCLUDE_DIR  — oveRTOS public include directory
#
# Optional:
#   CONFIG_OVE_INFER_CMSIS_NN — when defined, use CMSIS-NN optimized kernels
#   OVE_CMSIS_NN_DIR          — path to CMSIS-NN sources

macro(ove_build_tflm)
    set(_TFLM_PATH "${OVE_DL_DIR}/tflite-micro")

    if(NOT EXISTS "${_TFLM_PATH}")
        message(FATAL_ERROR
            "TFLM sources not found at ${_TFLM_PATH}. "
            "Run 'ove download' first.")
    endif()

    # ── TFLM core sources ────────────────────────────────────────────
    # Use GLOB_RECURSE to capture all subdirectories (arena_allocator,
    # memory_planner, tflite_bridge, etc.)
    # Include all TFLM sources including core/api (needed for Parse* functions).
    # The tflite_bridge/ directory wraps ErrorReporter for micro context.
    file(GLOB_RECURSE _TFLM_ALL_SRC CONFIGURE_DEPENDS
        "${_TFLM_PATH}/tensorflow/lite/micro/*.cc"
        "${_TFLM_PATH}/tensorflow/lite/micro/*.c"
        "${_TFLM_PATH}/tensorflow/lite/core/api/*.cc"
        "${_TFLM_PATH}/tensorflow/lite/core/c/*.cc"
        "${_TFLM_PATH}/tensorflow/lite/core/c/*.c"
        "${_TFLM_PATH}/tensorflow/lite/kernels/*.cc"
        "${_TFLM_PATH}/tensorflow/lite/kernels/internal/*.cc"
        "${_TFLM_PATH}/tensorflow/lite/schema/*.cc"
        "${_TFLM_PATH}/tensorflow/compiler/mlir/lite/core/api/*.cc"
        "${_TFLM_PATH}/tensorflow/compiler/mlir/lite/schema/*.cc"
    )

    # Exclude test files, examples, benchmarks, tools, and CMSIS-NN (handled below)
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*_test\\.cc$")
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*test_helpers\\.cc$")
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*testing/.*")
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*/test_data_generation/.*")
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*/examples/.*")
    # Exclude upstream micro_time.cc — replaced by ove_tflm_time.cc
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX "micro_time\\.cc$")
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*/benchmarks/.*")
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*/tools/.*")
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*/integration_tests/.*")
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*/python/.*")
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*/codegen/.*")
    # Exclude platform/accelerator-specific kernel subdirectories
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*/kernels/cmsis_nn/.*")
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*/kernels/arc_mli/.*")
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*/kernels/ceva/.*")
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*/kernels/xtensa/.*")
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*/kernels/hexagon/.*")
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*/kernels/ethos_u/.*")
    # Exclude all platform-specific top-level dirs under micro/
    # (keep only: arena_allocator, compression, kernels, memory_planner, tflite_bridge)
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*/micro/arc_custom/.*")
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*/micro/arc_emsdp/.*")
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*/micro/bluepill/.*")
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*/micro/ceva/.*")
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*/micro/chre/.*")
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*/micro/cortex_m_corstone_300/.*")
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*/micro/cortex_m_generic/.*")
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*/micro/hexagon/.*")
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*/micro/riscv32_generic/.*")
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*/micro/models/.*")
    # Exclude TFLM's default debug_log.cc (we provide our own in the platform port)
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*/micro/debug_log\\.cc$")

    # GCC 15+ fails on std::max/min with mixed float/double types in
    # upstream TFLM LSTM kernels.  Exclude them — only needed for LSTM
    # models, not typical TinyML (classification, keyword spotting).
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*/lstm_eval_common\\.cc$")
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*/lstm_eval\\.cc$")
    list(FILTER _TFLM_ALL_SRC EXCLUDE REGEX ".*/unidirectional_sequence_lstm\\.cc$")

    # ── CMSIS-NN optimized kernels (ARM targets only) ────────────────
    if(DEFINED CONFIG_OVE_INFER_CMSIS_NN)
        file(GLOB _CMSIS_NN_KERNELS CONFIGURE_DEPENDS
            "${_TFLM_PATH}/tensorflow/lite/micro/kernels/cmsis_nn/*.cc"
        )
        if(_CMSIS_NN_KERNELS)
            # Replace reference kernels with CMSIS-NN variants where available
            foreach(_cmsis_src ${_CMSIS_NN_KERNELS})
                get_filename_component(_name ${_cmsis_src} NAME)
                set(_ref_src "${_TFLM_PATH}/tensorflow/lite/micro/kernels/${_name}")
                list(REMOVE_ITEM _TFLM_ALL_SRC "${_ref_src}")
            endforeach()
            list(APPEND _TFLM_ALL_SRC ${_CMSIS_NN_KERNELS})
        endif()
    endif()

    # ── Signal processing sources (referenced by micro_mutable_op_resolver) ──
    file(GLOB_RECURSE _TFLM_SIGNAL_SRC CONFIGURE_DEPENDS
        "${_TFLM_PATH}/signal/src/*.cc"
        "${_TFLM_PATH}/signal/src/*.c"
        "${_TFLM_PATH}/signal/micro/kernels/*.cc"
    )
    list(FILTER _TFLM_SIGNAL_SRC EXCLUDE REGEX ".*_test\\.cc$")
    list(FILTER _TFLM_SIGNAL_SRC EXCLUDE REGEX ".*/hexagon/.*")
    list(FILTER _TFLM_SIGNAL_SRC EXCLUDE REGEX ".*/xtensa/.*")
    list(FILTER _TFLM_SIGNAL_SRC EXCLUDE REGEX ".*/ceva/.*")
    list(APPEND _TFLM_ALL_SRC ${_TFLM_SIGNAL_SRC})

    # Own code has project diagnostics; upstream code keeps its warning policy.
    set(_TFLM_OWN_SRC
        ${OVE_DIR}/backends/common/tflm/ove_tflm_debug_log.cc
        ${OVE_DIR}/backends/common/tflm/ove_tflm_time.cc
        ${OVE_DIR}/backends/common/tflm/ove_tflm_arena.cc
        ${OVE_DIR}/backends/common/ove_infer.cc
    )
    add_library(ove_tflm STATIC ${_TFLM_ALL_SRC})
    add_library(ove_tflm_port OBJECT ${_TFLM_OWN_SRC})
    target_sources(ove_tflm PRIVATE $<TARGET_OBJECTS:ove_tflm_port>)

    # TFLM downloads third-party deps via its own make system into
    # tensorflow/lite/micro/tools/make/downloads/.  Use those paths.
    set(_TFLM_DOWNLOADS "${_TFLM_PATH}/tensorflow/lite/micro/tools/make/downloads")

    set(_TFLM_SDK_INCLUDES
        ${_TFLM_PATH}
        ${_TFLM_DOWNLOADS}/flatbuffers/include
        ${_TFLM_DOWNLOADS}/gemmlowp
        ${_TFLM_DOWNLOADS}/ruy
        ${_TFLM_DOWNLOADS}/kissfft
        ${_TFLM_PATH}/signal/src
        ${_TFLM_PATH}/signal/micro/kernels
    )
    foreach(_tflm_target ove_tflm ove_tflm_port)
        target_include_directories(${_tflm_target} PUBLIC
            ${_TFLM_SDK_INCLUDES}
            ${OVE_INCLUDE_DIR}
            ${OVE_GEN_DIR}
            ${OVE_BACKENDS_COMMON_DIR}
        )
        if(DEFINED CONFIG_OVE_INFER_CMSIS_NN AND DEFINED OVE_CMSIS_NN_DIR)
            target_include_directories(${_tflm_target} PRIVATE ${OVE_CMSIS_NN_DIR})
            if(DEFINED OVE_CMSIS5_DIR)
                target_include_directories(${_tflm_target} PRIVATE
                    ${OVE_CMSIS5_DIR}/Core/Include
                    ${OVE_CMSIS5_DIR}/DSP/Include
                )
            elseif(DEFINED OVE_CMSIS_NN_DIR)
                get_filename_component(_cmsis_parent "${OVE_CMSIS_NN_DIR}/.." ABSOLUTE)
                target_include_directories(${_tflm_target} PRIVATE
                    ${_cmsis_parent}/Core/Include
                    ${_cmsis_parent}/DSP/Include
                )
            endif()
            target_compile_definitions(${_tflm_target} PRIVATE CMSIS_NN)
        endif()
        set_target_properties(${_tflm_target} PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON
        )
        target_compile_options(${_tflm_target} PRIVATE
            -fno-exceptions
            -fno-rtti
            -fno-threadsafe-statics
        )
        target_compile_definitions(${_tflm_target} PRIVATE TF_LITE_STATIC_MEMORY)
    endforeach()
    target_compile_options(ove_tflm PRIVATE -w)
    target_include_directories(ove_tflm_port SYSTEM PRIVATE ${_TFLM_SDK_INCLUDES})
    target_compile_options(ove_tflm_port PRIVATE -Wall -Werror)
    if(COMMAND ove_werror_own_sources)
        ove_werror_own_sources(ove_tflm_port "${_TFLM_OWN_SRC}")
    endif()

    # Clean up local variables
    unset(_TFLM_PATH)
    unset(_TFLM_ALL_SRC)
    unset(_CMSIS_NN_KERNELS)
    unset(_TFLM_OWN_SRC)
    unset(_TFLM_SDK_INCLUDES)
    unset(_tflm_target)
endmacro()
