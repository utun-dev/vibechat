# INI-конфигурация ядра

`config_parser.c/h` загружает собственную модель `utun_config` с настройками и списками.
`parse_config(path)` читает файл тем же разборщиком, что `parse_config_from_buf`.
Парсер не запускает ядро и не генерирует ключи; для файлового запуска отдельно используется config_updater.

## Вход, владение и ошибки

Файл открывается только для чтения. Ошибки открытия/чтения/закрытия дают NULL.
Буфер содержит ровно len байтов без завершающего NUL, NULL допустим при len=0.
filename буферного API используется только в диагностике; NULL заменяется на `<buffer>`.
Вход не меняется. Результат и все списки освобождает `free_config`; NULL безопасен.
`CFG_CLIENT_LINK.local_srv` заимствован из servers того же конфига; server_name содержит копию имени.

Ошибка обработчика известной опции, памяти или key=value отклоняет всю модель.
Частичные списки освобождаются. Каждое вхождение проверяется: ошибочный ранний дубль
не маскируется поздним корректным. Повторные корректные скаляры применяются по порядку.
Config updater исправляет управляемые ключи устройства и ошибочные ранние дубли перед загрузкой.

Ненулевой результат не гарантирует запуск: обязательность отсутствующих полей,
устройства, комбинации опций и доступность портов дополнительно проверяют владельцы ресурсов.
Адреса server/client/control разрешаются синхронно через getaddrinfo с выбором первого адреса.
На Windows вызывающий заранее инициализирует socket subsystem; сетевые сокеты парсер не открывает.

## Формат и списки

Пробелы вокруг секций, ключей и значений удаляются. Секции нечувствительны к регистру, ключи чувствительны.
Комментарий # удаляется из значения; заголовок секции должен занимать полную строку.
Встроенный NUL и физические строки длиннее 1022 байтов до LF запрещены; CR входит в предел.
Неизвестные/нераспознанные секции и их ключи игнорируются с WARN.
Известные секции отклоняют неизвестные опции, кроме ограниченной обработки debug/gui.
В debug хранится до DEBUG_CATEGORY_COUNT строк; лишние игнорируются с WARN.
Категории/уровни хранятся в полях до 15 байтов, фактические имена проверяются debug API при применении.
GUI читает только utun_enabled, остальные ключи остаются frontend.

Именованные server/client/network секции создают отдельные записи, дубли имён не объединяются.
Servers, clients, networks, маршруты, allowed_keys и NTP добавляются в голову списка;
порядок обхода обратный вводу. STUN сохраняет порядок.
В link=local_server:host:port локальный сервер должен быть объявлен выше.
Если имя не найдено, весь link рассматривается как remote-only host:port с local_srv=NULL.

## Значения

- Endpoint: IPv4/IPv6/hostname и десятичный порт 0..65535; IPv6 допускает скобки.
  Порт 0 оставлен для автоматического bind, применимость проверяет ресурс.
- SOCKS: непустой host до 63 байтов, порт 1..65535. STUN: IPv4/A-hostname[:port], по умолчанию 3478;
  при включении минимум две разные строки server, независимость IP проверяет stun_nat.
- Подсеть: IPv4/IPv6[/prefix], диапазон 0..32/0..128. Без / используется /32 или /128;
  явный IPv6 /32 сохраняется. Host-биты не нормализуются. Control allow — только IPv4, включая /0.
- Числа проверяются целиком до сужения. Адрес Ed25519 занимает 64 HEX без префикса. Беззнаковые значения не принимают знак.
  Bool: yes/no, true/false, 1/0. Keepalive interval помещается в uint16_t; таймеры проверяются до умножения.
  NTP interval меньше 60 секунд приводится к 60, по умолчанию 3600.
- Storage: uint64_t байты либо один суффикс M/G (1024²/1024³), без переполнения, знака и хвоста MB.
  MTU: 0..65535, дополнительная проверка у ресурса.
- HEX проверяется целиком. Network требует оба 32-байтных ключа. Индекс сети — полный pubkey.
  Пустые необязательные ключи идентичности/пира остаются незаданными для последующей инициализации.
- REALITY short_ids проверяется всем списком; version — ровно три десятичных октета.
  time_window=0..3600, relay_idle_timeout=0..INT_MAX/10000.
- Слишком длинные строки отклоняются; my_node_name явно усекается до 63 байтов с WARN.

