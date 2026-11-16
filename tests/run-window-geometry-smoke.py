#!/usr/bin/env python3
"""Check first-camera window placement in disposable X11/WM sessions."""

import argparse
import importlib.util
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import time


CASES = ("normal-window", "maximized-window")


def private_session(args):
    root = Path(args.private_root)
    ready = root / "openbox-ready"
    with (root / "openbox.log").open("w") as log:
        # Openbox runs this only after taking ownership of the private display;
        # waiting for the marker avoids racing its decoration/maximize setup.
        wm = subprocess.Popen(["openbox", "--sm-disable", "--startup",
                               "touch -- " + shlex.quote(str(ready))],
                              stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 5
            while not ready.exists():
                if wm.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("The private Openbox window manager did not become ready")
                time.sleep(0.05)
            return subprocess.run([
                str(args.test_executable),
                "openingEmbeddedCameraPreservesTopLevelGeometry:" + args.case,
            ], timeout=60).returncode
        finally:
            wm.terminate()
            try:
                wm.wait(timeout=5)
            except subprocess.TimeoutExpired:
                wm.kill()
                wm.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-executable", required=True, type=Path)
    parser.add_argument("--private-root", help=argparse.SUPPRESS)
    parser.add_argument("--case", choices=CASES, help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.test_executable = args.test_executable.resolve()
    if args.private_root:
        if not args.case:
            parser.error("A private session requires its test case")
        return private_session(args)
    if not args.test_executable.is_file() or not os.access(args.test_executable, os.X_OK):
        parser.error("The test executable must exist and be executable")
    for program in ("xvfb-run", "xauth", "dbus-run-session", "openbox"):
        if not shutil.which(program):
            raise RuntimeError("Required isolated window test dependency is missing: " + program)

    # Share the IME runner's bounded process-group teardown, so an interrupted
    # or timed-out test cannot leave Xvfb, Openbox or Chromium descendants alive.
    spec = importlib.util.spec_from_file_location(
        "inputmethod_runner", Path(__file__).with_name("run-inputmethod-smoke.py"))
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    failed = False
    for case in CASES:
        # Each row starts a new application, display, WM and D-Bus session.
        # Reusing the application would hide the first-WebEngine path.
        with tempfile.TemporaryDirectory(prefix="matterdesk-geometry-") as directory:
            root = Path(directory)
            env = dict(os.environ)
            for name in ("DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY", "AT_SPI_BUS_ADDRESS",
                         "DBUS_SESSION_BUS_ADDRESS", "DBUS_SESSION_BUS_PID",
                         "DBUS_STARTER_ADDRESS", "DBUS_STARTER_BUS_TYPE"):
                env.pop(name, None)
            for name in ("config", "data", "cache", "state", "runtime"):
                (root / name).mkdir(mode=0o700)
            env.update(HOME=directory, XDG_CONFIG_HOME=str(root / "config"),
                       XDG_DATA_HOME=str(root / "data"), XDG_CACHE_HOME=str(root / "cache"),
                       XDG_STATE_HOME=str(root / "state"), XDG_RUNTIME_DIR=str(root / "runtime"),
                       QT_QPA_PLATFORM="xcb", QT_IM_MODULE="compose", QT_SCALE_FACTOR="1",
                       QTWEBENGINE_CHROMIUM_FLAGS="--disable-gpu")
            env.pop("QTWEBENGINE_DISABLE_SANDBOX", None)
            # No activation directories: this bus cannot start desktop services.
            bus = root / "dbus.conf"
            bus.write_text('''<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen>
<auth>EXTERNAL</auth><policy context="default"><allow send_destination="*"/>
<allow receive_sender="*"/><allow own="*"/></policy></busconfig>''')
            command = ["xvfb-run", "-a", "-s", "-screen 0 1920x1200x24 -nolisten tcp",
                       "dbus-run-session", "--config-file", str(bus), "--", sys.executable,
                       str(Path(__file__).resolve()), "--private-root", directory,
                       "--test-executable", str(args.test_executable), "--case", case]
            print("Isolated X11 camera geometry:", case, flush=True)
            try:
                result = runner.run_isolated(command, env, timeout=90)
                failed |= result != 0
            finally:
                log = root / "openbox.log"
                if log.exists() and log.stat().st_size:
                    print("Private Openbox log (" + case + "):", file=sys.stderr)
                    print(log.read_text(), file=sys.stderr)
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
