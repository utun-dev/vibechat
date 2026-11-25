#!/bin/bash
# Однократно создаёт ключ релизной подписи и локальные настройки Gradle.
set -euo pipefail
cd "$(dirname "$0")"

if [[ "${1:-}" == "--help" ]]; then
    echo "Usage: $0"
    echo "Creates signing/release.p12 and signing/keystore.properties (excluded from Git)."
    exit 0
fi
if [[ $# -ne 0 ]]; then
    echo "ERROR: Unexpected arguments. Use $0 --help." >&2
    exit 1
fi

KEYTOOL="${JAVA_HOME:+$JAVA_HOME/bin/}keytool"
for tool in "$KEYTOOL" openssl; do
    if ! command -v "$tool" > /dev/null; then
        echo "ERROR: Required tool not found: $tool (install JDK 17+ and OpenSSL)." >&2
        exit 1
    fi
done
if [[ -e signing || -L signing ]]; then
    echo "ERROR: signing already exists; refusing to replace the release key or its settings." >&2
    exit 1
fi

umask 077
mkdir signing
trap 'echo "ERROR: Key creation failed at line $LINENO; inspect signing/ before retrying." >&2' ERR
export UTUN_KEYSTORE_PASSWORD
UTUN_KEYSTORE_PASSWORD=$(openssl rand -hex 32)
# Сохраняем пароль до генерации ключа, чтобы ошибка записи не оставила ключ без пароля.
cat > signing/keystore.properties <<EOF
storeFile=release.p12
storePassword=$UTUN_KEYSTORE_PASSWORD
keyAlias=utun-release
keyPassword=$UTUN_KEYSTORE_PASSWORD
EOF
echo "Creating release signing key: signing/release.p12 (RSA 3072, validity 10000 days)..."
"$KEYTOOL" -genkeypair -noprompt -keystore signing/release.p12 -storetype PKCS12 \
    -alias utun-release -keyalg RSA -keysize 3072 -sigalg SHA256withRSA -validity 10000 \
    -dname "CN=uTun Android Release" -storepass:env UTUN_KEYSTORE_PASSWORD -keypass:env UTUN_KEYSTORE_PASSWORD
unset UTUN_KEYSTORE_PASSWORD
echo "Release signing key and settings created; permissions: directory 700, files 600."
echo "Back up the entire signing/ directory securely; keep it for all future application updates."
echo "Build a signed APK: ./build.sh --release noinstall"
