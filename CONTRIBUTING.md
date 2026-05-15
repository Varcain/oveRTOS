# Contributing — for app authors

This file is aimed at people **building applications on top of oveRTOS**. If you want to contribute to the framework itself (new modules, backends, BSPs), look at the in-tree code conventions and open an issue describing what you'd like to work on.

## Reporting issues

Open an issue at <https://github.com/Varcain/oveRTOS/issues>. Useful issues include:

1. **What you ran** — the exact `make` command and any flags.
2. **What you expected** — and what actually happened.
3. **`make doctor` output** — paste the whole table. It captures host environment in one go.
4. **`.config` excerpt** — `grep -E '^CONFIG_OVE' .config | head -40` gives the relevant slice without the noise from RTOS-internal options.
5. **Minimum reproducer** — an `app.yaml` + `src/app.c` that triggers the issue. If the bug only happens in your full app, that's still fine; just say so.

If your issue is a request for a new feature, lead with the use case (`"I'm trying to read 24-bit I2S audio from board X and the binding stops at 16-bit"`) rather than the proposed API. The maintainers will steer the API; the use case is the part only you can describe.

## Sharing your app

Two ways to share an app you've built:

### As an external app repository

Most useful for application code that's domain-specific and not generally reusable. Layout:

```
my-cool-app/
├── README.md
├── Makefile         # 3 lines: APP_DIR, OVE_DIR, include ove_app.mk
├── app.yaml
├── src/
└── (optional) nuttx/, zephyr/, patches/
```

Set `OVE_DIR` as a git submodule, an environment variable, or a path documented in your README. The `ove app new` scaffolder produces this layout out of the box:

```bash
ove app new --lang c --name my-cool-app
```

See [External Apps](docs-site/docs/build-system/external-apps.md) for the full reference.

### As a sample app submission

If your app demonstrates a useful pattern that newcomers would benefit from, open a PR adding it under `apps/{lang}/heap/<name>/`. Include:

- `app.yaml` with a descriptive `description` field
- Working source for at least one backend
- A `docs-site/docs/examples/<name>/index.md` and per-language walkthrough(s)

Sample apps are held to higher conventions than your own external apps:

- Builds on **every** backend the language supports (FreeRTOS, NuttX, Zephyr, POSIX).
- Both heap and zero-heap variants under `apps/{lang}/heap/<name>/` and `apps/{lang}/zeroheap/<name>/`.
- Listed in the `make help` output automatically via `appgen`.
- Wired into `make alldefconfigs` so CI catches regressions across the matrix.

If "every backend × both modes" is too much for the pattern you have in mind, open it as an external-app repo or a blog post link rather than a sample PR.

## Testing your app

Before opening a PR:

```bash
# Lint
make lint

# Build your app on every backend you target
make host.posix.<your_app>     && make
make qemu.freertos.<your_app>  && make
make qemu.nuttx.<your_app>     && make
make qemu.zephyr.<your_app>    && make

# Run the simulator tests if you're affecting in-tree code
make test
```

For a comprehensive matrix sweep:

```bash
make alldefconfigs
```

This builds every `(board, RTOS, app)` combination via fragment composition. The end-of-run summary shows pass/fail per combination. Takes ~15-30 min on a modern laptop.

## Code style for apps

| Language | Formatter | Linter |
|---|---|---|
| C / C++ | `clang-format` (Linux kernel style with project overrides) | `clang-tidy` (config in `.clang-tidy`) |
| Rust | `cargo fmt` | `cargo clippy -- -D warnings -W pedantic -W nursery` |
| Zig | `zig fmt` | `zig ast-check` |
| Python | `ruff format` | `ruff check` |

`make format` rewrites everything in place; `make lint` is the gating check.

You don't have to follow project style for your *own* external apps — that's your codebase. But if you're submitting a PR, the linters gate it.

## Commit hygiene

- One logical change per commit.
- Imperative subject line, ≤72 chars (`add foo bar widget`, not `Added foo bar widget`).
- Body explains *why*, since the *what* is in the diff. If the change is small and self-explanatory, no body is needed.

The project doesn't enforce conventional-commits, but consistency with the existing log is appreciated. `git log --oneline -20` shows the style.

## Licensing

oveRTOS is **GPL-3.0-or-later**. Patches submitted to the framework are taken under the same license. Apps you build on top of oveRTOS are *your* code under whatever license you choose — GPL's "linked work" clause kicks in only if you redistribute the built firmware as a derivative work; for personal/internal use there's no obligation.

If your app's license matters for your situation, talk to your legal team. The maintainers cannot give license advice.

## Where to go next

- [Quickstart](docs-site/docs/getting-started/quickstart.md) — bring up your first app
- [External Apps](docs-site/docs/build-system/external-apps.md) — full reference for off-tree apps
- [Cookbook](docs-site/docs/cookbook/index.md) — common patterns
- [API Reference](docs-site/docs/api/index.md) — every module
- [Troubleshooting](docs-site/docs/getting-started/troubleshooting.md) — when things break
