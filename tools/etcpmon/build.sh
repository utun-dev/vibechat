#!/bin/bash
# ETCP Monitor Build Script for MSYS2 UCRT64 / MinGW
# Usage: ./build.sh
# Log is automatically saved to build.log

set -eo pipefail

# Capture all output to log file
LOG_FILE="build.log"

# Function to log and print
log() {
    echo "$1"
    echo "$1" >> "$LOG_FILE"
}

# Clear or create log file
> "$LOG_FILE"

log "================================"
log "ETCP Monitor Build Script"
log "================================"
log ""

# Compiler settings
CC=${CC:-gcc}

log "Compiler: $CC"
log "Target: etcpmon.exe"
log ""

# Build using Makefile
log "[1/3] Building..."
make CC="$CC" 2>&1 | tee -a "$LOG_FILE"

BUILD_RESULT=${PIPESTATUS[0]}

if [ $BUILD_RESULT -ne 0 ]; then
    log ""
    log "================================"
    log "Build FAILED!"
    log "================================"
    exit 1
fi

# Verify build
log ""
log "[2/3] Verifying build..."
if [ -f "etcpmon.exe" ]; then
    FILE_SIZE=$(du -h "etcpmon.exe" | cut -f1)
    log "  OK: etcpmon.exe created ($FILE_SIZE)"
    
    log ""
    log "================================"
    log "Build SUCCESS!"
    log "================================"
    log ""
    log "Executable: etcpmon.exe"
    log "Size: $FILE_SIZE"
    log "Location: $(pwd)/etcpmon.exe"
    log ""
else
    log ""
    log "================================"
    log "Build FAILED!"
    log "================================"
    exit 1
fi
