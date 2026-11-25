#!/bin/bash
# Сборка VibeChat для Windows в MSYS2 UCRT64: устанавливает зависимости, собирает GUI и копирует DLL/Qt-плагины.
# С --installer дополнительно создаёт установщик Inno Setup; ход работы сохраняется в build_win.log.
set -eo pipefail

LOG_FILE="build_win.log"
> "$LOG_FILE"

log() { echo "$1"; echo "$1" >> "$LOG_FILE"; }

log "================================"
log "ChatGUI Build Script (MSYS2 UCRT64)"
log "================================"
log ""

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -f "$SRC_DIR/../../proxy.sh" ]; then
    . "$SRC_DIR/../../proxy.sh" || { echo 'ERROR: cannot load proxy.sh' >&2; exit 1; }
fi
BUILD_DIR="$SRC_DIR/build"
RUNTIME_PREFIX="${MSYSTEM_PREFIX:-/ucrt64}"
JOBS=$(nproc 2>/dev/null || echo 4)
BUILD_INSTALLER=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --installer) BUILD_INSTALLER=1 ;;
        -j)
            [[ $# -gt 1 ]] || { log "ERROR: -j requires a positive job count"; exit 1; }
            shift; JOBS=$1 ;;
        -j*) JOBS=${1#-j} ;;
        *) log "ERROR: unknown argument: $1"; exit 1 ;;
    esac
    shift
done
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { log "ERROR: invalid job count: $JOBS"; exit 1; }
CMAKE_ARGS=()
bash "$SRC_DIR/install_deps_win.sh" 2>&1 | tee -a "$LOG_FILE"
for component in common wintun; do
    python3 "$SRC_DIR/../prepare_dependencies.py" --only "$component" --if-needed 2>&1 | tee -a "$LOG_FILE" || {
        log "ERROR: $component dependency preparation failed"; exit 1;
    }
done
if [[ "$BUILD_INSTALLER" = 1 ]]; then
    bash "$SRC_DIR/package_win.sh" --check 2>&1 | tee -a "$LOG_FILE"
    CMAKE_ARGS+=(-DCMAKE_BUILD_TYPE=Release)
fi

log "Source:  $SRC_DIR"
log "Build:   $BUILD_DIR"
log ""

if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    OLD_GEN=$(grep 'CMAKE_GENERATOR:' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null | cut -d= -f2)
    if [ "$OLD_GEN" != "Ninja" ]; then
        log "Generator changed ($OLD_GEN -> Ninja), cleaning build dir..."
        rm -rf "$BUILD_DIR"
    fi
fi

# Старые DLL приложения в build могут мешать запуску cc1/cc1plus после обновления MSYS2.
source "$SRC_DIR/compiler_runtime.sh"
if [ "${MSYSTEM:-}" = UCRT64 ]; then
    chatgui_refresh_compiler_runtime "$BUILD_DIR" "$RUNTIME_PREFIX/bin" 2>&1 | tee -a "$LOG_FILE"
    if [ -f "$SRC_DIR/ffmpeg/ffmpeg_build/install/lib/pkgconfig/libavcodec.pc" ]; then
        log "Checking bundled FFmpeg profile (including MJPEG video previews)..."
        bash "$SRC_DIR/ffmpeg/build_ffmpeg.sh" 2>&1 | tee -a "$LOG_FILE" || {
            log "  ERROR: bundled FFmpeg profile update failed"; exit 1;
        }
    fi
    log "Preparing bundled rlottie..."
    python3 "$SRC_DIR/../prepare_dependencies.py" --only rlottie --if-needed --jobs "$JOBS" 2>&1 | tee -a "$LOG_FILE" || {
        log "  ERROR: bundled rlottie preparation failed"; exit 1;
    }
    log "  OK: bundled rlottie ready in $SRC_DIR/rlottie_build"
fi

log "[1/4] Configuring CMake (generator: Ninja)..."
cmake -S "$SRC_DIR" -B "$BUILD_DIR" -G Ninja "${CMAKE_ARGS[@]}" 2>&1 | tee -a "$LOG_FILE"
test ${PIPESTATUS[0]} -eq 0 || { log "CMake configure FAILED"; exit 1; }

log ""
log "[2/4] Building (parallel: $JOBS jobs)..."
cmake --build "$BUILD_DIR" -j"$JOBS" 2>&1 | tee -a "$LOG_FILE"
test ${PIPESTATUS[0]} -eq 0 || { log "Build FAILED"; exit 1; }

log ""
log "[3/4] Deploying runtime DLLs..."
DEPLOY_STARTED=$SECONDS
source "$SRC_DIR/runtime_cache.sh"
runtime_cache_init
IMPORT_DUMP=$(mktemp) || { log "  ERROR: cannot create temporary import dump"; exit 1; }
trap 'rm -f -- "$IMPORT_DUMP" || log "  WARN: cannot remove temporary import dump: $IMPORT_DUMP"' EXIT
# Повторная сборка восстанавливает DLL, даже если EXE не перелинковывался.
runtime_copy "$SRC_DIR/../../lib/wintun.dll" "$BUILD_DIR/wintun.dll"
RLOTTIE_DLL="$SRC_DIR/rlottie_build/librlottie.dll"
[ -f "$RLOTTIE_DLL" ] || { log "  ERROR: librlottie.dll not found: $RLOTTIE_DLL"; exit 1; }
runtime_copy "$RLOTTIE_DLL" "$BUILD_DIR/librlottie.dll"
FFMPEG_BIN="$SRC_DIR/ffmpeg/ffmpeg_build/install/bin"
QT_PACKAGE_FILE="$BUILD_DIR/chatgui_qt_package.txt"
[ -f "$QT_PACKAGE_FILE" ] || { log "  ERROR: Qt package information missing: $QT_PACKAGE_FILE"; exit 1; }
QT_PACKAGE=$(tr -d '\r\n' < "$QT_PACKAGE_FILE")
case "$QT_PACKAGE" in
    Qt5|Qt6) QT_MAJOR="${QT_PACKAGE#Qt}" ;;
    *) log "  ERROR: invalid configured Qt package: $QT_PACKAGE"; exit 1 ;;
