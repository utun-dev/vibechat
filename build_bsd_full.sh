#!/bin/sh
# Полная сборка FreeBSD: зависимости и общий инкрементальный скрипт.
set -e
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -f "$SCRIPT_DIR/proxy.sh" ]; then
    . "$SCRIPT_DIR/proxy.sh" || { echo 'ERROR: cannot load proxy.sh' >&2; exit 1; }
fi
case " ${*} " in
    *" --help "*|*" -h "*)
        echo "Usage: ./build_bsd_full.sh [--asan|--hardened] [-jN] [build.sh arguments]"
        echo "Installs missing build tools and runs build.sh --full"
        exit 0 ;;
esac

need_install=0
for tool in bash gmake autoreconf aclocal automake libtoolize pkg-config python3 curl patch; do
    command -v "$tool" >/dev/null 2>&1 || need_install=1
done
if [ "$need_install" -eq 1 ]; then
    echo "INFO: installing missing FreeBSD build dependencies (requires root)"
    pkg install -y bash gmake autoconf automake libtool pkgconf openssl python3 curl || {
        echo "ERROR: cannot install FreeBSD build dependencies" >&2; exit 1;
    }
fi

jobs_found=0
for argument do
    case "$argument" in -j*) jobs_found=1 ;; esac
done
if [ "$jobs_found" -eq 0 ]; then
    jobs=$(sysctl -n hw.ncpu) || { echo "ERROR: cannot determine CPU count" >&2; exit 1; }
    set -- "-j$jobs" "$@"
fi
echo "INFO: running shared build script for FreeBSD"
exec bash "$SCRIPT_DIR/build.sh" --full "$@"
