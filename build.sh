#!/usr/bin/env bash
# Инкрементальная сборка Linux, FreeBSD и Windows/MSYS2; тесты собираются целями check.
set -euo pipefail
cd "$(dirname "$0")"
if [ -f ./proxy.sh ]; then
    . ./proxy.sh || { echo 'ERROR: cannot load proxy.sh' >&2; exit 1; }
fi

MODE=normal
CLEAN=0
FULL=0
BUILD_GUI=1
BUILD_INSTALLER=0
INSTALLER_KIND=""
JOBS=4
MAKE_ARGS=()
MAKE_SETTINGS=()
STATE=.utun-build-state

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            echo "Usage: ./build.sh [--clean] [--full] [--asan|--hardened] [--no-gui] [--installer|--deb] [-jN] [make arguments]"
            echo "  --clean     Rebuild objects without forcing configure"
            echo "  --full      Regenerate, configure and rebuild everything"
            echo "  --asan      AddressSanitizer"
            echo "  --hardened  Enable compiler security hardening"
            echo "  --no-gui    Skip the separately configured ChatGUI"
            echo "  --installer Build ChatGUI Release installer: DEB on Linux, Inno Setup on Windows"
            echo "  --deb       Build Linux ChatGUI DEB in tools/chatgui/build-deb/installer"
            echo "  -jN, -j N   Parallel jobs (default: 4)"
            echo "  CC/CFLAGS/CPPFLAGS/LDFLAGS/LIBS assignments are passed to configure"
            echo "  Tests are built on demand: ./check.sh or ./build.sh --no-gui check"
            exit 0 ;;
        --clean) CLEAN=1 ;;
        --full) FULL=1 ;;
        --no-gui) BUILD_GUI=0 ;;
        --installer) BUILD_INSTALLER=1 ;;
        --deb) BUILD_INSTALLER=1; INSTALLER_KIND=deb ;;
        --asan|--hardened)
            selected_mode=${1#--}
            if [[ "$MODE" != normal && "$MODE" != "$selected_mode" ]]; then
                echo "ERROR: --asan and --hardened cannot be combined" >&2; exit 1
            fi
            MODE=$selected_mode ;;
        -j)
            [[ $# -gt 1 ]] || { echo "ERROR: -j requires a positive job count" >&2; exit 1; }
            shift; JOBS=$1 ;;
        -j*) JOBS=${1#-j} ;;
        CC=*|CFLAGS=*|CPPFLAGS=*|LDFLAGS=*|LIBS=*) export "$1" ;;
        CC+=*|CFLAGS+=*|CPPFLAGS+=*|LDFLAGS+=*|LIBS+=*)
            echo "ERROR: use NAME=value for configure flags; append assignments are not supported" >&2; exit 1 ;;
        *)
            MAKE_ARGS+=("$1")
            [[ "$1" != *=* ]] || MAKE_SETTINGS+=("$1") ;;
    esac
    shift
done
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo "ERROR: invalid job count: $JOBS" >&2; exit 1; }
PLATFORM=$(uname -s)
if [[ "${MSYSTEM:-}" = UCRT64 && "$BUILD_GUI" = 1 ]]; then
    bash tools/chatgui/install_deps_win.sh || { echo "ERROR: Windows build dependencies could not be installed" >&2; exit 1; }
fi
if [[ "$BUILD_INSTALLER" = 1 ]]; then
    [[ "$BUILD_GUI" = 1 && "$MODE" = normal ]] || { echo "ERROR: --installer requires GUI and normal build mode" >&2; exit 1; }
    if [[ "$PLATFORM" = Linux && -z "${MSYSTEM:-}" ]]; then
        INSTALLER_KIND=deb
        bash tools/chatgui/package_deb.sh --check
    elif [[ "${MSYSTEM:-}" = UCRT64 && "$INSTALLER_KIND" != deb ]]; then
        INSTALLER_KIND=windows
        bash tools/chatgui/package_win.sh --check
    else
        echo "ERROR: --installer requires Linux or Windows/UCRT64; --deb requires Linux" >&2; exit 1
    fi
fi

MAKE_TOOL=${MAKE:-make}
if [[ "$PLATFORM" = FreeBSD ]]; then
    MAKE_TOOL=${MAKE:-gmake}
    export CC="${CC:-clang}"
