# ETCP Connections

## 1. Назначение

Подмодуль обслуживает UDP/TCP-сокеты и физические пути (`ETCP_LINK`) общего
соединения с пиром (`ETCP_CONN`). Сокет обслуживает несколько пиров, одно соединение
может использовать несколько UDP/STCP-линков. Все вызовы выполняются в uasync-потоке instance.

Отвечает за:
- Создание серверных сокетов из конфига и подключение линков, которыми управляет NCD
- Приём сырых пакетов, дешифровку, диспетчеризацию (INIT/PING/PONG/KEEPALIVE/данные)
- INIT handshake: обмен node_id, ключами pubkey (X25519 + Ed25519), MTU, keepalive-параметрами
- Keepalive, отдельный RX watchdog и согласование сна через флаги keepalive-пакета
- Управление inflight-лимитами, burst-измерения bandwidth, BBR congestion control per-link
- Обмен наблюдаемыми endpoint; профили адресов и проверки NAT ведут socket_monitor/nat_detection

## 2. Как пользоваться

### Инициализация сокетов и соединений

Двухфазная инициализация:
```c
// Ядро создаёт listen-сокеты до инициализации групп.
if (init_sockets(instance) < 0) return -1;

// Подготовить сокеты, NCD control и подписки keepalive.
if (init_connections(instance) < 0) return -1;
```

`init_connections` использует результат прежнего `init_sockets` либо вызывает его,
если сокетов ещё нет. При `auto_sockets` сокеты создаёт отдельный модуль.
Запросы к `[client]` запускает UTUN-сервис через `init_client_connections`.
Другие владельцы открывают транспорт через `node_conn_direct_open/open_node` и ждут `NCD_EVENT_UP`.

### Входящий пакет: полный путь

```
socket_recvfrom() → etcp_connections_read_callback_socket()
  ├─ Есть линк и session_ready → sc_decrypt() по session_key
  │   ├─ KEEPALIVE → обновить period, выйти
  │   ├─ INIT_RESPONSE → handle_init_response_client()
  │   ├─ HELLO/CHALLENGE/CONFIRM → etcp_session
  │   └─ SESSION_DATA → проверить эпохи, снять обёртку → etcp_conn_input(pkt)
  │
  └─ Нет ключей линка или обычная дешифровка не удалась → попытка INIT-дешифровки
      ├─ Извлечь соль + obfuscated pubkey из хвоста пакета
      ├─ sc_obfuscate_pubkey() → восстановить pubkey пира
      ├─ sc_set_peer_public_key() → вычислить session_key
      ├─ sc_decrypt() → расшифровать
      ├─ PING → handle_ping(): обработать пробу, обычно ответить PONG
      ├─ PONG → handle_pong(): найти PING_CONTEXT, вызвать cb
      └─ INIT_REQUEST → обработать handshake, отправить INIT_RESPONSE
```

### Отправка зашифрованного пакета

```c
#include "etcp_keepalive.h"

// Сформировать корректный keepalive с периодом, flags и padding и сразу отправить.
etcp_link_send_keepalive(link);
```

`etcp_encrypt_send` делает:
1. Заполняет timestamp(2) и flag_up(1); перед секциями DATA/ACK добавляет 17-байтную обёртку с эпохами сессии.
2. Для UDP шифрует тело AES-128-CCM и добавляет открытый хвост `noencrypt_len` (40 байт salt/pubkey у INIT-запроса).
3. Отправляет UDP через SOCKS5-релей сокета, иначе `send_hook`, иначе `socket_sendto`.
4. Для TCP передаёт сформированное тело в `stcp_link_send`; шифрование и очередь обеспечивает STCP/REALITY.

`etcp_encrypt_send` не освобождает переданный `ETCP_DGRAM`; отрицательный результат означает ошибку отправки.

### PING/PONG (one-shot пробники)

