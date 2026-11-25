#!/bin/bash
# cross-build-win.sh — кросс-компиляция uTun для Windows 10 из Linux
# Использование: ./cross-build-win.sh [--clean]
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

step()  { echo -e "${CYAN}===${NC} $*"; }
ok()    { echo -e "${GREEN}[OK]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARNING]${NC} $*"; }
fatal() { echo -e "${RED}FATAL:${NC} $*" >&2; exit 1; }

trap 'fatal "script failed at line $LINENO (exit code: $?)"' ERR

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -f "$SCRIPT_DIR/proxy.sh" ]; then
    . "$SCRIPT_DIR/proxy.sh" || { echo 'ERROR: cannot load proxy.sh' >&2; exit 1; }
fi
BUILD_DIR="$SCRIPT_DIR/build-win"
PREFIX="$BUILD_DIR/prefix"

MINGW_CC="x86_64-w64-mingw32-gcc"
MINGW_AR="x86_64-w64-mingw32-ar"
MINGW_RANLIB="x86_64-w64-mingw32-ranlib"

MINGW_PKGS="gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix python3 curl patch"
OPENSSL_VER="3.0.16"
OPENSSL_URL="https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VER}/openssl-${OPENSSL_VER}.tar.gz"

# -------------------------------------------------------------------
# 0. Аргументы
# -------------------------------------------------------------------
CLEAN=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean) CLEAN=1; shift ;;
        *) fatal "unknown option: $1" ;;
    esac
done

if [ "$CLEAN" -eq 1 ]; then
    echo -e "${YELLOW}Cleaning build directory: $BUILD_DIR${NC}"
    rm -rf "$BUILD_DIR"
fi

echo -e "${CYAN}==============================================${NC}"
echo -e "${CYAN}  uTun Cross-Compilation for Windows 10${NC}"
echo -e "${CYAN}==============================================${NC}"
echo ""

# -------------------------------------------------------------------
# 1. Проверка системы
# -------------------------------------------------------------------
step "Checking system..."
if [ "$(uname -s)" != "Linux" ]; then
    fatal "This script only runs on Linux. Detected: $(uname -s)"
fi
if [ "$(uname -m)" != "x86_64" ]; then
    fatal "This script requires x86_64 architecture. Detected: $(uname -m)"
fi

if ! command -v apt-get &>/dev/null; then
    fatal "apt-get not found. Only Debian/Ubuntu are supported."
fi
ok "System: Linux x86_64 with apt"

# -------------------------------------------------------------------
# 2. Установка пакетов
# -------------------------------------------------------------------
step "Installing required packages..."

need_install=()
for pkg in $MINGW_PKGS; do
    if ! dpkg -s "$pkg" &>/dev/null 2>&1; then
        need_install+=("$pkg")
    fi
done
if ! command -v wget &>/dev/null && ! command -v curl &>/dev/null; then
    need_install+=("wget")
fi

