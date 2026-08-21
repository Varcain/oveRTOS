#!/usr/bin/env python3
"""Run the shell/Lua/MicroPython hammer matrix on STM32 using SSH only."""

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
import time
from decimal import Decimal, InvalidOperation
from pathlib import Path


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
BUILDROOT = ROOT.parent / "buildroot"
ASKPASS = os.environ.get("OVE_SSH_ASKPASS")
PI_HOST = os.environ.get("OVE_PI_HOST", "pi")
TARGET = "root@172.1.1.2"
RESULT_DIR = Path(os.environ.get(
    "OVE_HAMMER_OUTDIR", str(ROOT / "output/hardware-hammer")
))
SERVER_SOURCE = HERE / "hammer_stream_server.py"

FLASH = {
    "freertos": ROOT / "output/stm32f746g-discovery/freertos/linux_interop/flash",
    "nuttx": ROOT / "output/stm32f746g-discovery/nuttx/linux_interop/flash",
    "zephyr": ROOT / "output/stm32f746g-discovery/zephyr/linux_interop/flash",
}

FIRMWARE = {
    "freertos": FLASH["freertos"].parent / "images/hard-guest-hard/firmware.bin",
    "nuttx": FLASH["nuttx"].parent / "images/firmware.bin",
    "zephyr": FLASH["zephyr"].parent / "images/firmware.bin",
}

SHELL_SOURCE = (
    BUILDROOT / "board/overtos/rootfs-overlay/usr/libexec/ove-hammer-shell"
)
LUA_SOURCE = (
    BUILDROOT / "board/overtos/rootfs-overlay/usr/libexec/ove-hammer.lua"
)
MICROPYTHON_SOURCE = (
    BUILDROOT / "board/overtos/rootfs-overlay/usr/libexec/ove-hammer.py"
)

SYSMON_RE = re.compile(
    r"sysmon:\s+(?P<fps>\d+) FPS \(refr_cnt: (?P<refr>\d+) \| "
    r"redraw_cnt: (?P<redraw>\d+)\), refr (?P<refr_ms>\d+)ms "
    r"\(render (?P<render_ms>\d+)ms \| flush (?P<flush_ms>\d+)ms\), "
    r"CPU (?P<cpu>\d+)%"
)


def ssh_env():
    env = os.environ.copy()
    if ASKPASS:
        env.update({
            "DISPLAY": "dummy",
            "SSH_ASKPASS": ASKPASS,
            "SSH_ASKPASS_REQUIRE": "force",
        })
    return env


def pi_argv(command):
    return [
        "setsid", "-w", "ssh", "-o", "ConnectTimeout=10", PI_HOST, command
    ]


def target_argv(command):
    return [
        "setsid", "-w", "ssh",
        "-o", "ConnectTimeout=10",
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null",
        "-J", PI_HOST,
        TARGET,
        command,
    ]


def run(argv, *, timeout, input_data=None, check=True):
    result = subprocess.run(
        argv,
        env=ssh_env(),
        input=input_data,
        text=isinstance(input_data, str) or input_data is None,
        errors="replace",
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
    )
    if check and result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {argv!r}\n{result.stdout}"
        )
    return result


def target_exec(command, timeout=30):
    return run(target_argv(command), timeout=timeout).stdout


def pi_http(path):
    result = run(
        pi_argv(f"curl -fsS http://127.0.0.1:8082/{path}"), timeout=30
    )
    return result.stdout


def deploy_server():
    destination = "/home/varcain/ove-hammer-stream-server.py"
    run(
        [
            "setsid", "-w", "scp", "-o", "ConnectTimeout=10",
            str(SERVER_SOURCE), f"{PI_HOST}:{destination}",
        ],
        timeout=60,
    )
    run(
        pi_argv(
            "if test -r /home/varcain/ove-hammer-stream-server.pid; then "
            "read old_pid </home/varcain/ove-hammer-stream-server.pid; "
            "kill \"$old_pid\" 2>/dev/null || true; fi; "
            "pkill -f '[/]stream_server.py' 2>/dev/null || true; "
            f"nohup python3 {destination} "
            ">/home/varcain/ove-hammer-stream-server.log 2>&1 </dev/null & "
            "echo $! >/home/varcain/ove-hammer-stream-server.pid; "
            "sleep 1; curl -fsS http://127.0.0.1:8082/metrics"
        ),
        timeout=30,
    )


