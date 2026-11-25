#!/usr/bin/env bash
# Создаёт Windows-установщик VibeChat через Inno Setup из готовой сборки в MSYS2 UCRT64.
# Проверяет DLL/Qt-плагины и лицензии, исключает профили/БД/логи; --check проверяет инструменты упаковки.
set -euo pipefail

log() { echo "$1"; }

SRC_DIR=$(cd "$(dirname "$0")" && pwd)
BUILD_DIR="$SRC_DIR/build"
RUNTIME_PREFIX="${MSYSTEM_PREFIX:-/ucrt64}"
STAGE=""
trap 'rc=$?; if [[ $rc != 0 ]]; then echo "ERROR: installer packaging failed (exit=$rc)" >&2; fi
    if [[ -n "$STAGE" ]]; then rm -rf -- "$STAGE" || echo "WARN: cannot remove installer staging directory: $STAGE" >&2; fi' EXIT

[[ "${MSYSTEM:-}" = UCRT64 ]] || { echo "ERROR: Windows installer requires MSYS2 UCRT64" >&2; exit 1; }
[[ $# = 0 || ( $# = 1 && $1 = --check ) ]] || { echo "ERROR: usage: bash package_win.sh [--check]" >&2; exit 1; }
for tool in cygpath objdump pacman; do
    command -v "$tool" >/dev/null || { echo "ERROR: required packaging tool not found: $tool" >&2; exit 1; }
done

if [[ -n "${ISCC:-}" ]]; then
    ISCC=$(cygpath -u "$ISCC")
else
    ISCC=$(command -v ISCC.exe || command -v iscc.exe || true)
    if [[ -z "$ISCC" ]]; then
        for version in 7 6; do
            for variable in 'ProgramFiles(x86)' PROGRAMFILES 'ProgramFiles'; do
                program_dir=$(printenv "$variable" || true)
                [[ -n "$program_dir" ]] || continue
                candidate=$(cygpath -u "$program_dir/Inno Setup $version/ISCC.exe")
                if [[ -f "$candidate" ]]; then ISCC=$candidate; break 2; fi
            done
            [[ -z "$ISCC" ]] || break
        done
    fi
fi
[[ -n "$ISCC" && -f "$ISCC" ]] || {
    echo 'ERROR: Inno Setup 6.3+ not found; install it or set ISCC to the full path of ISCC.exe' >&2
    exit 1
}
echo "INFO: installer compiler: $ISCC"
[[ "${1:-}" != --check ]] || exit 0

for file in vibechat.exe chatgui_version.txt chatgui_qt_package.txt; do
    [[ -f "$BUILD_DIR/$file" ]] || { echo "ERROR: missing build output: $BUILD_DIR/$file" >&2; exit 1; }
done
APP_VERSION=$(tr -d '\r\n' < "$BUILD_DIR/chatgui_version.txt")
[[ "$APP_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "ERROR: invalid application version: $APP_VERSION" >&2; exit 1; }
QT_PACKAGE=$(tr -d '\r\n' < "$BUILD_DIR/chatgui_qt_package.txt")
[[ "$QT_PACKAGE" = Qt5 || "$QT_PACKAGE" = Qt6 ]] || { echo "ERROR: invalid Qt package: $QT_PACKAGE" >&2; exit 1; }
CAPTURE_RUNTIME=()
if [[ -f "$BUILD_DIR/chatgui_capture_enabled.txt" && "$(tr -d '\r\n' < "$BUILD_DIR/chatgui_capture_enabled.txt")" = 1 ]]; then
    CAPTURE_RUNTIME=(Qt6Multimedia.dll Qt6MultimediaWidgets.dll multimedia/ffmpegmediaplugin.dll)
fi
for file in "${QT_PACKAGE}Core.dll" "${QT_PACKAGE}Gui.dll" "${QT_PACKAGE}Network.dll" "${QT_PACKAGE}Widgets.dll" \
    librlottie.dll wintun.dll platforms/qwindows.dll imageformats/qjpeg.dll imageformats/qwebp.dll "${CAPTURE_RUNTIME[@]}"; do
    [[ -f "$BUILD_DIR/$file" ]] || { echo "ERROR: missing runtime file: $BUILD_DIR/$file" >&2; exit 1; }
done
QT_MULTIMEDIA_RUNTIME=""
if (( ${#CAPTURE_RUNTIME[@]} )); then
    [[ -f "$BUILD_DIR/chatgui_qt_multimedia_runtime.txt" ]] || { echo 'ERROR: rebuild Qt Multimedia with Unicode camera errors' >&2; exit 1; }
    QT_MULTIMEDIA_RUNTIME=$(tr -d '\r\n' < "$BUILD_DIR/chatgui_qt_multimedia_runtime.txt")
    for file in "${CAPTURE_RUNTIME[@]}"; do
        source_file="$QT_MULTIMEDIA_RUNTIME/bin/$file"
        [[ "$file" != */* ]] || source_file="$QT_MULTIMEDIA_RUNTIME/plugins/$file"
        cmp -s "$source_file" "$BUILD_DIR/$file" || { echo "ERROR: patched Qt runtime differs: $file; rebuild GUI" >&2; exit 1; }
    done
    [[ -f "$QT_MULTIMEDIA_RUNTIME/licenses/SOURCE.txt" ]] || { echo 'ERROR: Qt Multimedia source notices missing' >&2; exit 1; }
fi

STAGE=$(mktemp -d "$BUILD_DIR/installer-stage.XXXXXX")
PACKAGE_DIR="$STAGE/package"
mkdir -p "$PACKAGE_DIR/licenses" "$STAGE/output" "$BUILD_DIR/installer"
shopt -s nullglob nocasematch
for file in LICENSE LICENSING THIRD_PARTY_NOTICES; do
    cp "$SRC_DIR/../../$file" "$PACKAGE_DIR/licenses/"
done
mkdir -p "$PACKAGE_DIR/licenses/rlottie"
cp "$SRC_DIR/rlottie_src/COPYING" "$PACKAGE_DIR/licenses/rlottie/"
cp -R "$SRC_DIR/rlottie_src/licenses" "$PACKAGE_DIR/licenses/rlottie/"
if [[ -n "$QT_MULTIMEDIA_RUNTIME" ]]; then
    cp -R "$QT_MULTIMEDIA_RUNTIME/licenses" "$PACKAGE_DIR/licenses/qt-multimedia"
fi
mkdir -p "$PACKAGE_DIR/licenses/wintun"
cp "$SRC_DIR/../../lib/wintun-LICENSE.txt" "$SRC_DIR/../../lib/wintun-api-LICENSE.txt" "$PACKAGE_DIR/licenses/wintun/"

# Проверяем архитектуру/полноту уже собранного комплекта, включая динамические Qt-плагины.
SYSTEM_DIR=$(cygpath -u "${WINDIR:-C:\\Windows}/System32")
source "$SRC_DIR/compiler_runtime.sh"
source "$SRC_DIR/runtime_cache.sh"
runtime_cache_init
IMPORT_DUMP="$STAGE/imports.txt"
VERIFY_STARTED=$SECONDS
declare -A LICENSE_PACKAGES=()
declare -A INCLUDED=()
PACKAGE_FILES=() PACKAGE_SOURCES=() PACKAGE_STAMPS=() LICENSE_SOURCES=()
FILES=(vibechat.exe wintun.dll platforms/qwindows.dll imageformats/qjpeg.dll imageformats/qwebp.dll)
FILES+=("${CAPTURE_RUNTIME[@]}")
HAS_FFMPEG=0
for ((i = 0; i < ${#FILES[@]}; i++)); do
    relative=${FILES[i]}
    [[ -z "${INCLUDED[${relative,,}]:-}" ]] || continue
    INCLUDED[${relative,,}]=1
    name=${relative##*/}
    if [[ "$name" = Qt[56]*.dll && "$name" != "$QT_PACKAGE"*.dll ]]; then
        echo "ERROR: mixed Qt runtime: $relative does not belong to $QT_PACKAGE" >&2
        exit 1
    fi
    # Ключ кеша относится к build, а не к новой временной копии установщика.
    runtime_inspect "$BUILD_DIR/$relative"
    [[ "$binary_format" = pei-x86-64 ]] || { echo "ERROR: $name is not a Windows x64 binary: $binary_format" >&2; exit 1; }
    PACKAGE_FILES+=("$relative")
    PACKAGE_SOURCES+=("$BUILD_DIR/$relative")
    PACKAGE_STAMPS+=("$binary_stamp")
    while read -r dep; do
        [[ -n "$dep" ]] || continue
        if [[ -f "$BUILD_DIR/$dep" ]]; then FILES+=("$dep"); continue; fi
        case "$dep" in api-ms-win-*.dll|ext-ms-win-*.dll) continue ;; esac
        if [[ -f "$SYSTEM_DIR/$dep" && "$dep" != msvcp*.dll && "$dep" != vcruntime*.dll ]]; then continue; fi
        echo "ERROR: incomplete installer runtime: $name requires $dep" >&2
        exit 1
    done <<< "$import_list"
    # Лицензии внешних библиотек берём из пакетов той же UCRT64-сборки.
    [[ "$name" != vibechat.exe && "$name" != librlottie.dll && "$name" != wintun.dll ]] || continue
    if [[ -n "$QT_MULTIMEDIA_RUNTIME" && ( "$name" = Qt6Multimedia.dll || "$name" = Qt6MultimediaWidgets.dll ||
        "$relative" = multimedia/ffmpegmediaplugin.dll ) ]]; then continue; fi
    if [[ -f "$SRC_DIR/ffmpeg/ffmpeg_build/install/bin/$name" ]] && ! chatgui_is_compiler_runtime_dll "$name"; then
        HAS_FFMPEG=1
        continue
    fi
    runtime_file="$RUNTIME_PREFIX/bin/$name"
    if [[ "$relative" = platforms/* || "$relative" = imageformats/* || "$relative" = multimedia/* ]]; then
        runtime_file="$RUNTIME_PREFIX/share/qt${QT_PACKAGE#Qt}/plugins/$relative"
        [[ -f "$runtime_file" ]] || runtime_file="$RUNTIME_PREFIX/lib/qt${QT_PACKAGE#Qt}/plugins/$relative"
    fi
    [[ -f "$runtime_file" ]] || { echo "ERROR: cannot identify runtime source for $name" >&2; exit 1; }
    LICENSE_SOURCES+=("$runtime_file")
done
# Один cp вместо отдельных mkdir/cp на каждую DLL; --parents сохраняет каталоги Qt-плагинов.
(cd "$BUILD_DIR" && cp --parents -- "${PACKAGE_FILES[@]}" "$PACKAGE_DIR/")
# Проверяем одним stat, что все источники остались теми же после анализа и копирования.
copied_stamps=$(stat -Lc '%n|%s|%y|%z' -- "${PACKAGE_SOURCES[@]}") || {
    echo "ERROR: cannot stat installer sources after copying" >&2; exit 1;
}
mapfile -t COPIED_STAMPS <<< "$copied_stamps"
for ((i = 0; i < ${#PACKAGE_FILES[@]}; i++)); do
    [[ "${COPIED_STAMPS[i]:-}" = "${PACKAGE_STAMPS[i]}" ]] || {
        echo "ERROR: installer source changed during packaging: ${PACKAGE_FILES[i]}; rebuild and retry" >&2; exit 1;
    }
done
echo "INFO: installer runtime staged: files=${#PACKAGE_FILES[@]} scanned=$RUNTIME_SCANNED cached=$RUNTIME_HITS elapsed=$((SECONDS - VERIFY_STARTED))s"

# На MSYS2 запуск процессов дорог: все владельцы файлов и списки лицензий запрашиваются пакетно.
if (( ${#LICENSE_SOURCES[@]} )); then
    pacman -Qqo -- "${LICENSE_SOURCES[@]}" > "$STAGE/license-packages.txt" || {
        echo "ERROR: cannot identify packages owning installer runtime files" >&2; exit 1;
    }
    while IFS= read -r pkg; do
        [[ -z "$pkg" ]] || LICENSE_PACKAGES["$pkg"]=1
    done < "$STAGE/license-packages.txt"
    (( ${#LICENSE_PACKAGES[@]} )) || { echo "ERROR: no package owns installer runtime files" >&2; exit 1; }
    pacman -Qql -- "${!LICENSE_PACKAGES[@]}" > "$STAGE/package-files.txt" || {
        echo "ERROR: cannot list installer runtime license files" >&2; exit 1;
    }
    while IFS= read -r file; do
        [[ "$file" = */share/licenses/* && -f "$file" ]] || continue
        relative=${file#*/share/licenses/}
        mkdir -p "$PACKAGE_DIR/licenses/runtime/${relative%/*}"
        cp "$file" "$PACKAGE_DIR/licenses/runtime/$relative"
    done < "$STAGE/package-files.txt"
    echo "INFO: included runtime license notices: packages=${#LICENSE_PACKAGES[@]}"
fi
if [[ "$HAS_FFMPEG" = 1 ]]; then
    FFMPEG_SOURCE="$SRC_DIR/ffmpeg/ffmpeg_build/src"
    [[ -f "$FFMPEG_SOURCE/ffmpeg/COPYING.GPLv3" ]] || { echo "ERROR: FFmpeg source license notices missing: $FFMPEG_SOURCE" >&2; exit 1; }
    # Сохраняем уведомления FFmpeg и его встроенных статических кодеков.
    for file in "$FFMPEG_SOURCE/"*/COPYING* "$FFMPEG_SOURCE/"*/LICENSE* "$FFMPEG_SOURCE/"*/PATENTS; do
        [[ -f "$file" ]] || continue
        relative=${file#"$FFMPEG_SOURCE/"}
        mkdir -p "$PACKAGE_DIR/licenses/ffmpeg/${relative%/*}"
        cp "$file" "$PACKAGE_DIR/licenses/ffmpeg/$relative"
    done
fi

echo "INFO: compiling VibeChat $APP_VERSION installer (Qt package: $QT_PACKAGE)"
COMPILE_STARTED=$SECONDS
# MSYS2 не должен преобразовывать ключи /D и /O; пути передаём явно в Windows-формате.
MSYS2_ARG_CONV_EXCL='*' "$ISCC" "/DAppVersion=$APP_VERSION" "/DPackageDir=$(cygpath -w "$PACKAGE_DIR")" \
    "/O$(cygpath -w "$STAGE/output")" "$(cygpath -w "$SRC_DIR/installer/vibechat.iss")"
INSTALLER_NAME="VibeChat-$APP_VERSION-win64-setup.exe"
[[ -s "$STAGE/output/$INSTALLER_NAME" ]] || { echo "ERROR: compiler did not produce $INSTALLER_NAME" >&2; exit 1; }
echo "INFO: installer compilation completed: elapsed=$((SECONDS - COMPILE_STARTED))s"
mv -f "$STAGE/output/$INSTALLER_NAME" "$BUILD_DIR/installer/$INSTALLER_NAME"
echo "INFO: installer created: $BUILD_DIR/installer/$INSTALLER_NAME"
