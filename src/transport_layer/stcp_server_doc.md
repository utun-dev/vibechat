# STCP Server — приём и рукопожатие соединений

## 1. Назначение

STCP Server — серверная сторона протокола STCP. Создаёт слушающий TCP-сокет на указанном порту, принимает входящие соединения, выполняет серверную часть X25519-рукопожатия и поднимает зашифрованный канал AES-CTR для обмена сообщениями.

Использует общую структуру `struct stcp_conn` из `stcp.h`.

## 2. Как пользоваться

```c
struct stcp_server *srv = stcp_server_create(ua, port, &my_keys,
    my_ed25519_pubkey, instance,
    on_connect_cb, conn_arg,   // вызывается после успешного рукопожатия
    on_close_cb, close_arg,    // вызывается при закрытии/ошибке
    AF_INET);                 // AF_INET6 — отдельный IPv6-only listener
if (!srv) return -1;

uint16_t actual_port = stcp_server_get_port(srv); // port=0 выбирает ОС

// ... use ...

stcp_server_destroy(srv);
```

После завершения обычного рукопожатия соединение переходит в `STCP_STATE_DATA`, вызывается
`connect_cb(conn, arg)`. Он получает заимствованный conn; память отдельно не освобождается вызывающим.
`tx_queue` принимает сообщения для шифрования с CRC32, `rx_queue` — отдельные копии расшифрованных сообщений.
Все операции выполняются в uasync-потоке. Полный контракт очередей и close находится в `stcp.h`.

Ключевые нюансы:
- Сервер принимает клиентский pubkey+salt (обфусцированный), извлекает pubkey через `sc_obfuscate_pubkey`, выполняет ECDH.
- Отправляет ответ: открытый блок salt/pubkey, зашифрованное тело handshake с CRC32 и зашифрованный padding.
- Handshake, padding и последующие кадры DATA используют непрерывный AES-128-CTR stream каждого направления.
- `server_hs_phase1_cb` разбирает обязательные 91 байт, `server_hs_phase2_cb` принимает padding и формирует ответ.
- Флаг `STCP_HANDSHAKE_FLAG_PING` вызывает ответ и мягкое закрытие пробы без `connect_cb` и DATA-сессии.
- `stcp_server_set_reality` включает внешний REALITY-handshake и relay неавторизованных клиентов; параметры — `reality_config`.

## 3. API

### stcp_server — внутренняя структура
Слушающий сокет, ключи сервера, коллбэки `connect_cb` / `close_cb`. Не экспортируется, создаётся только через `stcp_server_create`.

### Функции
- `stcp_server_create(ua, port, keys, ed25519_pubkey, inst, connect_cb, arg, close_cb, close_arg, family)` —
  создать TCP listener на всех адресах IPv4 или IPv6, `listen(16)`, зарегистрировать accept callback;
  результат — владеющий handle либо NULL. Ключи копируются, `ua/inst` заимствуются до destroy.
- `stcp_server_get_port(srv)` — фактический порт listener, 0 при ошибке.
- `stcp_server_destroy(srv)` — закрыть listener и все принятые conn, включая вызовы их close callbacks, затем освободить сервер.

### Протокол рукопожатия (серверная сторона)
1. Сервер читает `SC_PUBKEY_ENC_SIZE + STCP_HS_ENC_CLIENT` байт (открытый salt/pubkey и 51 байт зашифрованного тела).
2. Деобфусцирует 32-байтный клиентский pubkey через XOR с `SHA256(salt || server_pubkey)`.
3. Выполняет ECDH (`sc_init_ctx` + `sc_set_peer_public_key`) → получает `session_key`, запускает `stream_recv` с клиентской солью. Для ответа генерирует собственную соль и запускает `stream_send` с ней; соль входит в вывод CTR IV вместе с ключом и направлением.
4. Расшифровывает тело, проверяет CRC32 и границы padding_size, затем принимает и расшифровывает padding.
5. Формирует ответ: открытые salt2(8) + обфусцированный pubkey(32), затем зашифрованные 51 байт handshake и нулевой padding случайной длины. Диапазон берётся у существующего ETCP_CONN, иначе у instance. Приём padding продвигает CTR; при длине 0 handshake завершается сразу.
6. Для обычного соединения переходит в `STCP_STATE_DATA` и вызывает `connect_cb`; для PING завершает пробу.

47 байт тела handshake: Ed25519 pubkey(32), got_initial_pkt(1), reset_id(8 BE),
padding_size(2 LE), device_type(1), keepalive_interval(2 BE), flags(1).
CRC32(4 LE) считается по этому телу и шифруется вместе с ним. Размер обязательной
части каждого направления — 40 + 47 + 4 = 91 байт; padding не входит в CRC тела handshake.
`reset_id` — собственная эпоха отправителя, подтверждение новой ETCP-сессии выполняет `etcp_session`.