def reset_target():
    run(
        [
            "timeout", "-k", "2", "30", "openocd",
            "-f", "board/stm32f7discovery.cfg",
            "-c", "init", "-c", "reset run", "-c", "exit",
        ],
        timeout=45,
    )


def quiesce_target_storage():
    """Leave removable media idle before an intentional hardware reset."""
    result = run(
        target_argv("sync; umount /data"), timeout=120, check=False
    )
    if result.returncode != 0:
        raise RuntimeError(
            "refusing to reset target with /data still mounted:\n"
            + result.stdout
        )


def flash_target(engine):
    result = run([str(FLASH[engine])], timeout=180)
    if "Verified OK" not in result.stdout:
        raise RuntimeError(
            f"{engine} flash did not report Verified OK:\n{result.stdout}"
        )


def firmware_identity(engine, allow_dirty):
    path = FIRMWARE[engine]
    if not path.is_file():
        raise RuntimeError(f"missing {engine} firmware artifact: {path}")
    identities = {
        match.decode()
        for match in re.findall(rb"ove-[0-9a-f]{7,}(?:-dirty)?", path.read_bytes())
    }
    if len(identities) != 1:
        raise RuntimeError(
            f"expected one oveRTOS identity in {path}, found {sorted(identities)}"
        )
    identity = identities.pop()
    if identity.endswith("-dirty") and not allow_dirty:
        raise RuntimeError(
            f"refusing dirty {engine} firmware artifact {identity}: {path}"
        )
    return identity


def wait_for_target(engine, expected):
    deadline = time.monotonic() + 120
    last = ""
    while time.monotonic() < deadline:
        try:
            identity = target_exec("uname -a", timeout=20).strip()
            if engine.lower() not in identity.lower():
                raise RuntimeError(
                    f"expected {engine}, target reports {identity!r}"
                )
            reported = re.search(
                r"\bove-[0-9a-f]{7,}(?:-dirty)?\b", identity
            )
            if not reported or reported.group(0) != expected:
                raise RuntimeError(
                    f"expected {expected}, target reports {identity!r}"
                )
            # A mountpoint can exist before the asynchronous native SD mount is
            # writable (notably on NuttX). Require a real create/remove cycle.
            target_exec(
                "probe=/data/.ove-hammer-ready; : >\"$probe\" && rm -f \"$probe\"",
                timeout=20,
            )
            return identity
        except Exception as error:
            last = str(error)
            time.sleep(1)
    raise RuntimeError(f"target did not become ready: {last}")


def stage(path, content):
    name = Path(path).name
    remote_name = f".ove-stage-{name}"
    with tempfile.NamedTemporaryFile("w", delete=False) as source:
        source.write(content)
        source_path = source.name
    try:
        run(
            [
                "setsid", "-w", "scp", "-o", "ConnectTimeout=10",
                source_path, f"{PI_HOST}:/home/varcain/{remote_name}",
            ],
            timeout=60,
        )
    finally:
        os.unlink(source_path)
    run(
        pi_argv(
            "curl -fsS http://172.1.1.1:8083/ >/dev/null 2>&1 || "
            "(nohup python3 -m http.server 8083 --bind 172.1.1.1 "
            "--directory /home/varcain >/home/varcain/.ove-stage-http.log "
            "2>&1 </dev/null & sleep 1)"
        ),
        timeout=20,
    )
    run(
        target_argv(
            f"/usr/bin/wget -q -T 30 -O {path} "
            f"http://172.1.1.1:8083/{remote_name} && test -s {path}"
        ),
        timeout=60,
    )


