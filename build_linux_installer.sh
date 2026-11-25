#!/usr/bin/env bash
# Устанавливает зависимости сборки, пересобирает ChatGUI в Release и создаёт DEB.
set -euo pipefail
exec "$(dirname "$0")/tools/chatgui/build_nix.sh" --full --deb -j4 "$@"
