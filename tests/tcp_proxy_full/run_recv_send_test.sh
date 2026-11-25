#!/bin/bash
# run_recv_send_test.sh — интеграционный тест tcp_proxy: recv→send 1MB
#
# 3 клиента параллельно, каждый по 3 запроса (recv 1MB → send 1MB).
# Всего 9 запросов, макс 3 одновременных TCP-соединения через проксю.
#
# Топология:
#   recv_send_client.py (SO_MARK=1) → tun_test_proxy
#     → tcp_proxy(lwIP) → ETCP → exit(remote_proxy)
#     → connect(10.200.100.N) → iptables DNAT → 127.0.0.1:19092 → recv_send_server.py
#
# Запуск: sudo ./run_recv_send_test.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
UTUN_BIN="$SCRIPT_DIR/../../utun"
LOG_DIR="$SCRIPT_DIR/log"
CLIENT_PY="$SCRIPT_DIR/recv_send_client.py"
SERVER_PY="$SCRIPT_DIR/recv_send_server.py"

SERVER_PORT=19092
CLIENT_COUNT=3
REQ_PER_CLIENT=3
REQ_SIZE=1048576
N_IP=20

# -------- networking --------

setup_net() {
    echo "=== Setting up networking ==="
    sysctl -w net.ipv4.conf.all.rp_filter=0

    ip rule add fwmark 1 priority 199 table 100 2>/dev/null || true
    local gw; gw=$(ip route show default | awk '/via/ {print $3; exit}')
    [ -n "$gw" ] && ip route replace default via "$gw" table 100 2>/dev/null || true

    for i in $(seq 1 $N_IP); do
        iptables -t nat -C OUTPUT -d "10.200.100.${i}" -p tcp -m mark ! --mark 1 -j DNAT --to-destination 127.0.0.1:"$SERVER_PORT" 2>/dev/null \
            || iptables -t nat -A OUTPUT -d "10.200.100.${i}" -p tcp -m mark ! --mark 1 -j DNAT --to-destination 127.0.0.1:"$SERVER_PORT"
    done
    echo "  Done"
}

setup_tun_route() {
    for i in $(seq 1 60); do
        if [ -e "/proc/sys/net/ipv4/conf/tun_test_proxy/rp_filter" ]; then
            sysctl -w net.ipv4.conf.tun_test_proxy.rp_filter=0
            ip route replace 10.200.100.0/24 dev tun_test_proxy table 100
            ip route replace 10.200.100.0/24 dev tun_test_proxy
            echo "  tun route ready (${i}x0.3s)"
            return 0
        fi
        sleep 0.3
    done
    echo "  WARN: tun_test_proxy not found after 18s"
}

cleanup_net() {
    echo "=== Cleaning up networking ==="
    for i in $(seq 1 $N_IP); do
        iptables -t nat -D OUTPUT -d "10.200.100.${i}" -p tcp -m mark ! --mark 1 -j DNAT --to-destination 127.0.0.1:"$SERVER_PORT" 2>/dev/null || true
    done
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
    kill $EXIT_PID 2>/dev/null || true
    kill $CLIENT_PID 2>/dev/null || true
    kill $SERVER_PID 2>/dev/null || true
    wait $EXIT_PID 2>/dev/null || true
    wait $CLIENT_PID 2>/dev/null || true
    wait $SERVER_PID 2>/dev/null || true
    sleep 0.5; cleanup_net
}

wait_for_etcp() {
    for i in $(seq 1 30); do
        grep -q "Connection established\|initialized and marked as UP (client)" "$LOG_DIR/exit_utun.log" 2>/dev/null && return 0
        grep -q "initialized and marked as UP (client)" "$LOG_DIR/client_utun.log" 2>/dev/null && return 0
        sleep 0.5
    done
    return 1
}

# -------- main --------

if [ "$(id -u)" -ne 0 ]; then echo "ERROR: must be root"; exit 1; fi
[ -x "$UTUN_BIN" ] || { echo "ERROR: utun not found. Build first."; exit 1; }

echo "=== tcp_proxy recv→send 1MB integration test ==="
echo "Clients: $CLIENT_COUNT parallel × $REQ_PER_CLIENT requests × $(($REQ_SIZE / 1048576))MB = $(($CLIENT_COUNT * $REQ_PER_CLIENT)) total"

mkdir -p "$LOG_DIR"; rm -f "$LOG_DIR"/*.log
setup_net

echo "Starting recv_send_server on 127.0.0.1:$SERVER_PORT ..."
python3 "$SERVER_PY" 127.0.0.1 "$SERVER_PORT" >"$LOG_DIR/server.log" 2>&1 &
SERVER_PID=$!; sleep 0.3

echo "Starting utun exit ..."
"$UTUN_BIN" -c "$SCRIPT_DIR/exit.conf" -f -l "$LOG_DIR/exit_utun.log" >"$LOG_DIR/exit_stdout.log" 2>&1 &
EXIT_PID=$!

echo "Starting utun client ..."
"$UTUN_BIN" -c "$SCRIPT_DIR/client.conf" -f -l "$LOG_DIR/client_utun.log" >"$LOG_DIR/client_stdout.log" 2>&1 &
CLIENT_PID=$!

trap cleanup EXIT

setup_tun_route

echo "Waiting for ETCP connection ..."
wait_for_etcp || { echo "ERROR: ETCP timeout"; tail -20 "$LOG_DIR/client_utun.log"; exit 1; }
echo "ETCP ready"; sleep 1

echo ""; echo "=== Running test ==="

T0=$(date +%s%3N)
PASS=0; FAIL=0; pids=()

declare -A client_results

for c in $(seq 1 $CLIENT_COUNT); do
    ip="10.200.100.${c}"
    name="cl${c}"
    python3 "$CLIENT_PY" \
        --host "$ip" --port "$SERVER_PORT" \
        --size "$REQ_SIZE" --verify --timeout 120 \
        --count "$REQ_PER_CLIENT" --name "$name" \
        >"$LOG_DIR/client_${c}.log" 2>&1 &
    pids+=($!)
done

for pid in "${pids[@]}"; do
    wait "$pid" && ((++PASS)) || ((++FAIL))
done

ELAPSED=$(($(date +%s%3N) - T0))
echo ""; echo "=========================================="
echo "Results: $PASS passed, $FAIL failed ($((PASS + FAIL)) clients)"
echo "Total time: ${ELAPSED}ms (${ELAPSED}ms for ${REQ_PER_CLIENT}×${CLIENT_COUNT}=$(($CLIENT_COUNT * $REQ_PER_CLIENT)) requests × $(($REQ_SIZE / 1048576))MB)"
echo "Logs: $LOG_DIR/"
echo "=========================================="

for c in $(seq 1 $CLIENT_COUNT); do
    log="$LOG_DIR/client_${c}.log"
    if [ -f "$log" ]; then
        echo "  client_${c}: $(tail -1 "$log")"
    fi
done

trap - EXIT; cleanup
[ "$FAIL" -gt 0 ] && exit 1
exit 0