def instrumented_scripts(data_directory):
    shell = SHELL_SOURCE.read_text().replace("/data/.ove-hammer", data_directory)
    original = "/usr/bin/lvmusic >/dev/console 2>&1 &"
    replacement = "/usr/bin/lvmusic >/tmp/ove-hammer-lvgl.log 2>&1 &"
    if original not in shell:
        raise RuntimeError("shell lvmusic launch pattern changed")
    shell = shell.replace(original, replacement, 1)
    shell_marker = "printf '__HAMMER_END__:shell\\n'"
    shell_insert = (
        "printf '__LVGL_LOG_BEGIN__\\n'\n"
        "cat /tmp/ove-hammer-lvgl.log\n"
        "printf '__LVGL_LOG_END__\\n'\n"
        + shell_marker
    )
    if shell_marker not in shell:
        raise RuntimeError("shell end marker changed")
    shell = shell.replace(shell_marker, shell_insert, 1)

    lua = LUA_SOURCE.read_text().replace("/data/.ove-hammer", data_directory)
    old_redirect = (
        'stdout = "/dev/console",\n'
        '        stderr = "/dev/console",'
    )
    new_redirect = (
        'stdout = "/tmp/ove-hammer-lvgl.log",\n'
        '        stderr = "/tmp/ove-hammer-lvgl.err",'
    )
    if old_redirect not in lua:
        raise RuntimeError("Lua lvmusic redirection pattern changed")
    lua = lua.replace(old_redirect, new_redirect, 1)
    lua_marker = '    print("__HAMMER_END__:lua")'
    lua_insert = (
        '    print("__LVGL_LOG_BEGIN__")\n'
        '    for line in io.lines("/tmp/ove-hammer-lvgl.log") do print(line) end\n'
        '    print("__LVGL_LOG_END__")\n'
        + lua_marker
    )
    if lua_marker not in lua:
        raise RuntimeError("Lua end marker changed")
    lua = lua.replace(lua_marker, lua_insert, 1)

    micropython = MICROPYTHON_SOURCE.read_text().replace(
        "/data/.ove-hammer", data_directory
    )
    old_launch = (
        '"/usr/bin/lvmusic", (), None, "/dev/console", "/dev/console")'
    )
    new_launch = (
        '"/usr/bin/lvmusic", (), None, "/tmp/ove-hammer-lvgl.log", '
        '"/tmp/ove-hammer-lvgl.err")'
    )
    if old_launch not in micropython:
        raise RuntimeError("MicroPython lvmusic launch pattern changed")
    micropython = micropython.replace(old_launch, new_launch, 1)
    mp_marker = '    emit("__HAMMER_END__:micropython")'
    mp_insert = (
        '    emit("__LVGL_LOG_BEGIN__")\n'
        '    try:\n'
        '        lvgl_file = open("/tmp/ove-hammer-lvgl.log", "r")\n'
        '        while True:\n'
        '            lvgl_chunk = lvgl_file.read(1024)\n'
        '            if not lvgl_chunk:\n'
        '                break\n'
        '            sys.stdout.write(lvgl_chunk)\n'
        '        lvgl_file.close()\n'
        '    except OSError:\n'
        '        pass\n'
        '    emit("__LVGL_LOG_END__")\n'
        + mp_marker
    )
    if mp_marker not in micropython:
        raise RuntimeError("MicroPython end marker changed")
    micropython = micropython.replace(mp_marker, mp_insert, 1)
    return shell, lua, micropython


def validate_sources():
    checks = (
        (["sh", "-n", str(SHELL_SOURCE)], 30),
        (["luac", "-p", str(LUA_SOURCE)], 30),
        ([sys.executable, "-m", "py_compile", str(MICROPYTHON_SOURCE)], 30),
    )
    env = os.environ.copy()
    env["PYTHONPYCACHEPREFIX"] = "/tmp/ove-hammer-pycache"
    for argv, timeout in checks:
        result = subprocess.run(
            argv, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, errors="replace", timeout=timeout,
        )
        if result.returncode != 0:
            raise RuntimeError(
                f"source validation failed ({result.returncode}): {argv!r}\n"
                f"{result.stdout}"
            )


def parse_block(text, begin, end):
    match = re.search(re.escape(begin) + r"\s*(.*?)" + re.escape(end), text, re.S)
    if not match:
        return {}
    values = {}
    for line in match.group(1).splitlines():
        fields = line.strip().split(None, 1)
        if len(fields) != 2:
            continue
        try:
            values[fields[0]] = int(fields[1])
        except ValueError:
            values[fields[0]] = fields[1]
    return values


def parse_key_values(text, marker):
    match = re.search(re.escape(marker) + r"([^\r\n]*)", text)
    if not match:
        return {}
    result = {}
    for field in match.group(1).strip().split():
        if "=" not in field:
            continue
        key, value = field.split("=", 1)
        if re.fullmatch(r"-?\d+", value):
            result[key] = int(value)
        else:
            try:
                result[key] = float(value)
            except ValueError:
                result[key] = value
    return result


def delta(before, after, keys):
    result = {}
    for key in keys:
        if isinstance(before.get(key), int) and isinstance(after.get(key), int):
            result[key] = after[key] - before[key]
    return result


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    index = round((len(ordered) - 1) * fraction)
    return ordered[index]


