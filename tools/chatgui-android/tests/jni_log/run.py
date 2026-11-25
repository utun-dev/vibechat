#!/usr/bin/env python3
"""Linux host: собирает настоящий Android bridge с JNI из JDK и запускает fault injection без телефона."""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java-home", type=Path)
    args = parser.parse_args()
    directory = Path(__file__).resolve().parent
    root = directory.parents[3]
    javac = shutil.which("javac")
    java_home = args.java_home or (Path(javac).resolve().parents[1] if javac else None)
    if java_home is None or not (java_home / "include/jni.h").is_file():
        parser.error("JDK JNI headers missing; pass --java-home")
    compiler = shlex.split(os.environ.get("CC", "cc"))
    includes = [directory, java_home / "include", java_home / "include/linux",
                root / "src", root / "src/transport_layer", root / "src/routing_layer",
                root / "src/chat", root / "src/media_async", root / "lib"]
    with tempfile.TemporaryDirectory(prefix="utun-jni-log-") as output:
        binary = Path(output) / "test_jni_log"
        command = compiler + ["-std=c99", "-D_DEFAULT_SOURCE", "-D__ANDROID__", "-DUSE_OPENSSL",
                   "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-pthread",
                   # Android JNI принимает JNIEnv**, JDK — void**; ABI одинаков, чужие callbacks здесь не вызываются.
                   "-Wno-incompatible-pointer-types"]
        command += [f"-I{path}" for path in includes]
        command += [str(directory / "test_jni_log.c"), "-o", str(binary)]
        subprocess.run(command, check=True, timeout=90)
        subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == "__main__":
    main()
