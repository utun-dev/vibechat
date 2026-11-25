#!/bin/bash
set -e
cd "$(dirname "$0")/../../"
echo "=== Building BBR integration test ==="
./build.sh -j4
echo ""
echo "=== Running BBR integration test ==="
cd build/tests
./test_bbr_integration