## Секции

| Секция | Основные опции |
|---|---|
| global | ключи устройства, client_type, TUN, MTU, keepalive, BBR, auto_sockets, log/debug, db_sync |
| server:name | addr, netif, so_mark, fib, type, transport, mtu, only_local, reality, socks |
| client:name | peer_public_key, link, keepalive и клиентские REALITY-параметры |
| network:name | pubkey, signing_key |
| routing | route_subnet, my_subnet |
| firewall | allow=all или IPv4[:port]; 0 означает весь IP, CIDR запрещён |
| control | ip/control_ip, port/control_port, allow/control_allow; порядок ip/port не влияет на итог |
| allowed_keys | allow_all, key (32-byte HEX) |
| nat | tun_ifname, tun_ip (IPv4), nat_via, port_start/end, forward=proto:Ed25519:IPv4:internal_port:external_port |
| tcp_proxy_client | enabled, tun_name/ip, mtu, via_node, socks/http_proxy enabled/addr, forward=port -> IPv4:port |
| tcp_proxy_server | allowed_group (utun или Ed25519 группы), tcp_recv_buf, fib, so_mark, netif, bind_ip, connect_timeout |
| chatserver / chat | db_path, add_key, allowed_groups, storage sizes, headless/audio binds и chat_setting API |
| ntp | enabled, server, interval |
| stun | enabled, server |
| reality | enabled, server_name, dest, short_id/short_ids, public/private_key, version, time_window, relay_idle_timeout, fingerprint |
| socks | enabled, addr, username, password |
| log_udp | ip, port |
| debug | category=level |
| gui | utun_enabled |

`[chatserver] allowed_groups` принимает `all` или список публичных Ed25519 групп через запятую. Сейчас значение только читается из конфигурации. Для добавления сервера в группу JOIN проверяет `add_key` из SERVER-ссылки.

Наличие nat, tcp_proxy_client/server, chat/chatserver включает соответствующий режим даже при пустой секции;
chat также включает db_sync. Явный tcp_proxy_client enabled=no сохраняется при последующих опциях.
tun_enabled по умолчанию 0; при включении без имени используется tun0.
auto_sockets принимает no/yes/android, auto_socket_protocols — tcp/udp/tcp+udp (по умолчанию оба).
В Android frontend генерирует отдельные server записи. Addr temporary_ipv6:port/permanent_ipv6:port
задаёт выбор IPv6 интерфейса; обычный повторный addr сбрасывает этот режим.

## Диагностика и вспомогательные API

`print_config` выводит снимок в config/INFO. Private/signing keys заменены признаками наличия;
сети выводятся независимо от количества firewall rules.
`[chatserver] add_key` — ненулевой ключ из 32 случайных байтов (64 HEX-символа), разрешающий запросить
добавление сервера в произвольный канал приглашающего. При загрузке из файла отсутствующий ключ
создаёт и атомарно сохраняет config_updater; заданный ошибочный/нулевой ключ отклоняется.
Ключ постоянный: перезапуск сохраняет ссылку, явная замена запрещает новые операции по старой ссылке.
Уже добавленные каналы от смены ключа не удаляются. Старое join_password больше не используется.

Серверную ссылку собирает `chat_server_link_build` через общий кодек из текущей проекции NODEINFO:
публичный ключ, socket_id, IP/port, протоколы и REALITY плюс add_key. Отдельный тип SERVER версии 0x04
не содержит ID канала или временного JOIN-ключа. Получить её можно через `chatcli server_link`;
foreground CLI также выводит её в stderr после старта. Обычный лог содержит только node/endpoints,
полную ссылку и add_key в него не выводим.

`update_config_keys` — устаревшее добавление global-секции с двумя проверенными HEX-ключами в конец файла.
Возвращает 0/-1, проверяет запись/закрытие, но не атомарно и не согласует node_id; штатный путь — config_updater.
`config_peer_in_clients(config, pubkey[32])` возвращает 1 при бинарном совпадении ключа, 0 при отсутствии/NULL.

Собственный Ed25519 выводится из seed. Адрес узла в `nat_via`, NAT `forward` и `via_node` — канонический публичный Ed25519, ровно 64 HEX. В `allowed_group` задаётся такой же ключ группы или `utun`. Транспортные `my_public_key`, `peer_public_key` и `[allowed_keys] key` хранят полный X25519.
