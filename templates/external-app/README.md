# oveRTOS external-app templates

Each subdirectory is a minimal, buildable scaffold for a new external app in one of the four supported languages. They are consumed by `ove app new` to stamp a working starting point.

| Directory | Language | Entry source |
|---|---|---|
| `c/` | C | `src/app.c` |
| `cpp/` | C++ | `src/app.cpp` |
| `rust/` | Rust | `src/lib.rs` |
| `zig/` | Zig | `src/main.zig` |

## File convention

Files inside each template that contain placeholders use a `.in` suffix (`app.yaml.in`, `src/app.c.in`, etc.). The scaffolder strips `.in` when materialising the file. The convention matches CMake's `configure_file()` style and keeps the templates from being picked up by language tooling.

Files without a `.in` suffix are copied verbatim (e.g., `.gitignore`, `rust/build.rs`).

## Placeholders

Inside `.in` files, the following tokens are substituted:

| Token | Meaning |
|---|---|
| `{{NAME}}` | Human-readable app name, as typed by the user (e.g., `my-cool-app`) |
| `{{CONFIG_NAME}}` | snake-case identifier for `config_name:` and `make` target (`my_cool_app`) |
| `{{CONFIG_NAME_UPPER}}` | UPPER_SNAKE_CASE version (`MY_COOL_APP`) — used in macro names |
| `{{LIB_NAME}}` | same as `CONFIG_NAME`, used for `rust.lib_name` / `zig.lib_name` |
| `{{OVE_DIR}}` | Absolute path to the oveRTOS checkout |

To hand-stamp without `ove app new`:

```bash
cp -r templates/external-app/c ~/my_app
cd ~/my_app
# strip .in
find . -type f -name '*.in' | while read f; do mv "$f" "${f%.in}"; done
# substitute
sed -i \
  -e 's|{{NAME}}|my-app|g' \
  -e 's|{{CONFIG_NAME}}|my_app|g' \
  -e 's|{{CONFIG_NAME_UPPER}}|MY_APP|g' \
  -e 's|{{LIB_NAME}}|my_app|g' \
  -e 's|{{OVE_DIR}}|/path/to/oveRTOS|g' \
  Makefile app.yaml src/app.c include/app_conf.h README.md
```

The scaffolder does the same plus argument validation, optional `git init`, and refusing to overwrite non-empty directories.

## Adding a new template

A new template (`net`, `lvgl`, `audio`, …) is a sibling directory with the same file layout (`.in` for everything templated). Add the directory's name to the `--template` choices in `config/ove-cli/ove/app_new.py`.
