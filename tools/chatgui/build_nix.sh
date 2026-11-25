#!/usr/bin/env bash
# Сборка VibeChat для Linux/FreeBSD; --full устанавливает зависимости и выполняет чистую сборку.
# На Linux --deb создаёт DEB, --user-archive — архив для установки без sudo на Ubuntu 26.04 x64.
set -euo pipefail
trap 'echo "ERROR: ChatGUI build failed at line $LINENO" >&2' ERR

# ============================================================================
# build_nix.sh — ChatGUI build (Linux / FreeBSD)
#
#   ./build_nix.sh            quick (incremental) build
#   ./build_nix.sh --full     "под ключ": deps + clean + configure + build
#   ./build_nix.sh -jN        parallel jobs
#   ./build_nix.sh --asan     build with AddressSanitizer
#   ./build_nix.sh --deb      Release build + Linux DEB package (alias: --installer)
# ============================================================================

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -f "$SRC_DIR/../../proxy.sh" ]; then
    . "$SRC_DIR/../../proxy.sh" || { echo 'ERROR: cannot load proxy.sh' >&2; exit 1; }
fi
BUILD_DIR="$SRC_DIR/build"

FULL=0
ASAN=0
BUILD_DEB=0
BUILD_USER=0
JOBS=""

usage() {
    echo "Usage: $0 [--full] [-jN] [--asan] [--deb|--installer|--user-archive]"
    echo "  --full   install deps + clean rebuild (all-in-one)"
    echo "  -jN      parallel build jobs (default: CPU count)"
    echo "  --asan   build with AddressSanitizer"
    echo "  --deb    build a Release DEB in build-deb/installer (Linux only)"
    echo "  --installer  alias for --deb"
    echo "  --user-archive  Ubuntu 26.04 x64 archive with system libraries"
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --full) FULL=1 ;;
        --asan) ASAN=1 ;;
        --deb|--installer) BUILD_DEB=1 ;;
        --user-archive) BUILD_USER=1 ;;
        -j)
            [[ $# -gt 1 ]] || { echo "ERROR: -j requires a positive job count" >&2; exit 1; }
            shift; JOBS=$1 ;;
        -j*) JOBS="${1#-j}" ;;
        --jobs=*) JOBS="${1#--jobs=}" ;;
        -h|--help) usage ;;
        *) echo "ERROR: unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done

OS_NAME="$(uname -s)"
case "$OS_NAME" in
    Linux)   OS_ID=linux; ;;
    FreeBSD) OS_ID=bsd;   ;;
    *) echo "Unsupported OS: $OS_NAME (supported: Linux, FreeBSD)" >&2; exit 1 ;;
esac
if [[ "$BUILD_DEB" = 1 ]]; then
    [[ "$OS_ID" = linux && "$ASAN" = 0 ]] || { echo "ERROR: --deb requires Linux without --asan" >&2; exit 1; }
    BUILD_DIR="$SRC_DIR/build-deb"
fi

if [[ "$BUILD_USER" = 1 ]]; then
    [[ "$OS_ID" = linux && "$ASAN" = 0 && "$BUILD_DEB" = 0 ]] || { echo "ERROR: --user-archive requires Linux without --asan/--deb" >&2; exit 1; }
    BUILD_DIR="$SRC_DIR/build-user"
    source /etc/os-release
    [[ "$ID" = ubuntu && "$VERSION_ID" = 26.04 && $(uname -m) = x86_64 ]] || {
        echo "ERROR: --user-archive requires Ubuntu 26.04 x64" >&2; exit 1;
    }
fi

if [ -z "$JOBS" ]; then
    if [ "$OS_ID" = linux ]; then
        JOBS="$(nproc)"
    else
        JOBS="$(sysctl -n hw.ncpu)"
    fi
fi
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo "ERROR: invalid job count: $JOBS" >&2; exit 1; }

