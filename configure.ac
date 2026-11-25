AC_INIT([utun],[2.0.0],[https://github.com/anomalyco/utun3/issues])
AC_CONFIG_SRCDIR([src/utun.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([m4])

# === ОБЯЗАТЕЛЬНО для правильного определения host ===
AC_CANONICAL_HOST
AC_CANONICAL_BUILD

# Required programs
AC_PROG_CC
AC_PROG_RANLIB
AM_PROG_AR
AM_INIT_AUTOMAKE([foreign subdir-objects -Wall])

AC_PROG_INSTALL
AC_PROG_MAKE_SET

# OpenSSL обязателен: X25519, AES-CCM, Ed25519 и HPKE используют его API напрямую.
# FreeBSD использует пакетный OpenSSL: системный libcrypto может не поддерживать HPKE.
case "$host" in
    *freebsd*)
        AC_PATH_TOOL([PKG_CONFIG], [pkg-config])
        if test -z "$PKG_CONFIG" || ! "$PKG_CONFIG" --exists 'libcrypto >= 3.2'; then
            AC_MSG_ERROR([FreeBSD requires pkgconf and OpenSSL 3.2 or newer: pkg install pkgconf openssl])
        fi
        OPENSSL_CFLAGS=$("$PKG_CONFIG" --cflags libcrypto) || AC_MSG_ERROR([Cannot read OpenSSL compiler flags])
        OPENSSL_LDFLAGS=$("$PKG_CONFIG" --libs-only-L libcrypto) || AC_MSG_ERROR([Cannot read OpenSSL library paths])
        CPPFLAGS="$CPPFLAGS $OPENSSL_CFLAGS"
        LDFLAGS="$LDFLAGS $OPENSSL_LDFLAGS"
        AC_MSG_NOTICE([Using FreeBSD package OpenSSL: CPPFLAGS=$OPENSSL_CFLAGS LDFLAGS=$OPENSSL_LDFLAGS])
        ;;
    # MSYS2 OpenSSL paths (для Windows)
    *mingw* | *msys* | *cygwin* | *win*)
        if test -d "/c/msys64/ucrt64/include/openssl"; then
            OPENSSL_CFLAGS="-I/c/msys64/ucrt64/include"
            OPENSSL_LIBS="-L/c/msys64/ucrt64/lib -lcrypto"
            AC_MSG_NOTICE([Using MSYS2 UCRT64 OpenSSL])
        elif test -d "/ucrt64/include/openssl"; then
            OPENSSL_CFLAGS="-I/ucrt64/include"
            OPENSSL_LIBS="-L/ucrt64/lib -lcrypto"
            AC_MSG_NOTICE([Using MSYS2 UCRT64 OpenSSL])
        elif test -d "/mingw64/include/openssl"; then
            OPENSSL_CFLAGS="-I/mingw64/include"
            OPENSSL_LIBS="-L/mingw64/lib -lcrypto"
            AC_MSG_NOTICE([Using MSYS2 MINGW64 OpenSSL])
        fi
        CFLAGS="$CFLAGS $OPENSSL_CFLAGS"
        LDFLAGS="$LDFLAGS $OPENSSL_LIBS"
        ;;
esac

save_LIBS="$LIBS"
LIBS="-lcrypto $LIBS"
AC_CHECK_LIB([crypto], [SHA256_Init],
    [AC_DEFINE([USE_OPENSSL], [1], [Define to use OpenSSL for cryptography])],
    [AC_MSG_ERROR([OpenSSL crypto library with HPKE support is required (OpenSSL 3.2+).])])
AC_CHECK_HEADERS([openssl/hpke.h], [], [AC_MSG_ERROR([DM requires OpenSSL 3.2 or newer with HPKE headers.])])
AC_CHECK_LIB([crypto], [OSSL_HPKE_CTX_new], [], [AC_MSG_ERROR([DM requires an OpenSSL crypto library with HPKE support.])])
LIBS="$save_LIBS"

# Криптографическая проверка ключей CHAT. Каждый target получает свой статический архив.
AC_ARG_VAR([SODIUM_PREFIX], [prefix of the target libsodium prepared by tools/prepare_dependencies.py])
AS_IF([test -z "$SODIUM_PREFIX"], [
    AC_PATH_PROG([PYTHON3], [python3])
    AS_IF([test -z "$PYTHON3"], [AC_MSG_ERROR([Python 3 is required to prepare libsodium])])
    sodium_target=$($CC -dumpmachine) || AC_MSG_ERROR([cannot identify libsodium compiler target])
    sodium_sources=$(cd "$srcdir" && pwd)
    CC="$CC" "$PYTHON3" "$sodium_sources/tools/prepare_dependencies.py" --only sodium --sodium-target "$sodium_target" --if-needed \
        --output "$PWD" --cache "$sodium_sources/.cache/dependencies/downloads" ||
        AC_MSG_ERROR([cannot prepare libsodium for $sodium_target])
    SODIUM_PREFIX="$PWD/.cache/dependencies/sodium/$sodium_target"
])
CPPFLAGS="$CPPFLAGS -I$SODIUM_PREFIX/include -DSODIUM_STATIC"
LIBS="$SODIUM_PREFIX/lib/libsodium.a $LIBS"
# Приватные символы архива не подменяют другую libsodium, загруженную зависимостями приложения.
AS_CASE([$host_os], [linux*|freebsd*], [LDFLAGS="$LDFLAGS -Wl,--exclude-libs,libsodium.a"])
AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <sodium.h>]], [[return sodium_init();]])], [],
    [AC_MSG_ERROR([cannot link the prepared libsodium in $SODIUM_PREFIX])])