esac
log "  Runtime package: $QT_PACKAGE"
QT_COMPONENTS=(Core Gui Network Widgets)
QT_PLUGINS=(platforms/qwindows.dll imageformats/qjpeg.dll imageformats/qwebp.dll)
QT_MULTIMEDIA_RUNTIME=""
if [ -f "$BUILD_DIR/chatgui_capture_enabled.txt" ] && [ "$(tr -d '\r\n' < "$BUILD_DIR/chatgui_capture_enabled.txt")" = 1 ]; then
    QT_COMPONENTS+=(Multimedia MultimediaWidgets)
    QT_PLUGINS+=(multimedia/ffmpegmediaplugin.dll)
    log "  Building Qt Multimedia with Unicode camera errors..."
    bash "$SRC_DIR/qt_multimedia/build.sh" "$BUILD_DIR" "$JOBS" 2>&1 | tee -a "$LOG_FILE"
    QT_MULTIMEDIA_RUNTIME=$(tr -d '\r\n' < "$BUILD_DIR/chatgui_qt_multimedia_runtime.txt")
fi
MAIN_DLLS=(
    "Qt${QT_MAJOR}Core.dll" "Qt${QT_MAJOR}Gui.dll" "Qt${QT_MAJOR}Network.dll" "Qt${QT_MAJOR}Widgets.dll"
    "platforms/qwindows.dll" "imageformats/qjpeg.dll" "imageformats/qwebp.dll"
    "libstdc++-6.dll" "libgcc_s_*.dll" "libwinpthread-1.dll" "librlottie.dll" "wintun.dll" "zlib1.dll" "libcrypto-*.dll"
)
if [ -d "$FFMPEG_BIN" ]; then
    MAIN_DLLS+=("avcodec-*.dll" "avformat-*.dll" "avutil-*.dll" "swscale-*.dll" "swresample-*.dll")