fi
MAKE_PATH=$(command -v "$MAKE_TOOL") || { echo "ERROR: GNU make not found: $MAKE_TOOL (FreeBSD: pkg install gmake)" >&2; exit 1; }
MAKE_ID=$("$MAKE_TOOL" --version) || { echo "ERROR: GNU make is required; use MAKE=gmake on BSD" >&2; exit 1; }
[[ "$MAKE_ID" = *'GNU Make'* ]] || { echo "ERROR: GNU make is required; use MAKE=gmake on BSD" >&2; exit 1; }
export MAKE="$MAKE_TOOL"
if command -v sha256sum >/dev/null; then
    HASH_COMMAND=(sha256sum)
elif command -v sha256 >/dev/null; then
    HASH_COMMAND=(sha256 -r)
elif command -v shasum >/dev/null; then
    HASH_COMMAND=(shasum -a 256)
else
    echo "ERROR: SHA-256 utility not found: sha256sum, sha256 or shasum is required" >&2; exit 1
fi
read -r -a COMPILER <<< "${CC:-gcc}"
COMPILER_PATH=$(command -v "${COMPILER[0]}") || { echo "ERROR: compiler not found: ${CC:-gcc}" >&2; exit 1; }
COMPILER_ID=$("${COMPILER[@]}" --version) || { echo "ERROR: cannot identify compiler: ${CC:-gcc}" >&2; exit 1; }
COMPILER_TARGET=$("${COMPILER[@]}" -dumpmachine) || { echo "ERROR: cannot identify compiler target: ${CC:-gcc}" >&2; exit 1; }

if [[ "$PLATFORM" = Linux && -z "${MSYSTEM:-}" && "$COMPILER_TARGET" = x86_64*-linux-gnu* ]]; then
    source tools/build_openssl.sh
fi

command -v python3 >/dev/null || {
    echo "ERROR: Python 3.9+ is required to prepare dependencies (MSYS2: pacman -S python curl patch)" >&2; exit 1;
}
DEPENDENCIES=(common sodium dnsmasq)
if [[ -n "${MSYSTEM:-}" || "$COMPILER_TARGET" = *mingw* || "$COMPILER_TARGET" = *msys* ||
      "$COMPILER_TARGET" = *cygwin* || "$COMPILER_TARGET" = *windows* ]]; then
    DEPENDENCIES+=(wintun)
fi
echo "INFO: dependency components: target=$COMPILER_TARGET components=${DEPENDENCIES[*]}"
for component in "${DEPENDENCIES[@]}"; do
    dependency_args=(--only "$component" --if-needed)
    if [[ "$component" = sodium ]]; then dependency_args+=(--sodium-target "$COMPILER_TARGET"); fi
    CC="${CC:-gcc}" python3 tools/prepare_dependencies.py "${dependency_args[@]}" || {
        echo "ERROR: $component dependency preparation failed; build stopped before configure" >&2; exit 1;
    }
done

# Хеши учитывают содержимое и имена файлов, включая добавление/удаление макросов.
hash_files() {
    local result
    result=$({ printf '%s\0' "$@"; "${HASH_COMMAND[@]}" "$@"; } | "${HASH_COMMAND[@]}")
    printf '%s\n' "${result%% *}"
}

BUILD_DIRS=(lib lib/libopus src tests)
AUTOTOOLS_INPUTS=(configure.ac Makefile.am lib/Makefile.am lib/libopus/Makefile.am src/Makefile.am tests/Makefile.am)
AUTOTOOLS_OUTPUTS=(configure aclocal.m4 config.h.in Makefile.in lib/Makefile.in lib/libopus/Makefile.in src/Makefile.in tests/Makefile.in)
CONFIG_OUTPUTS=(config.status config.h Makefile lib/Makefile lib/libopus/Makefile src/Makefile tests/Makefile)
shopt -s globstar nullglob
for macro in m4/**/*.m4; do AUTOTOOLS_INPUTS+=("$macro"); done
GENERATION_INPUT=$(hash_files "${AUTOTOOLS_INPUTS[@]}")
GENERATION_OUTPUT=missing
CONFIG_OUTPUT=missing
missing_generation=0
missing_configuration=0
for file in "${AUTOTOOLS_OUTPUTS[@]}"; do [[ -f "$file" ]] || missing_generation=1; done
for file in "${CONFIG_OUTPUTS[@]}"; do [[ -f "$file" ]] || missing_configuration=1; done
if [[ "$missing_generation" = 0 ]]; then GENERATION_OUTPUT=$(hash_files "${AUTOTOOLS_OUTPUTS[@]}"); fi
if [[ "$missing_configuration" = 0 ]]; then CONFIG_OUTPUT=$(hash_files "${CONFIG_OUTPUTS[@]}"); fi

