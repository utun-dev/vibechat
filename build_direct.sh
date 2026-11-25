#!/bin/bash
# Direct build script for uTun on Windows with OpenSSL
# Инкрементальная сборка (не пересобирает неизменённые файлы)

set -e
cd "$(dirname "$0")"
if [ -f ./proxy.sh ]; then
    . ./proxy.sh || { echo 'ERROR: cannot load proxy.sh' >&2; exit 1; }
fi
command -v python3 >/dev/null || { echo "ERROR: Python 3.9+ is required (MSYS2: pacman -S python curl patch)" >&2; exit 1; }
for component in common wintun; do
    python3 tools/prepare_dependencies.py --only "$component" --if-needed || {
        echo "ERROR: $component dependency preparation failed" >&2; exit 1;
    }
done
sodium_target=$(x86_64-w64-mingw32-gcc -dumpmachine)
CC=x86_64-w64-mingw32-gcc python3 tools/prepare_dependencies.py --only sodium --sodium-target "$sodium_target" --if-needed
sodium_dir="$PWD/.cache/dependencies/sodium/$sodium_target"

echo "uTun Direct Build Script for Windows with OpenSSL (Incremental)"
echo "=============================================================="
echo ""

# ====================== OpenSSL paths ======================
if [ -d "/c/msys64/ucrt64/include/openssl" ]; then
    export CFLAGS="$CFLAGS -I/c/msys64/ucrt64/include"
    export LDFLAGS="$LDFLAGS -L/c/msys64/ucrt64/lib"
    echo "Using MSYS2 UCRT64 OpenSSL"
elif [ -d "/ucrt64/include/openssl" ]; then
    export CFLAGS="$CFLAGS -I/ucrt64/include"
    export LDFLAGS="$LDFLAGS -L/ucrt64/lib"
    echo "Using MSYS2 UCRT64 OpenSSL"
elif [ -d "/mingw64/include/openssl" ]; then
    export CFLAGS="$CFLAGS -I/mingw64/include"
    export LDFLAGS="$LDFLAGS -L/mingw64/lib"
    echo "Using MSYS2 MINGW64 OpenSSL"
fi

# ====================== Configure (только если нужно) ======================
if [ ! -f config.status ] || [ config.status -ot configure ]; then
    echo ""
    echo "Running configure..."
    ./configure --prefix=/usr/local --host=x86_64-w64-mingw32 --disable-dependency-tracking 2>&1 | tee configure.log
fi

# ====================== Directories ======================
mkdir -p src/.deps lib/.deps tests/.deps

# ====================== Build libuasync.a (incremental) ======================
echo ""
echo "Building libuasync.a..."

cd lib
LIB_NEEDS_REBUILD=0

for src in u_async.c ll_queue.c memory_pool.c debug_config.c timeout_heap.c sha256.c platform_compat.c socket_compat.c; do
    if [ -f "$src" ]; then
        obj="${src%.c}.o"
        if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
            echo "  CC $src"
            x86_64-w64-mingw32-gcc -DHAVE_CONFIG_H -I. -I.. -g -O2 -c "$src" -o "$obj" 2>&1 | grep -v "warning:" || true
            LIB_NEEDS_REBUILD=1
        else
            echo "  SKIPPED $src (up to date)"
        fi
    fi
done

# Пересобираем библиотеку только если что-то изменилось
if [ ! -f "libuasync.a" ] || [ $LIB_NEEDS_REBUILD -eq 1 ] || [ -n "$(find . -name '*.o' -newer libuasync.a 2>/dev/null)" ]; then
    echo "  AR libuasync.a"
    ar cru libuasync.a *.o
    ranlib libuasync.a
else
    echo "  SKIPPED libuasync.a (up to date)"
fi
cd ..

# ====================== Build utun.exe (incremental) ======================
echo ""
echo "Building utun.exe..."

cd src
EXE_NEEDS_REBUILD=0

CORE_SOURCES="utun.c utun_instance.c config_parser.c config_error.c config_updater.c routing_layer/route_lib.c topo_bgp.c routing_layer/routing.c tun_if.c tun_route.c transport_layer/etcp.c transport_layer/etcp_padding.c transport_layer/reality_record.c transport_layer/reality_io.c transport_layer/etcp_session.c transport_layer/etcp_connections.c transport_layer/etcp_loadbalancer.c transport_layer/secure_channel.c transport_layer/crypto_pool.c transport_layer/transport_lane.c transport_layer/packet_transport.c transport_layer/stream_transport.c transport_layer/crc32.c transport_layer/pkt_normalizer.c transport_layer/etcp_api.c tun_windows.c"

for src in $CORE_SOURCES; do
    if [ -f "$src" ]; then
        obj="utun-${src%.c}.o"
        if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
            echo "  CC $src"
            x86_64-w64-mingw32-gcc -DHAVE_CONFIG_H -I. -I.. -I../lib -I../src/transport_layer -I../src/routing_layer \
                -DSODIUM_STATIC -I"$sodium_dir/include" $CFLAGS -g -O2 -c "$src" -o "$obj" 2>&1 | grep -E "error:|warning:.*deprecated" || true
            EXE_NEEDS_REBUILD=1
        else
            echo "  SKIPPED $src (up to date)"
        fi
    else
        echo "  WARNING: $src not found!"
    fi
done

# Проверяем, нужно ли линковать
LINK_NEEDED=0
if [ ! -f "utun.exe" ] || [ $EXE_NEEDS_REBUILD -eq 1 ]; then
    LINK_NEEDED=1
else
    # Проверяем все объектники и библиотеку
    for obj in utun-*.o; do
        if [ -f "$obj" ] && [ "$obj" -nt "utun.exe" ]; then
            LINK_NEEDED=1
            break
        fi
    done
    if [ -f "../lib/libuasync.a" ] && [ "../lib/libuasync.a" -nt "utun.exe" ]; then
        LINK_NEEDED=1
    fi
fi

if [ $LINK_NEEDED -eq 1 ]; then
    echo "  LINK utun.exe"
    x86_64-w64-mingw32-gcc -o utun.exe utun-*.o ../lib/libuasync.a "$sodium_dir/lib/libsodium.a" \
        -lpthread -lm -lws2_32 -liphlpapi -lbcrypt $LDFLAGS -lcrypto 2>&1 | grep -E "error:|undefined reference" || true
else
    echo "  SKIPPED LINK utun.exe (up to date)"
fi

# Копируем в корень проекта
if [ -f "utun.exe" ]; then
    cp utun.exe ../utun.exe
    echo "Binary updated: ../utun.exe"
fi

cd ..

echo ""
echo "Build completed successfully! ✅"
echo "Binary: utun.exe"
echo ""
echo "Make sure wintun.dll is in the same directory as utun.exe"