```c
etcp_send_ping(instance, peer_pubkey_bin, &addr, timeout_ms,
               my_ping_callback, user_arg,
               user_data, user_data_len);

// Коллбэк:
void my_ping_callback(int success, uint16_t rtt, void* arg, uint64_t nonce,
                      const uint8_t* resp_data, size_t resp_data_len);
```

PING/PONG не требуют ETCP_CONN/LINK: используются временный `sc_context_t` и
обфусцированный pubkey в хвосте. RTT callback измерен в 0.1 мс; данные ответа
заимствованы только на время callback. `etcp_cancel_pings` отменяет совпадающие
callback/arg без уведомления; аргумент освобождает владелец запроса.

### Адаптивный keepalive

- Базовый `keepalive_interval` согласуется по типам устройств: при наличии mobile выбирается max, иначе min двух значений
- Период `ka_period_ms` начинается с базового интервала; UDP handshake ограничивает базу снизу 10 мс
- Если был трафик (`pkt_sent_since_keepalive=1`): период сбрасывается к `keepalive_interval`
- Без трафика и при `keepalive_adaptive` период растёт как `period * 105 / 100 + 1`, максимум 10 с
- Принятый ненулевой период пира задаёт `keepalive_timeout = period * KA_TIMEOUT_MULT` (×10)
- Отдельный RX watchdog отсчитывает срок от последнего допустимого приёма; собственные отправки не продлевают жизнь линка
- Сервер не шлёт keepalive при `recv_keepalive=0` (линк уже мёртв), клиент шлёт всегда

### Сон и пробуждение

Сон передаётся тем же `ETCP_KEEPALIVE` (0x08). Обязательное тело:
`code(1) + flags(1) + period_ms(2 LE)`; при `KA_FLAG_SLEEP` добавляются
`active_remaining(2 LE) + sleep_duration(2 LE)` в единицах 0.1 с. Далее допустим нулевой padding до `ETCP_PADDING_MAX`.

При сне одной из сторон обычный периодический keepalive останавливается.
Спящий узел объявляет фазу `KA_FLAG_SLEEP`, UDP-пир отвечает `KA_FLAG_SLEEP_ACK`;
у TCP отдельное подтверждение не требуется. UDP-анонс повторяется до ACK или
исчерпания попыток; накопленные провалы приводят к link DOWN.
Обычный keepalive сообщает пробуждение, снимает фазовый таймер пира и возвращает normal-режим.

RX watchdog действует и во сне. К обычному timeout применяется максимум с
`(active_remaining + sleep_duration) * 100мс + keepalive_interval * 10`.
Фазы и повторные отправки сами по себе не продлевают срок от последнего RX.
С `UTUN_HAVE_STANDBY` модуль подписан на switch/phase callbacks standby;
в остальных сборках — на активность мобильного клиента. Формат и детали: `etcp_keepalive.h`.

## 3. API

### Ключевые структуры

**ETCP_SOCKET** — один слушающий/клиентский сокет из конфига:
- `fd` — UDP-сокет или TCP listener
- `links_queue` — хеш-очередь UDP-линков с 19-байтным ключом port/addr/family
- `interface_addr` — IP интерфейса (автоопределённый или из конфига)
- `nat_addr`, `address_profile` — endpoint NAT и профиль mapping/inbound/проверок адреса
- `type` — конфигурационный `CFG_SERVER_TYPE_*`
- `sock_id` — уникальный ID (0-255) для сопоставления при NAT-матчинге

