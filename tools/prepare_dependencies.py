#!/usr/bin/env python3
"""Скачать внешние зависимости uTun и собрать Android OpenSSL и настольную rlottie.

  python3 tools/prepare_dependencies.py                  # общие зависимости и desktop rlottie
  python3 tools/prepare_dependencies.py --only common    # SQLite, miniaudio, Silero
  python3 tools/prepare_dependencies.py --only sodium --sodium-target TARGET  # статическая libsodium для CC
  python3 tools/prepare_dependencies.py --only wintun    # DLL для Windows x64, включая кросс-сборку
  python3 tools/prepare_dependencies.py --only android   # ONNX Runtime и OpenSSL для двух ABI
  python3 tools/prepare_dependencies.py --only rlottie   # исходники + локальные патчи + сборка
  python3 tools/prepare_dependencies.py --only dnsmasq   # архив исходников dnsmasq для build.sh
  python3 tools/prepare_dependencies.py --output /tmp/deps-check --jobs 4

Версии и SHA-256 закреплены ниже. Архивы остаются в .cache/dependencies/downloads;
--offline использует только этот кеш. --output меняет каталог назначения всех файлов.
Системные пакеты и Android NDK скрипт не устанавливает. Нужны Python 3.9+, curl,
tar, patch, CMake и C/C++ компиляторы; для Android также Perl, make и NDK 29.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import zipfile
from urllib.parse import urlsplit


REPO = Path(__file__).resolve().parents[1]
PATCHES = REPO / "tools/dependency_patches"
RLOTTIE = "ea06d2f29ba01b8d06c00a838d107f5e484ae59b"
NDK_VERSION = "29.0.14206865"
ABIS = {"arm64-v8a": "android-arm64", "x86_64": "android-x86_64"}
ANDROID_CPP = Path("tools/chatgui-android/app/src/main/cpp")

# Только официальные источники. При смене версии обновляются URL, SHA-256 и патчи.
DOWNLOADS = {
    "libsodium-1.0.22.tar.gz": (
        "https://download.libsodium.org/libsodium/releases/libsodium-1.0.22.tar.gz",
        "adbdd8f16149e81ac6078a03aca6fc03b592b89ef7b5ed83841c086191be3349"),
    "libsodium-1.0.22-mingw.tar.gz": (
        "https://download.libsodium.org/libsodium/releases/libsodium-1.0.22-mingw.tar.gz",
        "1d99e0afaf27bce664249232e9dc628ae6bb7b49f0ab53ad6db372b028a35d9d"),
    "android-commandlinetools.zip": (
        "https://dl.google.com/android/repository/commandlinetools-linux-13114758_latest.zip",
        "7ec965280a073311c339e571cd5de778b9975026cfcbe79f2b1cdcb1e15317ee"),
    "sqlite.zip": (
        "https://www.sqlite.org/2026/sqlite-amalgamation-3530200.zip",
        "8a310d0a16c7a90cacd4c884e70faa51c902afed2a89f63aaa0126ab83558a32"),
    "miniaudio.h": (
        "https://raw.githubusercontent.com/mackron/miniaudio/9634bedb5b5a2ca38c1ee7108a9358a4e233f14d/miniaudio.h",
        "ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89"),
    "silero_vad.onnx": (
        "https://raw.githubusercontent.com/snakers4/silero-vad/bfdc0193023f121ea5b3cc7b176dbed570a68a59/src/silero_vad/data/silero_vad.onnx",
        "1a153a22f4509e292a94e67d6f9b85e8deb25b4988682b7e174c65279d8788e3"),
    "wintun.zip": (
        "https://www.wintun.net/builds/wintun-0.14.1.zip",
        "07c256185d6ee3652e09fa55c0b673e2624b565e02c4b9091c79ca7d2f24ef51"),
    "dnsmasq-2.93.tar.xz": (
        "https://thekelleys.org.uk/dnsmasq/dnsmasq-2.93.tar.xz",
        "0c00d4e5c97c8306e5fb932b348b34269c9c29a0e7df0e8e82958b407092bc19"),
    "onnxruntime.aar": (
        "https://repo.maven.apache.org/maven2/com/microsoft/onnxruntime/onnxruntime-android/1.23.2/onnxruntime-android-1.23.2.aar",
        "82048d1f462218adae4ba76477089ab0ba76093d84f733540066db1a8ba6b827"),
    "openssl.tar.gz": (
        "https://github.com/openssl/openssl/releases/download/openssl-3.5.5/openssl-3.5.5.tar.gz",
        "b28c91532a8b65a1f983b4c28b7488174e4a01008e29ce8e69bd789f28bc2a89"),
    "rlottie.tar.gz": (
        f"https://codeload.github.com/Samsung/rlottie/tar.gz/{RLOTTIE}",
        "a89e071a3455ccbf8f61af67aad0657259ae9aabdf3afe3a0670954370cdd37a"),
}


def log(message):
    print(f"INFO: {message}", flush=True)


def configure_output():
    """Логи и дочерние Python-процессы используют UTF-8, включая перенаправление в tee на Windows."""
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="backslashreplace")
    os.environ["PYTHONUTF8"] = "1"
    os.environ["PYTHONIOENCODING"] = "utf-8:backslashreplace"
    log("Кодировка логов Python: UTF-8")


def load_proxy(root=REPO):
    """Прочитать литеральный URL из общего proxy.sh и передать его сетевым дочерним процессам."""
    path = root / "proxy.sh"
    if not path.is_file():
        return
    values = re.findall(r"^\s*UTUN_HTTP_PROXY='([^'\r\n]*)'\s*(?:#.*)?$", path.read_text(encoding="utf-8-sig"), re.MULTILINE)
    if len(values) != 1:
        raise RuntimeError(f"{path}: expected exactly one UTUN_HTTP_PROXY='http://host:port' assignment")
    proxy = values[0]
    if not proxy:
        return
    try:
        address = urlsplit(proxy)
        valid = address.scheme in ("http", "https") and address.hostname and address.port != 0
    except ValueError:
        valid = False
    if not valid:
        raise RuntimeError(f"{path}: invalid HTTP proxy URL")
    for name in ("http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY"):
        os.environ[name] = proxy
    log(f"HTTP proxy loaded from {path}")


def run(command, cwd=None, env=None):
    """Показать команду и остановить весь скрипт при ненулевом коде возврата."""
    command = [arg.as_posix() if isinstance(arg, Path) else str(arg) for arg in command]
    log(f"{cwd or Path.cwd()}: {shlex.join(command)}")
    subprocess.run(command, cwd=cwd, env=env, check=True)


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def download(name, cache, offline):
    """Сначала проверить кеш; новый файл принимать только после проверки SHA-256."""
    url, expected = DOWNLOADS[name]
    path = cache / name
    if path.is_file() and digest(path) == expected:
        log(f"SHA-256 OK, кеш: {name}")
        return path
    if path.exists():
        raise RuntimeError(f"SHA-256 кеша не совпадает: {path}; удалите повреждённый файл и повторите")
    if offline:
        raise RuntimeError(f"Нет файла в офлайн-кеше: {path}")
    # Windows ищет curl.exe в системном каталоге раньше PATH, где находится curl из MSYS2.
    curl = shutil.which("curl")
    if not curl:
        raise RuntimeError(f"curl не найден в PATH; невозможно скачать {url}")
    log(f"Скачивание: {url}")
    # Частичный ответ сервера не должен выглядеть как готовый архив.
    with tempfile.TemporaryDirectory(prefix="download-", dir=cache) as tmp:
        part = Path(tmp) / name
        run([Path(curl).absolute(), "--fail", "--location", "--show-error", "--silent", "--retry", "3",
             "--connect-timeout", "15", "--max-time", "600", url, "--output", part])
        actual = digest(part)
        if actual != expected:
            raise RuntimeError(f"SHA-256 {name}: ожидался {expected}, получен {actual}")
        part.replace(path)
    log(f"SHA-256 OK: {name}")
    return path


def install(data, destination):
    """Не трогать совпадающий файл; новый записать через соседний временный файл."""
    if destination.is_file() and destination.read_bytes() == data:
        return destination
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(data)
    try:
        temporary.chmod(0o644)
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)
    return destination


def copy_tree(source, destination):
    # Не удаляем существующие файлы и каталоги сборки в дереве назначения.
    outputs = []
    for path in sorted(source.rglob("*")):
        if path.is_file():
            target = destination / path.relative_to(source)
            install(path.read_bytes(), target)
            target.chmod(path.stat().st_mode & 0o777)
            outputs.append(target)
    return outputs


def generate_model(model, destination):
    """ONNX уже обучена: здесь лишь встраивание тех же байтов в C-массив."""
    data = model.read_bytes()
    lines = ["/* Generated from lib/silero_vad.onnx (Silero VAD v6.2, MIT license). Do not edit. */",
             "static const unsigned char silero_vad_model_data[] = {"]
    for offset in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{value:02x}" for value in data[offset:offset + 16]) + ",")
    lines.extend(["};", "static const size_t silero_vad_model_len = sizeof(silero_vad_model_data);", ""])
    install("\n".join(lines).encode(), destination)
    log(f"ONNX → C: {len(data)} байт, SHA-256={digest(model)}, файл={destination}")
    return destination


def preparation_fingerprint(component, root, ndk):
    """Изменение рецепта, патчей, платформы или toolchain инвалидирует готовую группу."""
    inputs = {"recipe": digest(Path(__file__)), "root": str(root), "platform": platform.platform(),
              "machine": platform.machine(), "ndk": str(ndk) if component == "android" else None}
    patch = {"common": "miniaudio.patch", "rlottie": "rlottie.patch"}.get(component)
    if patch:
        inputs["patch"] = digest(PATCHES / patch)
    if component in ("android", "rlottie", "sodium"):
        inputs["environment"] = {key: os.environ.get(key) for key in
                                 ("CC", "CXX", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "CMAKE_GENERATOR", "MSYSTEM")}
        if ndk and component == "android":
            inputs["ndk_version"] = digest(ndk / "source.properties")
    return hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()


def preparation_current(stamp, root, fingerprint):
    """Сверить рецепт и каждый результат; одного наличия libcrypto.a недостаточно."""
    if not stamp.is_file():
        log(f"Нет состояния подготовки: {stamp.name}")
        return False
    try:
        state = json.loads(stamp.read_text())
        if not isinstance(state, dict) or not isinstance(state.get("files"), dict) or not state["files"]:
            raise ValueError("нет списка результатов")
        if state.get("fingerprint") != fingerprint:
            log(f"Рецепт или toolchain изменились: {stamp.name}")
            return False
        for name, expected in state["files"].items():
            path = root / name
            if not path.is_file() or digest(path) != expected:
                log(f"Восстановление отсутствующей или изменённой зависимости: {path}")
                return False
    except (OSError, ValueError, TypeError) as error:
        print(f"WARN: состояние подготовки {stamp} не прочитано: {error}", file=sys.stderr, flush=True)
        return False
    log(f"Зависимости проверены, подготовка не нужна: {stamp.stem} ({len(state['files'])} файлов)")
    return True


def prepare_common(root, cache, offline, work):
    with zipfile.ZipFile(download("sqlite.zip", cache, offline)) as archive:
        for name in ("sqlite3.c", "sqlite3.h"):
            data = archive.read(f"sqlite-amalgamation-3530200/{name}")
            install(data, root / "lib" / name)
            install(data, root / "tools/chatgui/db" / name)
    log("SQLite 3.53.2: исходники готовы")

    # miniaudio имеет наши исправления аудиотракта; чистый upstream их не содержит.
    source = work / "miniaudio"
    source.mkdir()
    install(download("miniaudio.h", cache, offline).read_bytes(), source / "miniaudio.h")
    run(["patch", "--batch", "-p1", "-i", PATCHES / "miniaudio.patch"], cwd=source)
    install((source / "miniaudio.h").read_bytes(), root / "lib/miniaudio.h")
    log("miniaudio 0.11.25: локальные исправления применены")

    model = download("silero_vad.onnx", cache, offline)
    install(model.read_bytes(), root / "lib/silero_vad.onnx")
    generate_model(model, root / "lib/silero_vad_model.inc")
    return [root / name for name in ("lib/sqlite3.c", "lib/sqlite3.h", "tools/chatgui/db/sqlite3.c",
            "tools/chatgui/db/sqlite3.h", "lib/miniaudio.h", "lib/silero_vad.onnx", "lib/silero_vad_model.inc")]


def prepare_sodium(root, cache, offline, work, target, jobs, mingw):
    """Официальный static MinGW x64 или сборка тем же CC, что и ядро, включая Android NDK."""
    archive = download("libsodium-1.0.22-mingw.tar.gz" if mingw else "libsodium-1.0.22.tar.gz", cache, offline)
    run(["tar", "-xzf", archive.name, "-C", work], cwd=archive.parent)
    if mingw:
        prefix = work / "libsodium-win64"
    else:
        source = work / "libsodium-1.0.22"
        prefix = work / "install"
        run([source / "configure", f"--host={target}", f"--prefix={prefix}", "--enable-static",
             "--disable-shared", "--with-pic", "--disable-dependency-tracking"], cwd=source)
        make = "gmake" if platform.system() == "FreeBSD" else "make"
        run([make, f"-j{jobs}", "install"], cwd=source)
    dest = root / ".cache/dependencies/sodium" / target
    outputs = copy_tree(prefix / "include", dest / "include")
    outputs.append(install((prefix / "lib/libsodium.a").read_bytes(), dest / "lib/libsodium.a"))
    log(f"libsodium 1.0.22: {target} готов")
    return outputs


def prepare_android(root, cache, offline, work, ndk, jobs):
    outputs = []
    # AAR содержит и C/C++ заголовки, и готовые .so; Java/JNI обёртка нам не нужна.
    with zipfile.ZipFile(download("onnxruntime.aar", cache, offline)) as archive:
        for name in archive.namelist():
            if name.startswith("headers/") and name.endswith(".h"):
                outputs.append(install(archive.read(name), root / ANDROID_CPP / "onnxruntime/include/onnxruntime" / Path(name).name))
        for abi in ABIS:
            outputs.append(install(archive.read(f"jni/{abi}/libonnxruntime.so"),
                                   root / ANDROID_CPP.parent / "jniLibs" / abi / "libonnxruntime.so"))
    log("ONNX Runtime 1.23.2: заголовки и обе Android-библиотеки готовы")

    archive = download("openssl.tar.gz", cache, offline)
    source = work / "openssl"
    source.mkdir()
    run(["tar", "-xzf", archive, "-C", source, "--strip-components=1"])
    env = dict(os.environ, ANDROID_NDK_ROOT=str(ndk))
    env["PATH"] = str(ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin") + os.pathsep + env["PATH"]
    for abi, target in ABIS.items():
        build = work / abi
        build.mkdir()
        prefix = build / "install"
        # Отдельная сборка для каждого ABI, без общих объектов и системной установки.
        run(["perl", source / "Configure", target, "-D__ANDROID_API__=26", "no-shared", "no-tests",
             "no-engine", "no-comp", "no-docs",
             f"--prefix={prefix}", "--libdir=lib"], cwd=build, env=env)
        run(["make", f"-j{jobs}", "build_libs"], cwd=build, env=env)
        run(["make", "install_dev"], cwd=build, env=env)
        outputs.extend(copy_tree(prefix / "include", root / ANDROID_CPP / "openssl" / abi / "include"))
        outputs.append(install((prefix / "lib/libcrypto.a").read_bytes(), root / ANDROID_CPP / "openssl" / abi / "lib/libcrypto.a"))
        log(f"OpenSSL 3.5.5: {abi} готов")
    return outputs


def prepare_rlottie(root, cache, offline, work, jobs):
    source = work / "rlottie"
    source.mkdir()
    archive = download("rlottie.tar.gz", cache, offline)
    # Имя без C:/ работает и с GNU tar, и с Windows bsdtar без GNU-опции --force-local.
    run(["tar", "-xzf", archive.name, "-C", source, "--strip-components=1"], cwd=archive.parent)
    run(["patch", "--batch", "-p1", "-i", PATCHES / "rlottie.patch"], cwd=source)
    destination = root / "tools/chatgui/rlottie_src"
    outputs = copy_tree(source, destination)
    build = root / "tools/chatgui/rlottie_build"
    generator = ["-G", "Ninja"] if os.name == "nt" or os.environ.get("MSYSTEM") else []
    run(["cmake", "-S", destination, "-B", build, *generator, "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_SHARED_LIBS=ON",
         "-DLOTTIE_MODULE=OFF", "-DCMAKE_EXPORT_NO_PACKAGE_REGISTRY=ON"])
    run(["cmake", "--build", build, "--target", "rlottie", "--parallel", jobs])
    log(f"rlottie {RLOTTIE}: готова; CMake find_package использует {build}")
    outputs.extend(path for path in build.glob("librlottie.*") if path.is_file())
    outputs.extend(build / name for name in ("rlottieConfig.cmake", "rlottieConfigVersion.cmake", "rlottieTargets.cmake"))
    return outputs


def main():
    if sys.version_info < (3, 9):
        raise RuntimeError(f"Нужен Python 3.9 или новее; запущен {platform.python_version()}")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--only", choices=("common", "sodium", "wintun", "android", "rlottie", "dnsmasq"), help="подготовить только одну группу")
    parser.add_argument("--sodium-target", help="целевой triplet компилятора при --only sodium")
    parser.add_argument("--output", type=Path, default=REPO, help="корень назначения (по умолчанию репозиторий)")
    parser.add_argument("--cache", type=Path, help="каталог скачанных файлов (по умолчанию OUTPUT/.cache/dependencies/downloads)")
    parser.add_argument("--offline", action="store_true", help="не обращаться в сеть")
    parser.add_argument("--if-needed", action="store_true", help="пропустить группы с неизменными рецептами и результатами")
    parser.add_argument("--ndk", type=Path, help="Android NDK 29; иначе ANDROID_NDK_ROOT/HOME или SDK/ndk/29.0.14206865")
    parser.add_argument("--jobs", type=int, default=4, help="параллельных компиляций (по умолчанию 4)")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs должен быть положительным")
    load_proxy()
    if args.only == "sodium" and not re.fullmatch(r"[A-Za-z0-9_+.]+(?:-[A-Za-z0-9_+.]+)+", args.sodium_target or ""):
        parser.error("--only sodium требует --sodium-target с целевым triplet")
    components = [args.only] if args.only else ["common", "rlottie"]
    root = args.output.resolve()
    cache = (args.cache or root / ".cache/dependencies/downloads").resolve()

    # Проверка инструментов до первого скачивания, а не посреди длинной сборки.
    commands = {"curl"} if not args.offline else set()
    if "sodium" in components:
        commands.add("tar")
        mingw = args.sodium_target.startswith("x86_64-") and ("mingw" in args.sodium_target or "windows-gnu" in args.sodium_target)
        if not mingw:
            commands.add("gmake" if platform.system() == "FreeBSD" else "make")
    if "common" in components or "rlottie" in components:
        commands.add("patch")
    if "rlottie" in components:
        commands.update(("cmake", "tar", "ninja" if os.name == "nt" or os.environ.get("MSYSTEM") else "make"))
        for variable, default in (("CC", "cc"), ("CXX", "c++")):
            compiler = shlex.split(os.environ.get(variable, default))
            if not compiler:
                raise RuntimeError(f"Пустая переменная {variable}; укажите компилятор или удалите переменную из окружения")
            commands.add(compiler[0])
    ndk = None
    if "android" in components:
        commands.update(("perl", "make", "tar"))
        if platform.system() != "Linux" or platform.machine() != "x86_64":
            raise RuntimeError("Сборка Android OpenSSL поддерживает Linux x86_64; используйте --only common или rlottie")
        sdk = Path(os.environ.get("ANDROID_HOME", os.environ.get("ANDROID_SDK_ROOT", str(Path.home() / "android"))))
        ndk = (args.ndk or Path(os.environ.get("ANDROID_NDK_ROOT", os.environ.get("ANDROID_NDK_HOME", str(sdk / "ndk" / NDK_VERSION))))).resolve()
        if not (ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/clang").is_file():
            raise RuntimeError(f"Android NDK не найден: {ndk}; укажите --ndk или установите ndk;{NDK_VERSION}")
    missing = [command for command in sorted(commands) if not shutil.which(command)]
    if missing:
        raise RuntimeError("Не найдены инструменты: " + ", ".join(missing) +
                           ". Ubuntu/Debian: sudo apt-get install build-essential python3 curl tar patch cmake perl. "
                           "После установки повторите запуск; для другого дистрибутива используйте его пакетный менеджер.")

    cache.mkdir(parents=True, exist_ok=True)
    log(f"Компоненты={','.join(components)}, назначение={root}, кеш={cache}, jobs={args.jobs}")
    # Временные исходники OpenSSL и объекты автоматически удаляются; готовые файлы остаются в output.
    with tempfile.TemporaryDirectory(prefix="prepare-", dir=cache.parent) as tmp:
        work = Path(tmp)
        for component in components:
            fingerprint = preparation_fingerprint(component, root, ndk)
            stamp = root / ".cache/dependencies" / f"{component}.json"
            if component == "sodium":
                stamp = stamp.with_name(f"sodium-{args.sodium_target}.json")
            if args.if_needed and preparation_current(stamp, root, fingerprint):
                continue
            if component == "common":
                outputs = prepare_common(root, cache, args.offline, work)
            elif component == "sodium":
                outputs = prepare_sodium(root, cache, args.offline, work, args.sodium_target, args.jobs, mingw)
            elif component == "wintun":
                with zipfile.ZipFile(download("wintun.zip", cache, args.offline)) as archive:
                    outputs = [install(archive.read("wintun/bin/amd64/wintun.dll"), root / "lib/wintun.dll")]
                log(f"Wintun 0.14.1 AMD64: готов, SHA-256={digest(outputs[0])}, файл={outputs[0]}")
            elif component == "android":
                outputs = prepare_android(root, cache, args.offline, work, ndk, args.jobs)
            elif component == "dnsmasq":
                archive = download("dnsmasq-2.93.tar.xz", cache, args.offline)
                outputs = [install(archive.read_bytes(), root / "src/dnsmasq" / archive.name)]
                log(f"dnsmasq 2.93: архив готов, SHA-256={digest(outputs[0])}, файл={outputs[0]}")
            else:
                outputs = prepare_rlottie(root, cache, args.offline, work, args.jobs)
            state = {"fingerprint": fingerprint, "files": {str(path.relative_to(root)): digest(path) for path in outputs}}
            install((json.dumps(state, sort_keys=True, indent=2) + "\n").encode(), stamp)
            log(f"Состояние подготовки сохранено: {component} ({len(state['files'])} файлов)")
    log("Все выбранные зависимости готовы")


if __name__ == "__main__":
    try:
        configure_output()
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError, zipfile.BadZipFile, KeyError) as error:
        print(f"ERROR: {error}", file=sys.stderr, flush=True)
        sys.exit(1)
