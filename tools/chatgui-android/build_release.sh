#!/bin/bash
# Сборка и установка подписанного релиза; аргументы передаются общему скрипту сборки.
set -euo pipefail
exec "$(dirname "$0")/build.sh" --release "$@"
