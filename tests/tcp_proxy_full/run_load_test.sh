#!/bin/bash
# run_load_test.sh — нагрузочный тест tcp_proxy: N параллельных соединений,
# двусторонний byte-exact трафик без idle, замер пропускной способности.
#
# Это пресет поверх run_burst_test.sh (тот же движок, другие параметры).
#
# Параметры (env):
#   LOAD_CONNECTIONS  — число параллельных соединений (по умолчанию 8)
#   LOAD_TOTAL        — байт на направление на соединение (по умолчанию 8MB)
#
# Запуск: sudo ./run_load_test.sh

set -euo pipefail

BURST_NAME=load \
BURST_CONNECTIONS="${LOAD_CONNECTIONS:-8}" \
BURST_TOTAL="${LOAD_TOTAL:-8388608}" \
BURST_MIN_IDLE_MS=0 \
BURST_MAX_IDLE_MS=0 \
BURST_STALL_MS="${LOAD_STALL_MS:-30000}" \
bash "$(dirname "$0")/run_burst_test.sh"
