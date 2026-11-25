#!/bin/sh
# Изолированная IPv6-only LAN без default route. Необязательный аргумент — имя LAN-теста.
# Namespace исчезает вместе с тестом.
set -eu
if [ "${1:-}" != "--inside" ]; then
    exec unshare --user --map-root-user --net --fork -- sh "$0" --inside "${1:-test_local_transport}"
fi
ip link set lo up
ip link add local-a type veth peer name local-b
ip link set local-a addrgenmode none
ip link set local-b addrgenmode none
ip link set local-a up
ip link set local-b up
ip -6 addr add fe80::1/64 dev local-a nodad
ip -6 addr add fe80::2/64 dev local-b nodad
exec "$(dirname "$0")/${2:-test_local_transport}" --ipv6