# ==================== HARDENING ====================
AC_ARG_ENABLE([hardening],
    AS_HELP_STRING([--enable-hardening], [Enable compiler security hardening (FORTIFY, stack protector, PIE, RELRO)]),
    [enable_hardening=$enableval],
    [enable_hardening=no])

if test "x$enable_hardening" = "xyes"; then
    HARDEN_CFLAGS="-D_FORTIFY_SOURCE=2 -fstack-protector-strong -Wformat=2"
    compiler_version=$($CC --version 2>&1) || AC_MSG_ERROR([Cannot identify compiler for hardening])
    case "$compiler_version" in
        *clang*) ;;
        *) HARDEN_CFLAGS="$HARDEN_CFLAGS -fstack-clash-protection -Wformat-overflow=2" ;;
    esac
    case "$host" in
        *mingw* | *msys* | *cygwin*)
            HARDEN_LDFLAGS="-Wl,--dynamicbase,--nxcompat,--high-entropy-va"
            ;;
        *)
            HARDEN_CFLAGS="$HARDEN_CFLAGS -fPIE"
            HARDEN_LDFLAGS="-Wl,-z,relro -Wl,-z,now -pie"
            ;;
    esac
    CFLAGS="$CFLAGS $HARDEN_CFLAGS"
    LDFLAGS="$LDFLAGS $HARDEN_LDFLAGS"
    AC_MSG_NOTICE([Hardening flags enabled: CFLAGS=$HARDEN_CFLAGS LDFLAGS=$HARDEN_LDFLAGS])
fi

# ==================== НАДЁЖНЫЙ ДЕТЕКТ WINDOWS ====================
AC_MSG_CHECKING([for Windows])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#ifndef _WIN32
#error "Not Windows"
#endif
]], [[ return 0; ]])],
    [os_windows=yes],
    [os_windows=no])
AC_MSG_RESULT([$os_windows])

AM_CONDITIONAL([OS_WINDOWS], [test "x$os_windows" = "xyes"])

# ==================== DETECT FREEBSD ====================
AC_MSG_CHECKING([for FreeBSD])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#ifndef __FreeBSD__
#error "Not FreeBSD"
#endif
]], [[ return 0; ]])],
    [os_freebsd=yes],
    [os_freebsd=no])
AC_MSG_RESULT([$os_freebsd])

AM_CONDITIONAL([OS_FREEBSD], [test "x$os_freebsd" = "xyes"])

# ==================== DETECT LINUX ====================
AC_MSG_CHECKING([for Linux])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#ifndef __linux__
#error "Not Linux"
#endif
]], [[ return 0; ]])],
    [os_linux=yes],
    [os_linux=no])
AC_MSG_RESULT([$os_linux])

AM_CONDITIONAL([OS_LINUX], [test "x$os_linux" = "xyes"])

# Set TUN libs based on platform
if test "x$os_windows" = "xyes"; then
    TUN_LIBS="-liphlpapi -lws2_32 -ladvapi32 -lbcrypt -ldbghelp"
else
    TUN_LIBS=""
fi
AC_SUBST([TUN_LIBS])

# ==================== WHISPER (OPTIONAL) ====================
AC_ARG_WITH([whisper],
    AS_HELP_STRING([--with-whisper], [Enable Whisper speech-to-text (requires libwhisper)]),
    [with_whisper=$withval],
    [with_whisper=no])

if test "x$with_whisper" = "xyes"; then
    AC_CHECK_HEADER([whisper.h], [
        AC_CHECK_LIB([whisper], [whisper_init_from_file_with_params], [
            WHISPER_LIBS="-lwhisper -lggml -lggml-base"
            AC_DEFINE([HAVE_WHISPER], [1], [Define to enable Whisper speech-to-text])
            AC_MSG_NOTICE([Whisper enabled])
        ], [
            AC_MSG_WARN([libwhisper not found, disabling whisper])
            with_whisper=no
        ])
    ], [
        AC_MSG_WARN([whisper.h not found, disabling whisper])
        with_whisper=no
    ])
fi
AM_CONDITIONAL([HAVE_WHISPER], [test "x$with_whisper" = "xyes"])
AC_SUBST([WHISPER_LIBS])

