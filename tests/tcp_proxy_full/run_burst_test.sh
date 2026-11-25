#!/bin/bash
# run_burst_test.sh — burst-тест tcp_proxy: двусторонний byte-exact трафик со случайными idle
#
# Топология (3 узла, как в run_test.sh):
#   burst_peer.py --client (SO_MARK=1) → 10.200.100.1 → tun_test_proxy
#     → tcp_proxy_client (lwIP) → ETCP → intermediate (транзит) → ETCP → exit
#     → tcp_proxy_server → connect(10.200.100.1) → iptables DNAT(!mark) → 127.0.0.1:KA_PORT
#     → burst_peer.py --server
#
# Проверяется: оба направления шлют известную псевдослучайную последовательность
# случайными пачками (10Б..1МБ) со случайными паузами (100..500мс), принимающая сторона
# сверяет каждый байт. Если за stall_ms нет прогресса при неполном приёме — «застряло».
#
# Запуск: sudo ./run_burst_test.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
UTUN_BIN="$SCRIPT_DIR/../../utun"
LOG_DIR="$SCRIPT_DIR/log"
BURST_PY="$SCRIPT_DIR/burst_peer.py"

KA_PORT=19193
BURST_IP="10.200.100.1"

NAME="${BURST_NAME:-burst}"
CONNECTIONS="${BURST_CONNECTIONS:-1}"
TOTAL="${BURST_TOTAL:-4194304}"          # байт на направление на соединение
STALL_MS="${BURST_STALL_MS:-2000}"
MIN_CHUNK="${BURST_MIN_CHUNK:-10}"
MAX_CHUNK="${BURST_MAX_CHUNK:-1048576}"
MIN_IDLE_MS="${BURST_MIN_IDLE_MS:-100}"
MAX_IDLE_MS="${BURST_MAX_IDLE_MS:-500}"
CLIENT_TIMEOUT="${BURST_CLIENT_TIMEOUT:-180}"

# -------- setup / cleanup --------

setup_net() {
    echo "=== Setting up networking ==="
    sysctl -w net.ipv4.conf.all.rp_filter=0

    ip rule add fwmark 1 priority 199 table 100 2>/dev/null || true
    local gw
    gw=$(ip route show default | awk '/via/ {print $3; exit}')
    [ -n "$gw" ] && ip route replace default via "$gw" table 100 2>/dev/null || true

    iptables -t nat -C OUTPUT -d "$BURST_IP" -p tcp --dport "$KA_PORT" -m mark ! --mark 1 -j DNAT --to-destination 127.0.0.1:"$KA_PORT" 2>/dev/null \
        || iptables -t nat -A OUTPUT -d "$BURST_IP" -p tcp --dport "$KA_PORT" -m mark ! --mark 1 -j DNAT --to-destination 127.0.0.1:"$KA_PORT"
    echo "  Done"
}

setup_tun_route() {
    for i in $(seq 1 60); do
        if [ -e "/proc/sys/net/ipv4/conf/tun_test_proxy/rp_filter" ]; then
            sysctl -w net.ipv4.conf.tun_test_proxy.rp_filter=0
            ip route replace 10.200.100.0/24 dev tun_test_proxy table 100
            ip route replace 10.200.100.0/24 dev tun_test_proxy          # main table anti-martian
            echo "  tun route ready (${i}x0.3s)"
            return 0
        fi
        sleep 0.3
    done
    echo "  WARN: tun_test_proxy not found after 18s"
}

cleanup_net() {
    echo "=== Cleaning up networking ==="
    iptables -t nat -D OUTPUT -d "$BURST_IP" -p tcp --dport "$KA_PORT" -m mark ! --mark 1 -j DNAT --to-destination 127.0.0.1:"$KA_PORT" 2>/dev/null || true
    ip rule del fwmark 1 priority 199 table 100 2>/dev/null || true
    ip route del 10.200.100.0/24 dev tun_test_proxy table 100 2>/dev/null || true
    ip route del 10.200.100.0/24 dev tun_test_proxy 2>/dev/null || true
    ip route flush table 100 2>/dev/null || true
    sysctl -w net.ipv4.conf.all.rp_filter=1
    sysctl -w net.ipv4.conf.tun_test_proxy.rp_filter=1 2>/dev/null || true
    echo "  Done"
}

cleanup() {
    echo ""; echo "=== Cleanup ==="
    kill $SERVER_PID 2>/dev/null || true
    kill $EXIT_PID 2>/dev/null || true
    kill $INTER_PID 2>/dev/null || true
    kill $CLIENT_PID 2>/dev/null || true
    wait $SERVER_PID 2>/dev/null || true
    wait $EXIT_PID 2>/dev/null || true
    wait $INTER_PID 2>/dev/null || true
    wait $CLIENT_PID 2>/dev/null || true
    sleep 0.5; cleanup_net
}

