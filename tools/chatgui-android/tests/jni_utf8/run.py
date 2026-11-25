#!/usr/bin/env python3
"""Linux: проверить обычный UTF-8 настоящего Android bridge в JVM с -Xcheck:jni."""
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
        parser.error("JDK with JNI headers/libjvm required; pass --java-home")
    server = java_home / "lib/server"
    includes = [directory.parent / "jni_log", java_home / "include", java_home / "include/linux",
                root / "src", root / "src/transport_layer", root / "src/routing_layer",
                root / "src/chat", root / "src/media_async", root / "lib"]
    with tempfile.TemporaryDirectory(prefix="utun-jni-utf8-") as output:
        binary = Path(output) / "test_jni_utf8"
        subprocess.run([str(java_home / "bin/javac"), "-d", output, str(directory / "EventSink.java")], check=True, timeout=30)
        command = shlex.split(os.environ.get("CC", "cc")) + ["-std=c99", "-D_DEFAULT_SOURCE", "-D__ANDROID__", "-DUSE_OPENSSL",
                   "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-pthread", "-Wno-incompatible-pointer-types"]
        command += [f"-I{path}" for path in includes]
        command += [str(directory / "test_jni_utf8.c"), str(root / "src/db_storage.c"), str(root / "lib/debug_config.c"),
                    str(root / "src/chat/chat_channel.c"), str(root / "src/chat/object_link.c"), str(root / "src/media_async/attachment.c"),
                    f"-L{server}", f"-Wl,-rpath,{server}", "-ljvm", "-o", str(binary)]
        subprocess.run(command, check=True, timeout=90)
        subprocess.run([str(binary)], cwd=output, check=True, timeout=30)


if __name__ == "__main__":
    main()
