# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

# ove_build_tflm() — compile TensorFlow Lite Micro with oveRTOS platform port.
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
    file(GLOB_RECURSE _TFLM_ALL_SRC
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
        file(GLOB _CMSIS_NN_KERNELS
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
    file(GLOB_RECURSE _TFLM_SIGNAL_SRC
        "${_TFLM_PATH}/signal/src/*.cc"
        "${_TFLM_PATH}/signal/src/*.c"
        "${_TFLM_PATH}/signal/micro/kernels/*.cc"
    )
    list(FILTER _TFLM_SIGNAL_SRC EXCLUDE REGEX ".*_test\\.cc$")
    list(FILTER _TFLM_SIGNAL_SRC EXCLUDE REGEX ".*/hexagon/.*")
    list(FILTER _TFLM_SIGNAL_SRC EXCLUDE REGEX ".*/xtensa/.*")
    list(FILTER _TFLM_SIGNAL_SRC EXCLUDE REGEX ".*/ceva/.*")
    list(APPEND _TFLM_ALL_SRC ${_TFLM_SIGNAL_SRC})

    # ── oveRTOS platform port + inference wrapper ────────────────────
    list(APPEND _TFLM_ALL_SRC
        ${OVE_DIR}/backends/common/tflm/ove_tflm_debug_log.cc
        ${OVE_DIR}/backends/common/tflm/ove_tflm_time.cc
        ${OVE_DIR}/backends/common/ove_infer.cc
    )

    # ── Build as static library ──────────────────────────────────────
    add_library(ove_tflm STATIC ${_TFLM_ALL_SRC})

    # TFLM downloads third-party deps via its own make system into
    # tensorflow/lite/micro/tools/make/downloads/.  Use those paths.
    set(_TFLM_DOWNLOADS "${_TFLM_PATH}/tensorflow/lite/micro/tools/make/downloads")

    target_include_directories(ove_tflm PUBLIC
        ${_TFLM_PATH}
        ${_TFLM_DOWNLOADS}/flatbuffers/include
        ${_TFLM_DOWNLOADS}/gemmlowp
        ${_TFLM_DOWNLOADS}/ruy
        ${_TFLM_DOWNLOADS}/kissfft
        ${_TFLM_PATH}/signal/src
        ${_TFLM_PATH}/signal/micro/kernels
        ${OVE_INCLUDE_DIR}
        ${OVE_GEN_DIR}
        ${OVE_BACKENDS_COMMON_DIR}
    )

    if(DEFINED CONFIG_OVE_INFER_CMSIS_NN AND DEFINED OVE_CMSIS_NN_DIR)
        # TFLM uses #include "Include/arm_nnfunctions.h" (relative to NN dir)
        target_include_directories(ove_tflm PRIVATE
            ${OVE_CMSIS_NN_DIR}
        )
        # CMSIS-NN depends on CMSIS-Core and CMSIS-DSP headers.
        # OVE_CMSIS5_DIR should point to the CMSIS/ subtree (containing Core/, DSP/, NN/).
        if(DEFINED OVE_CMSIS5_DIR)
            target_include_directories(ove_tflm PRIVATE
                ${OVE_CMSIS5_DIR}/Core/Include
                ${OVE_CMSIS5_DIR}/DSP/Include
            )
        elseif(DEFINED OVE_CMSIS_NN_DIR)
            # Derive: NN is at CMSIS/NN, so ../.. = root, ../Core = sibling
            get_filename_component(_cmsis_parent "${OVE_CMSIS_NN_DIR}/.." ABSOLUTE)
            target_include_directories(ove_tflm PRIVATE
                ${_cmsis_parent}/Core/Include
                ${_cmsis_parent}/DSP/Include
            )
        endif()
        target_compile_definitions(ove_tflm PRIVATE CMSIS_NN)
    endif()

    set_target_properties(ove_tflm PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )

    target_compile_options(ove_tflm PRIVATE
        -w                  # Suppress TFLM warnings (upstream code)
        -fno-exceptions
        -fno-rtti
        -DTF_LITE_STATIC_MEMORY
    )


    # Clean up local variables
    unset(_TFLM_PATH)
    unset(_TFLM_MICRO_SRC)
    unset(_TFLM_CORE_SRC)
    unset(_TFLM_KERNELS_SRC)
    unset(_TFLM_MICRO_KERNELS_SRC)
    unset(_TFLM_ALL_SRC)
    unset(_CMSIS_NN_KERNELS)
endmacro()
