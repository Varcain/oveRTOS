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
    // bindgen-generated module for the hand-written bindings_stub.rs,
    // so the crate compiles without LVGL/storage-sizes plumbing —
    // reused by `ove lint` for clippy without a configured workspace.
    if std::env::var("DOCS_RS").is_ok() {
        println!("cargo:rustc-cfg=docsrs");
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
        // board_<name> cfgs (used by SpinMutex gate etc.); doc-only
        // build registers them as known cfgs without setting any.
        for b in &["qemu_mps2", "stm32f746g_disco", "host_posix", "wasm"] {
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

    // Detect enabled subsystems from ove_config.h and emit cfg flags
    {
        let config_path = format!("{ove_gen_dir}/ove_config.h");
        let config = std::fs::read_to_string(&config_path).unwrap_or_default();
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
        // `ove` crate itself can gate on `board_<name>` (e.g. SpinMutex
        // is excluded on WASM).
        let boards = [
            ("QEMU_MPS2_AN500", "qemu_mps2"),
            ("STM32F746G_DISCO", "stm32f746g_disco"),
            ("HOST_POSIX", "host_posix"),
            ("WASM", "wasm"),
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
    let lvgl_include = env::var("LVGL_INCLUDE_PATH")
        .expect("LVGL_INCLUDE_PATH env var must be set by build system");
    let lvgl_parent =
        env::var("LVGL_PARENT_PATH").expect("LVGL_PARENT_PATH env var must be set by build system");

    let is_native = env::var("RUST_IS_NATIVE").unwrap_or_default() == "1";
    let is_wasm = env::var("OVE_WASM_BUILD").unwrap_or_default() == "1"
        || std::env::var("TARGET")
            .unwrap_or_default()
            .contains("wasm32");

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
        let backends = [
            ("FREERTOS", "freertos"),
            ("ZEPHYR", "zephyr"),
            ("NUTTX", "nuttx"),
            ("POSIX", "posix"),
        ];
        backends.iter().find_map(|(define, dir)| {
            let needle = format!("#define CONFIG_OVE_RTOS_{define} 1");
            if config.contains(&needle) {
                Some(format!("{ove_dir}/backends/{dir}/include"))
            } else {
                None
            }
        })
    };

    // Generate storage_sizes.h from build-time measurements.
    if is_wasm {
        // WASM uses heap mode — real sizes not needed.  Provide large
        // dummy values so the __BINDGEN__ opaque types compile.
        let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
        let header_path = out_path.join("storage_sizes.h");
        let mut header = String::from(
            "/* Dummy storage sizes for WASM (heap mode only). */\n\
             #include <stdint.h>\n\n",
        );
        let types = [
            "MUTEX",
            "SEM",
            "EVENT",
            "CONDVAR",
            "THREAD",
            "QUEUE",
            "TIMER",
            "EVENTGROUP",
            "WORKQUEUE",
            "WORK",
            "STREAM",
            "WATCHDOG",
            "FILE",
            "DIR",
            "SOCKET",
            "NETIF",
            "HTTP_CLIENT",
            "MQTT_CLIENT",
            "TLS",
            "MODEL",
            "UART",
            "SPI",
            "I2C",
            "I2S",
        ];
        for t in &types {
            let _ = write!(
                header,
                "#define OVE_SIZEOF_OVE_{t}_STORAGE 1024\n\
                 #define OVE_ALIGNOF_OVE_{t}_STORAGE 8\n",
            );
        }
        std::fs::write(&header_path, &header).expect("Failed to write storage_sizes.h");
        wrapper = format!(
            "#include \"{}\"\n{}",
            header_path.to_str().unwrap(),
            wrapper
        );
    } else {
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
        let mut count = 0usize;
        for line in content.lines() {
            if let Some((key, val)) = line.split_once('=') {
                let _ = writeln!(header, "#define OVE_{} {}", key.trim(), val.trim());
                count += 1;
            }
        }
        assert!(count > 0, "storage sizes file is empty: {sizes_path}");
        std::fs::write(&header_path, &header).expect("Failed to write storage_sizes.h");

        wrapper = format!(
            "#include \"{}\"\n{}",
            header_path.to_str().unwrap(),
            wrapper
        );
        println!("cargo:rerun-if-changed={sizes_path}");
    }

    let mut builder = bindgen::Builder::default()
        .header_contents("wrapper.h", &wrapper)
        .clang_arg("-D__BINDGEN__")
        .clang_arg("-DLV_CONF_INCLUDE_SIMPLE");
    // WASM: bindgen parses with host target (x86_64) but Rust compiles
    // for wasm32 (4-byte pointers).  Disable layout tests to avoid
    // size assertion mismatches between host and wasm32.
    if is_wasm {
        builder = builder.layout_tests(false);
    }
    builder = builder
        .clang_args(&[
            format!("-I{ove_dir}/include"),
            format!("-I{ove_gen_dir}"),
            format!("-I{lvgl_include}"),
            format!("-I{lvgl_parent}"),
        ])
        // Generate enum constants without type prefix (LV_EVENT_CLICKED, not
        // lv_event_code_t_LV_EVENT_CLICKED) — matches stub anonymous-enum output.
        .prepend_enum_name(false);

    // Add backend storage header include path
    if is_wasm {
        // WASM uses its own storage header
        builder = builder.clang_arg(format!("-I{ove_dir}/backends/wasm/include"));
        // Also need POSIX include for shared types (sync, queue, etc.)
        builder = builder.clang_arg(format!("-I{ove_dir}/backends/posix/include"));
    } else if let Some(ref inc) = backend_include {
        builder = builder.clang_arg(format!("-I{inc}"));
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
            .allowlist_function("lv_obj_has_state")
            .allowlist_function("lv_obj_set_grid_dsc_array")
            .allowlist_function("lv_obj_set_grid_cell")
            .allowlist_function("lv_obj_get_child")
            .allowlist_function("lv_obj_set_style_pad_row")
            .allowlist_function("lv_obj_set_style_pad_column")
            // LVGL — events
            .allowlist_function("lv_obj_add_event_cb")
            .allowlist_function("lv_event_get_user_data")
            .allowlist_function("lv_event_get_target")
            .allowlist_function("lv_event_get_current_target")
            .allowlist_function("lv_event_get_code")
            .allowlist_function("lv_event_get_param")
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
            .allowlist_function("lv_obj_set_style_text_align")
            .allowlist_function("lv_obj_set_flex_flow")
            .allowlist_function("lv_obj_set_flex_align")
            .allowlist_function("lv_obj_set_flex_grow")
            .allowlist_function("lv_obj_set_layout")
            .allowlist_function("lv_obj_set_style_arc_color")
            .allowlist_function("lv_obj_set_style_arc_width")
            .allowlist_function("lv_obj_set_style_arc_opa")
            .allowlist_function("lv_obj_set_style_arc_rounded")
            .allowlist_function("lv_obj_set_style_translate_y")
            .allowlist_function("lv_obj_set_style_margin_top")
            .allowlist_function("lv_obj_set_style_margin_bottom")
            .allowlist_function("lv_obj_set_style_margin_left")
            .allowlist_function("lv_obj_set_style_margin_right")
            .allowlist_function("lv_obj_set_style_max_height")
            .allowlist_function("lv_obj_set_style_opa_layered")
            // Scrolling / layout queries
            .allowlist_function("lv_obj_scroll_to_y")
            .allowlist_function("lv_obj_get_scroll_bottom")
            .allowlist_function("lv_obj_update_layout")
            .allowlist_function("lv_obj_get_content_width")
            // Image extras
            .allowlist_function("lv_image_set_inner_align")
            // Display info
            .allowlist_function("lv_display_get_horizontal_resolution")
            .allowlist_function("lv_display_get_vertical_resolution")
            .allowlist_function("lv_display_get_dpi")
            .allowlist_function("lv_display_get_default")
            // Top layer access
            .allowlist_function("lv_layer_top")
            // Text metrics
            .allowlist_function("lv_text_get_size")
            .allowlist_type("lv_point_t")
            // Subject/observer extras
            .allowlist_function("lv_subject_get_pointer")
            .allowlist_function("lv_observer_get_target_obj")
            // Scale widget
            .allowlist_function("lv_scale_create")
            .allowlist_function("lv_scale_set_mode")
            .allowlist_function("lv_scale_set_range")
            .allowlist_function("lv_scale_set_total_tick_count")
            .allowlist_function("lv_scale_set_major_tick_every")
            .allowlist_function("lv_scale_set_angle_range")
            .allowlist_function("lv_scale_set_rotation")
            .allowlist_function("lv_scale_add_section")
            .allowlist_function("lv_scale_section_set_range")
            .allowlist_function("lv_scale_section_set_style")
            .allowlist_type("lv_scale_section_t")
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
            .allowlist_function("lv_style_set_arc_color")
            .allowlist_function("lv_style_set_arc_width")
            .allowlist_function("lv_obj_add_style")
            .allowlist_function("lv_obj_remove_style_all")
            // LVGL — color helpers
            .allowlist_function("lv_palette_main")
            .allowlist_function("lv_palette_lighten")
            .allowlist_function("lv_palette_darken")
            .allowlist_function("lv_color_make")
            .allowlist_function("lv_color_white")
            .allowlist_function("lv_color_black")
            .allowlist_function("lv_color_hex")
            .allowlist_function("lv_color_hex3")
            // LVGL — widgets
            .allowlist_function("lv_label_create")
            .allowlist_function("lv_label_set_text")
            .allowlist_function("lv_label_set_text_static")
            .allowlist_function("lv_label_set_long_mode")
            .allowlist_type("lv_label_long_mode_t")
            .allowlist_function("lv_bar_create")
            .allowlist_function("lv_bar_set_value")
            .allowlist_function("lv_bar_set_range")
            .allowlist_function("lv_button_create")
            .allowlist_function("lv_slider_create")
            .allowlist_function("lv_slider_set_value")
            .allowlist_function("lv_slider_set_range")
            .allowlist_function("lv_slider_get_value")
            .allowlist_function("lv_switch_create")
            .allowlist_function("lv_checkbox_create")
            .allowlist_function("lv_checkbox_set_text")
            .allowlist_function("lv_checkbox_set_text_static")
            .allowlist_function("lv_arc_create")
            .allowlist_function("lv_arc_set_value")
            .allowlist_function("lv_arc_set_range")
            .allowlist_function("lv_arc_set_bg_angles")
            .allowlist_function("lv_arc_set_angles")
            .allowlist_function("lv_arc_set_rotation")
            .allowlist_function("lv_arc_get_value")
            .allowlist_function("lv_image_create")
            .allowlist_function("lv_image_set_src")
            .allowlist_function("lv_image_set_rotation")
            .allowlist_function("lv_image_set_scale")
            .allowlist_function("lv_image_set_pivot")
            .allowlist_type("lv_image_dsc_t")
            .allowlist_type("lv_image_header_t")
            .allowlist_function("lv_msgbox_create")
            .allowlist_function("lv_msgbox_add_title")
            .allowlist_function("lv_msgbox_add_text")
            .allowlist_function("lv_msgbox_add_close_button")
            .allowlist_function("lv_msgbox_add_footer_button")
            .allowlist_function("lv_msgbox_get_content")
            .allowlist_function("lv_msgbox_get_header")
            .allowlist_function("lv_msgbox_get_footer")
            .allowlist_function("lv_msgbox_close")
            .allowlist_function("lv_spinner_create")
            .allowlist_function("lv_spinner_set_anim_params")
            .allowlist_function("lv_led_create")
            .allowlist_function("lv_led_set_color")
            .allowlist_function("lv_led_set_brightness")
            .allowlist_function("lv_led_on")
            .allowlist_function("lv_led_off")
            .allowlist_function("lv_led_toggle")
            .allowlist_function("lv_led_get_brightness")
            // LVGL — forms (Phase 3.2)
            .allowlist_function("lv_textarea_create")
            .allowlist_function("lv_textarea_set_text")
            .allowlist_function("lv_textarea_add_text")
            .allowlist_function("lv_textarea_set_placeholder_text")
            .allowlist_function("lv_textarea_set_one_line")
            .allowlist_function("lv_textarea_set_password_mode")
            .allowlist_function("lv_textarea_set_max_length")
            .allowlist_function("lv_textarea_set_accepted_chars")
            .allowlist_function("lv_textarea_set_cursor_pos")
            .allowlist_function("lv_textarea_set_cursor_click_pos")
            .allowlist_function("lv_textarea_get_text")
            .allowlist_function("lv_textarea_get_cursor_pos")
            .allowlist_function("lv_textarea_get_password_mode")
            .allowlist_function("lv_textarea_get_one_line")
            .allowlist_function("lv_textarea_add_char")
            .allowlist_function("lv_textarea_delete_char")
            .allowlist_function("lv_dropdown_create")
            .allowlist_function("lv_dropdown_set_options")
            .allowlist_function("lv_dropdown_set_options_static")
            .allowlist_function("lv_dropdown_add_option")
            .allowlist_function("lv_dropdown_clear_options")
            .allowlist_function("lv_dropdown_set_selected")
            .allowlist_function("lv_dropdown_get_selected")
            .allowlist_function("lv_dropdown_get_option_count")
            .allowlist_function("lv_dropdown_get_options")
            .allowlist_function("lv_dropdown_get_selected_str")
            .allowlist_function("lv_dropdown_set_dir")
            .allowlist_function("lv_dropdown_set_symbol")
            .allowlist_function("lv_dropdown_is_open")
            .allowlist_function("lv_dropdown_open")
            .allowlist_function("lv_dropdown_close")
            .allowlist_function("lv_roller_create")
            .allowlist_function("lv_roller_set_options")
            .allowlist_function("lv_roller_set_selected")
            .allowlist_function("lv_roller_set_visible_row_count")
            .allowlist_function("lv_roller_get_selected")
            .allowlist_function("lv_roller_get_option_count")
            .allowlist_function("lv_roller_get_options")
            .allowlist_function("lv_roller_get_selected_str")
            .allowlist_function("lv_spinbox_create")
            .allowlist_function("lv_spinbox_set_value")
            .allowlist_function("lv_spinbox_set_range")
            .allowlist_function("lv_spinbox_set_step")
            .allowlist_function("lv_spinbox_set_digit_format")
            .allowlist_function("lv_spinbox_set_rollover")
            .allowlist_function("lv_spinbox_set_cursor_pos")
            .allowlist_function("lv_spinbox_get_value")
            .allowlist_function("lv_spinbox_get_step")
            .allowlist_function("lv_spinbox_increment")
            .allowlist_function("lv_spinbox_decrement")
            .allowlist_function("lv_keyboard_create")
            .allowlist_function("lv_keyboard_set_textarea")
            .allowlist_function("lv_keyboard_set_mode")
            .allowlist_function("lv_keyboard_set_popovers")
            .allowlist_function("lv_keyboard_get_textarea")
            .allowlist_type("lv_roller_mode_t")
            .allowlist_type("lv_keyboard_mode_t")
            .allowlist_type("lv_dir_t")
            // LVGL — chart (Phase 4.1)
            .allowlist_function("lv_chart_create")
            .allowlist_function("lv_chart_set_type")
            .allowlist_function("lv_chart_set_point_count")
            .allowlist_function("lv_chart_set_axis_range")
            .allowlist_function("lv_chart_set_update_mode")
            .allowlist_function("lv_chart_set_div_line_count")
            .allowlist_function("lv_chart_add_series")
            .allowlist_function("lv_chart_remove_series")
            .allowlist_function("lv_chart_set_next_value")
            .allowlist_function("lv_chart_set_series_value_by_id")
            .allowlist_type("lv_chart_series_t")
            .allowlist_type("lv_chart_type_t")
            .allowlist_type("lv_chart_axis_t")
            .allowlist_type("lv_chart_update_mode_t")
            // LVGL — table (Phase 4.2)
            .allowlist_function("lv_table_create")
            .allowlist_function("lv_table_set_cell_value")
            .allowlist_function("lv_table_set_row_count")
            .allowlist_function("lv_table_set_column_count")
            .allowlist_function("lv_table_set_column_width")
            .allowlist_function("lv_table_get_cell_value")
            .allowlist_function("lv_table_get_row_count")
            .allowlist_function("lv_table_get_column_count")
            .allowlist_function("lv_table_get_column_width")
            // LVGL — tabview
            .allowlist_function("lv_tabview_create")
            .allowlist_function("lv_tabview_add_tab")
            .allowlist_function("lv_tabview_set_tab_text")
            .allowlist_function("lv_tabview_set_active")
            .allowlist_function("lv_tabview_set_tab_bar_position")
            .allowlist_function("lv_tabview_set_tab_bar_size")
            .allowlist_function("lv_tabview_get_tab_count")
            .allowlist_function("lv_tabview_get_tab_active")
            .allowlist_function("lv_tabview_get_content")
            // LVGL — list
            .allowlist_function("lv_list_create")
            .allowlist_function("lv_list_add_text")
            .allowlist_function("lv_list_add_button")
            .allowlist_function("lv_list_get_button_text")
            // LVGL — canvas (Phase 4.3)
            .allowlist_function("lv_canvas_create")
            .allowlist_function("lv_canvas_set_buffer")
            .allowlist_function("lv_canvas_fill_bg")
            .allowlist_function("lv_canvas_set_px")
            .allowlist_function("lv_canvas_init_layer")
            .allowlist_function("lv_canvas_finish_layer")
            .allowlist_type("lv_color_format_t")
            .allowlist_type("lv_layer_t")
            // LVGL — calendar (Phase 4.4)
            .allowlist_function("lv_calendar_create")
            .allowlist_function("lv_calendar_set_today_date")
            .allowlist_function("lv_calendar_set_month_shown")
            .allowlist_function("lv_calendar_set_highlighted_dates")
            .allowlist_function("lv_calendar_get_pressed_date")
            .allowlist_function("lv_calendar_add_header_arrow")
            .allowlist_function("lv_calendar_add_header_dropdown")
            .allowlist_type("lv_calendar_date_t")
            // LVGL — groups / focus
            .allowlist_function("lv_group_create")
            .allowlist_function("lv_group_delete")
            .allowlist_function("lv_group_set_default")
            .allowlist_function("lv_group_get_default")
            .allowlist_function("lv_group_add_obj")
            .allowlist_function("lv_group_remove_obj")
            .allowlist_function("lv_group_remove_all_objs")
            .allowlist_function("lv_group_focus_obj")
            .allowlist_function("lv_group_focus_next")
            .allowlist_function("lv_group_focus_prev")
            .allowlist_function("lv_group_focus_freeze")
            .allowlist_function("lv_group_get_focused")
            .allowlist_function("lv_group_set_editing")
            .allowlist_function("lv_group_get_editing")
            .allowlist_function("lv_group_get_obj_count")
            .allowlist_type("lv_group_t")
            // LVGL — subject / observer (reactive state)
            .allowlist_function("lv_subject_init_int")
            .allowlist_function("lv_subject_set_int")
            .allowlist_function("lv_subject_get_int")
            .allowlist_function("lv_subject_deinit")
            .allowlist_function("lv_subject_add_observer_obj")
            .allowlist_function("lv_subject_notify")
            .allowlist_function("lv_observer_remove")
            .allowlist_function("lv_label_bind_text")
            .allowlist_function("lv_arc_bind_value")
            .allowlist_function("lv_slider_bind_value")
            .allowlist_function("lv_roller_bind_value")
            .allowlist_function("lv_dropdown_bind_value")
            .allowlist_type("lv_subject_t")
            .allowlist_type("lv_observer_t")
            // LVGL — animations (pattern allowlist covers all lv_anim_*)
            .allowlist_function("lv_anim_.*")
            .allowlist_type("lv_anim_t")
            .allowlist_type("lv_anim_path_cb_t")
            .allowlist_type("lv_anim_exec_xcb_t")
            .allowlist_type("lv_anim_completed_cb_t")
            .allowlist_function("lv_obj_set_x")
            .allowlist_function("lv_obj_set_y")
            .allowlist_function("lv_obj_get_x")
            .allowlist_function("lv_obj_get_y")
            .allowlist_function("lv_obj_set_style_opa")
            // LVGL — timers
            .allowlist_function("lv_timer_create")
            .allowlist_function("lv_timer_delete")
            .allowlist_function("lv_timer_pause")
            .allowlist_function("lv_timer_resume")
            .allowlist_function("lv_timer_set_period")
            .allowlist_function("lv_timer_set_repeat_count")
            .allowlist_function("lv_timer_reset")
            .allowlist_function("lv_timer_ready")
            .allowlist_function("lv_timer_get_user_data")
            .allowlist_type("lv_timer_t")
            .allowlist_type("lv_timer_cb_t")
            .allowlist_function("lv_screen_active")
            .allowlist_function("lv_screen_load")
            .allowlist_function("lv_screen_load_anim")
            // LVGL — types & constants
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

    if is_native || is_wasm {
        // Native/POSIX or WASM: parse headers using the HOST target.
        // Bindgen generates .rs source which cargo then cross-compiles
        // to wasm32.  The header parsing must use host target so clang
        // finds the correct system headers (pthread.h, etc.).
        // __BINDGEN__ opaque types make struct layouts irrelevant.
        if is_wasm {
            let host = std::env::var("HOST").unwrap_or_default();
            if !host.is_empty() {
                builder = builder.clang_arg(format!("--target={host}"));
            }
        }
    } else {
        // ARM cross-compilation: freestanding ARM target
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

    if is_native || is_wasm {
        // Native/WASM: use std-compatible types
    } else {
        builder = builder.use_core().ctypes_prefix("core::ffi");
    }

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
    println!("cargo:rerun-if-env-changed=LV_CONF_PATH");
    println!("cargo:rerun-if-env-changed=LVGL_INCLUDE_PATH");
    println!("cargo:rerun-if-env-changed=LVGL_PARENT_PATH");
    println!("cargo:rerun-if-env-changed=CMSIS_DSP_INCLUDE");
    println!("cargo:rerun-if-env-changed=CMSIS_CORE_INCLUDE");
}
