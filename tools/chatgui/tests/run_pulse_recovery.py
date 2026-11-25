#!/usr/bin/env python3
"""Изолированный PipeWire/PulseAudio с виртуальным sink; системный сервер не затрагивается."""
import os
import json
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time


def wait_for(predicate, seconds=10):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.02)
    raise RuntimeError("private audio server/test did not reach expected state")


def main():
    for executable in ("pipewire", "pipewire-pulse", "wireplumber", "dbus-run-session", "pactl"):
        if not shutil.which(executable):
            raise RuntimeError(f"required executable missing: {executable}")
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else "tools/chatgui/build/test_audio_recovery").resolve()
    with tempfile.TemporaryDirectory(prefix="utun-pulse-") as root:
        directory = Path(root)
        config = directory / "config/wireplumber/wireplumber.conf.d"
        config.mkdir(parents=True)
        (config / "test.conf").write_text('''wireplumber.profiles = {
            main = {
                monitor.alsa = disabled
                monitor.alsa-midi = disabled
                monitor.bluez = disabled
                monitor.bluez-midi = disabled
                monitor.v4l2 = disabled
                monitor.libcamera = disabled
            }
        }''')
        env = os.environ.copy()
        env.update(XDG_RUNTIME_DIR=root, PIPEWIRE_RUNTIME_DIR=root, XDG_CONFIG_HOME=str(directory / "config"),
                   XDG_STATE_HOME=str(directory / "state"), PIPEWIRE_REMOTE="pipewire-0", PULSE_SERVER=f"unix:{root}/pulse/native")
        processes, logs = [], []

        def launch(command, label):
            log = (directory / f"{label}.log").open("w+")
            logs.append(log)
            process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            processes.append(process)
            return process

        def pulse_ready():
            return subprocess.run(["pactl", "info"], env=env, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL, timeout=2).returncode == 0

        def pactl(*args):
            return subprocess.check_output(["pactl", *args], env=env, text=True, timeout=5)

        def create_sinks():
            for name in ("utun_test_main", "utun_test_voice"):
                pactl("load-module", "module-null-sink", f"sink_name={name}")
            pactl("set-default-sink", "utun_test_main")
            pactl("set-default-source", "utun_test_main.monitor")

        try:
            launch(["pipewire"], "pipewire")
            wait_for(lambda: (directory / "pipewire-0").exists())
            launch(["dbus-run-session", "--", "wireplumber"], "wireplumber")
            pulse = launch(["pipewire-pulse"], "pulse-before")
            wait_for(pulse_ready)
            create_sinks()
            test = launch([str(binary), "--pulse-integration", root], "client")
            wait_for(lambda: (directory / "idle_ready").exists() or test.poll() is not None)
            if test.poll() is not None:
                raise RuntimeError("client failed during startup idle")
            streams = json.loads(pactl("--format=json", "list", "sink-inputs"))
            if streams:
                raise RuntimeError("startup idle must not create a playback stream")
            wait_for(lambda: (directory / "routes_ready").exists() or test.poll() is not None)
            if test.poll() is not None:
                raise RuntimeError("client failed before route move")
            sinks = json.loads(pactl("--format=json", "list", "sinks"))
            main_sink = next(sink["index"] for sink in sinks if sink["name"] == "utun_test_main")
            streams = json.loads(pactl("--format=json", "list", "sink-inputs"))
            main_streams = [stream for stream in streams if stream["sink"] == main_sink]
            if len(main_streams) != 1:
                raise RuntimeError("expected one shared default stream")
            # Явно выбранный stream имеет DONT_MOVE; default stream разрешено перемещать сервером.
            pactl("move-sink-input", str(main_streams[0]["index"]), "utun_test_voice")
            (directory / "moved").touch()
            wait_for(lambda: (directory / "ready").exists() or test.poll() is not None)
            if test.poll() is not None:
                raise RuntimeError("client failed before server restart")
            pulse.terminate(); pulse.wait(timeout=5)
            wait_for(lambda: (directory / "closed").exists() or test.poll() is not None)
            launch(["pipewire-pulse"], "pulse-after")
            wait_for(pulse_ready)
            create_sinks()
            wait_for(lambda: (directory / "idle_closed").exists() or test.poll() is not None)
            if not (directory / "idle_closed").exists():
                raise RuntimeError("client failed before final idle close")
            streams = json.loads(pactl("--format=json", "list", "sink-inputs"))
            if streams:
                raise RuntimeError("idle close must remove the playback stream")
            if test.wait(timeout=15) != 0:
                raise RuntimeError("client recovery failed")
            logs[3].flush(); logs[3].seek(0)
            print(logs[3].read())
            print("private PulseAudio idle stream removal and server restart: PASS")
        except Exception:
            for log in logs:
                log.flush(); log.seek(0)
                print(f"--- {Path(log.name).name} ---\n{log.read()}", file=sys.stderr)
            raise
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL); process.wait()
            for log in logs:
                log.close()


if __name__ == "__main__":
    main()