wait_for_etcp() {
    for i in $(seq 1 40); do
        grep -q "Connection UP" "$LOG_DIR/exit_utun.log" 2>/dev/null \
            && grep -q "Connection UP" "$LOG_DIR/intermediate_utun.log" 2>/dev/null \
            && grep -q "Connection UP" "$LOG_DIR/client_utun.log" 2>/dev/null \
            && return 0
        sleep 0.5
    done
    return 1
}

# -------- main --------

if [ "$(id -u)" -ne 0 ]; then echo "ERROR: must be root"; exit 1; fi
[ -x "$UTUN_BIN" ] || { echo "ERROR: utun not found. Build first."; exit 1; }

echo "=== tcp_proxy burst test (full-duplex, byte-exact) ==="
echo "name=$NAME connections=$CONNECTIONS total=$TOTAL/direction chunk=$MIN_CHUNK..$MAX_CHUNK idle=${MIN_IDLE_MS}..${MAX_IDLE_MS}ms stall=${STALL_MS}ms"

mkdir -p "$LOG_DIR"; rm -f "$LOG_DIR"/burst_*.log
setup_net

echo "Starting burst server on 127.0.0.1:$KA_PORT ..."
python3 "$BURST_PY" --server --host 127.0.0.1 --port "$KA_PORT" \
    --connections "$CONNECTIONS" --total "$TOTAL" \
    --min-chunk "$MIN_CHUNK" --max-chunk "$MAX_CHUNK" \
    --min-idle-ms "$MIN_IDLE_MS" --max-idle-ms "$MAX_IDLE_MS" \
    --stall-ms "$STALL_MS" --name "$NAME" \
    >"$LOG_DIR/burst_server.log" 2>&1 &
SERVER_PID=$!; sleep 0.3

echo "Starting utun exit ..."
"$UTUN_BIN" -c "$SCRIPT_DIR/exit.conf" -f -l "$LOG_DIR/exit_utun.log" >"$LOG_DIR/exit_stdout.log" 2>&1 &
EXIT_PID=$!

echo "Starting utun intermediate ..."
"$UTUN_BIN" -c "$SCRIPT_DIR/intermediate.conf" -f -l "$LOG_DIR/intermediate_utun.log" >"$LOG_DIR/intermediate_stdout.log" 2>&1 &
INTER_PID=$!

echo "Starting utun client ..."
"$UTUN_BIN" -c "$SCRIPT_DIR/client.conf" -f -l "$LOG_DIR/client_utun.log" >"$LOG_DIR/client_stdout.log" 2>&1 &
CLIENT_PID=$!

trap cleanup EXIT

setup_tun_route

echo "Waiting for ETCP connection ..."
wait_for_etcp || { echo "ERROR: ETCP timeout"; tail -20 "$LOG_DIR/client_utun.log"; exit 1; }
echo "ETCP ready"; sleep 1

echo ""; echo "=== Running burst (client) ==="
set +e
timeout "$CLIENT_TIMEOUT" python3 "$BURST_PY" --host "$BURST_IP" --port "$KA_PORT" --mark 1 \
    --connections "$CONNECTIONS" --total "$TOTAL" \
    --min-chunk "$MIN_CHUNK" --max-chunk "$MAX_CHUNK" \
    --min-idle-ms "$MIN_IDLE_MS" --max-idle-ms "$MAX_IDLE_MS" \
    --stall-ms "$STALL_MS" --name "$NAME" \
    >"$LOG_DIR/burst_client.log" 2>&1
CLIENT_RC=$?
wait "$SERVER_PID" 2>/dev/null
SERVER_RC=$?
set -e

echo ""; echo "=== $NAME results ==="
echo "  client: rc=$CLIENT_RC  $(tail -1 "$LOG_DIR/burst_client.log" 2>/dev/null)"
echo "  server: rc=$SERVER_RC  $(tail -1 "$LOG_DIR/burst_server.log" 2>/dev/null)"

trap - EXIT; cleanup

if [ "$CLIENT_RC" -eq 0 ] && [ "$SERVER_RC" -eq 0 ]; then
    echo "$NAME test: PASS"
    exit 0
else
    echo "$NAME test: FAIL (client_rc=$CLIENT_RC server_rc=$SERVER_RC)"
    echo "  Logs: $LOG_DIR/burst_client.log, $LOG_DIR/burst_server.log, $LOG_DIR/client_utun.log"
    exit 1
fi
