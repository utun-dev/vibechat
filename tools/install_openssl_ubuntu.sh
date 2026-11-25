#!/usr/bin/env bash
# OpenSSL с HPKE для Ubuntu Server x86_64: sudo ./tools/install_openssl_ubuntu.sh
# Установка в /opt/openssl-3.5; команды сборки uTun выводятся после установки.
set -Eeuo pipefail
trap 'printf "ERROR: line %s: %s\n" "$LINENO" "$BASH_COMMAND" >&2' ERR

if [[ $# = 1 && ( $1 = --help || $1 = -h ) ]]; then
    echo "Usage: sudo ./tools/install_openssl_ubuntu.sh"
    echo "Builds and tests OpenSSL 3.5.9 with HPKE, then installs it in /opt/openssl-3.5."
    echo "Requires Ubuntu Server x86_64 and root; build files are retained in /tmp."
    exit 0
fi
if [[ $# != 0 ]]; then
    echo "ERROR: unexpected arguments; use --help" >&2; exit 1
fi
if [[ $EUID != 0 ]]; then
    echo "ERROR: root is required; run sudo ./tools/install_openssl_ubuntu.sh" >&2; exit 1
fi
if [[ $(uname -s) != Linux || $(uname -m) != x86_64 ]]; then
    echo "ERROR: this script requires Linux x86_64" >&2; exit 1
fi

VERSION=3.5.9
SCRIPT_DIR=$(cd "$(dirname "$0")/.." && pwd)
if [ -f "$SCRIPT_DIR/proxy.sh" ]; then
    . "$SCRIPT_DIR/proxy.sh" || { echo 'ERROR: cannot load proxy.sh' >&2; exit 1; }
fi
PREFIX=/opt/openssl-3.5
echo "INFO: installing OpenSSL build dependencies"
apt-get update
apt-get install -y build-essential perl curl ca-certificates

workdir=$(mktemp -d /tmp/openssl-build.XXXXXX)
echo "INFO: OpenSSL $VERSION: prefix=$PREFIX build_directory=$workdir"
cd "$workdir"
archive="openssl-$VERSION.tar.gz"
url="https://github.com/openssl/openssl/releases/download/openssl-$VERSION"
echo "INFO: downloading OpenSSL sources and SHA-256 checksum"
curl -fL "$url/$archive" -o "$archive"
curl -fL "$url/$archive.sha256" -o openssl.sha256
read -r digest rest < openssl.sha256
if [[ ! $digest =~ ^[[:xdigit:]]{64}$ ]]; then
    echo "ERROR: invalid SHA-256 checksum in $workdir/openssl.sha256" >&2; exit 1
fi
printf '%s  %s\n' "$digest" "$archive" | sha256sum -c -
tar -xzf "$archive"
cd "openssl-$VERSION"

echo "INFO: configuring OpenSSL with runtime library path $PREFIX/lib"
./Configure linux-x86_64 shared --prefix="$PREFIX" --openssldir=/etc/ssl --libdir=lib "-Wl,-rpath,$PREFIX/lib"
echo "INFO: compiling OpenSSL (jobs=4)"
make -j4
echo "INFO: running OpenSSL tests"
make test
echo "INFO: installing OpenSSL in $PREFIX"
make install_sw
"$PREFIX/bin/openssl" version

echo "INFO: OpenSSL installed; build files retained in $workdir"
cat <<'EOF'
Run from the uTun repository directory:
./build.sh --no-gui -j4

build.sh selects /opt/openssl-3.5 automatically. Tests are built when you run ./check.sh.

Check the linked OpenSSL library:
ldd src/utun | grep -E 'libcrypto|libssl'
EOF
