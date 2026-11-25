#!/bin/bash
#
# chat_tcp_test/run.sh — TCP integration test with diagnostics
#
set -eu
TESTDIR="$(cd "$(dirname "$0")" && pwd)"
cd "$TESTDIR"

CHATCLI="python3 $TESTDIR/../chatcli"
UTUN="$TESTDIR/../../src/utun"

# ── cleanup handler ──
cleanup() {
    local rc=${1:-$?}
    echo ""
    echo "=== cleanup (exit=$rc) ==="
    pkill -9 -f "utun.*15001\|utun.*15002\|utun.*15003\|utun.*15004" 2>/dev/null || true
    sleep 0.3
    rm -f "$TESTDIR"/*.pid
    if [ "$rc" -ne 0 ]; then
        echo "=== a.log (last 40) ==="; tail -40 "$TESTDIR/a.log" 2>/dev/null || true
        echo "=== b.log (last 40) ==="; tail -40 "$TESTDIR/b.log" 2>/dev/null || true
    fi
    exit "$rc"
}
trap 'cleanup 1' INT TERM
trap 'cleanup' EXIT

die() { echo "FATAL: $*" >&2; exit 1; }

# ── kill old + clean start ──
echo "--- Killing old processes on test ports ---"
for port in 15001 15002 15003 15004; do
    fuser -k ${port}/tcp 2>/dev/null || true
done
sleep 0.5

rm -rf "$TESTDIR"/db_a "$TESTDIR"/db_b "$TESTDIR"/*.log "$TESTDIR"/*.pid
mkdir -p "$TESTDIR"/db_a "$TESTDIR"/db_b

# ── start ──
echo "--- Starting utun ---"
"$UTUN" -f -p "$TESTDIR/a.pid" -l "$TESTDIR/a.log" -c "$TESTDIR/a.conf" > /dev/null 2>&1 &
PID_A=$!
"$UTUN" -f -p "$TESTDIR/b.pid" -l "$TESTDIR/b.log" -c "$TESTDIR/b.conf" > /dev/null 2>&1 &
PID_B=$!

# ── wait ready (max 5 sec) ──
echo "--- Wait ready ---"
for i in $(seq 1 50); do
    if $CHATCLI --port 15002 ping 2>/dev/null | grep -q uptime; then echo "  A ready (${i}00ms)"; break; fi
    sleep 0.1
    [ $i -lt 50 ] || die "node A not ready"
done
for i in $(seq 1 50); do
    if $CHATCLI --port 15004 ping 2>/dev/null | grep -q uptime; then echo "  B ready (${i}00ms)"; break; fi
    sleep 0.1
    [ $i -lt 50 ] || die "node B not ready"
done

# ── create channel ──
echo "--- Create channel ---"
$CHATCLI --port 15002 create "TestTCP" > /dev/null 2>&1
sleep 0.3
CH_ID=$($CHATCLI --port 15002 channels 2>/dev/null | python3 -c "import sys,json;print(json.load(sys.stdin)[0]['id'])")
[ -n "$CH_ID" ] || die "failed to get channel_id"
echo "  ch_id=$CH_ID"

# ── send messages ──
echo "--- Send messages ---"
for i in 1 2 3 4; do
    $CHATCLI --port 15002 send "$CH_ID" "Message $i" > /dev/null 2>&1 || die "send $i failed"
done
echo "  sent 4 OK"

# ── invite ──
echo "--- Invite ---"
LINK=$($CHATCLI --port 15002 invite "$CH_ID" 2>/dev/null | python3 -c "import sys,json;print(json.load(sys.stdin).get('link',''))")
[ -n "$LINK" ] || die "invite failed"
echo "  link OK"

# ── connect B ──
echo "--- Connect B ---"
$CHATCLI --port 15004 connect "$LINK" > /dev/null 2>&1 || die "connect failed"

# ── wait ETCP:UP (max 5 sec) ──
echo "--- Wait ETCP:UP ---"
UP=0
for i in $(seq 1 50); do
    sleep 0.1
    if $CHATCLI --port 15004 status 2>/dev/null | grep -q 'ETCP:UP'; then
        echo "  ETCP:UP after ${i}00ms"
        UP=1; break
    fi
done
[ $UP -eq 1 ] || die "no ETCP:UP after 5s"

# ── wait members sync ──
echo "--- Wait members sync ---"
MEM_OK=0
for i in $(seq 1 30); do
    MEM_COUNT=$($CHATCLI --port 15004 members "$CH_ID" 2>/dev/null | python3 -c "import sys,json;print(len(json.load(sys.stdin)))" 2>/dev/null || echo 0)
    if [ "$MEM_COUNT" -ge 2 ]; then
        echo "  members=$MEM_COUNT OK (${i}00ms)"
        MEM_OK=1; break
    fi
    sleep 0.1
done
[ $MEM_OK -eq 1 ] || die "members sync timeout"

# ── messages ──
echo "--- Check messages ---"
MSG_COUNT=$($CHATCLI --port 15004 messages "$CH_ID" 10 2>/dev/null | python3 -c "import sys,json;print(len(json.load(sys.stdin)))" 2>/dev/null || echo 0)
[ "$MSG_COUNT" -eq 4 ] || die "expected 4 messages, got $MSG_COUNT"
echo "  messages=$MSG_COUNT OK"

echo ""
echo "=== TEST PASSED ==="
exit 0
