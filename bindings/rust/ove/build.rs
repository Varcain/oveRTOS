// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use std::env;
use std::path::PathBuf;

fn main() {
    // Doc-only builds: skip bindgen, emit all feature flags
    if std::env::var("DOCS_RS").is_ok() {
        let modules = [
            "audio", "fs", "lvgl", "nvs", "shell", "watchdog",
            "bsp", "board", "gpio", "led", "time", "console",
            "stream", "workqueue",
        ];
        for m in &modules {
            println!("cargo:rustc-check-cfg=cfg(has_{})", m);
            println!("cargo:rustc-cfg=has_{}", m);
        }
        println!("cargo:rustc-check-cfg=cfg(zero_heap)");
        for r in &["freertos", "zephyr", "nuttx", "posix"] {
            println!("cargo:rustc-check-cfg=cfg(rtos_{})", r);
        }
        return;
    }

    let ove_dir =
        env::var("OVE_DIR").expect("OVE_DIR env var must be set by build system");
    let ove_gen_dir =
        env::var("OVE_GEN_DIR").expect("OVE_GEN_DIR env var must be set by build system");

    // Detect enabled subsystems from ove_config.h and emit cfg flags
    {
        let config_path = format!("{}/ove_config.h", ove_gen_dir);
        let config = std::fs::read_to_string(&config_path).unwrap_or_default();
        let modules = [
            "AUDIO", "FS", "LVGL", "NVS", "SHELL", "WATCHDOG", "BSP", "BOARD",
            "GPIO", "LED", "TIME", "CONSOLE", "STREAM", "WORKQUEUE",
        ];
        for m in &modules {
            let cfg_name = format!("has_{}", m.to_lowercase());
            println!("cargo:rustc-check-cfg=cfg({})", cfg_name);
            let define = format!("#define CONFIG_OVE_{} 1", m);
            if config.contains(&define) {
                println!("cargo:rustc-cfg={}", cfg_name);
            }
        }

        /* Detect zero-heap mode */
        println!("cargo:rustc-check-cfg=cfg(zero_heap)");
        if config.contains("#define CONFIG_OVE_ZERO_HEAP 1") {
            println!("cargo:rustc-cfg=zero_heap");
        }

        let rtos_backends = ["FREERTOS", "ZEPHYR", "NUTTX", "POSIX"];
        for r in &rtos_backends {
            let cfg_name = format!("rtos_{}", r.to_lowercase());
            println!("cargo:rustc-check-cfg=cfg({})", cfg_name);
            let define = format!("#define CONFIG_OVE_RTOS_{} 1", r);
            if config.contains(&define) {
                println!("cargo:rustc-cfg={}", cfg_name);
            }
        }
        println!("cargo:rerun-if-changed={}", config_path);
    }
    let lvgl_include =
        env::var("LVGL_INCLUDE_PATH").expect("LVGL_INCLUDE_PATH env var must be set by build system");
    let lvgl_parent =
        env::var("LVGL_PARENT_PATH").expect("LVGL_PARENT_PATH env var must be set by build system");

    let is_native = env::var("RUST_IS_NATIVE").unwrap_or_default() == "1";

    // Detect enabled features from ove_config.h
    let config_path = format!("{}/ove_config.h", ove_gen_dir);
    let config_text = std::fs::read_to_string(&config_path).unwrap_or_default();
    let has_lvgl = config_text.contains("#define CONFIG_OVE_LVGL 1");
    let has_cmsis_dsp = env::var("CMSIS_DSP_INCLUDE").is_ok();

    // Build the wrapper header — conditionally include LVGL and CMSIS-DSP
    let mut wrapper = String::from("#include \"ove/ove.h\"\n");
    if has_lvgl {
        wrapper.push_str("#include \"lvgl/lvgl.h\"\n");
    }
    if has_cmsis_dsp {
        wrapper.push_str("#include \"arm_math.h\"\n");
        wrapper.push_str("#include \"arm_const_structs.h\"\n");
    }

    // Determine backend storage include path from ove_config.h
    let backend_include = {
        let config_path = format!("{}/ove_config.h", ove_gen_dir);
        let config = std::fs::read_to_string(&config_path).unwrap_or_default();
        let backends = [
            ("FREERTOS", "freertos"),
            ("ZEPHYR", "zephyr"),
            ("NUTTX", "nuttx"),
            ("POSIX", "posix"),
        ];
        backends.iter().find_map(|(define, dir)| {
            let needle = format!("#define CONFIG_OVE_RTOS_{} 1", define);
            if config.contains(&needle) {
                Some(format!("{}/backends/{}/include", ove_dir, dir))
            } else {
                None
            }
        })
    };

    // Generate storage_sizes.h from build-time measurements.
    // ove_rust.cmake must produce OVE_STORAGE_SIZES pointing to
    // a KEY=VALUE file with sizeof/alignof for each storage type.
    {
        let sizes_path = env::var("OVE_STORAGE_SIZES")
            .expect("OVE_STORAGE_SIZES env var not set — \
                     ove_rust.cmake must generate storage sizes before cargo runs");
        let content = std::fs::read_to_string(&sizes_path)
            .unwrap_or_else(|e| panic!("Failed to read {}: {}", sizes_path, e));

        let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
        let header_path = out_path.join("storage_sizes.h");
        let mut header = String::from(
            "/* Auto-generated by build.rs — do not edit. */\n\
             #include <stdint.h>\n\n"
        );
        let mut count = 0usize;
        for line in content.lines() {
            if let Some((key, val)) = line.split_once('=') {
                header.push_str(&format!(
                    "#define OVE_{} {}\n", key.trim(), val.trim()
                ));
                count += 1;
            }
        }
        assert!(count > 0, "storage sizes file is empty: {}", sizes_path);
        std::fs::write(&header_path, &header).expect("Failed to write storage_sizes.h");

        wrapper = format!(
            "#include \"{}\"\n{}",
            header_path.to_str().unwrap(),
            wrapper
        );
        println!("cargo:rerun-if-changed={}", sizes_path);
    }

    let mut builder = bindgen::Builder::default()
        .header_contents("wrapper.h", &wrapper)
        .clang_arg("-D__BINDGEN__")
        .clang_args(&[
            format!("-I{}/include", ove_dir),
            format!("-I{}", ove_gen_dir),
            format!("-I{}", lvgl_include),
            format!("-I{}", lvgl_parent),
        ])
        // Generate enum constants without type prefix (LV_EVENT_CLICKED, not
        // lv_event_code_t_LV_EVENT_CLICKED) — matches stub anonymous-enum output.
        .prepend_enum_name(false);

    // Add backend storage header include path (for non-bindgen compilation)
    if let Some(ref inc) = backend_include {
        builder = builder.clang_arg(format!("-I{}", inc));
    }

    let mut builder = builder
        // oveRTOS symbols
        .allowlist_function("ove_.*")
        .allowlist_type("ove_.*")
        .allowlist_var("OVE_.*");

    // LVGL symbols (only when LVGL is enabled)
    if has_lvgl {
    builder = builder
        // LVGL — object core
        .allowlist_function("lv_obj_create")
        .allowlist_function("lv_obj_delete")
        .allowlist_function("lv_obj_clean")
        .allowlist_function("lv_obj_get_parent")
        .allowlist_function("lv_obj_get_child_count")
        .allowlist_function("lv_obj_get_width")
        .allowlist_function("lv_obj_get_height")
        .allowlist_function("lv_obj_set_size")
        .allowlist_function("lv_obj_set_width")
        .allowlist_function("lv_obj_set_height")
        .allowlist_function("lv_obj_set_pos")
        .allowlist_function("lv_obj_center")
        .allowlist_function("lv_obj_align")
        .allowlist_function("lv_obj_add_flag")
        .allowlist_function("lv_obj_remove_flag")
        .allowlist_function("lv_obj_add_state")
        .allowlist_function("lv_obj_remove_state")
        .allowlist_function("lv_obj_set_user_data")
        .allowlist_function("lv_obj_get_user_data")
        .allowlist_function("lv_obj_set_flex_flow")
        // LVGL — events
        .allowlist_function("lv_obj_add_event_cb")
        .allowlist_function("lv_event_get_user_data")
        .allowlist_function("lv_event_get_target")
        // LVGL — inline styles (individual pad setters; pad_all/hor/ver/gap
        // are static inline in LVGL 9, so we bind the primitives and compose
        // the compound helpers in Rust)
        .allowlist_function("lv_obj_set_style_bg_color")
        .allowlist_function("lv_obj_set_style_bg_opa")
        .allowlist_function("lv_obj_set_style_border_color")
        .allowlist_function("lv_obj_set_style_border_width")
        .allowlist_function("lv_obj_set_style_radius")
        .allowlist_function("lv_obj_set_style_pad_top")
        .allowlist_function("lv_obj_set_style_pad_bottom")
        .allowlist_function("lv_obj_set_style_pad_left")
        .allowlist_function("lv_obj_set_style_pad_right")
        .allowlist_function("lv_obj_set_style_pad_row")
        .allowlist_function("lv_obj_set_style_pad_column")
        .allowlist_function("lv_obj_set_style_text_color")
        .allowlist_function("lv_obj_set_style_text_font")
        // LVGL — style objects
        .allowlist_function("lv_style_init")
        .allowlist_function("lv_style_reset")
        .allowlist_function("lv_style_set_bg_color")
        .allowlist_function("lv_style_set_bg_opa")
        .allowlist_function("lv_style_set_radius")
        .allowlist_function("lv_style_set_border_color")
        .allowlist_function("lv_style_set_border_width")
        .allowlist_function("lv_style_set_pad_top")
        .allowlist_function("lv_style_set_pad_bottom")
        .allowlist_function("lv_style_set_pad_left")
        .allowlist_function("lv_style_set_pad_right")
        .allowlist_function("lv_style_set_text_color")
        .allowlist_function("lv_style_set_text_font")
        .allowlist_function("lv_obj_add_style")
        .allowlist_function("lv_obj_remove_style_all")
        // LVGL — color helpers
        .allowlist_function("lv_palette_main")
        .allowlist_function("lv_color_make")
        .allowlist_function("lv_color_white")
        .allowlist_function("lv_color_black")
        .allowlist_function("lv_color_hex")
        // LVGL — widgets
        .allowlist_function("lv_label_create")
        .allowlist_function("lv_label_set_text")
        .allowlist_function("lv_label_set_text_static")
        .allowlist_function("lv_bar_create")
        .allowlist_function("lv_bar_set_value")
        .allowlist_function("lv_bar_set_range")
        .allowlist_function("lv_screen_active")
        // LVGL — types & constants
        .allowlist_type("lv_obj_t")
        .allowlist_type("lv_color_t")
        .allowlist_type("lv_font_t")
        .allowlist_type("lv_style_t")
        .allowlist_type("lv_event_cb_t")
        .allowlist_type("lv_event_code_t")
        .allowlist_type("lv_obj_flag_t")
        .allowlist_type("lv_state_t")
        .allowlist_type("lv_flex_flow_t")
        .allowlist_type("lv_opa_t")
        .allowlist_var("LV_ALIGN_.*")
        .allowlist_var("LV_EVENT_.*")
        .allowlist_var("LV_OBJ_FLAG_.*")
        .allowlist_var("LV_FLEX_FLOW_.*")
        .allowlist_var("LV_PART_.*")
        .allowlist_var("LV_PALETTE_.*")
        .allowlist_var("LV_SIZE_CONTENT")
        .allowlist_var("lv_font_.*");
    }

    // CMSIS-DSP symbols (only when CMSIS_DSP_INCLUDE is set)
    if has_cmsis_dsp {
        builder = builder
            .allowlist_function("arm_cfft_q31")
            .allowlist_function("arm_cmplx_mult_cmplx_q31")
            .allowlist_function("arm_add_q31")
            .allowlist_type("arm_cfft_instance_q31")
            .allowlist_var("arm_cfft_sR_q31_len.*");

        if let Ok(dsp_inc) = env::var("CMSIS_DSP_INCLUDE") {
            builder = builder.clang_arg(format!("-I{}", dsp_inc));
        }
        if let Ok(core_inc) = env::var("CMSIS_CORE_INCLUDE") {
            builder = builder.clang_arg(format!("-I{}", core_inc));
        }
    }

    // Board-specific lv_conf.h directory for bindgen
    if let Ok(lv_conf_path) = env::var("LV_CONF_PATH") {
        builder = builder.clang_arg(format!("-I{}", lv_conf_path));
    }

    if is_native {
        // Native/POSIX build: use std types
    } else {
        // Cross-compilation: freestanding ARM target
        builder = builder
            .clang_arg("-DARM_MATH_CM7")
            .clang_arg("-D__FPU_PRESENT=1")
            .clang_arg("--target=arm-none-eabihf");

        // Add ARM toolchain sysroot include path if available
        if let Ok(sysroot) = env::var("ARM_SYSROOT_INCLUDE") {
            builder = builder.clang_arg(format!("-isystem{}", sysroot));
        }
    }

    if is_native {
        // Native build: use std-compatible types
    } else {
        builder = builder.use_core().ctypes_prefix("core::ffi");
    }

    let bindings = builder.generate().expect("Failed to generate oveRTOS bindings");

    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("ove_bindings.rs"))
        .expect("Failed to write bindings");

    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=OVE_DIR");
    println!("cargo:rerun-if-env-changed=OVE_GEN_DIR");
    println!("cargo:rerun-if-env-changed=ARM_SYSROOT_INCLUDE");
    println!("cargo:rerun-if-env-changed=RUST_IS_NATIVE");
    println!("cargo:rerun-if-env-changed=LV_CONF_PATH");
    println!("cargo:rerun-if-env-changed=LVGL_INCLUDE_PATH");
    println!("cargo:rerun-if-env-changed=LVGL_PARENT_PATH");
    println!("cargo:rerun-if-env-changed=CMSIS_DSP_INCLUDE");
    println!("cargo:rerun-if-env-changed=CMSIS_CORE_INCLUDE");
}