install_deps() {
    echo "[deps] installing build dependencies..."
    local -a privilege=()
    if [[ $(id -u) != 0 ]]; then
        command -v sudo >/dev/null || { echo "ERROR: installing GUI dependencies requires root or sudo" >&2; exit 1; }
        privilege=(sudo --preserve-env=http_proxy,https_proxy,HTTP_PROXY,HTTPS_PROXY,no_proxy,NO_PROXY)
    fi
    if [ "$OS_ID" = linux ]; then
        command -v apt-get >/dev/null || {
            echo "ERROR: --full installs Linux GUI dependencies through apt; install them for your distribution and omit --full" >&2
            exit 1
        }
        "${privilege[@]}" apt-get update || { echo "ERROR: apt update failed; check network access and package-manager permissions" >&2; exit 1; }
        "${privilege[@]}" apt-get install -y build-essential cmake pkg-config python3 curl tar patch dpkg-dev file binutils appstream \
            qt6-base-dev qt6-multimedia-dev qt6-image-formats-plugins qt6-tools-dev-tools qt6-translations-l10n \
            librlottie-dev zlib1g-dev libssl-dev libx11-dev libxss-dev libpipewire-0.3-dev dbus-daemon \
            libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev libkf6syntaxhighlighting-dev libfuse3-dev fuse3 || {
            echo "ERROR: could not install GUI dependencies; check the apt error above" >&2; exit 1;
        }
        if [[ "$BUILD_USER" = 1 ]]; then "${privilege[@]}" apt-get install -y libonnxruntime-dev; fi
    else
        "${privilege[@]}" pkg install -y cmake gmake python3 curl qt6-base qt6-imageformats qt6-multimedia qt6-tools qt6-translations \
            rlottie openssl ffmpeg kf6-syntax-highlighting
    fi
}

# Auto-detect FFmpeg (optional video transcoding). CMake finds it via pkg-config.
if command -v pkg-config >/dev/null 2>&1 && \
   pkg-config --exists libavformat libavcodec libavutil libswscale libswresample 2>/dev/null; then
    echo "[ffmpeg] detected — video transcoding enabled"
else
    echo "[ffmpeg] not found — video transcoding disabled"
fi

SAN_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g"
if [ "$ASAN" -eq 1 ]; then
    echo "[build] ASAN ENABLED"
    CMAKE_ARGS=(-DCMAKE_C_FLAGS="$SAN_FLAGS" -DCMAKE_CXX_FLAGS="$SAN_FLAGS" -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address)
else
    CMAKE_ARGS=()
fi
if [[ "$BUILD_DEB" = 1 ]]; then
    CMAKE_ARGS+=(-DCMAKE_BUILD_TYPE=Release -DCHATGUI_BUILD_DEB=ON)
fi

if [[ "$BUILD_USER" = 1 ]]; then
    CMAKE_ARGS+=(-DCMAKE_BUILD_TYPE=Release -DCHATGUI_BUILD_USER_ARCHIVE=ON -DCMAKE_PREFIX_PATH=/usr
        -DCMAKE_C_FLAGS= -DCMAKE_CXX_FLAGS= -DCMAKE_EXE_LINKER_FLAGS=)
fi

if [ "$FULL" -eq 1 ]; then
    install_deps
    echo "[build] clean rebuild..."
    rm -rf "$BUILD_DIR"
fi
if [[ "$BUILD_DEB" = 1 ]]; then
    bash "$SRC_DIR/package_deb.sh" --check
fi

command -v python3 >/dev/null || { echo "ERROR: Python 3.9+ is required; run $0 --full to install build tools" >&2; exit 1; }
python3 "$SRC_DIR/../prepare_dependencies.py" --only common --if-needed || {
    echo "ERROR: common dependency preparation failed" >&2; exit 1;
}
if [[ "$BUILD_USER" = 0 ]]; then
    python3 "$SRC_DIR/../prepare_dependencies.py" --only rlottie --if-needed --jobs "$JOBS" || {
        echo "ERROR: desktop rlottie preparation failed" >&2; exit 1;
    }
fi

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ] || [ "$ASAN" -eq 1 ] || [ "$BUILD_DEB" -eq 1 ] || [ "$BUILD_USER" -eq 1 ]; then
    cmake -S "$SRC_DIR" -B "$BUILD_DIR" "${CMAKE_ARGS[@]}"
fi

if [[ "$BUILD_DEB" = 1 ]]; then
    cmake --build "$BUILD_DIR" --target vibechat -j"$JOBS"
    bash "$SRC_DIR/package_deb.sh"
elif [[ "$BUILD_USER" = 1 ]]; then
    cmake --build "$BUILD_DIR" --target vibechat -j"$JOBS"
    python3 "$SRC_DIR/installer/package_user.py" "$BUILD_DIR"
else
    cmake --build "$BUILD_DIR" -j"$JOBS"
fi

echo ""
if [ "$ASAN" -eq 1 ]; then
    echo "OK. Run: ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 $BUILD_DIR/vibechat"
else
    echo "OK. Run: $BUILD_DIR/vibechat"
fi