fi
for component in "${QT_COMPONENTS[@]}"; do MAIN_DLLS+=("Qt${QT_MAJOR}${component}.dll"); done
MAIN_DLLS+=("${QT_PLUGINS[@]}")
DLLS_PRESENT=1
for d in "${MAIN_DLLS[@]}"; do
    if ! compgen -G "$BUILD_DIR/$d" >/dev/null; then
        log "  Main DLL missing: $d; deploying runtime DLLs"
        DLLS_PRESENT=0
        break
    fi
done
[ "$DLLS_PRESENT" -ne 1 ] || log "  Main runtime DLLs present; verifying their dependencies"
# Проверяем зависимости на каждом запуске: основные DLL не гарантируют полный комплект.
{
    runtime_inspect "$BUILD_DIR/vibechat.exe"
    EXE_IMPORTS="$import_list"
    QT_SOURCES=()
    declare -A QT_COMPONENT_SOURCES=()
    declare -A QT_PLUGIN_SOURCES=()
    for component in "${QT_COMPONENTS[@]}"; do
        component_source="$RUNTIME_PREFIX/bin/Qt${QT_MAJOR}${component}.dll"
        if [[ -n "$QT_MULTIMEDIA_RUNTIME" && ( "$component" = Multimedia || "$component" = MultimediaWidgets ) ]]; then
            component_source="$QT_MULTIMEDIA_RUNTIME/bin/Qt${QT_MAJOR}${component}.dll"
        fi
        QT_SOURCES+=("$component_source"); QT_COMPONENT_SOURCES["$component"]="$component_source"
    done
    for plugin in "${QT_PLUGINS[@]}"; do
        plugin_source=""
        for dir in "$RUNTIME_PREFIX/share/qt${QT_MAJOR}/plugins" "$RUNTIME_PREFIX/lib/qt${QT_MAJOR}/plugins"; do
            [ -f "$dir/$plugin" ] && { plugin_source="$dir/$plugin"; break; }
        done
        if [[ -n "$QT_MULTIMEDIA_RUNTIME" && "$plugin" = multimedia/ffmpegmediaplugin.dll ]]; then
            plugin_source="$QT_MULTIMEDIA_RUNTIME/plugins/$plugin"
        fi
        [ -n "$plugin_source" ] && [ -f "$plugin_source" ] || { log "  ERROR: $QT_PACKAGE plugin missing: $plugin"; exit 1; }
        QT_SOURCES+=("$plugin_source")
        QT_PLUGIN_SOURCES["$plugin"]="$plugin_source"
    done
    QT_TOOLS=()
    for candidate in "$RUNTIME_PREFIX/bin/windeployqt${QT_MAJOR}.exe" "$RUNTIME_PREFIX/bin/windeployqt-qt${QT_MAJOR}.exe" "$RUNTIME_PREFIX/bin/windeployqt.exe"; do
        [ ! -x "$candidate" ] || QT_TOOLS+=("$candidate")
    done
    QT_STAMP=$(stat -Lc '%n|%s|%y|%z' -- "${QT_SOURCES[@]}" "${QT_TOOLS[@]}") || {
        log "  ERROR: cannot fingerprint Qt deployment inputs"; exit 1;
    }
    QT_STAMP="$CACHE_POLICY"$'\n'"$QT_PACKAGE"$'\n'"$EXE_IMPORTS"$'\n'"$QT_STAMP"
    QT_CACHED=""
    [ ! -f "$RUNTIME_CACHE/qt-deploy" ] || QT_CACHED=$(< "$RUNTIME_CACHE/qt-deploy")
    QT_STARTED=$SECONDS
    if [ "$QT_CACHED" = "$QT_STAMP" ]; then
        log "  Qt deployment inputs unchanged; using cached deployment"
    else
        WINDEPLOYQT=""
        for candidate in "${QT_TOOLS[@]}"; do
            tool_version=$("$candidate" --version 2>&1) || { log "  WARN: cannot query $candidate: $tool_version"; continue; }
            if [[ "$tool_version" =~ (^|[[:space:]])${QT_MAJOR}\.[0-9]+ ]]; then WINDEPLOYQT="$candidate"; break; fi
            log "  WARN: ignoring $candidate ($tool_version); build uses $QT_PACKAGE"
        done
        if [ -x "$WINDEPLOYQT" ]; then
            log "  Running $WINDEPLOYQT..."
            if "$WINDEPLOYQT" --no-translations --no-compiler-runtime "$BUILD_DIR/vibechat.exe" 2>&1 | tee -a "$LOG_FILE"; then
                log "  OK: windeployqt completed"
            else
                RC=${PIPESTATUS[0]}
                log "  WARN: windeployqt failed (exit $RC); verifying direct DLL deployment"
            fi
        else
            log "  WARN: matching $QT_PACKAGE windeployqt not found; copying dependencies directly"
        fi
    fi
    log "  Qt deployment: elapsed=$((SECONDS - QT_STARTED))s"

    # Основные Qt DLL и динамические плагины всегда берём из выбранного комплекта.
    for component in "${QT_COMPONENTS[@]}"; do
        qt_dll="Qt${QT_MAJOR}${component}.dll"
        runtime_copy "${QT_COMPONENT_SOURCES[$component]}" "$BUILD_DIR/$qt_dll"
    done
    for plugin in "${QT_PLUGINS[@]}"; do
        runtime_copy "${QT_PLUGIN_SOURCES[$plugin]}" "$BUILD_DIR/$plugin"
    done

    # Резолвим НЕ-Qt зависимости (FFmpeg, mingw-runtime): windeployqt копирует только Qt.
    # FFmpeg ищем ПЕРВЫМ в $FFMPEG_BIN: exe линкован именно под кастомный билд,
    # а pacman-ffmpeg той же мажорной версии в /ucrt64/bin дал бы неверный набор кодеков.
    shopt -s nocasematch nullglob
    # Обновляем весь комплект FFmpeg, включая DLL, не импортируемые напрямую приложением.
    for src in "$FFMPEG_BIN/"*.dll; do
        # Общий runtime берём из установленного UCRT64, а кодеки — из собственной сборки FFmpeg.
        if chatgui_is_compiler_runtime_dll "${src##*/}"; then
            src="$RUNTIME_PREFIX/bin/${src##*/}"
        fi
        runtime_copy "$src" "$BUILD_DIR/${src##*/}"
    done
    log "  Resolving non-Qt dependencies recursively..."
    log "  Windows system DLLs are provided by the OS and excluded from deployment"
    # Проверяем также уже скопированные DLL: наличие файла не гарантирует наличие его зависимостей.
    todo=("$BUILD_DIR/vibechat.exe" "$BUILD_DIR/"*.dll "$BUILD_DIR/platforms/"*.dll "$BUILD_DIR/imageformats/"*.dll "$BUILD_DIR/multimedia/"*.dll)
    declare -A scanned=() required_by=()
    MISSING=0
    IMPORTS_STARTED=$SECONDS
    for ((i = 0; i < ${#todo[@]}; i++)); do
        src="${todo[i]}"
        key="${src,,}"
        [ -n "${scanned[$key]:-}" ] && continue
        scanned[$key]=1
        if [ "$src" = "$BUILD_DIR/vibechat.exe" ]; then
            import_list="$EXE_IMPORTS"
        else
            # Обновляем и уже имеющиеся транзитивные DLL до чтения их импортов.
            if [ -z "${RUNTIME_SOURCES[$key]:-}" ]; then
                dep=${src#"$BUILD_DIR/"}
                dep_src=""
                dep_dirs=("$FFMPEG_BIN" "$RUNTIME_PREFIX/bin" "$SRC_DIR/rlottie_build")
                if chatgui_is_compiler_runtime_dll "$dep"; then
                    dep_dirs=("$RUNTIME_PREFIX/bin")
                elif [[ "$dep" = */* ]]; then
                    dep_dirs=("$RUNTIME_PREFIX/share/qt${QT_MAJOR}/plugins" "$RUNTIME_PREFIX/lib/qt${QT_MAJOR}/plugins")
                fi
                for dir in "${dep_dirs[@]}"; do
                    [ -f "$dir/$dep" ] && { dep_src="$dir/$dep"; break; }
                done
                if [ -n "$dep_src" ]; then
                    runtime_copy "$dep_src" "$src"
                elif [ ! -f "$src" ]; then
                    log "  ERROR: ${required_by[$key]:-runtime} requires $dep; not found in FFmpeg, UCRT64 or rlottie directories"
                    MISSING=$((MISSING + 1))
                    continue
                fi
            fi
            runtime_inspect "$src" || { MISSING=$((MISSING + 1)); continue; }
        fi
        while read -r dep; do
            [ -z "$dep" ] && continue
            case "$dep" in
                kernel32.dll|msvcrt.dll|msvcp140.dll|vcruntime140.dll|ucrtbase.dll|\
                user32.dll|gdi32.dll|gdi32full.dll|shell32.dll|shlwapi.dll|shcore.dll|advapi32.dll|\
                ole32.dll|oleaut32.dll|comdlg32.dll|comctl32.dll|ws2_32.dll|wsock32.dll|\
                iphlpapi.dll|netapi32.dll|dnsapi.dll|winhttp.dll|winmm.dll|imm32.dll|version.dll|\
                setupapi.dll|crypt32.dll|authz.dll|bcrypt.dll|bcryptprimitives.dll|ncrypt.dll|dbghelp.dll|imagehlp.dll|kernelbase.dll|\
                rpcrt4.dll|psapi.dll|powrprof.dll|cfgmgr32.dll|userenv.dll|secur32.dll|\
                uxtheme.dll|dwmapi.dll|d3d9.dll|d3d11.dll|d3d12.dll|dxgi.dll|opengl32.dll|glu32.dll|\
                wtsapi32.dll|mpr.dll|ntdll.dll|avicap32.dll|msimg32.dll|wldap32.dll|gdiplus.dll|usp10.dll|\
                dwrite.dll|winspool.drv|api-ms-win-*)
                    continue ;;
                mf.dll|mfplat.dll|mfreadwrite.dll|mfuuid.dll|dxva2.dll|mmdevapi.dll|avrt.dll|propsys.dll|ksuser.dll|msdmo.dll)
                    continue ;;
            esac
            required_by["${BUILD_DIR,,}/${dep,,}"]="$src"
            todo+=("$BUILD_DIR/$dep")
        done <<< "$import_list"
    done
    shopt -u nocasematch nullglob
    [ "$MISSING" -eq 0 ] || { log "  ERROR: runtime DLL deployment incomplete"; exit 1; }
    # Проверяем тот же основной комплект после копирования.
    log "  Verifying main runtime DLLs..."
    MISSING=0
    for d in "${MAIN_DLLS[@]}"; do
        if ! compgen -G "$BUILD_DIR/$d" >/dev/null; then
            log "  ERROR: $d MISSING in build dir"; MISSING=1
        fi
    done
    if [ "$MISSING" -eq 1 ]; then
        log "  ERROR: required runtime DLLs missing"; exit 1
    fi
    [ "$QT_CACHED" = "$QT_STAMP" ] || runtime_cache_write "$RUNTIME_CACHE/qt-deploy" "$QT_STAMP"
    log "  PE imports: scanned=$RUNTIME_SCANNED cached=$RUNTIME_HITS elapsed=$((SECONDS - IMPORTS_STARTED))s"
}
log "  Runtime DLLs: copied=$RUNTIME_COPIED unchanged=$RUNTIME_CURRENT elapsed=$((SECONDS - DEPLOY_STARTED))s"
cp "$SRC_DIR/setup_firewall.bat" "$BUILD_DIR/" 2>/dev/null && log "  OK: setup_firewall.bat" || log "  WARN: setup_firewall.bat not found"

log ""
log "[4/4] Verifying..."
EXE="$BUILD_DIR/vibechat.exe"
if [ -f "$EXE" ]; then
    SIZE=$(du -h "$EXE" | cut -f1)
    log "  OK: vibechat.exe ($SIZE)"
    if [[ "$BUILD_INSTALLER" = 1 ]]; then
        log "Creating Windows installer..."
        bash "$SRC_DIR/package_win.sh" 2>&1 | tee -a "$LOG_FILE"
    fi
    log ""
    log "================================"
    log "Build SUCCESS"
    log "================================"
    log "Executable: $EXE"
else
    log "  FAIL: vibechat.exe not found"
    exit 1
fi