**ETCP_LINK** — одно динамическое соединение (один путь между двумя узлами):
- Принадлежит одному `ETCP_CONN`; TCP-клиент может работать без локального `ETCP_SOCKET`
- `remote_addr` — адрес пира; для UDP поиск в `links_queue` соответствующего сокета
- `mtu`, `mtu_local`, `mtu_remote` — согласованный MTU (min)
- `is_server` — 0=клиент инициирует, 1=сервер принимает
- `initialized=1` — handshake линка принят; готовность потока также зависит от подтверждения сессии
- `link_status` — итоговый статус: `recv_keepalive && remote_keepalive`
- `link_state`: 0=init, 1=handshake, 2=reconnect, 3=connected
- `init_timer` / `keepalive_timer` / `shaper_timer` — таймеры uasync
- `rx_timeout_timer` — независимый контроль приёма; `retrans_timer` — таймер физических DATA-попыток этого пути
- `bbr` — состояние BBR congestion control per-link
- Поля burst-измерений bandwidth (sender + receiver)
- `send_hook` — перехватчик отправки (для net_emulator)
- Статистика: encrypt/decrypt/send/recv errors, total_encrypted/decrypted, retransmissions

**ETCP_DGRAM** — незашифрованная кодограмма:
- `link` — куда отправлять / откуда получено
- `data_len` — длина data, включая открытый хвост, без timestamp/flag_up
- `noencrypt_len` — байты с конца, не подлежащие шифрованию (SC_PUBKEY_ENC_SIZE для INIT)
- `timestamp`, `flag_up` — заголовок, шифруется вместе с data

**ETCP_INIT_REQUEST_PKT** (67 байт) — обязательное тело запроса, до padding:
- `code` (0x02/0x04), `node_id[8]`, `reset_id[8]`, `mtu[2]`, `keepalive[2]`, `recovery[2]`
- `link_id`, `socket_id`, `only_local`, `type`
- `src_ipv4[4]`, `src_port[2]` — заявленный локальный endpoint клиента
- `collision` — флаг разрешения коллизий
- `ed25519_pubkey[32]`, `device_type`

**ETCP_INIT_RESPONSE_PKT** (64 байта) — обязательное тело ответа, до padding:
- `code` (0x03/0x05), `node_id[8]`, `reset_id[8]`, `mtu[2]`
- `link_id`, `remote_socket_id`, `only_local`, `type`
- `peer_ipv4[4]`, `peer_port[2]` — внешний NAT-адрес клиента
- `ed25519_pubkey[32]`, `device_type`, `keepalive[2]`

Числовые поля INIT длиной более байта записываются BE. Каждая сторона сообщает собственный `reset_id`.
Повтор INIT не сбрасывает надёжный поток: смену известной эпохи подтверждает HELLO/CHALLENGE/CONFIRM.
До изменения состояния проверяются размер тела, node_id по X25519-ключу, ненулевой link_id и минимальный UDP MTU.
Минимум равен `ETCP_MIN_UDP_MTU(family)` (191 байт IPv4, 211 IPv6); MTU больше 1500 ограничивается сверху.

Известный пир входящего UDP/TCP индексируется в реестре до callback создания линка.
Публикация `INIT/UP` происходит отдельно. Callback может отменить линк, открыть NCD handle или создать другой путь;
отмена входящего линка сохраняет новый conn при наличии другого линка или NCD-владельца.
`etcp_link_close` сразу отключает линк, снимает таймеры и выполняет уведомления; память удерживается `active_users`
до выхода последних внутренних стеков конструктора/RX/callback. Подробный контракт — в `etcp_connections.h`.

### Типы кодограмм

| Константа | Значение | Назначение |
|-----------|----------|-----------|
| `ETCP_INIT_REQUEST` | 0x02 | Начальный handshake UDP-линка |
| `ETCP_INIT_RESPONSE` | 0x03 | Вариант INIT-ответа |
| `ETCP_INIT_REQUEST_NOINIT` | 0x04 | Восстановление UDP-линка / разрешение коллизии |
| `ETCP_INIT_RESPONSE_NOINIT` | 0x05 | Второй вариант INIT-ответа; эпоха проверяется так же |
| `ETCP_PING` | 0x06 | One-shot пробник |
| `ETCP_PONG` | 0x07 | Ответ на пробник |
| `ETCP_KEEPALIVE` | 0x08 | keepalive-пакет |
| `ETCP_SESSION_DATA` | 0x09 | Секции DATA/ACK с эпохами отправителя и получателя |
| `ETCP_SESSION_HELLO/CHALLENGE/CONFIRM` | 0x0A/0x0B/0x0C | Подтверждение сессии |
| `ETCP_TCP_ADDRESS` | 0x0D | STCP: наблюдаемый IP клиента, без порта |

