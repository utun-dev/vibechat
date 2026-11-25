#!/usr/bin/env bash
# Полная сборка Linux: системные инструменты → внешние зависимости → ядро → GUI.
# Все реальные сборки выполняют штатные build.sh и tools/chatgui/build_nix.sh.
set -euo pipefail
cd "$(dirname "$0")"
if [ -f ./proxy.sh ]; then
    . ./proxy.sh || { echo 'ERROR: cannot load proxy.sh' >&2; exit 1; }
fi

JOBS=4
GUI=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --help|-h)
            echo "Usage: ./build_linux_full.sh [-jN] [--no-gui]"
            echo "Prepares dependencies and builds uTun and VibeChat; missing packages are installed through apt/sudo."
            exit 0 ;;
        --no-gui) GUI=0 ;;
        -j)
            [[ $# -gt 1 ]] || { echo "ERROR: -j requires a job count" >&2; exit 1; }
            shift; JOBS=$1 ;;
        -j*) JOBS=${1#-j} ;;
        *) echo "ERROR: unknown argument: $1; use --help" >&2; exit 1 ;;
    esac
    shift
done
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo "ERROR: job count must be positive: $JOBS" >&2; exit 1; }
[[ $(uname -s) = Linux ]] || { echo "ERROR: this script requires Linux; FreeBSD: ./build_bsd_full.sh" >&2; exit 1; }

# Проверяем инструменты до скачивания исходников. Для других дистрибутивов перечисляем недостающее.
required=(python3 curl tar patch make gcc g++ autoreconf automake libtoolize pkg-config)
[[ "$GUI" = 0 ]] || required+=(cmake)
missing=()
for tool in "${required[@]}"; do command -v "$tool" >/dev/null || missing+=("$tool"); done
if (( ${#missing[@]} )); then
    echo "INFO: missing build tools: ${missing[*]}"
    command -v apt-get >/dev/null || {
        echo "ERROR: install these tools with your distribution package manager, then rerun: ${missing[*]}" >&2; exit 1;
    }
    SUDO=()
    if [[ $(id -u) != 0 ]]; then
        command -v sudo >/dev/null || { echo "ERROR: installing build tools requires root or sudo" >&2; exit 1; }
        SUDO=(sudo --preserve-env=http_proxy,https_proxy,HTTP_PROXY,HTTPS_PROXY,no_proxy,NO_PROXY)
    fi
    "${SUDO[@]}" apt-get update &&
        "${SUDO[@]}" apt-get install -y build-essential autoconf automake libtool pkg-config python3 curl ca-certificates tar patch cmake || {
        echo "ERROR: apt could not install build tools; check network access and package-manager permissions" >&2; exit 1;
    }
    for tool in "${required[@]}"; do
        command -v "$tool" >/dev/null || { echo "ERROR: $tool is still missing after apt installation" >&2; exit 1; }
    done
fi

# Одинаковые пути OpenSSL для обычной/полной сборки и дочерней CMake-сборки GUI.
source tools/build_openssl.sh

# Заголовок и символ HPKE проверяются вместе: одной подходящей версии openssl CLI недостаточно.
probe=$(mktemp -d)
trap 'rm -rf "$probe"' EXIT
read -r -a cpp_options <<< "${CPPFLAGS:-}"
read -r -a ld_options <<< "${LDFLAGS:-}"
if ! printf '%s\n' '#include <openssl/hpke.h>' \
    'int main(void) { void (* volatile symbol)(void) = (void (*)(void))OSSL_HPKE_CTX_new; return symbol == 0; }' |
    gcc "${cpp_options[@]}" "${ld_options[@]}" -x c - -lcrypto -o "$probe/hpke" > "$probe/error.log" 2>&1; then
    cat "$probe/error.log" >&2
    echo "ERROR: OpenSSL development headers/library with HPKE are required (OpenSSL 3.2+)." >&2
    echo "On Ubuntu x86_64 run: sudo ./tools/install_openssl_ubuntu.sh; then rerun this build script." >&2
    exit 1
fi

# Каждая штатная сборка сама готовит свои зависимости; Android-компоненты здесь не нужны.
echo "INFO: building uTun"
bash build.sh --full --no-gui "-j$JOBS"
if [[ "$GUI" = 1 ]]; then
    echo "INFO: building VibeChat"
    if command -v apt-get >/dev/null; then
        bash tools/chatgui/build_nix.sh --full "-j$JOBS"
    else
        # Без apt используем установленные системные библиотеки; CMake назовёт недостающие.
        bash tools/chatgui/build_nix.sh "-j$JOBS"
    fi
fi
echo "INFO: build complete: src/utun"
[[ "$GUI" = 0 ]] || echo "INFO: GUI: tools/chatgui/build/vibechat"
