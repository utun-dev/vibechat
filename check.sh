#!/usr/bin/env bash
set -eo pipefail
cd "$(dirname "$0")"
MAKE_TOOL=${MAKE:-$(command -v gmake || command -v make)}
TIMEOUT_TOOL=$(command -v timeout || command -v gtimeout) || {
    echo "ERROR: timeout or gtimeout is required for the test runner" >&2; exit 1;
}

LAST=0; [ -f .check_time ] && read -r LAST < .check_time
TMO=$(( LAST + 30 ))
[ $TMO -lt 90 ] && TMO=90

TMP=$(mktemp)
echo "[INFO] building the core and test programs before running tests"
bash ./build.sh --no-gui -j4 check-build > "$TMP" 2>&1 || {
    tail -40 "$TMP"; rm -f "$TMP"
    echo "ERROR: test preparation failed; tests were not started" >&2
    exit 1
}

# Таймаут измеряет запуск тестов, а не первую компиляцию всех тестовых программ.
START=$(date +%s)
"$TIMEOUT_TOOL" "$TMO" "$MAKE_TOOL" check -j4 > "$TMP" 2>&1 || {
    tail -40 "$TMP"; rm -f "$TMP"
    [ $(date +%s) -ge $((START + TMO)) ] && echo "TIMEOUT (${TMO}s)" >&2
    exit 1
}
grep -E '\[INFO|\[PASS|\[FAIL|\[SKIP|Test Results|Failed tests|Log directory|^====' "$TMP"
rm -f "$TMP"

# Интеграционный тест tcp_proxy_full (требует sudo; без passwordless sudo — пропускаем)
echo ""
if [ "$(uname -s)" != Linux ]; then
    echo "[SKIP] tcp_proxy_full integration tests require Linux networking tools"
elif [ "$(id -u)" -eq 0 ]; then
    "$MAKE_TOOL" -C tests check-proxy
    "$MAKE_TOOL" -C tests check-burst
    "$MAKE_TOOL" -C tests check-load
elif command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
    echo "=== tcp_proxy_full integration test (sudo) ==="
    sudo -n "$MAKE_TOOL" -C tests check-proxy
    echo "=== tcp_proxy_full burst test (sudo) ==="
    sudo -n "$MAKE_TOOL" -C tests check-burst
    echo "=== tcp_proxy_full load test (sudo) ==="
    sudo -n "$MAKE_TOOL" -C tests check-load
else
    echo "[SKIP] tcp_proxy_full integration test (requires passwordless sudo)"
fi

ELAPSED=$(( $(date +%s) - START ))
echo "Time: ${ELAPSED}s"
echo "$ELAPSED" > .check_time