def distribution(values):
    if not values:
        return {}
    return {
        "count": len(values),
        "min": min(values),
        "p05": percentile(values, 0.05),
        "median": percentile(values, 0.50),
        "mean": sum(values) / len(values),
        "p95": percentile(values, 0.95),
        "p99": percentile(values, 0.99),
        "max": max(values),
    }


def playback_samples(text):
    block = re.search(
        r"__LVGL_LOG_BEGIN__\s*(.*?)__LVGL_LOG_END__", text, re.S
    )
    if not block:
        return [], {"reason": "LVGL log markers missing"}
    all_samples = []
    for match in SYSMON_RE.finditer(block.group(1)):
        all_samples.append(
            {key: int(value) for key, value in match.groupdict().items()}
        )

    # Find the startup redraw block, a settled idle run, then the first redraw
    # caused by the injected Play click. This excludes the stock intro animation.
    saw_intro = False
    idle = 0
    armed = False
    play_at = None
    for index, sample in enumerate(all_samples):
        if sample["redraw"] > 0:
            if armed:
                play_at = index
                break
            saw_intro = True
            idle = 0
        elif saw_intro:
            idle += 1
            if idle >= 5:
                armed = True
    if play_at is None:
        return [], {
            "reason": "no post-intro redraw; Play injection was not observed",
            "all_sysmon_samples": len(all_samples),
        }
    active = [
        sample for sample in all_samples[play_at:]
        if sample["redraw"] > 0 and sample["render_ms"] > 0
    ]
    return active, {
        "all_sysmon_samples": len(all_samples),
        "play_sample_index": play_at,
    }


def parse_summary(engine, mode, duration, identity, text, wall_s, network):
    before = parse_block(text, "__RT_SCOPE_BEFORE__", "__RT_SCOPE_BEFORE_END__")
    after = parse_block(text, "__RT_SCOPE_AFTER__", "__RT_SCOPE_AFTER_END__")
    fs_before = parse_block(text, "__FS_BEFORE__", "__FS_BEFORE_END__")
    fs_after = parse_block(text, "__FS_AFTER__", "__FS_AFTER_END__")
    samples, play = playback_samples(text)
    render = {}
    if samples:
        for field in ("fps", "refr_ms", "render_ms", "flush_ms", "cpu"):
            render[field] = distribution([sample[field] for sample in samples])

    summary = {
        "engine": engine,
        "driver": mode,
        "identity": identity,
        "requested_duration_s": duration,
        "wall_s": wall_s,
        "network_server": network,
        "network_mbps": (
            network.get("bytes", 0) * 8 /
            max(network.get("elapsed_s", 0), 0.001) / 1_000_000
        ),
        "playback_detection": play,
        "active_render_samples": len(samples),
        "render": render,
        "rt_scope_before": before,
        "rt_scope_after": after,
        "rt_scope_delta": delta(
            before, after,
            (
                "releases", "executions", "missed", "late_finish",
                "irq_overrun", "dispatch_samples", "svc_calls",
            ),
        ),
        "fs_before": fs_before,
        "fs_after": fs_after,
        "fs_delta": delta(fs_before, fs_after, set(fs_before) | set(fs_after)),
        "fault_lines": [
            line for line in text.splitlines()
            if re.search(
                r"memory-fault|HOST FAULT|Cannot allocate|Segmentation|PANIC|FATAL|assert",
                line,
                re.I,
            )
        ],
    }
    if mode == "shell":
        sqlite = parse_key_values(text, "__HAMMER_SQLITE__:")
        timing = parse_key_values(text, "__HAMMER_TIMING__:")
        try:
            sqlite["elapsed_s"] = float(
                Decimal(str(timing["ended_uptime"])) -
                Decimal(str(timing["started_uptime"]))
            )
        except (KeyError, InvalidOperation):
            sqlite["elapsed_s"] = None
        error = re.search(r"__HAMMER_DB_ERROR_BYTES__:(\d+)", text)
        sqlite["error_bytes"] = int(error.group(1)) if error else None
        summary["sqlite"] = sqlite
        match = re.search(r"__HAMMER_NETWORK__:(\{.*?\})", text)
        summary["network_guest"] = (
            json.loads(match.group(1)) if match else {"error": "unparsed"}
        )
    else:
        match = re.search(r"__HAMMER_SQLITE__:(\{.*?\})", text)
        summary["sqlite"] = (
            json.loads(match.group(1)) if match else {"error": "unparsed"}
        )
        match = re.search(r"__HAMMER_NETWORK__:(\{.*?\})", text)
        summary["network_guest"] = (
            json.loads(match.group(1)) if match else {"error": "unparsed"}
        )
    return summary