FFMPEG_PACKAGES=(libavformat libavcodec libavutil libswscale libswresample)
FFMPEG_OPTION=--without-ffmpeg
FFMPEG_ID=unavailable
FFMPEG_CFLAGS=""
FFMPEG_LDFLAGS=""
if command -v pkg-config >/dev/null && pkg-config --exists "${FFMPEG_PACKAGES[@]}"; then
    FFMPEG_OPTION=--with-ffmpeg
    FFMPEG_ID=$(pkg-config --modversion "${FFMPEG_PACKAGES[@]}")
    FFMPEG_CFLAGS=$(pkg-config --cflags "${FFMPEG_PACKAGES[@]}")
    FFMPEG_LDFLAGS=$(pkg-config --libs-only-L "${FFMPEG_PACKAGES[@]}")
fi
request_sum=$({
    printf '%s\0' "$MODE" "$PLATFORM" "${MSYSTEM:-}" "${MSYSTEM_PREFIX:-}" "${CC:-gcc}" "$COMPILER_PATH" "$COMPILER_ID" "$COMPILER_TARGET"
    printf '%s\0' "$MAKE_PATH" "$MAKE_ID" "${HASH_COMMAND[@]}"
    printf '%s\0' "${CFLAGS+x}" "${CFLAGS-}" "${CPPFLAGS+x}" "${CPPFLAGS-}" "${LDFLAGS+x}" "${LDFLAGS-}"
    printf '%s\0' "${LIBS+x}" "${LIBS-}" "${MAKEFLAGS-}" "${DEBUG_FLAGS+x}" "${DEBUG_FLAGS-}"
    printf '%s\0' "$FFMPEG_OPTION" "$FFMPEG_ID" "$FFMPEG_CFLAGS" "$FFMPEG_LDFLAGS"
    printf '%s\0' "${MAKE_SETTINGS[@]}"
    hash_files build.sh
} | "${HASH_COMMAND[@]}")
CONFIG_REQUEST=${request_sum%% *}

OLD_GENERATION_INPUT=""; OLD_GENERATION_OUTPUT=""; OLD_CONFIG_REQUEST=""; OLD_CONFIG_OUTPUT=""
if [[ -f "$STATE" ]]; then
    if ! {
        read -r OLD_GENERATION_INPUT; read -r OLD_GENERATION_OUTPUT
        read -r OLD_CONFIG_REQUEST; read -r OLD_CONFIG_OUTPUT
    } < "$STATE"; then
        echo "WARN: incomplete build state; adopting the build with a clean rebuild"
        OLD_GENERATION_INPUT=""; OLD_GENERATION_OUTPUT=""; OLD_CONFIG_REQUEST=""; OLD_CONFIG_OUTPUT=""
    fi
fi

REGENERATE=0
RECONFIGURE=0
if [[ "$FULL" = 1 || "$GENERATION_INPUT" != "$OLD_GENERATION_INPUT" || "$GENERATION_OUTPUT" != "$OLD_GENERATION_OUTPUT" ]]; then
    echo "INFO: regenerating Autotools: full=$FULL"
    if [[ "$GENERATION_INPUT" != "$OLD_GENERATION_INPUT" ]]; then echo "INFO: Autotools descriptions changed or unrecorded"; fi
    if [[ "$GENERATION_OUTPUT" != "$OLD_GENERATION_OUTPUT" ]]; then echo "INFO: generated Autotools files changed or missing"; fi
    command -v autoreconf >/dev/null || { echo "ERROR: autoreconf is required to regenerate the build" >&2; exit 1; }
    mkdir -p m4
    autoreconf -fi 2>&1 | tee autoreconf.log || { echo "ERROR: autoreconf failed; see autoreconf.log" >&2; exit 1; }
    REGENERATE=1
fi
if [[ "$REGENERATE" = 1 || "$CONFIG_REQUEST" != "$OLD_CONFIG_REQUEST" || "$CONFIG_OUTPUT" != "$OLD_CONFIG_OUTPUT" ]]; then
    echo "INFO: configuring: mode=$MODE regenerated=$REGENERATE"
    if [[ "$CONFIG_REQUEST" != "$OLD_CONFIG_REQUEST" ]]; then echo "INFO: compiler, flags or optional dependencies changed or unrecorded"; fi
    if [[ "$CONFIG_OUTPUT" != "$OLD_CONFIG_OUTPUT" ]]; then echo "INFO: configuration files changed or missing"; fi
    CONFIG_ARGS=("$FFMPEG_OPTION" --disable-hardening)
    if [[ "$MODE" = asan ]]; then
        export CFLAGS="${CFLAGS-} -g -Og -fsanitize=address -fno-omit-frame-pointer -fno-pie"
        export LDFLAGS="${LDFLAGS-} -fsanitize=address -no-pie"
        if [[ "$PLATFORM" = FreeBSD ]]; then export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"; fi
    elif [[ "$MODE" = hardened ]]; then
        CONFIG_ARGS+=(--enable-hardening)
    fi
    if [[ "$FFMPEG_OPTION" = --with-ffmpeg ]]; then
        export CPPFLAGS="${CPPFLAGS-} $FFMPEG_CFLAGS"
        export LDFLAGS="${LDFLAGS-} $FFMPEG_LDFLAGS"
    fi
    SODIUM_PREFIX="$PWD/.cache/dependencies/sodium/$COMPILER_TARGET" ./configure "${CONFIG_ARGS[@]}" 2>&1 | tee configure.log || {
        echo "ERROR: configure failed; see configure.log" >&2; exit 1;
    }
    RECONFIGURE=1
