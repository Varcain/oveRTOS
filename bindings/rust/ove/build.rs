// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use std::env;
use std::fmt::Write as _;
use std::path::PathBuf;

#[allow(clippy::too_many_lines, clippy::large_stack_frames)]
fn main() {
    // Doc-only builds: skip bindgen, emit all feature flags so every
    // module (including `lvgl`) is included in the generated docs.
    // Also activates the `cfg(docsrs)` switch in lib.rs that swaps the
    // bindgen-generated module for the checked-in bindings_stub.rs,
    // so the crate compiles without LVGL headers or a storage probe.
    if std::env::var("DOCS_RS").is_ok() {
        println!("cargo:rustc-cfg=docsrs");
        println!("cargo:rustc-check-cfg=cfg(ove_test)");
        let modules = [
            "sync",
            "queue",
            "timer",
            "eventgroup",
            "log",
            "audio",
            "fs",
            "lvgl",
            "nvs",
            "shell",
            "watchdog",
            "bsp",
            "board",
            "gpio",
            "led",
            "time",
            "console",
            "stream",
            "workqueue",
            "infer",
            "net",
            "net_tls",
            "net_http",
            "net_mqtt",
            "net_httpd",
            "net_sntp",
            "net_httpd_ws",
            "pm",
            "uart",
            "spi",
            "i2c",
            "i2s",
            "async",
            "async_net",
            "trace",
        ];
        for m in &modules {
            println!("cargo:rustc-check-cfg=cfg(has_{m})");
            println!("cargo:rustc-cfg=has_{m}");
        }
        println!("cargo:rustc-check-cfg=cfg(zero_heap)");
        println!("cargo:rustc-check-cfg=cfg(docsrs)");
        for r in &["freertos", "zephyr", "nuttx", "posix"] {
            println!("cargo:rustc-check-cfg=cfg(rtos_{r})");
        }
        // board_<name> cfgs (for board-specific gates); doc-only
        // build registers them as known cfgs without setting any.
        for b in &["qemu_mps2", "stm32f746g_disco"] {
            println!("cargo:rustc-check-cfg=cfg(board_{b})");
        }
        // config_ove_<name> cfgs (G3 generic Kconfig surface); doc-only
        // build also generates an empty config_consts.rs so src/config.rs
        // include! resolves.
        println!("cargo:rustc-check-cfg=cfg(config_ove_async)");
        let out_dir = env::var("OUT_DIR").unwrap_or_default();
        if !out_dir.is_empty() {
            let path = format!("{out_dir}/config_consts.rs");
            let _ = std::fs::write(
                &path,
                "// docs.rs stub — real consts come from build.rs's non-docsrs branch.\n",
            );
            println!("cargo:rustc-env=OVE_CONFIG_CONSTS={path}");
        }
        return;
    }

    let ove_dir = env::var("OVE_DIR").expect("OVE_DIR env var must be set by build system");
    let ove_gen_dir =
        env::var("OVE_GEN_DIR").expect("OVE_GEN_DIR env var must be set by build system");

    println!("cargo:rustc-check-cfg=cfg(docsrs)");
    println!("cargo:rustc-check-cfg=cfg(ove_test)");
    println!("cargo:rustc-check-cfg=cfg(has_trace)");

    // Detect enabled subsystems from ove_config.h and emit cfg flags
    {
        let config_path = format!("{ove_gen_dir}/ove_config.h");
        let config = std::fs::read_to_string(&config_path).unwrap_or_default();
        // With markers off, bindgen omits the C header's static inline stubs.
        if config
            .lines()
            .any(|line| line == "#define CONFIG_OVE_TRACE_MARKERS 1")
        {
            println!("cargo:rustc-cfg=has_trace");
        }
        let modules = [
            "SYNC",
            "QUEUE",
            "TIMER",
            "EVENTGROUP",
            "LOG",
            "AUDIO",
            "FS",
            "LVGL",
            "NVS",
            "SHELL",
            "WATCHDOG",
            "BSP",
            "BOARD",
            "GPIO",
            "LED",
            "TIME",
            "CONSOLE",
            "STREAM",
            "WORKQUEUE",
            "INFER",
            "NET",
            "NET_TLS",
            "NET_HTTP",
            "NET_MQTT",
            "NET_HTTPD",
            "NET_SNTP",
            "NET_HTTPD_WS",
            "UART",
            "SPI",
            "I2C",
            "I2S",
            "PM",
            "ASYNC",
            "ASYNC_NET",
        ];
        for m in &modules {
            let cfg_name = format!("has_{}", m.to_lowercase());
            println!("cargo:rustc-check-cfg=cfg({cfg_name})");
            let define = format!("#define CONFIG_OVE_{m} 1");
            if config.contains(&define) {
                println!("cargo:rustc-cfg={cfg_name}");
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
            println!("cargo:rustc-check-cfg=cfg({cfg_name})");
            let define = format!("#define CONFIG_OVE_RTOS_{r} 1");
            if config.contains(&define) {
                println!("cargo:rustc-cfg={cfg_name}");
            }
        }

        // Board cfgs — mirror `apps/rust/ove_build_common.rs` so the
        // `ove` crate itself can gate on `board_<name>`.
        let boards = [
            ("QEMU_MPS2_AN500", "qemu_mps2"),
            ("STM32F746G_DISCO", "stm32f746g_disco"),
        ];
        for (cfg_token, rust_cfg) in &boards {
            let cfg_name = format!("board_{rust_cfg}");
            println!("cargo:rustc-check-cfg=cfg({cfg_name})");
            let define = format!("#define CONFIG_OVE_BOARD_{cfg_token} 1");
            if config.contains(&define) {
                println!("cargo:rustc-cfg={cfg_name}");
            }
        }

        // Generic CONFIG_OVE_* surface (G3): emit per-symbol cfg flags
        // for booleans and a Rust `const`-bearing config_consts.rs for
        // numeric / string symbols. The generated file is `include!`d
        // by src/config.rs.
        let out_dir = env::var("OUT_DIR").unwrap_or_default();
        let consts_path = format!("{out_dir}/config_consts.rs");
        let mut consts = String::new();
        consts.push_str("// Auto-generated from ove_config.h by build.rs (G3). Do not edit.\n");
        // Each line: `#define CONFIG_OVE_<NAME> <value>` where value is
        // 1 (bool), an integer literal, or a quoted string. We use a
        // line-based parse rather than a regex to keep the build.rs
        // dep-free.
        for raw in config.lines() {
            let line = raw.trim();
            let Some(rest) = line.strip_prefix("#define CONFIG_OVE_") else {
                continue;
            };
            let Some((name, value)) = rest.split_once(char::is_whitespace) else {
                continue;
            };
            let name = name.trim();
            let value = value.trim();
            // Skip name-only defines (no value), and the RTOS/has_/
            // board_ shapes already handled above.
            if value.is_empty() {
                continue;
            }
            let cfg_name = format!("config_ove_{}", name.to_lowercase());
            println!("cargo:rustc-check-cfg=cfg({cfg_name})");

            if value == "1" {
                println!("cargo:rustc-cfg={cfg_name}");
                writeln!(consts, "pub const CONFIG_OVE_{name}: bool = true;").unwrap();
            } else if let Ok(n) = value.parse::<i64>() {
                // Numeric — emit both i64 and a usize-friendly cast.
                writeln!(consts, "pub const CONFIG_OVE_{name}: i64 = {n};").unwrap();
                if n >= 0 {
                    writeln!(
                        consts,
                        "pub const CONFIG_OVE_{name}_USIZE: usize = {n} as usize;"
                    )
                    .unwrap();
                }
            } else if value.starts_with('"') && value.ends_with('"') && value.len() >= 2 {
                writeln!(consts, "pub const CONFIG_OVE_{name}: &str = {value};").unwrap();
            }
            // Anything else (hex literal, complex expression) is
            // ignored — Kconfig in oveRTOS only emits the three shapes
            // above.
        }
        std::fs::write(&consts_path, consts).expect("write config_consts.rs");
        println!("cargo:rustc-env=OVE_CONFIG_CONSTS={consts_path}");

        println!("cargo:rerun-if-changed={config_path}");
    }
    let is_native = env::var("RUST_IS_NATIVE").unwrap_or_default() == "1";

    // Detect enabled features from ove_config.h
    let config_path = format!("{ove_gen_dir}/ove_config.h");
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
        let config_path = format!("{ove_gen_dir}/ove_config.h");
        let config = std::fs::read_to_string(&config_path).unwrap_or_default();
        // POSIX is the host test backend; it lives with the tests.
        let backends = [
            ("FREERTOS", "backends/freertos"),
            ("ZEPHYR", "backends/zephyr"),
            ("NUTTX", "backends/nuttx"),
            ("POSIX", "tests/host/posix"),
        ];
        backends.iter().find_map(|(define, dir)| {
            let needle = format!("#define CONFIG_OVE_RTOS_{define} 1");
            if config.contains(&needle) {
                Some(format!("{ove_dir}/{dir}/include"))
            } else {
                None
            }
        })
    };

    // Generate storage_sizes.h from build-time measurements.
    let sizes_path = env::var("OVE_STORAGE_SIZES").expect(
        "OVE_STORAGE_SIZES env var not set — \
                 ove_rust.cmake must generate storage sizes before cargo runs",
    );
    let content = std::fs::read_to_string(&sizes_path)
        .unwrap_or_else(|e| panic!("Failed to read {sizes_path}: {e}"));

    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    let header_path = out_path.join("storage_sizes.h");
    let mut header = String::from(
        "/* Auto-generated by build.rs — do not edit. */\n\
         #include <stdint.h>\n\n",
    );
    // Storage keys become #defines for bindgen; struct layout keys (from
    // cmake/ove_abi_types.def) become const asserts against bindgen's view.
    let mut contract = String::from(
        "const fn field_size<T, F>(_: fn(&T) -> &F) -> usize {\n    ::core::mem::size_of::<F>()\n}\n",
    );
    let mut count = 0usize;
    for line in content.lines() {
        let Some((key, val)) = line.split_once('=') else {
            continue;
        };
        let (key, val) = (key.trim(), val.trim());
        let lower = key.to_lowercase();
        let check = if let Some(tag) = lower.strip_prefix("sizeof_struct_") {
            format!("::core::mem::size_of::<{tag}>()")
        } else if let Some(tag) = lower.strip_prefix("alignof_struct_") {
            format!("::core::mem::align_of::<{tag}>()")
        } else if let Some((tag, field)) = lower
            .strip_prefix("offsetof_")
            .and_then(|k| k.split_once("__"))
        {
            // bindgen renames fields that are Rust keywords (`type`).
            let field = if field == "type" { "type_" } else { field };
            format!("::core::mem::offset_of!({tag}, {field})")
        } else if let Some((tag, field)) = lower
            .strip_prefix("fieldsize_")
            .and_then(|k| k.split_once("__"))
        {
            let field = if field == "type" { "type_" } else { field };
            format!("field_size(|s: &{tag}| &s.{field})")
        } else {
            let _ = writeln!(header, "#define OVE_{key} {val}");
            count += 1;
            continue;
        };
        let _ = writeln!(
            contract,
            "const _: () = assert!({check} == {val}, \"C ABI mismatch: {lower} is {val} in C\");"
        );
    }
    assert!(count > 0, "storage sizes file is empty: {sizes_path}");
    assert!(
        contract.contains("assert!"),
        "no struct layouts in {sizes_path}"
    );
    std::fs::write(&header_path, &header).expect("Failed to write storage_sizes.h");
    std::fs::write(out_path.join("abi_contract.rs"), contract)
        .expect("Failed to write abi_contract.rs");

    wrapper = format!(
        "#include \"{}\"\n{}",
        header_path.to_str().unwrap(),
        wrapper
    );
    println!("cargo:rerun-if-changed={sizes_path}");

    let mut builder = bindgen::Builder::default()
        .header_contents("wrapper.h", &wrapper)
        .clang_arg("-D__BINDGEN__")
        .clang_arg("-DLV_CONF_INCLUDE_SIMPLE");
    builder = builder
        .clang_args([format!("-I{ove_dir}/include"), format!("-I{ove_gen_dir}")])
        // Emit C enum names such as LV_EVENT_CLICKED without a type prefix.
        .prepend_enum_name(false);

    // Add backend storage header include path
    if let Some(ref inc) = backend_include {
        builder = builder.clang_arg(format!("-I{inc}"));
    }

    let mut builder = builder
        // oveRTOS symbols
        .allowlist_function("ove_.*")
        .allowlist_type("ove_.*")
        .allowlist_var("OVE_.*");

    // LVGL symbols (only when LVGL is enabled)
    if has_lvgl {
        let lvgl_include = env::var("LVGL_INCLUDE_PATH")
            .expect("LVGL_INCLUDE_PATH env var must be set by build system");
        let lvgl_parent = env::var("LVGL_PARENT_PATH")
            .expect("LVGL_PARENT_PATH env var must be set by build system");
        builder = builder
            .clang_args([format!("-I{lvgl_include}"), format!("-I{lvgl_parent}")])
            // Static inline helpers like lv_obj_set_style_pad_all have no linkable symbol.
            // Bindgen skips them, so the binding composes them in Rust.
            .allowlist_function("lv_.*")
            // Skip unused bitfield accessors that trigger unnecessary_transmutes.
            .allowlist_type("lv_draw_dsc_base_t")
            .opaque_type("lv_draw_dsc_base_t")
            .allowlist_type("lv_point_t")
            .allowlist_type("lv_scale_section_t")
            .allowlist_type("lv_label_long_mode_t")
            .allowlist_type("lv_image_dsc_t")
            .allowlist_type("lv_image_header_t")
            .allowlist_type("lv_roller_mode_t")
            .allowlist_type("lv_keyboard_mode_t")
            .allowlist_type("lv_dir_t")
            .allowlist_type("lv_chart_series_t")
            .allowlist_type("lv_chart_type_t")
            .allowlist_type("lv_chart_axis_t")
            .allowlist_type("lv_chart_update_mode_t")
            .allowlist_type("lv_color_format_t")
            .allowlist_type("lv_layer_t")
            .allowlist_type("lv_calendar_date_t")
            .allowlist_type("lv_group_t")
            .allowlist_type("lv_subject_t")
            .allowlist_type("lv_observer_t")
            .allowlist_type("lv_anim_t")
            .allowlist_type("lv_anim_path_cb_t")
            .allowlist_type("lv_anim_exec_xcb_t")
            .allowlist_type("lv_anim_completed_cb_t")
            .allowlist_type("lv_timer_t")
            .allowlist_type("lv_timer_cb_t")
            .allowlist_type("lv_obj_t")
            .allowlist_type("lv_color_t")
            .allowlist_type("lv_font_t")
            .allowlist_type("lv_style_t")
            .allowlist_type("lv_event_cb_t")
            .allowlist_type("lv_event_code_t")
            .allowlist_type("lv_obj_flag_t")
            .allowlist_type("lv_part_t")
            .allowlist_type("lv_state_t")
            .allowlist_type("lv_flex_flow_t")
            .allowlist_type("lv_grid_align_t")
            .allowlist_type("lv_screen_load_anim_t")
            .allowlist_type("lv_opa_t")
            .allowlist_var("LV_ALIGN_.*")
            .allowlist_var("LV_SCR_LOAD_ANIM_.*")
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
            builder = builder.clang_arg(format!("-I{dsp_inc}"));
        }
        if let Ok(core_inc) = env::var("CMSIS_CORE_INCLUDE") {
            builder = builder.clang_arg(format!("-I{core_inc}"));
        }
    }

    // Board-specific lv_conf.h directory for bindgen.
    // Skip empty values: an empty `-I` swallows the next clang arg.
    if let Ok(lv_conf_path) = env::var("LV_CONF_PATH") {
        if !lv_conf_path.is_empty() {
            builder = builder.clang_arg(format!("-I{lv_conf_path}"));
        }
    }

    // Native builds parse host headers; other builds use a freestanding ARM target.
    // Rust namespace selection below is independent of that C target choice.
    if !is_native {
        builder = builder
            .clang_arg("-DARM_MATH_CM7")
            .clang_arg("-D__FPU_PRESENT=1")
            .clang_arg("-fshort-enums")
            .clang_arg("--target=arm-none-eabihf");

        if let Ok(sysroot) = env::var("ARM_SYSROOT_INCLUDE") {
            if !sysroot.is_empty() {
                builder = builder.clang_arg(format!("-isystem{sysroot}"));
            }
        }
    }

    builder = builder.use_core().ctypes_prefix("core::ffi");

    let bindings = builder
        .generate()
        .expect("Failed to generate oveRTOS bindings");

    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("ove_bindings.rs"))
        .expect("Failed to write bindings");

    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=OVE_DIR");
    println!("cargo:rerun-if-env-changed=OVE_GEN_DIR");
    println!("cargo:rerun-if-env-changed=ARM_SYSROOT_INCLUDE");
    println!("cargo:rerun-if-env-changed=RUST_IS_NATIVE");
    /* Force bindgen rerun when public C headers change.  Without
     * this, cargo caches `ove_bindings.rs` and new substrate symbols
     * (e.g. Phase 4's ove_thread_request_stop) don't surface until
     * something else invalidates the build.  Per-file directives so
     * the watch list stays accurate vs. a glob. */
    let header_dir = format!("{ove_dir}/include/ove");
    println!("cargo:rerun-if-changed={header_dir}");
    println!("cargo:rerun-if-changed={ove_dir}/include/ove/thread.h");
    println!("cargo:rerun-if-changed={ove_dir}/include/ove/types.h");
    println!("cargo:rerun-if-changed={ove_dir}/include/ove/sync.h");
    println!("cargo:rerun-if-changed={ove_dir}/include/ove/queue.h");
    println!("cargo:rerun-if-changed={ove_dir}/include/ove/stream.h");
    println!("cargo:rerun-if-changed={ove_dir}/include/ove/eventgroup.h");
    println!("cargo:rerun-if-changed={ove_dir}/include/ove/time.h");
    println!("cargo:rerun-if-changed={ove_dir}/include/ove/ove.h");
    println!("cargo:rerun-if-changed={ove_dir}/include/ove/version.h");
    println!("cargo:rerun-if-env-changed=LV_CONF_PATH");
    println!("cargo:rerun-if-env-changed=LVGL_INCLUDE_PATH");
    println!("cargo:rerun-if-env-changed=LVGL_PARENT_PATH");
    println!("cargo:rerun-if-env-changed=CMSIS_DSP_INCLUDE");
    println!("cargo:rerun-if-env-changed=CMSIS_CORE_INCLUDE");
}
