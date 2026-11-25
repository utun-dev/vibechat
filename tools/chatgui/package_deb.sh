#!/usr/bin/env bash
# Создаёт DEB VibeChat с библиотеками и Qt-плагинами: проверяет инструменты, пересобирает Release и запускает CPack.
# Использует build-deb, настроенный build_nix.sh --deb; --check только проверяет инструменты упаковки.
set -euo pipefail
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SRC_DIR/build-deb"
trap 'echo "ERROR: DEB packaging failed at line $LINENO" >&2' ERR
[[ $# = 0 || ( $# = 1 && $1 = --check ) ]] || { echo "ERROR: expected no arguments or --check" >&2; exit 1; }
[[ $(uname -s) = Linux && -z ${MSYSTEM:-} ]] || { echo "ERROR: DEB packaging requires Linux" >&2; exit 1; }
for tool in cmake cpack dpkg dpkg-deb dpkg-shlibdeps dpkg-query file objdump readelf python3 appstreamcli; do
    command -v "$tool" >/dev/null || {
        echo "ERROR: missing DEB build tool: $tool; run build_nix.sh --full --deb to install build dependencies" >&2
        exit 1
    }
done
cmake_version=$(cmake --version)
if [[ ! "$cmake_version" =~ version[[:space:]]([0-9]+)\.([0-9]+) ]] ||
   (( BASH_REMATCH[1] < 3 || (BASH_REMATCH[1] == 3 && BASH_REMATCH[2] < 21) )); then
    echo "ERROR: DEB packaging requires CMake 3.21 or newer" >&2; exit 1
fi
architecture=$(dpkg --print-architecture)
echo "INFO: DEB packaging tools ready: architecture=$architecture"
[[ $# = 0 ]] || exit 0
[[ -f "$BUILD_DIR/CPackConfig.cmake" ]] || { echo "ERROR: configure the DEB build with build_nix.sh --deb first" >&2; exit 1; }
echo "INFO: rebuilding the Release executable before packaging"
cmake --build "$BUILD_DIR" --target vibechat --parallel
echo "INFO: packaging VibeChat DEB with bundled libraries and automatic system dependencies"
cpack --config "$BUILD_DIR/CPackConfig.cmake" -G DEB -B "$BUILD_DIR/installer"
read -r version < "$BUILD_DIR/chatgui_version.txt"
artifact="$BUILD_DIR/installer/vibechat_${version}_${architecture}.deb"
[[ -f "$artifact" ]] || { echo "ERROR: CPack did not produce the expected package: $artifact" >&2; exit 1; }
dependencies=$(dpkg-deb -f "$artifact" Depends)
echo "INFO: DEB system dependencies: $dependencies"
echo "INFO: DEB ready: $artifact"
echo "INFO: install with: sudo apt install \"$artifact\""
