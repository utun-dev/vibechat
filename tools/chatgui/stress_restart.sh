#!/bin/bash
# Стресс-тест перезапуска GUI: бесконечно запускает build/vibechat и посылает случайный SIGKILL или SIGHUP.
# Использует DISPLAY (по умолчанию :0), делает случайные паузы и подавляет вывод приложения.
set -u
cd "$(dirname "$0")/build" || exit 1
export DISPLAY="${DISPLAY:-:0}"

rnd() { local m=$((200 + RANDOM % 2801)) f; printf -v f '%03d' $((m%1000)); sleep "$((m/1000)).$f"; }

while :; do
    env DISPLAY="$DISPLAY" QT_ACCESSIBILITY=1 ./vibechat >/dev/null 2>&1 &
    p=$!
    rnd
    (( RANDOM % 2 )) && kill -9 "$p" || kill -HUP "$p"
    wait "$p" 2>/dev/null
    rnd
done
