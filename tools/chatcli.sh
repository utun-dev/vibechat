#!/bin/bash
# chatcli — headless chat CLI client for uTun
#
# Usage:
#   chatcli [--host HOST] [--port PORT] <command> [args...]
#
# Commands:
#   channels                           list channels
#   members <ch_id>                    list members with full state
#   messages <ch_id> [count]           read last messages
#   send <ch_id> <content_type> <text> send message
#   invite <ch_id>                     create invite link
#   connect <utun://...>               join channel via invite link
#   create <name>                      create new channel
#   status                             show connection status
#   listen                             subscribe to events (interactive)
#   --help                             show this help

set -e
HOST="${CHATCLI_HOST:-127.0.0.1}"
PORT="${CHATCLI_PORT:-9999}"
ID=1
NC_TIMEOUT="${CHATCLI_TIMEOUT:-5}"

_send() {
    local json="$1"
    if command -v socat &>/dev/null; then
        echo "$json" | socat -t "$NC_TIMEOUT" - TCP:"$HOST":"$PORT" 2>/dev/null || true
    elif command -v nc &>/dev/null; then
        if nc -h 2>&1 | grep -q '^BusyBox'; then
            echo "$json" | nc "$HOST" "$PORT" 2>/dev/null || true
        else
            echo "$json" | nc -q "$NC_TIMEOUT" "$HOST" "$PORT" 2>/dev/null || true
        fi
    else
        echo "ERROR: socat or nc required" >&2; exit 1
    fi
}

_request() {
    local cmd="$1" data="$2"
    local json='{"id":'"$ID"',"cmd":"'"$cmd"'"'
    if [ -n "$data" ]; then json="$json,$data"; fi
    json="$json}"
    _send "$json"
    ID=$((ID + 1))
}

_show() { local json="$1"; echo "$json" | python3 -m json.tool 2>/dev/null || echo "$json"; }

case "${1:-}" in
    channels)      r=$(_request channels); _show "$r" ;;
    members)       r=$(_request members '"ch":"'"$2"'"'); _show "$r" ;;
    messages|msgs)
        cnt="${3:-20}"
        r=$(_request messages '"ch":"'"$2"'","count":'"$cnt"','"offset":0'); _show "$r" ;;
    send)
        ct="${3:-text}"; text="$4"
        r=$(_request send '"ch":"'"$2"'","content_type":"'"$ct"'","data":"'"$text"'"'); _show "$r" ;;
    invite)        r=$(_request invite '"ch":"'"$2"'"'); _show "$r" ;;
    connect|join)  r=$(_request connect '"link":"'"$2"'"'); _show "$r" ;;
    create)        r=$(_request create_channel '"name":"'"$2"'"'); _show "$r" ;;
    status|stat)   r=$(_request status); _show "$r" ;;
    listen)
        echo "Connecting to $HOST:$PORT (Ctrl+C to quit)..."
        {
            printf '{"id":1,"cmd":"subscribe","enable":1}\n'
            cat
        } | nc "$HOST" "$PORT"
        ;;
    ping)          r=$(_request ping); _show "$r" ;;
    --help|help|-h|"")
        head -20 "$0" | sed 's/^# //' | grep -v '^#'
        ;;
    *)             echo "Unknown command: $1"; echo "Use --help for usage" >&2; exit 1 ;;
esac
