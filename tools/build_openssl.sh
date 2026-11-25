#!/usr/bin/env bash
# Подключается Linux-скриптами сборки: выбирает OpenSSL из OPENSSL_ROOT_DIR или штатного /opt/openssl-3.5.
# Согласует пути Autotools/CMake; повторное подключение сохраняет флаги без дубликатов.

openssl_prefix=${OPENSSL_ROOT_DIR:-/opt/openssl-3.5}
if [[ -n "${OPENSSL_ROOT_DIR:-}" || -d "$openssl_prefix" ]]; then
    openssl_libdir=""
    for directory in "$openssl_prefix/lib" "$openssl_prefix/lib64"; do
        if [[ -f "$directory/libcrypto.so" || -f "$directory/libcrypto.a" ]]; then openssl_libdir=$directory; break; fi
    done
    [[ -n "$openssl_libdir" && -f "$openssl_prefix/include/openssl/hpke.h" ]] || {
        echo "ERROR: incomplete OpenSSL in $openssl_prefix; need include/openssl/hpke.h and lib/libcrypto or lib64/libcrypto" >&2
        echo "On Ubuntu x86_64 run: sudo ./tools/install_openssl_ubuntu.sh; or set OPENSSL_ROOT_DIR to a complete installation" >&2
        exit 1
    }
    [[ " ${CPPFLAGS:-} " = *" -I$openssl_prefix/include "* ]] || export CPPFLAGS="-I$openssl_prefix/include${CPPFLAGS:+ $CPPFLAGS}"
    [[ " ${LDFLAGS:-} " = *" -Wl,-rpath,$openssl_libdir "* ]] || export LDFLAGS="-Wl,-rpath,$openssl_libdir${LDFLAGS:+ $LDFLAGS}"
    [[ " ${LDFLAGS:-} " = *" -L$openssl_libdir "* ]] || export LDFLAGS="-L$openssl_libdir${LDFLAGS:+ $LDFLAGS}"
    [[ ":${CMAKE_PREFIX_PATH:-}:" = *":$openssl_prefix:"* ]] || export CMAKE_PREFIX_PATH="$openssl_prefix${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
    export OPENSSL_ROOT_DIR="$openssl_prefix"
    echo "INFO: using OpenSSL from $openssl_prefix (libraries: $openssl_libdir)"
else
    echo "INFO: OpenSSL uses compiler defaults; $openssl_prefix is not installed"
fi
