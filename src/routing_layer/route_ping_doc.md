# route_ping — наблюдение рабочего UDP endpoint

Модуль кодирует адреса и выполняет прямое UDP-наблюдение через конкретный
`ETCP_SOCKET`. Выбор B/C, пересылка управляющих сообщений и классификация
профиля принадлежат `nat_detection`; алгоритм — в `doc/nat_detection.txt`.

`route_ping_observe(socket, peer_key, peer_address, group, round, timeout_ms, callback, arg)`
передаёт PING с `ETCP_PING_FLAG_OBSERVE`. PONG содержит фактически наблюдаемый
source IP:port и исходный тег. Профиль не меняется от единственного ответа.
`route_ping_observation` проверяет размер, ID/поколение сокета, round и group.

Адрес занимает 19 байт: family(1: 4/6), IP(16: IPv4 + 12 нулей либо IPv6),
port(2, BE). Тег занимает 33 байта: `NAT4UDP!`(8), socket_id(1),
generation(8, BE), round(8, BE), group(8, BE). Декодирование отвергает неправильные
family, скрытые IPv4-байты и нулевой порт. IPv6 scope_id в wire не передаётся.
Кодирование нулевой порт не запрещает. Числа тега передаются в BE.

В транспорте nonce генерируется CSPRNG; PONG должен соответствовать ожидаемому
ключу, source IP:port и принимающему socket generation. Pending снимается
до callback. `etcp_cancel_pings(instance, callback, arg)` отменяет только
совпадающие запросы без callback; arg остаётся собственностью вызывающего.

`ETCP_PING_FLAG_ADDRESS_CHECK` обозначает прямое входящее испытание. Его
приём передаётся `nat_detection_receive_ingress` без UDP-ответа: ответ мог бы
испортить историю контактов. Результат передаётся по защищённому управляющему каналу.
UDP-результаты не используются для TCP listener и SOCKS5 endpoint.

Проверки: `tests/test_route_ping.c`, `tests/test_nat_detection.c`,
изолированный стенд `tests/stream_netns/run.py --nat-only`.
