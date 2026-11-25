#!/bin/bash
# logreceiver_restart.sh — kill all, clear log, restart daemon
set -e

BIND_IP="0.0.0.0"
PORT=9999
SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
LOG_FILE="$SCRIPT_DIR/logreceiver_output.log"
PID_FILE="/tmp/logreceiver.pid"
BIN="/home/user/utun2/tools/logreceiver/build/logreceiver"

killall logreceiver 2>/dev/null || true
sleep 0.2
killall -9 logreceiver 2>/dev/null || true
rm -f "$PID_FILE"

rm -f "$LOG_FILE.9" 2>/dev/null
for i in 8 7 6 5 4 3 2 1; do
    mv -f "$LOG_FILE.$i" "$LOG_FILE.$((i+1))" 2>/dev/null || true
done
mv -f "$LOG_FILE" "$LOG_FILE.1" 2>/dev/null || true
echo "rotated $LOG_FILE (keeping last 10)"

"$BIN" -d -p "$PID_FILE" "$BIND_IP" "$PORT" "$LOG_FILE"
sleep 0.3

if [ -f "$PID_FILE" ]; then
    echo "logreceiver restarted: $BIND_IP:$PORT -> $LOG_FILE (pid=$(cat $PID_FILE))"
else
    echo "ERROR: logreceiver failed to start"
    exit 1
fi