# ==================== FFMPEG (OPTIONAL) ====================
AC_ARG_WITH([ffmpeg],
    AS_HELP_STRING([--with-ffmpeg], [Enable video transcoding (requires FFmpeg libav libraries)]),
    [with_ffmpeg=$withval],
    [with_ffmpeg=no])

if test "x$with_ffmpeg" = "xyes"; then
    save_LIBS="$LIBS"
    FFMPEG_LIBS="-lavformat -lavcodec -lavutil -lswscale -lswresample"
    LIBS="$FFMPEG_LIBS $LIBS"
    AC_CHECK_HEADER([libavformat/avformat.h], [
        AC_CHECK_HEADER([libavcodec/avcodec.h], [
            AC_CHECK_HEADER([libavutil/imgutils.h], [
                AC_CHECK_HEADER([libswscale/swscale.h], [
                    AC_CHECK_HEADER([libswresample/swresample.h], [
                        AC_CHECK_LIB([avformat], [avformat_open_input], [
                            AC_DEFINE([HAVE_FFMPEG], [1], [Define to enable FFmpeg video transcoding])
                            AC_MSG_NOTICE([FFmpeg enabled])
                        ], [
                            AC_MSG_WARN([FFmpeg libs not found, disabling ffmpeg])
                            with_ffmpeg=no
                        ])
                    ], [AC_MSG_WARN([swresample.h not found, disabling ffmpeg]); with_ffmpeg=no])
                ], [AC_MSG_WARN([swscale.h not found, disabling ffmpeg]); with_ffmpeg=no])
            ], [AC_MSG_WARN([imgutils.h not found, disabling ffmpeg]); with_ffmpeg=no])
        ], [AC_MSG_WARN([avcodec.h not found, disabling ffmpeg]); with_ffmpeg=no])
    ], [AC_MSG_WARN([avformat.h not found, disabling ffmpeg]); with_ffmpeg=no])
    LIBS="$save_LIBS"
fi

if test "x$with_ffmpeg" != "xyes"; then
    FFMPEG_LIBS=""
    FFMPEG_CFLAGS=""
fi
AM_CONDITIONAL([HAVE_FFMPEG], [test "x$with_ffmpeg" = "xyes"])
AC_SUBST([FFMPEG_LIBS])
AC_SUBST([FFMPEG_CFLAGS])

# ==================== SILERO VAD (OPTIONAL, requires ONNX Runtime) ====================
AC_ARG_WITH([silero-vad],
    AS_HELP_STRING([--with-silero-vad], [Enable Silero voice activity detector (requires ONNX Runtime)]),
    [with_silero_vad=$withval],
    [with_silero_vad=no])

if test "x$with_silero_vad" = "xyes"; then
    AC_CHECK_HEADER([onnxruntime/onnxruntime_c_api.h], [
        save_LIBS="$LIBS"
        LIBS="-lonnxruntime $LIBS"
        AC_CHECK_LIB([onnxruntime], [OrtGetApiBase], [
            SILERO_VAD_LIBS="-lonnxruntime"
            AC_DEFINE([HAVE_SILERO_VAD], [1], [Define to enable Silero VAD via ONNX Runtime])
            AC_MSG_NOTICE([Silero VAD enabled (ONNX Runtime)])
        ], [
            AC_MSG_WARN([libonnxruntime not found, disabling silero-vad])
            with_silero_vad=no
        ])
        LIBS="$save_LIBS"
    ], [
        AC_MSG_WARN([onnxruntime/onnxruntime_c_api.h not found, disabling silero-vad])
        with_silero_vad=no
    ])
fi

if test "x$with_silero_vad" != "xyes"; then
    SILERO_VAD_LIBS=""
fi
AM_CONDITIONAL([HAVE_SILERO_VAD], [test "x$with_silero_vad" = "xyes"])
AC_SUBST([SILERO_VAD_LIBS])

# Checks for header files, typedefs, functions...
AC_CHECK_HEADERS([arpa/inet.h fcntl.h limits.h netinet/in.h stdint.h stdlib.h string.h sys/ioctl.h sys/socket.h unistd.h])
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T
AC_TYPE_UINT8_T
AC_C_INLINE

AC_FUNC_MALLOC
AC_CHECK_FUNCS([gettimeofday memset socket strchr strdup strerror strstr])

# TUN/TAP (только для Linux)
AC_MSG_CHECKING([for TUN/TAP support])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[#include <linux/if_tun.h>]], [[int fd = open("/dev/net/tun", O_RDWR);]])],
    [AC_MSG_RESULT([yes]); AC_DEFINE([HAVE_TUN_TAP], [1], [Define if TUN/TAP is available])],
    [AC_MSG_RESULT([no])])

# Output
AC_CONFIG_FILES([Makefile src/Makefile tests/Makefile lib/Makefile lib/libopus/Makefile])
AC_OUTPUT

# Summary
echo "
Configuration summary:
---------------------
  Host:         ${host}
  Windows:      ${os_windows}
  Prefix:       ${prefix}
  Compiler:     ${CC}
  CFLAGS:       ${CFLAGS}
  Silero VAD:   ${with_silero_vad}
"
