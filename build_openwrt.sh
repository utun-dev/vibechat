#!/bin/bash
# OpenWRT build script for uTun
# Usage: ./build_openwrt.sh [TARGET] [PROFILE]
#
# Examples:
#   ./build_openwrt.sh                           # ASUS RT-AX53U (default)
#   ./build_openwrt.sh ramips/mt7621 asus_rt-ax53u
#   ./build_openwrt.sh ath79/generic tplink_archer-c7-v2
#   ./build_openwrt.sh x86/64
#   ./build_openwrt.sh --list                   # list available targets
#
# First run builds the full toolchain (30-120 min).
# Subsequent runs only rebuild the utun package.

set -e
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
if [ -f "$SCRIPT_DIR/proxy.sh" ]; then
    . "$SCRIPT_DIR/proxy.sh" || { echo 'ERROR: cannot load proxy.sh' >&2; exit 1; }
fi

OPENWRT_ROOT=/home/user/openwrt

# --- Default target: ASUS RT-AX53U ---
TARGET="${1:-ramips/mt7621}"
PROFILE="${2:-asus_rt-ax53u}"
J="${J:-$(nproc)}"

# --- Help / list ---
if [ "$1" = "--list" ] || [ "$1" = "-l" ]; then
    echo "=== Predefined targets ==="
    echo "  ramips/mt7621 asus_rt-ax53u    ASUS RT-AX53U (default)"
    echo "  ramips/mt7621 asus_rt-ax54     ASUS RT-AX54"
    echo "  ramips/mt7621 xiaomi_mi-router-4a-gigabit  Xiaomi Mi Router 4A"
    echo "  ath79/generic tplink_archer-c7-v2          TP-Link Archer C7 v2"
    echo "  x86/64                                    Generic x86_64 PC"
    echo "  x86/generic                               Generic x86 PC"
    echo ""
    echo "Full target list: grep 'TARGET_DEVICES' $OPENWRT_ROOT/target/linux/*/image/*.mk"
    exit 0
fi

# --- Check OpenWRT exists ---
if [ ! -d "$OPENWRT_ROOT" ]; then
    echo "ERROR: OpenWRT not found at $OPENWRT_ROOT"
    echo "Clone: git clone --depth 1 https://github.com/openwrt/openwrt.git $OPENWRT_ROOT"
    exit 1
fi

# --- Check utun package exists ---
if [ ! -f "$OPENWRT_ROOT/package/utun2/Makefile" ]; then
    echo "ERROR: utun package not found in OpenWRT"
    echo "Expected: $OPENWRT_ROOT/package/utun2/Makefile"
    exit 1
fi

echo "============================================"
echo "  uTun OpenWRT Build"
echo "  Target:  $TARGET"
echo "  Profile: $PROFILE"
echo "  Jobs:    $J"
echo "============================================"

cd "$OPENWRT_ROOT"

# --- Select target ---
TARGET_NAME=$(echo "$TARGET" | cut -d/ -f1)
SUBTARGET_NAME=$(echo "$TARGET" | cut -d/ -f2)

echo ""
echo "[1/3] Configuring target..."

# Write .config
cat > .config << EOF
CONFIG_TARGET_${TARGET_NAME}=y
CONFIG_TARGET_${TARGET_NAME}_${SUBTARGET_NAME}=y
CONFIG_TARGET_${TARGET_NAME}_${SUBTARGET_NAME}_DEVICE_${PROFILE}=y
CONFIG_PACKAGE_utun=m
# Basic system
CONFIG_PACKAGE_libc=y
CONFIG_PACKAGE_libopenssl=y
CONFIG_PACKAGE_libpthread=y
CONFIG_ALL_NONSHARED=n
CONFIG_ALL_KMODS=n
CONFIG_ALL=n
CONFIG_DEVEL=y
CONFIG_CCACHE=y
# Build options
CONFIG_BUILD_LOG=y
EOF

echo "  Target: ${TARGET_NAME}/${SUBTARGET_NAME}  Device: ${PROFILE}"

# Expand config
make defconfig V=s 2>/dev/null || true

echo ""
echo "[2/3] Building toolchain (if needed)..."
if [ ! -f "staging_dir/toolchain-*/bin/mips-openwrt-linux-gcc" ] && \
   [ ! -f "staging_dir/toolchain-*/bin/mipsel-openwrt-linux-gcc" ] && \
   [ ! -f "staging_dir/toolchain-*/bin/x86_64-openwrt-linux-gcc" ]; then
    echo "  Toolchain not found. Building... (this may take 30-120 min)"
    make tools/install -j$J V=s 2>&1 | tail -5
    make toolchain/install -j$J V=s 2>&1 | tail -5
    echo "  Toolchain built."
else
    echo "  Toolchain already built. Skipping."
fi

echo ""
echo "[3/3] Building utun package..."
make package/utun2/compile -j$J V=s 2>&1 | tail -10

# --- Show result ---
IPK=$(find bin/packages -name "utun_*.ipk" 2>/dev/null | head -1)
if [ -n "$IPK" ]; then
    echo ""
    echo "============================================"
    echo "  BUILD SUCCESS!"
    echo "  Package: $IPK"
    echo "  Size:    $(du -h "$IPK" | cut -f1)"
    echo "============================================"
else
    echo ""
    echo "ERROR: Package not found. Check build log above."
    # Try to find any built binary
    find bin -name "utun*" 2>/dev/null
    exit 1
fi
