#!/usr/bin/env python3
"""Подготовить Android-сборку на Linux x86_64: JDK → SDK/NDK/CMake → native-зависимости → Gradle."""

import argparse
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

import prepare_dependencies as deps


REPO = Path(__file__).resolve().parents[1]
PROJECT = REPO / "tools/chatgui-android"


def properties(path):
    """Нужны только локальные пути; остальные строки properties сохраняются при записи."""
    result = {}
    if path.is_file():
        for line in path.read_text().splitlines():
            if line.lstrip().startswith(("#", "!")) or "=" not in line:
                continue
            key, value = line.split("=", 1)
            result[key.strip()] = value.strip().replace("\\:", ":").replace("\\ ", " ").replace("\\\\", "\\")
    return result


def java_home():
    """Gradle 8.13 требует JDK 17–23; проверяем JDK, а не только наличие java CLI."""
    home = os.environ.get("JAVA_HOME")
    compiler = Path(home) / "bin/javac" if home else Path(shutil.which("javac") or "/missing-jdk")
    home = compiler.resolve().parent.parent
    if not compiler.is_file() or not (home / "bin/java").is_file():
        raise RuntimeError("Нужен JDK 17: Ubuntu/Debian — sudo apt-get install openjdk-17-jdk; проверьте JAVA_HOME")
    version = subprocess.run([str(home / "bin/java"), "-version"], capture_output=True, text=True, check=True)
    match = re.search(r'version "(\d+)', version.stdout + version.stderr)
    if not match or not 17 <= int(match[1]) <= 23:
        raise RuntimeError("Gradle 8.13 поддерживает JDK 17–23; установите openjdk-17-jdk и укажите его в JAVA_HOME")
    return home


def configure_project(sdk, cmake):
    """Меняем только пути инструментов, не теряя прочие локальные настройки."""
    path = PROJECT / "local.properties"
    values = {"sdk.dir": sdk, "cmake.dir": cmake}
    lines = path.read_text().splitlines() if path.is_file() else []
    # NDK выбирает Gradle по ndkVersion; устаревший ndk.dir вызывает предупреждения AGP.
    lines = [line for line in lines if line.split("=", 1)[0].strip() not in {*values, "ndk.dir"}]
    for key, value in values.items():
        escaped = str(value).replace("\\", "\\\\").replace(":", "\\:").replace(" ", "\\ ")
        lines.append(f"{key}={escaped}")
    deps.install(("\n".join(lines) + "\n").encode(), path)
    deps.log(f"Пути SDK/CMake записаны в {path}; NDK выбирается по ndkVersion в Gradle")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, help="каталог Android SDK; иначе окружение, local.properties или ~/Android/Sdk")
    parser.add_argument("--jobs", type=int, default=4, help="параллельных компиляций OpenSSL (по умолчанию 4)")
    parser.add_argument("--cache", type=Path, default=REPO / ".cache/dependencies/downloads", help="кеш скачанных архивов")
    parser.add_argument("--if-needed", action="store_true", help="не пересобирать проверенные native-зависимости")
    args = parser.parse_args()
    deps.load_proxy(REPO)
    if args.jobs < 1:
        parser.error("--jobs должен быть положительным")
    if sys.version_info < (3, 9):
        raise RuntimeError("Нужен Python 3.9 или новее")
    if platform.system() != "Linux" or platform.machine() != "x86_64":
        raise RuntimeError("Подготовка Android OpenSSL поддерживает Linux x86_64")
    missing = [name for name in ("curl", "tar", "patch", "make", "perl", "bash") if not shutil.which(name)]
    if missing:
        raise RuntimeError("Не найдены инструменты: " + ", ".join(missing) +
                           "; Ubuntu/Debian: sudo apt-get install curl tar patch make perl openjdk-17-jdk")
    jdk = java_home()

    # Версии платформы и NDK берём из проекта, чтобы подготовка соответствовала APK-сборке.
    build = (PROJECT / "app/build.gradle.kts").read_text()
    api = re.search(r"compileSdk\s*=\s*(\d+)", build)
    ndk_version = re.search(r'ndkVersion\s*=\s*"([^"]+)"', build)
    if not api or not ndk_version:
        raise RuntimeError(f"В {PROJECT / 'app/build.gradle.kts'} не найдены compileSdk и ndkVersion")
    saved = properties(PROJECT / "local.properties")
    sdk = (args.sdk or Path(os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT") or
                           saved.get("sdk.dir") or Path.home() / "Android/Sdk")).expanduser().resolve()
    cache = args.cache.expanduser().resolve()
    cache.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, JAVA_HOME=str(jdk), ANDROID_HOME=str(sdk), ANDROID_SDK_ROOT=str(sdk))
    deps.log(f"JDK={jdk}, SDK={sdk}, Android API={api[1]}, NDK={ndk_version[1]}")

    manager = sdk / "cmdline-tools/latest/bin/sdkmanager"
    if not manager.is_file():
        # Готовый официальный архив с фиксированной SHA-256; JDK остаётся системным.
        archive = deps.download("android-commandlinetools.zip", cache, False)
        with tempfile.TemporaryDirectory(prefix="android-sdk-", dir=cache.parent) as tmp:
            with zipfile.ZipFile(archive) as source:
                source.extractall(tmp)
            deps.copy_tree(Path(tmp) / "cmdline-tools", sdk / "cmdline-tools/latest")
        for tool in manager.parent.iterdir():
            if tool.is_file():
                tool.chmod(tool.stat().st_mode | 0o111)
        if not manager.is_file():
            raise RuntimeError(f"Архив command-line tools не создал {manager}")

    ndk = sdk / "ndk" / ndk_version[1]
    cmake = sdk / "cmake/3.22.1"
    # Build Tools 35.0.0 — версия по умолчанию для AGP 8.13; CMake 3.22.1 — для externalNativeBuild.
    packages = {
        "platform-tools": sdk / "platform-tools/adb",
        f"platforms;android-{api[1]}": sdk / f"platforms/android-{api[1]}/android.jar",
        "build-tools;35.0.0": sdk / "build-tools/35.0.0/aapt2",
        f"ndk;{ndk_version[1]}": ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/clang",
        "cmake;3.22.1": cmake / "bin/cmake",
    }
    missing = [name for name, marker in packages.items() if not marker.is_file()]
    if missing:
        deps.log("При запросе sdkmanager прочитайте и примите лицензии SDK в терминале")
        deps.run([manager, f"--sdk_root={sdk}", "--licenses"], env=env)
        deps.run([manager, f"--sdk_root={sdk}", *missing], env=env)
    for name, marker in packages.items():
        if not marker.is_file():
            raise RuntimeError(f"SDK-компонент {name} не установлен: {marker}; проверьте сеть и принятие лицензий SDK")

    configure_project(sdk, cmake)
    for component in ("common", "android"):
        deps.run([sys.executable, REPO / "tools/prepare_dependencies.py", "--only", component,
                  "--ndk", ndk, "--jobs", args.jobs, "--cache", cache, *(["--if-needed"] if args.if_needed else [])], env=env)
    # Wrapper сам скачает закреплённый Gradle и сообщит о проблемах сети или JDK.
    deps.run(["bash", PROJECT / "gradlew", "--version"], cwd=PROJECT, env=env)
    deps.log("Android-сборка подготовлена. Собрать APK: bash tools/chatgui-android/build.sh noinstall")


if __name__ == "__main__":
    try:
        deps.configure_output()
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError, zipfile.BadZipFile) as error:
        print(f"ERROR: {error}", file=sys.stderr, flush=True)
        sys.exit(1)
