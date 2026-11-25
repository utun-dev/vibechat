#!/bin/bash
# run_test.sh — интеграционный тест tcp_proxy (transparent proxy)
#
# Топология (3 узла, трафик идёт через промежуточный узел транзитом):
#   tcp_client.py (SO_MARK=1, dest=10.200.100.N) → table 100 → tun_test_proxy
#     → tcp_proxy(lwIP, клиент) → ETCP → intermediate (транзит) → ETCP → exit
#     → connect(10.200.100.N) → iptables DNAT (!mark=1) → 127.0.0.1 → echo → обратно
#
# Запуск: sudo ./run_test.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
UTUN_BIN="$SCRIPT_DIR/../../utun"
LOG_DIR="$SCRIPT_DIR/log"
CLIENT_PY="$SCRIPT_DIR/tcp_client.py"
ECHO_PY="$SCRIPT_DIR/echo_server.py"
STRESS_PY="$SCRIPT_DIR/stress_client.py"

ECHO_PORT=19090
HC_PORT=19190
REFUSED_PORT=19099
N_IP=20

PASS=0; FAIL=0

# -------- setup / cleanup --------

setup_net() {
    echo "=== Setting up networking ==="
    sysctl -w net.ipv4.conf.all.rp_filter=0

    # policy routing: marked packets → TUN (priority 199)
    ip rule add fwmark 1 priority 199 table 100 2>/dev/null || true
    local gw
    gw=$(ip route show default | awk '/via/ {print $3; exit}')
    [ -n "$gw" ] && ip route replace default via "$gw" table 100 2>/dev/null || true

    # DNAT: unmarked packets to fake IPs → real localhost.
    # half-close echo (dedicated port, вне stress-диапазона 19090-19109) — ДО общего правила,
    # иначе порт перезапишется на ECHO_PORT.
    iptables -t nat -C OUTPUT -d 10.200.100.1 -p tcp --dport "$HC_PORT" -m mark ! --mark 1 -j DNAT --to-destination 127.0.0.1:"$HC_PORT" 2>/dev/null \
        || iptables -t nat -A OUTPUT -d 10.200.100.1 -p tcp --dport "$HC_PORT" -m mark ! --mark 1 -j DNAT --to-destination 127.0.0.1:"$HC_PORT"
    for i in $(seq 1 $N_IP); do
        iptables -t nat -C OUTPUT -d "10.200.100.${i}" -p tcp -m mark ! --mark 1 -j DNAT --to-destination 127.0.0.1:"$ECHO_PORT" 2>/dev/null \
            || iptables -t nat -A OUTPUT -d "10.200.100.${i}" -p tcp -m mark ! --mark 1 -j DNAT --to-destination 127.0.0.1:"$ECHO_PORT"
    done
    # conn_refused: .99 → порт без слушателя, exit получает ECONNREFUSED (иначе SYN уходит в tun_test_proxy и зацикливается)
    iptables -t nat -C OUTPUT -d 10.200.100.99 -p tcp -m mark ! --mark 1 -j DNAT --to-destination 127.0.0.1:"$REFUSED_PORT" 2>/dev/null \
        || iptables -t nat -A OUTPUT -d 10.200.100.99 -p tcp -m mark ! --mark 1 -j DNAT --to-destination 127.0.0.1:"$REFUSED_PORT"
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
    iptables -t nat -D OUTPUT -d 10.200.100.1 -p tcp --dport "$HC_PORT" -m mark ! --mark 1 -j DNAT --to-destination 127.0.0.1:"$HC_PORT" 2>/dev/null || true
    for i in $(seq 1 $N_IP); do
        iptables -t nat -D OUTPUT -d "10.200.100.${i}" -p tcp -m mark ! --mark 1 -j DNAT --to-destination 127.0.0.1:"$ECHO_PORT" 2>/dev/null || true
    done
    iptables -t nat -D OUTPUT -d 10.200.100.99 -p tcp -m mark ! --mark 1 -j DNAT --to-destination 127.0.0.1:"$REFUSED_PORT" 2>/dev/null || true
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
    kill $INTER_PID 2>/dev/null || true
    kill $CLIENT_PID 2>/dev/null || true
    kill $ECHO_PID 2>/dev/null || true
    wait $EXIT_PID 2>/dev/null || true
    wait $INTER_PID 2>/dev/null || true
    wait $CLIENT_PID 2>/dev/null || true
    wait $ECHO_PID 2>/dev/null || true
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

# -------- test runners --------

# Монотонное время не зависит от wall clock и поддержки точности %N в date.
clock_ms() {
    python3 -c 'import time; print(time.monotonic_ns() // 1000000)'
}

run_test() {
    local name=$1; shift
    local t_start log rc elapsed
    t_start=$(clock_ms)
    echo -n "  $name ... "
    log="$LOG_DIR/tcp_client_${name}.log"
    if python3 "$CLIENT_PY" "$@" --name "$name" >"$log" 2>&1; then
        rc=0
    else
        rc=$?
    fi
    elapsed=$(($(clock_ms) - t_start))
    if [ $rc -eq 0 ]; then
        echo "PASS (${elapsed}ms)"
        tail -1 "$log"
        return 0
    else
        echo "FAIL (rc=$rc, ${elapsed}ms)"
        tail -3 "$log"
        return 1
    fi
}

run_test_neg() {
    local name=$1; shift
    local t_start log rc elapsed
    t_start=$(clock_ms)
    echo -n "  $name ... "
    log="$LOG_DIR/tcp_client_${name}.log"
    if ! python3 "$CLIENT_PY" "$@" --name "$name" >"$log" 2>&1; then
        rc=0
    else
        rc=1
    fi
    elapsed=$(($(clock_ms) - t_start))
    if [ $rc -eq 0 ]; then
        echo "PASS (expected error, ${elapsed}ms)"; return 0
    else
        echo "FAIL (expected error, got success, ${elapsed}ms)"; return 1
    fi
}

run_concurrent() {
    local name=$1 count=$2 size=$3; shift 3
    local t_start pids ok elapsed
    t_start=$(clock_ms)
    pids=()
    for i in $(seq 1 $count); do
        python3 "$CLIENT_PY" \
            --host "10.200.100.${i}" --port "$ECHO_PORT" \
            --size "$size" --verify --timeout 15 \
            --name "${name}_${i}" \
            >"$LOG_DIR/tcp_client_${name}_${i}.log" 2>&1 &
        pids+=($!)
    done
    ok=0
    for pid in "${pids[@]}"; do wait "$pid"; [ $? -eq 0 ] && ((++ok)); done
    elapsed=$(($(clock_ms) - t_start))
    echo -n "  $name ($count) ... "
    if [ "$ok" -eq "$count" ]; then
        echo "PASS ($ok/$count, ${elapsed}ms)"; return 0
    else
        echo "FAIL ($ok/$count, ${elapsed}ms)"
        local fail=''
        for i in $(seq 1 $count); do
            grep -q "\[PASS\]" "$LOG_DIR/tcp_client_${name}_${i}.log" 2>/dev/null || fail+=" ${i}"
        done
        echo "    failed:$fail"; return 1
    fi
}

run_stress() {
    local t_start elapsed
    t_start=$(clock_ms)
    echo -n "  stress_sessions ... "
    local log="$LOG_DIR/stress.log"
    if python3 "$STRESS_PY" \
        --host 10.200.100.1 --port-base "$ECHO_PORT" --ports 20 \
        --threads 10 --iters 20 --min-size 1024 --max-size 65536 --timeout 60 --verbose \
        >"$log" 2>&1; then
        elapsed=$(($(clock_ms) - t_start))
        echo "PASS (${elapsed}ms)"
        head -1 "$log"; return 0
    else
        elapsed=$(($(clock_ms) - t_start))
        echo "FAIL (${elapsed}ms)"
        tail -5 "$log"; return 1
    fi
}

# -------- main --------

if [ "$(id -u)" -ne 0 ]; then echo "ERROR: must be root"; exit 1; fi
[ -x "$UTUN_BIN" ] || { echo "ERROR: utun not found. Build first."; exit 1; }

echo "=== tcp_proxy full integration test ==="

mkdir -p "$LOG_DIR"; rm -f "$LOG_DIR"/*.log
setup_net

echo "Starting echo server on 127.0.0.1:$ECHO_PORT ..."
python3 "$ECHO_PY" 127.0.0.1 "$ECHO_PORT" >"$LOG_DIR/echo.log" 2>&1 &
ECHO_PID=$!; sleep 0.3

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

echo ""; echo "=== Running tests ==="

# 1: basic_1mb СНАЧАЛА (изолируем от стресса, чтобы понять — падает ли 1MB сам по себе)
run_test basic_1mb --host 10.200.100.1 --port "$ECHO_PORT" --size 1048576 --verify --timeout 10 \
    && ((++PASS)) || ((++FAIL))
sleep 1

# 2: stress (catch resource leaks)
run_stress && ((++PASS)) || ((++FAIL))
sleep 2

# 3: concurrent_5
run_concurrent concurrent_5 5 204800 && ((++PASS)) || ((++FAIL))
sleep 1

# 4: concurrent_20
run_concurrent concurrent_20 20 51200 && ((++PASS)) || ((++FAIL))
sleep 1

# 5: half_close
echo "  half_close: one-shot echo on $HC_PORT ..."
python3 "$ECHO_PY" 127.0.0.1 "$HC_PORT" --read-timeout 2 >"$LOG_DIR/echo_hc.log" 2>&1 &
HC_PID=$!; sleep 0.3

run_test half_close --host 10.200.100.1 --port "$HC_PORT" --size 65536 --half-close --timeout 8 \
    && ((++PASS)) || ((++FAIL))

kill $HC_PID 2>/dev/null || true; wait $HC_PID 2>/dev/null || true
sleep 1

# 6: idle
run_test idle --host 10.200.100.1 --port "$ECHO_PORT" --size 32768 --verify --sleep-before 1 --timeout 5 \
    && ((++PASS)) || ((++FAIL))
sleep 1

# 7: conn_refused
run_test_neg conn_refused --host 10.200.100.99 --port "$REFUSED_PORT" --size 1 --verify --timeout 5 \
     && ((++PASS)) || ((++FAIL))
sleep 1

# 8-9: проверка что цепочка не заклинила после стресса
run_test basic_1mb_2 --host 10.200.100.1 --port "$ECHO_PORT" --size 1048576 --verify --timeout 15 \
    && ((++PASS)) || ((++FAIL))
sleep 1

run_test basic_1mb_3 --host 10.200.100.1 --port "$ECHO_PORT" --size 1048576 --verify --timeout 15 \
    && ((++PASS)) || ((++FAIL))
sleep 1

echo ""; echo "=========================================="
echo "Results: $PASS passed, $FAIL failed ($((PASS + FAIL)) total)"
echo "Logs: $LOG_DIR/"
echo "=========================================="

trap - EXIT; cleanup
[ "$FAIL" -gt 0 ] && exit 1
exit 0
