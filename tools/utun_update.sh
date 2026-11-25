#!/bin/sh
set -eo pipefail

# utun_update.sh - Remote uTun update script
# Usage: ./tools/utun_update.sh [fast|full] [workdir]
#   fast: git pull + make (incremental build)
#   full: git pull + make distclean + autoreconf + configure + make
# Returns:
#   0 - success
#   1 - invalid arguments or not a git repo
#   2 - git pull failed
#   3 - build failed
#   4 - start failed

# Determine project root from script location
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)

# Parse arguments
MODE="${1:-fast}"
WORKDIR="${2:-$PROJECT_ROOT}"

# Validate mode
if [ "$MODE" != "fast" ] && [ "$MODE" != "full" ]; then
    echo "ERROR: Invalid mode '$MODE'. Use 'fast' or 'full'" >&2
    exit 1
fi

# Change to workdir
cd "$WORKDIR" || { echo "ERROR: Cannot cd to $WORKDIR" >&2; exit 1; }
if [ -f ./proxy.sh ]; then
    . ./proxy.sh || { echo 'ERROR: cannot load proxy.sh' >&2; exit 1; }
fi

# Validate git repository
if [ ! -d ".git" ]; then
    echo "ERROR: Not a git repository: $WORKDIR" >&2
    exit 1
fi

PIDFILE="/var/run/utun.pid"
UTUN_BIN="./utun"

echo "=== uTun Update ==="
echo "Mode: $MODE"
echo "Workdir: $(pwd)"

# Stop utun if running via pidfile
if [ -f "$PIDFILE" ]; then
    PID=$(cat "$PIDFILE")
    if kill -0 "$PID" 2>/dev/null; then
        echo "Stopping utun (PID $PID)..."
        kill -TERM "$PID" 2>/dev/null || true
        # Wait graceful shutdown (up to 10s)
        for i in 1 2 3 4 5 6 7 8 9 10; do
            if ! kill -0 "$PID" 2>/dev/null; then
                break
            fi
            sleep 1
        done
        # Force kill if still running
        if kill -0 "$PID" 2>/dev/null; then
            echo "Force killing utun (PID $PID)..."
            kill -9 "$PID" 2>/dev/null || true
            sleep 1
        fi
    fi
    rm -f "$PIDFILE"
fi

# Ensure pidfile is removed before start
rm -f "$PIDFILE"

# Git pull
echo "Pulling latest changes..."
if ! git pull; then
    echo "ERROR: git pull failed" >&2
    exit 2
fi

# Detect number of CPUs (Linux or BSD)
if command -v nproc >/dev/null 2>&1; then
    JOBS=$(nproc)
else
    JOBS=$(sysctl -n hw.ncpu 2>/dev/null || echo 1)
fi

# Build
if [ "$MODE" = "full" ]; then
    echo "Full rebuild (autotools, clean, configure)..."
    make distclean 2>/dev/null || true
    autoreconf -fi 2>&1 | tee autoreconf.log
    ./configure --prefix=/usr/local 2>&1 | tee configure.log
    make clean
else
    echo "Fast rebuild (incremental)..."
fi

echo "Building with $JOBS parallel jobs (lib + src only)..."
if ! (make -j"$JOBS" -C lib && make -j"$JOBS" -C src) 2>&1 | tee build.log; then
    echo "ERROR: Build failed" >&2
    exit 3
fi

# Verify binary exists
if [ ! -x "$UTUN_BIN" ]; then
    echo "ERROR: Binary not found or not executable: $UTUN_BIN" >&2
    exit 3
fi

echo "Build successful: $(ls -la $UTUN_BIN)"

# Start utun
echo "Starting utun..."
#nohup "$UTUN_BIN" > utun_cons.log 2> utun_err.log &
$UTUN_BIN

# Wait for pidfile to be created (up to 10s)
for i in 1 2 3 4 5 6 7 8 9 10; do
    if [ -f "$PIDFILE" ]; then
        break
    fi
    sleep 1
done

# Read PID from file
if [ -f "$PIDFILE" ]; then
    NEW_PID=$(cat "$PIDFILE")
else
    echo "ERROR: Failed to start utun (pidfile not created)" >&2
    exit 4
fi

# Verify process is running
if kill -0 "$NEW_PID" 2>/dev/null; then
    echo "uTun started successfully (PID $NEW_PID)"
    exit 0
else
    echo "ERROR: Failed to start utun (PID $NEW_PID died)" >&2
    exit 4
fi
