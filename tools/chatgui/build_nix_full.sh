#!/bin/bash
# Полная сборка VibeChat на Linux/FreeBSD: запускает build_nix.sh --full.
# Передаёт остальные аргументы основному скрипту; зависимости устанавливаются через sudo.
set -euo pipefail
exec "$(dirname "$0")/build_nix.sh" --full "$@"