### Основные функции

| Функция | Назначение |
|---------|-----------|
| `init_sockets(inst)` | Создать listen-сокеты из `[server]` конфига (до `topo_group_init`) |
| `init_connections(inst)` | Подготовить listen-сокеты, NCD control и keepalive; 0/-1 |
| `init_client_connections(inst, clients)` | Заменить запросы UTUN-группы к пирам конфигурации; 0/-1 |
| `etcp_socket_add(inst, srv)` | Создать UDP-сокет из `CFG_SERVER`: bind, non-block, send/recv buffers 4MB |
| `etcp_socket_remove(e_sock)` | Закрыть сокет, удалить все линки, освободить память |
| `etcp_link_new(etcp, conn, addr, is_server)` | Создать ETCP_LINK: выделить local_link_id, BBR, вставить в сокет и ETCP_CONN |
| `etcp_link_close(link)` | Удалить линк: таймеры, удаление из списков сокета и ETCP_CONN |
| `etcp_link_update_inflight_lim(link, new_lim)` | Применить положительное окно DATA, ограничив сверху max_inflight; ноль отклоняется |
| `etcp_encrypt_send(dgram)` | Сформировать защищённый UDP-пакет либо передать тело в STCP; dgram остаётся вызывающему |
| `etcp_udp_send(link, fd, buf, len, addr, addr_len)` | SOCKS5 сокета, иначе send_hook, иначе socket_sendto |
| `etcp_send_ping(inst, pubkey, addr, timeout, cb, arg, data, len)` | One-shot PING |
| `etcp_send_ping_to_socket(inst, sock, pubkey, addr, ...)` | PING через конкретный сокет |
| `etcp_link_find_by_addr(e_sock, addr, is_tcp)` | Хеш-поиск в links_queue сокета; is_tcp участвует в ключе; без очереди возвращает NULL |
| `etcp_link_find_by_remote_id(conn, remote_link_id)` | Найти линк по ID пира |
| `etcp_find_free_local_link_id(etcp)` | Свободный local_link_id в 1..255 циклически после последнего назначенного; -1 при исчерпании |
| `etcp_link_enter_init(link)` | Начать INIT handshake (link_state=1) |
| `etcp_link_enter_reinit(link)` | Начать переподключение (link_state=2, без сброса ETCP) |

### Burst-измерения

| Функция | Назначение |
|---------|-----------|
| `etcp_link_burst_start(link)` | Начать burst-замер bandwidth |
| `etcp_link_burst_check(link)` | Проверить условия для старта burst |
| `etcp_link_burst_finish(link)` | Завершить burst, установить таймер ожидания ответа |

### Константы

| Константа | Значение | Назначение |
|-----------|----------|-----------|
| `INFLIGHT_LIM_MIN` | 8192 | Минимальное допустимое значение настройки bbr_max_cwnd; текущий минимум окна задаёт BBR |
| `INFLIGHT_LIM_MAX` | 1048576 | Максимальный inflight (1M) |
| `PACKET_DATA_SIZE` | 2048 | Размер внутреннего буфера пакета |
| `PACKET_DATA_MAX_MTU` | 1500 | Максимальный wire-MTU UDP |
| `KA_PERIOD_MAX_MS` | 10000 | Макс. период keepalive |
| `KA_TIMEOUT_MULT` | 10 | Множитель таймаута = period×10 |
| `ACK_REZERV` | 100 | Резерв байт под ACK |
| `INIT_TIMEOUT_INITIAL` | 500 TB = 50 мс | Начальный интервал повторов INIT |
| `INIT_TIMEOUT_MAX` | 50000 TB = 5 с | Максимальный интервал повторов INIT |