if [ ${#need_install[@]} -gt 0 ]; then
    echo "  Installing: ${need_install[*]}"
    sudo --preserve-env=http_proxy,https_proxy,HTTP_PROXY,HTTPS_PROXY,no_proxy,NO_PROXY apt-get update -qq || fatal "apt-get update failed"
    sudo --preserve-env=http_proxy,https_proxy,HTTP_PROXY,HTTPS_PROXY,no_proxy,NO_PROXY apt-get install -y -qq "${need_install[@]}" ||
        fatal "apt-get install failed"
    ok "Packages installed: ${need_install[*]}"
else
    ok "All packages already installed"
fi

# Проверка компилятора
for tool in "$MINGW_CC" "$MINGW_AR" "$MINGW_RANLIB"; do
    if ! command -v "$tool" &>/dev/null; then
        fatal "$tool not found after package installation"
    fi
done
ok "Cross-compiler: $($MINGW_CC --version | head -1)"

for component in common wintun; do
    step "Preparing $component dependencies..."
    python3 "$SCRIPT_DIR/tools/prepare_dependencies.py" --only "$component" --if-needed || fatal "$component dependency preparation failed"
done

# -------------------------------------------------------------------
# 3. Кросс-компиляция OpenSSL
# -------------------------------------------------------------------
OPENSSL_SRC="$BUILD_DIR/openssl-${OPENSSL_VER}"
LIBCRYPTO="$PREFIX/lib/libcrypto.a"

if [ -f "$LIBCRYPTO" ]; then
    ok "OpenSSL already built: $LIBCRYPTO"
else
    step "Building OpenSSL ${OPENSSL_VER} for Windows..."

    mkdir -p "$BUILD_DIR"

    TARBALL="$BUILD_DIR/openssl-${OPENSSL_VER}.tar.gz"
    if [ ! -f "$TARBALL" ]; then
        echo "  Downloading OpenSSL ${OPENSSL_VER}..."
        if command -v wget &>/dev/null; then
            wget -q --show-progress -O "$TARBALL" "$OPENSSL_URL" || fatal "wget download failed for $OPENSSL_URL"
        else
            curl -L --progress-bar -o "$TARBALL" "$OPENSSL_URL" || fatal "curl download failed for $OPENSSL_URL"
        fi
        ok "Downloaded: $TARBALL"
    fi

    if [ ! -d "$OPENSSL_SRC" ]; then
        echo "  Extracting..."
        tar xzf "$TARBALL" -C "$BUILD_DIR" || fatal "tar extract failed"
    fi

    echo "  Configuring OpenSSL for mingw64..."
    cd "$OPENSSL_SRC"
    ./Configure --cross-compile-prefix=x86_64-w64-mingw32- \
        mingw64 no-shared no-tests \
        --prefix="$PREFIX" --libdir="$PREFIX/lib" --openssldir="$PREFIX/ssl" \
        || fatal "OpenSSL configure failed (see output above)"
    ok "OpenSSL configured"

    echo "  Compiling OpenSSL..."
    make -j"$(nproc)" || fatal "OpenSSL make failed"
    echo "  Installing OpenSSL..."
    make install_sw >/dev/null || fatal "OpenSSL make install_sw failed"
    cd "$SCRIPT_DIR"

    # OpenSSL может установить в lib64/ вместо lib/ — поправим
    if [ ! -f "$LIBCRYPTO" ] && [ -f "$PREFIX/lib64/libcrypto.a" ]; then
        ln -sf lib64 "$PREFIX/lib" 2>/dev/null || cp -r "$PREFIX/lib64" "$PREFIX/lib" 2>/dev/null || true
    fi
    if [ ! -f "$LIBCRYPTO" ]; then
        fatal "libcrypto.a was not found at $PREFIX/lib/ or $PREFIX/lib64/"
    fi
    ok "OpenSSL built: $LIBCRYPTO"
fi

# -------------------------------------------------------------------
# 4. Сборка libuasync.a
# -------------------------------------------------------------------
step "Building libuasync.a..."

LIB_OBJDIR="$BUILD_DIR/lib-objs"
mkdir -p "$LIB_OBJDIR"

# Все .c файлы из lib/Makefile.am + myip.c (реализация get_my_ip)
LIB_SOURCES=(
    u_async.c ll_queue.c memory_pool.c debug_config.c timeout_heap.c
    sha256.c mem.c serialize.c swm_min.c getmyip.c radix.c tcp_io.c
    myip.c platform_compat.c socket_compat.c
)

LIB_CFLAGS="-D_ISOC99_SOURCE -DDEBUG_OUTPUT_STDERR -DUSE_OPENSSL"
LIB_CFLAGS+=" -I$SCRIPT_DIR/lib -I$SCRIPT_DIR/src"
LIB_CFLAGS+=" -I$PREFIX/include"
LIB_CFLAGS+=" -g -O2 -Wno-deprecated-declarations"

for src in "${LIB_SOURCES[@]}"; do
    obj="$LIB_OBJDIR/$(basename "${src%.c}.o")"
    echo "  CC lib/$src"
    "$MINGW_CC" $LIB_CFLAGS -c "$SCRIPT_DIR/lib/$src" -o "$obj" \
        || fatal "compilation failed: lib/$src"
done

"$MINGW_AR" crs "$BUILD_DIR/libuasync.a" "$LIB_OBJDIR"/*.o \
    || fatal "ar failed for libuasync.a"
"$MINGW_RANLIB" "$BUILD_DIR/libuasync.a" 2>/dev/null || true
ok "libuasync.a ($(wc -c < "$BUILD_DIR/libuasync.a") bytes)"

# -------------------------------------------------------------------
# 5. Сборка объектных файлов для utun.exe
# -------------------------------------------------------------------
step "Compiling utun source files..."

SRC_OBJDIR="$BUILD_DIR/src-objs"
mkdir -p "$SRC_OBJDIR"

# Все .c файлы из src/Makefile.am (utun_CORE_SOURCES)
SRC_SOURCES=(
    utun.c utun_instance.c config_parser.c config_updater.c
    routing_layer/route_lib.c routing_layer/route6_lib.c topo_bgp.c routing_layer/route_ping.c
    routing_layer/topo_node.c routing_layer/topo_node_sqlite.c routing_layer/route_connectivity.c routing_layer/conn_mgr.c
    routing_layer/routing.c tun_if.c tun_route.c tun_linux.c tun_freebsd.c tun_windows.c
    transport_layer/etcp.c transport_layer/etcp_session.c transport_layer/etcp_connections.c transport_layer/etcp_bbr.c transport_layer/etcp_loadbalancer.c
    transport_layer/etcp_debug.c transport_layer/etcp_dump.c transport_layer/secure_channel.c transport_layer/crc32.c
    transport_layer/stcp_link.c transport_layer/stcp.c transport_layer/stcp_server.c transport_layer/stcp_client.c
    transport_layer/pkt_normalizer.c transport_layer/packet_dump.c transport_layer/etcp_api.c
    control_server.c
    firewall.c eim_nat.c nat_transport.c transport_layer/dummynet.c
    etcp_router.c route_crypto.c
    proxy/udp_proxy.c proxy/socks_proxy.c proxy/http_proxy.c
    lwip_tcp/lwip_pbuf.c
)

SRC_CFLAGS="-DUSE_OPENSSL"
SRC_CFLAGS+=" -I$SCRIPT_DIR/src -I$SCRIPT_DIR/lib"
SRC_CFLAGS+=" -I$SCRIPT_DIR/src/transport_layer -I$SCRIPT_DIR/src/routing_layer"
SRC_CFLAGS+=" -I$PREFIX/include"
SRC_CFLAGS+=" -g -O2 -Wno-deprecated-declarations"

TOTAL=${#SRC_SOURCES[@]}
CURRENT=0
FAILED=()

for src in "${SRC_SOURCES[@]}"; do
    CURRENT=$((CURRENT + 1))
    # Имя объектного файла: заменяем / на _ (для proxy/ и lwip_tcp/)
    obj_name="$(echo "$src" | tr '/' '_')"
    obj="$SRC_OBJDIR/${obj_name%.c}.o"
    printf "  [%3d/%3d] CC src/%s\n" "$CURRENT" "$TOTAL" "$src"
    if ! "$MINGW_CC" $SRC_CFLAGS -c "$SCRIPT_DIR/src/$src" -o "$obj" 2>&1; then
        FAILED+=("src/$src")
    fi
done

if [ ${#FAILED[@]} -gt 0 ]; then
    echo ""
    fatal "compilation failed for: ${FAILED[*]}"
fi
ok "All $TOTAL source files compiled"

# -------------------------------------------------------------------
# 6. Линковка utun.exe
# -------------------------------------------------------------------
step "Linking utun.exe..."

"$MINGW_CC" -o "$SCRIPT_DIR/utun.exe" \
    "$SRC_OBJDIR"/*.o \
    "$BUILD_DIR/libuasync.a" \
    "$LIBCRYPTO" \
    -lws2_32 -liphlpapi -ladvapi32 -lbcrypt -lcrypt32 -lpthread -lm \
    -static \
    || fatal "linking failed (see undefined references above)"

ok "utun.exe ($(wc -c < "$SCRIPT_DIR/utun.exe") bytes)"

# -------------------------------------------------------------------
# 7. Копирование wintun.dll и финал
# -------------------------------------------------------------------
echo ""
if [ -f "$SCRIPT_DIR/lib/wintun.dll" ]; then
    cp "$SCRIPT_DIR/lib/wintun.dll" "$SCRIPT_DIR/"
    ok "wintun.dll copied to project root"
else
    warn "wintun.dll not found in lib/ — place it manually alongside utun.exe"
fi

echo ""
echo -e "${GREEN}==============================================${NC}"
echo -e "${GREEN}  Build successful!${NC}"
echo -e "${GREEN}  Binary: utun.exe${NC}"
echo -e "${GREEN}==============================================${NC}"
echo ""

if [ -f "$SCRIPT_DIR/lib/wintun.dll" ]; then
    echo "Run on Windows:"
    echo "  1. Install Wintun driver (https://www.wintun.net)"
    echo "  2. Copy utun.exe + wintun.dll + LICENSE + LICENSING + THIRD_PARTY_NOTICES to the same directory"
    echo "  3. Run: utun.exe -f -p utun.pid -l utun.log"
else
    echo "Run on Windows:"
    echo "  1. Install Wintun driver (https://www.wintun.net)"
    echo "  2. Copy utun.exe + wintun.dll to the same directory"
    echo "  3. Run: utun.exe -f -p utun.pid -l utun.log"
fi
echo ""

exit 0
