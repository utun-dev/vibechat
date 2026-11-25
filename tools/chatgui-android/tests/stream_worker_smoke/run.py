#!/usr/bin/env python3
"""Run production APK media workers on a device without replacing the installed app."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import zipfile


def main():
    project = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("serial", help="adb serial of the test device")
    parser.add_argument("--sdk", type=Path, default=os.getenv("ANDROID_HOME") or os.getenv("ANDROID_SDK_ROOT"))
    parser.add_argument("--apk", type=Path, default=project / "app/build/outputs/apk/debug/app-debug.apk")
    args = parser.parse_args()
    sdk = args.sdk
    if sdk is None:
        properties = project / "local.properties"
        if properties.exists():
            for line in properties.read_text().splitlines():
                if line.startswith("sdk.dir="):
                    sdk = Path(line.split("=", 1)[1])
    if sdk is None or not args.apk.is_file():
        parser.error("Build assembleDebug first and specify --sdk or sdk.dir in local.properties")
    android = sdk / "platforms/android-36/android.jar"
    versions = sorted((sdk / "build-tools").glob("*/d8"), key=lambda p: tuple(int(n) for n in p.parent.name.split(".")))
    if not android.is_file() or not versions:
        parser.error("Android API 36 and build-tools with d8 are required")
    adb = ["adb", "-s", args.serial]
    remote = "/data/local/tmp/utun-stream-worker-smoke"

    def run(*command):
        subprocess.run([str(part) for part in command], check=True)

    with tempfile.TemporaryDirectory(prefix="utun-stream-worker-smoke-") as directory:
        work = Path(directory)
        classes = work / "classes"
        classes.mkdir()
        run("javac", "-cp", android, "-d", classes, Path(__file__).with_name("StreamWorkerSmoke.java"))
        jar = work / "smoke.jar"
        run(versions[-1], "--lib", android, "--min-api", "26", "--output", jar, classes / "StreamWorkerSmoke.class")
        frame = work / "key.h264"
        run("ffmpeg", "-nostdin", "-loglevel", "error", "-f", "lavfi", "-i", "color=size=128x72:rate=30:color=blue",
            "-frames:v", "1", "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
            "-x264-params", "repeat-headers=1", "-f", "h264", frame)
        abi = subprocess.check_output(adb + ["shell", "getprop", "ro.product.cpu.abi"], text=True).strip()
        libraries = []
        with zipfile.ZipFile(args.apk) as apk:
            for name in apk.namelist():
                if name.startswith(f"lib/{abi}/") and name.endswith(".so"):
                    target = work / Path(name).name
                    target.write_bytes(apk.read(name))
                    libraries.append(target)
        if not libraries:
            parser.error(f"APK has no native libraries for {abi}")
        run(*adb, "shell", "mkdir", "-p", remote)
        run(*adb, "push", jar, frame, *libraries, remote + "/")
        run(*adb, "push", args.apk, remote + "/app.apk")
        command = [f"CLASSPATH={remote}/smoke.jar:{remote}/app.apk", "app_process",
                   f"-Djava.library.path={remote}", remote, "StreamWorkerSmoke", remote + "/key.h264"]
        print("Testing real MediaCodec/Opus/AudioTrack: 500ms video stall, EOF and reset; audio content is silence", flush=True)
        run(*adb, "shell", " ".join(shlex.quote(part) for part in command))


if __name__ == "__main__":
    main()
