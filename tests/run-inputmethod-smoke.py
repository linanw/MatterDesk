#!/usr/bin/env python3
"""Exercise the packaged Qt Fcitx client with a disposable real Pinyin daemon.

Never uses or changes the user's D-Bus session, Fcitx profile, X display, or
configuration. The daemon enables only local Pinyin and required dependencies;
cloud Pinyin, notifications and desktop integrations are disabled.
"""
import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time


def stop_process_group(process):
    """Stop every process in our private session, including grandchildren."""
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        process.wait()
        return
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        process.poll()  # Reap the direct child even if descendants remain.
        try:
            os.killpg(process.pid, 0)
        except ProcessLookupError:
            process.wait()
            return
        time.sleep(0.05)
    # Do this even when the group leader has exited: a daemon or display child
    # may still be alive after ignoring TERM or outliving xvfb-run's shell.
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait(timeout=5)


def run_isolated(command, env, timeout=90):
    process = None
    previous_term = signal.signal(signal.SIGTERM, lambda signum, _frame: sys.exit(128 + signum))
    try:
        process = subprocess.Popen(command, env=env, start_new_session=True)
        return process.wait(timeout=timeout)
    finally:
        try:
            if process is not None:
                stop_process_group(process)
        finally:
            signal.signal(signal.SIGTERM, previous_term)


def private_session(args):
    root = Path(args.private_root)
    env = dict(os.environ)
    for relative in ("config", "data", "cache", "state", "runtime"):
        (root / relative).mkdir(mode=0o700, exist_ok=True)
    env.update(HOME=str(root), XDG_CONFIG_HOME=str(root / "config"),
               XDG_DATA_HOME=str(root / "data"), XDG_CACHE_HOME=str(root / "cache"),
               XDG_STATE_HOME=str(root / "state"), XDG_RUNTIME_DIR=str(root / "runtime"),
               QT_QPA_PLATFORM="xcb", QT_IM_MODULE="fcitx", QT_SCALE_FACTOR="1",
               MATTERDESK_REQUIRE_INPUT_METHOD="1", MATTERDESK_TEST_PRIVATE_DBUS="1")
    env.pop("WAYLAND_DISPLAY", None)
    profile = root / "config/fcitx5/profile"
    profile.parent.mkdir()
    profile.write_text("""[Groups/0]
Name=Regression
Default Layout=us
DefaultIM=pinyin

[Groups/0/Items/0]
Name=keyboard-us
Layout=

[Groups/0/Items/1]
Name=pinyin
Layout=

[GroupOrder]
0=Regression
""")
    daemon_env = dict(env)
    for name in ("LD_LIBRARY_PATH", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
                 "FONTCONFIG_FILE", "FONTCONFIG_PATH", "APPDIR"):
        daemon_env.pop(name, None)
    with (root / "fcitx5.log").open("w") as log:
        daemon = subprocess.Popen(["fcitx5", "-D", "--disable", "all", "--enable",
                                   "keyboard,dbus,dbusfrontend,pinyin,punctuation", "-u", "dummy"],
                                  env=daemon_env, stdout=log, stderr=subprocess.STDOUT)
        try:
            ready = False
            readiness_deadline = time.monotonic() + 5
            while time.monotonic() < readiness_deadline:
                if daemon.poll() is not None:
                    break
                # Query the bus itself, without activating a second default
                # daemon while our explicitly restricted daemon is starting.
                try:
                    probe = subprocess.run(["dbus-send", "--session", "--print-reply=literal",
                                            "--dest=org.freedesktop.DBus", "/org/freedesktop/DBus",
                                            "org.freedesktop.DBus.NameHasOwner", "string:org.fcitx.Fcitx5"],
                                           env=daemon_env, capture_output=True, text=True,
                                           timeout=min(2, max(0.1, readiness_deadline - time.monotonic())))
                except subprocess.TimeoutExpired:
                    continue
                if probe.returncode == 0 and "true" in probe.stdout:
                    ready = True
                    break
                time.sleep(0.1)
            if not ready:
                raise RuntimeError("The isolated Fcitx5 daemon did not become ready")
            command = [str(args.test_executable)]
            if args.appdir:
                appdir = args.appdir.resolve()
                env["MATTERDESK_INPUTMETHOD_PLUGIN_ROOT"] = str(appdir / "usr/plugins/platforminputcontexts")
                command = ["bash", "-c", 'export APPDIR="$1"; source "$APPDIR/usr/scripts/appimage-env.sh"; shift; exec "$@"',
                           "inputmethod-smoke", str(appdir), *command]
            return subprocess.run(command, env=env, timeout=60).returncode
        finally:
            try:
                subprocess.run(["fcitx5-remote", "-e"], env=daemon_env, capture_output=True, timeout=5)
            except subprocess.TimeoutExpired:
                daemon.terminate()
            try:
                daemon.wait(timeout=5)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-executable", required=True, type=Path)
    parser.add_argument("--appdir", type=Path)
    parser.add_argument("--private-root", help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.test_executable = args.test_executable.resolve()
    if args.appdir:
        plugin = args.appdir / "usr/plugins/platforminputcontexts/libfcitx5platforminputcontextplugin.so"
        if not plugin.is_file():
            raise RuntimeError("AppImage is missing its Fcitx5 Qt input method plugin: " + str(plugin))
    if args.private_root:
        return private_session(args)
    for program in ("dbus-run-session", "dbus-send", "xvfb-run", "fcitx5", "fcitx5-remote"):
        if not shutil.which(program):
            raise RuntimeError(f"Required real IME smoke dependency is missing: {program}")
    with tempfile.TemporaryDirectory(prefix="matterdesk-ime-") as directory:
        env = dict(os.environ)
        for name in ("DBUS_SESSION_BUS_ADDRESS", "DBUS_SESSION_BUS_PID", "DBUS_STARTER_ADDRESS",
                     "DBUS_STARTER_BUS_TYPE", "DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY"):
            env.pop(name, None)
        # Isolate the parent bus and display processes too. A D-Bus activation
        # must never inherit the real desktop HOME or configuration directories.
        root = Path(directory)
        for relative in ("config", "data", "cache", "state", "runtime"):
            (root / relative).mkdir(mode=0o700)
        env.update(HOME=directory, XDG_CONFIG_HOME=str(root / "config"),
                   XDG_DATA_HOME=str(root / "data"), XDG_CACHE_HOME=str(root / "cache"),
                   XDG_STATE_HOME=str(root / "state"), XDG_RUNTIME_DIR=str(root / "runtime"))
        # No standard service directories: this bus cannot auto-start desktop
        # portals, another IME, or any user-session service during the probe.
        bus_config = root / "dbus.conf"
        bus_config.write_text('''<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen><auth>EXTERNAL</auth>
<policy context="default"><allow send_destination="*"/><allow receive_sender="*"/><allow own="*"/></policy>
</busconfig>''')
        command = ["xvfb-run", "-a", "-s", "-screen 0 1024x768x24 -nolisten tcp", "dbus-run-session",
                   "--config-file", str(bus_config), "--",
                   sys.executable, str(Path(__file__).resolve()), "--private-root", directory,
                   "--test-executable", str(args.test_executable)]
        if args.appdir:
            command += ["--appdir", str(args.appdir.resolve())]
        try:
            return run_isolated(command, env)
        finally:
            log = Path(directory) / "fcitx5.log"
            if log.exists() and log.stat().st_size:
                print("Isolated Fcitx5 daemon log:", file=sys.stderr)
                print(log.read_text(), file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main())
