#!/usr/bin/env bash
# Собирает Qt Multimedia для MSYS2 UCRT64 с патчами Unicode-ошибок камеры и экспорта MinGW.
# Принимает каталог GUI build и число jobs; готовит DLL/плагин, лицензии и chatgui_qt_multimedia_runtime.txt.
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
if [ -f "$SCRIPT_DIR/../../../proxy.sh" ]; then
    . "$SCRIPT_DIR/../../../proxy.sh" || { echo 'ERROR: cannot load proxy.sh' >&2; exit 1; }
fi
GUI_BUILD=${1:?GUI build directory required}
JOBS=${2:?Job count required}
RUNTIME_PREFIX=${MSYSTEM_PREFIX:-/ucrt64}
SOURCE_STAGE=""
log() { echo "[Qt Multimedia] $1"; }
die() { log "ERROR: $1" >&2; exit 1; }
trap 'rc=$?; [[ -z "$SOURCE_STAGE" ]] || rm -rf -- "$SOURCE_STAGE";
    if [[ $rc != 0 ]]; then echo "[Qt Multimedia] ERROR: build failed (exit=$rc)" >&2; fi' EXIT
[[ ${MSYSTEM:-} = UCRT64 ]] || die "MSYS2 UCRT64 is required"
[[ $JOBS =~ ^[1-9][0-9]*$ ]] || die "Invalid job count: $JOBS"
for tool in cmake ninja pkg-config curl tar patch sha256sum pacman install awk; do
    command -v "$tool" >/dev/null || die "Required tool not found: $tool"
done
VERSION=$(pkg-config --modversion Qt6Multimedia) || die "Qt6Multimedia development files are missing"
CORE_VERSION=$(pkg-config --modversion Qt6Core) || die "Qt6Core development files are missing"
[[ $VERSION =~ ^6\.[0-9]+\.[0-9]+$ && $VERSION = "$CORE_VERSION" ]] || die "Qt version mismatch: Core=$CORE_VERSION Multimedia=$VERSION"
PACKAGES=$(pacman -Q mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-qt6-multimedia \
    mingw-w64-ucrt-x86_64-gcc) || die "Cannot identify installed Qt/compiler packages"
FFMPEG_PREFIX="$SCRIPT_DIR/../ffmpeg/ffmpeg_build/install"
[[ -f "$FFMPEG_PREFIX/lib/pkgconfig/libavcodec.pc" ]] || FFMPEG_PREFIX="$RUNTIME_PREFIX"
FFMPEG_VERSION=$(PKG_CONFIG_PATH="$FFMPEG_PREFIX/lib/pkgconfig" pkg-config --modversion \
    libavcodec libavformat libavutil libswscale libswresample) || die "FFmpeg development files are missing"
PATCH_ID=$(sha256sum "$SCRIPT_DIR/"*.patch "$0" | sha256sum)
INPUT_ID=$(printf '%s\n' "$PACKAGES" "$FFMPEG_PREFIX" "$FFMPEG_VERSION" "$PATCH_ID" | sha256sum)
WORK="$SCRIPT_DIR/build/$VERSION-${INPUT_ID:0:16}"
SOURCE="$WORK/source"
RUNTIME="$WORK/runtime"
ARCHIVE="qtmultimedia-everywhere-src-$VERSION.tar.xz"
URL="https://download.qt.io/official_releases/qt/${VERSION%.*}/$VERSION/submodules/$ARCHIVE"
mkdir -p "$WORK" "$GUI_BUILD"
if [[ ! -d "$SOURCE" ]]; then
    log "Downloading Qt Multimedia $VERSION (matching installed Qt)"
    curl --fail --location --retry 3 "$URL.sha256" -o "$WORK/$ARCHIVE.sha256"
    curl --fail --location --retry 3 "$URL" -o "$WORK/$ARCHIVE"
    (cd "$WORK" && sha256sum --check "$ARCHIVE.sha256") || die "Qt source checksum mismatch"
    SOURCE_STAGE=$(mktemp -d "$WORK/source.XXXXXX")
    tar -xf "$WORK/$ARCHIVE" --strip-components=1 -C "$SOURCE_STAGE"
    log "Applying Unicode camera errors and MSYS2 constexpr export fixes"
    patch --batch --fuzz=0 -d "$SOURCE_STAGE" -p1 -i "$SCRIPT_DIR/camera-error-unicode.patch"
    patch --batch --fuzz=0 -d "$SOURCE_STAGE" -p1 -i "$SCRIPT_DIR/mingw-constexpr.patch"
    mv "$SOURCE_STAGE" "$SOURCE"; SOURCE_STAGE=""