fi

# Проверяем только зависимости объектов, перечисленных Automake; посторонние сборки не затрагиваем.
MISSING_DEPS=0
for directory in "${BUILD_DIRS[@]}"; do
    while read -r keyword depfile marker; do
        [[ "$keyword" = include && "$marker" = '# am--include-marker' && "$depfile" = *.Po ]] || continue
        depfile=${depfile//\$\(DEPDIR\)/.deps}
        object=${depfile/.deps\//}
        object=${object%.Po}
        [[ -f "$directory/$object.o" || -f "$directory/$object.obj" ]] || continue
        dependency_line=""
        if [[ -s "$directory/$depfile" ]]; then IFS= read -r dependency_line < "$directory/$depfile" || true; fi
        if [[ "$dependency_line" != *:* ]]; then
            echo "WARN: missing or empty dependencies for $directory/$object: $directory/$depfile"
            MISSING_DEPS=1
            break
        fi
    done < "$directory/Makefile"
done
if [[ "$RECONFIGURE" = 1 || "$CLEAN" = 1 || "$MISSING_DEPS" = 1 ]]; then
    echo "INFO: cleaning objects: reconfigured=$RECONFIGURE requested=$CLEAN missing_dependencies=$MISSING_DEPS"
    "$MAKE_TOOL" clean 2>&1 | tee build.log || { echo "ERROR: clean failed; see build.log" >&2; exit 1; }
else
    echo "INFO: configuration unchanged; incremental build (mode=$MODE)"
    : > build.log
fi

echo "INFO: compiling: mode=$MODE jobs=$JOBS"
echo "INFO: tests are built on demand by check targets or an explicit test target"
"$MAKE_TOOL" "-j$JOBS" "${MAKE_ARGS[@]}" 2>&1 | tee -a build.log || { echo "ERROR: build failed; see build.log" >&2; exit 1; }

# Состояние обновляется после успешной сборки ядра; ошибка GUI не инвалидирует готовые объекты.
GENERATION_OUTPUT=$(hash_files "${AUTOTOOLS_OUTPUTS[@]}")
CONFIG_OUTPUT=$(hash_files "${CONFIG_OUTPUTS[@]}")
state_tmp="$STATE.tmp.$$"
trap 'rm -f "$state_tmp"' EXIT
printf '%s\n' "$GENERATION_INPUT" "$GENERATION_OUTPUT" "$CONFIG_REQUEST" "$CONFIG_OUTPUT" > "$state_tmp"
mv -f "$state_tmp" "$STATE"

if [[ "$BUILD_GUI" = 1 && ( "$BUILD_INSTALLER" = 1 || -f tools/chatgui/build/CMakeCache.txt ) ]]; then
    command -v cmake >/dev/null || { echo "ERROR: ChatGUI exists but cmake is not available" >&2; exit 1; }
    echo "INFO: building ChatGUI with its CMake configuration: jobs=$JOBS"
    if [[ "${MSYSTEM:-}" = UCRT64 ]]; then
        GUI_ARGS=("-j$JOBS")
        [[ "$BUILD_INSTALLER" != 1 ]] || GUI_ARGS+=(--installer)
        (cd tools/chatgui && bash build.sh "${GUI_ARGS[@]}") || { echo "ERROR: ChatGUI build failed" >&2; exit 1; }
    elif [[ "$INSTALLER_KIND" = deb ]]; then
        bash tools/chatgui/build_nix.sh --deb "-j$JOBS" || { echo "ERROR: ChatGUI DEB build failed" >&2; exit 1; }
    else
        bash tools/chatgui/build_nix.sh "-j$JOBS" || { echo "ERROR: ChatGUI build failed" >&2; exit 1; }
    fi
fi
echo "Build completed successfully!"
echo "Binary: src/utun"
