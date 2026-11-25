#!/usr/bin/env bash
# Подготовка Android SDK/NDK и native-зависимостей; параметры: --sdk PATH, --jobs N.
set -euo pipefail
cd "$(dirname "$0")/../.."
if [ -f ./proxy.sh ]; then
    . ./proxy.sh || { echo 'ERROR: cannot load proxy.sh' >&2; exit 1; }
fi
command -v python3 >/dev/null || {
    echo "ERROR: Python 3 is required; Ubuntu/Debian: sudo apt-get install python3" >&2; exit 1;
}
exec python3 tools/prepare_android.py "$@"