def run_one(
    engine, mode, duration, script, min_network_mbps, data_directory,
    expected_identity, reset_before,
):
    if reset_before:
        quiesce_target_storage()
        reset_target()
    identity = wait_for_target(engine, expected_identity)
    target_exec(f"mkdir -p {shlex.quote(data_directory)}", timeout=30)
    stage(f"/tmp/ove-hammer-{mode}", script)
    pi_http("reset")
    interpreter = {
        "shell": "/bin/sh",
        "lua": "/usr/bin/lua",
        "micropython": "/usr/bin/micropython -X heapsize=96K",
    }[mode]
    command = f"exec {interpreter} /tmp/ove-hammer-{mode} {duration}"
    if mode == "lua":
        command = (
            "export LUA_INIT='io.stdout:setvbuf(\"no\")'; " + command
        )
    print(f"[{engine}/{mode}] starting: {identity}", flush=True)
    started = time.monotonic()
    raw_path = RESULT_DIR / f"{engine}-{mode}.raw"
    try:
        result = run(
            target_argv(command),
            timeout=duration + 600,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        partial = error.stdout or ""
        if isinstance(partial, bytes):
            partial = partial.decode(errors="replace")
        raw_path.write_text(partial)
        raise RuntimeError(
            f"{engine}/{mode} exceeded its {duration + 600}s hard "
            f"deadline; partial output is in {raw_path}"
        ) from error
    wall_s = time.monotonic() - started
    text = result.stdout
    raw_path.write_text(text)
    if result.returncode != 0:
        raise RuntimeError(
            f"{engine}/{mode} SSH exited {result.returncode}; see {raw_path}"
        )
    if f"__HAMMER_END__:{mode}" not in text:
        raise RuntimeError(f"{engine}/{mode} did not finish; see {raw_path}")
    network = json.loads(pi_http("metrics"))
    summary = parse_summary(
        engine, mode, duration, identity, text, wall_s, network
    )
    lvgl = re.search(r"__LVGL_LOG_BEGIN__\s*(.*?)__LVGL_LOG_END__", text, re.S)
    (RESULT_DIR / f"{engine}-{mode}.lvgl").write_text(
        (lvgl.group(1) if lvgl else "")
    )

    database = data_directory + "/" + {
        "shell": "shell.db",
        "lua": "lua.db",
        "micropython": "micropython.db",
    }[mode]
    database_arg = shlex.quote(database)
    post = target_exec(
        f"printf '__DB__\\n'; sqlite3 {database_arg} "
        "'PRAGMA integrity_check;SELECT n FROM meta;SELECT count(*) FROM events;'; "
        "printf '__DB_END__\\n__RES__\\n'; cat /proc/lxp_resources; "
        "printf '__RES_END__\\n__FS__\\n'; cat /proc/lxp_fs; "
        "printf '__FS_END__\\n__RT__\\n'; cat /proc/rt_scope; "
        "printf '__RT_END__\\n__PROCS__\\n'; ps; printf '__PROCS_END__\\n'",
        timeout=120,
    )
    (RESULT_DIR / f"{engine}-{mode}.post").write_text(post)
    post_integrity = re.search(r"__DB__\s*([^\s]+)", post)
    summary["post_integrity"] = post_integrity.group(1) if post_integrity else None

    failures = []
    sqlite = summary.get("sqlite", {})
    if sqlite.get("transactions") is None or sqlite.get("transactions", 0) <= 0:
        failures.append("no SQLite transactions")
    if sqlite.get("integrity") not in (None, "ok"):
        failures.append(f"SQLite integrity={sqlite.get('integrity')}")
    if sqlite.get("error") not in (None, "null", "none"):
        failures.append(f"SQLite error={sqlite.get('error')}")
    if mode == "shell" and sqlite.get("error_bytes") not in (None, 0):
        failures.append(f"SQLite stderr bytes={sqlite.get('error_bytes')}")
    if summary["post_integrity"] != "ok":
        failures.append(f"post-run integrity={summary['post_integrity']}")
    transactions = sqlite.get("transactions")
    expected_rows = transactions * 8 if isinstance(transactions, int) else None
    if sqlite.get("rows") != expected_rows:
        failures.append(f"rows={sqlite.get('rows')} expected={expected_rows}")
    if sqlite.get("meta") != expected_rows:
        failures.append(f"meta={sqlite.get('meta')} expected={expected_rows}")
    expected_live = min(expected_rows, 128) if expected_rows is not None else None
    if sqlite.get("live_rows") != expected_live:
        failures.append(
            f"live_rows={sqlite.get('live_rows')} expected={expected_live}"
        )
    if network.get("requested_s") != duration:
        failures.append(f"server deadline={network.get('requested_s')} expected={duration}")
    if network.get("completed") != 1 or network.get("errors") != 0:
        failures.append(
            f"server completion={network.get('completed')} errors={network.get('errors')}"
        )
    if network.get("active") != 0:
        failures.append(f"server still has {network.get('active')} active stream(s)")
    if network.get("elapsed_s", 0) < duration - 1:
        failures.append(f"server early EOF elapsed={network.get('elapsed_s')}")
    if summary["network_mbps"] < min_network_mbps:
        failures.append(
            f"network throughput={summary['network_mbps']:.3f}Mbps "
            f"minimum={min_network_mbps:.3f}Mbps"
        )
    guest_network = summary.get("network_guest", {})
    if guest_network.get("error") not in (None, "none"):
        failures.append(f"network error={guest_network.get('error')}")
    if mode != "shell" and guest_network.get("bytes", 0) <= 0:
        failures.append("guest reported no network traffic")
    if summary.get("active_render_samples", 0) < 30:
        failures.append("too few active LVGL samples")
    if summary.get("rt_scope_delta", {}).get("missed", 0) != 0:
        failures.append(f"RT misses={summary['rt_scope_delta'].get('missed')}")
    failures.extend(summary.get("fault_lines", []))
    summary["failures"] = failures
    summary["status"] = "PASS" if not failures else "FAIL"

    (RESULT_DIR / f"{engine}-{mode}.json").write_text(
        json.dumps(summary, indent=2) + "\n"
    )
    print(
        f"[{engine}/{mode}] done wall={wall_s:.1f}s "
        f"sqlite={summary['sqlite'].get('transactions')} "
        f"net={summary['network_mbps']:.3f}Mbps "
        f"render_samples={summary['active_render_samples']} "
        f"missed={summary['rt_scope_delta'].get('missed')} "
        f"status={summary['status']}",
        flush=True,
    )
    if failures:
        raise RuntimeError(f"{engine}/{mode} validation failed: {failures}")
    return summary


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--duration", type=int, default=300)
    parser.add_argument(
        "--engines", nargs="+", default=["freertos", "nuttx", "zephyr"]
    )
    parser.add_argument(
        "--modes", nargs="+", choices=("shell", "lua", "micropython"),
        default=["shell", "lua", "micropython"]
    )
    parser.add_argument("--skip-flash", action="store_true")
    parser.add_argument(
        "--allow-dirty-firmware", action="store_true",
        help="permit an explicitly selected dirty artifact for development testing",
    )
    parser.add_argument(
        "--min-network-mbps", type=float, default=0.5,
        help="fail a run below this actual server-side throughput",
    )
    parser.add_argument(
        "--data-directory", default="/data/.ove-hammer",
        help="writable target directory used for benchmark databases",
    )
    args = parser.parse_args()
    if not re.fullmatch(r"/data/[A-Za-z0-9_.-]+", args.data_directory):
        parser.error("--data-directory must be one direct child of /data")
    validate_sources()
    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    deploy_server()
    shell, lua, micropython = instrumented_scripts(args.data_directory)
    scripts = {"shell": shell, "lua": lua, "micropython": micropython}
    combined = []
    for engine in args.engines:
        expected_identity = firmware_identity(
            engine, args.allow_dirty_firmware
        )
        if not args.skip_flash:
            print(
                f"[{engine}] flashing {expected_identity} via pi: "
                f"{FIRMWARE[engine]}", flush=True,
            )
            flash_target(engine)
        for mode_index, mode in enumerate(args.modes):
            combined.append(run_one(
                engine, mode, args.duration, scripts[mode],
                args.min_network_mbps, args.data_directory, expected_identity,
                args.skip_flash or mode_index != 0,
            ))
    (RESULT_DIR / "comparison.json").write_text(
        json.dumps(combined, indent=2) + "\n"
    )


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr, flush=True)
        raise