fi
log "Configuring Qt Multimedia $VERSION; FFmpeg=$FFMPEG_PREFIX"
cmake -S "$SOURCE" -B "$WORK/objects" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$RUNTIME_PREFIX" -DCMAKE_INSTALL_PREFIX="$WORK/install" \
    -DQT_BUILD_TESTS=OFF -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_EXAMPLES_BY_DEFAULT=OFF \
    -DFEATURE_ffmpeg=ON -DFEATURE_spatialaudio=OFF -DQT_DEPLOY_FFMPEG=OFF -DFFMPEG_DIR="$FFMPEG_PREFIX"
log "Building Multimedia, MultimediaWidgets and FFmpeg camera backend (jobs=$JOBS)"
cmake --build "$WORK/objects" --target MultimediaWidgets QFFmpegMediaPlugin --parallel "$JOBS"
QT_BIN=$(awk -F= '$1 == "INSTALL_BINDIR:STRING" {sub(/\r$/, ""); sub(/^[^=]*=/, ""); print}' "$WORK/objects/CMakeCache.txt")
QT_PLUGINS=$(awk -F= '$1 == "INSTALL_PLUGINSDIR:STRING" {sub(/\r$/, ""); sub(/^[^=]*=/, ""); print}' "$WORK/objects/CMakeCache.txt")
[[ -n "$QT_BIN" && -n "$QT_PLUGINS" ]] || die "Qt output directories missing in CMakeCache.txt"
for relative in "$QT_BIN/Qt6Multimedia.dll" "$QT_BIN/Qt6MultimediaWidgets.dll" "$QT_PLUGINS/multimedia/ffmpegmediaplugin.dll"; do
    [[ -f "$WORK/objects/$relative" ]] || die "Qt build output missing: $WORK/objects/$relative"
done
mkdir -p "$RUNTIME/bin" "$RUNTIME/plugins/multimedia" "$RUNTIME/licenses"
install -C -m644 "$WORK/objects/$QT_BIN/Qt6Multimedia.dll" "$WORK/objects/$QT_BIN/Qt6MultimediaWidgets.dll" "$RUNTIME/bin/"
install -C -m644 "$WORK/objects/$QT_PLUGINS/multimedia/ffmpegmediaplugin.dll" "$RUNTIME/plugins/multimedia/"
cp "$SOURCE/LICENSES/"* "$RUNTIME/licenses/"
cp "$SCRIPT_DIR/"*.patch "$RUNTIME/licenses/"
printf 'Qt Multimedia %s\nSource: %s\nSHA-256: %s\nMSYS2 constexpr patch: %s\n' "$VERSION" "$URL" \
    "$(cat "$WORK/$ARCHIVE.sha256")" \
    'https://github.com/msys2/MINGW-packages/tree/master/mingw-w64-qt6-multimedia' > "$RUNTIME/licenses/SOURCE.txt"
printf '%s\n' "$RUNTIME" > "$GUI_BUILD/chatgui_qt_multimedia_runtime.txt.tmp"
mv "$GUI_BUILD/chatgui_qt_multimedia_runtime.txt.tmp" "$GUI_BUILD/chatgui_qt_multimedia_runtime.txt"
log "Ready: $RUNTIME (camera errors use Unicode and include native HRESULT)"
