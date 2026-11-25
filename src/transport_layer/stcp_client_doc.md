# STCP Client

## 1. Назначение
Клиент устанавливает прямой TCP или SOCKS5 CONNECT, при необходимости выполняет
внешний REALITY-handshake, затем STCP-handshake с X25519-ключами и эпохами ETCP.
Handshake, padding и DATA шифруются AES-128-CTR; сообщения проверяются CRC32.
Все вызовы выполняются в uasync-потоке. Для ETCP используется адаптер `stcp_link`.

## 2. Как пользоваться

### Типовой сценарий (клиент):
```c
void on_ready(struct stcp_conn *conn, void *arg);
void on_close(struct stcp_conn *conn, int err, void *arg);

struct stcp_client *cli = stcp_client_connect(
    ua, addr, port, &my_keys, peer_pubkey_bin,
    my_ed25519_pubkey, 0, NULL, instance->client_type, instance->keepalive_interval, 0,
    on_ready, ready_ctx, NULL, NULL, on_close, close_ctx, NULL, 0, NULL, NULL, instance
);
if (!cli) return -1;
// После handshake — on_ready.
// Соединение доступно через stcp_client_get_conn(cli).
// Tx: пакеты в conn->tx_queue → tx_cb шифрует и отправляет.
// Rx: пакеты попадают в conn->rx_queue.
// Завершение: stcp_client_destroy(cli).
```

`cli` — владеющий handle, `stcp_client_get_conn(cli)` — заимствованный указатель на
встроенный conn. После сетевой ошибки handle остаётся владельцу до destroy;
самостоятельно освобождать conn нельзя. Контракт очередей и закрытия — в `stcp.h`.

### Ключевые нюансы
- **Handshake**: salt(8) + обфусцированный pubkey(32) открыты; далее 47 байт тела и CRC32(4 LE), затем padding.
  Тело: Ed25519 pubkey(32), got_initial_pkt(1), reset_id(8 BE), padding_size(2 LE),
  device_type(1), keepalive_interval(2 BE), flags(1). Обязательная часть — 91 байт в каждом направлении.
  Нулевой padding шифруется продолжением того же CTR-потока, отдельно от CRC тела handshake.
  При наличии ETCP_CONN длина выбирается из его текущего диапазона. Без ETCP_CONN клиент сохраняет
  выбранную длину при запуске; диапазон берётся у существующего пира/instance, без instance выбирается временный.
  Сервер выбирает свою длину независимо. При padding_size=0 приём handshake завершается сразу.
- **Стрим-шифр**: nonce — первые 12 байт `SHA256(session_key || stream_id_LE || salt(8) || "uTun3-stream")`, последние 4 байта CTR IV нулевые. `STCP_STREAM_CLIENT_SEND` использует соль клиента, `STCP_STREAM_SERVER_SEND` — соль сервера. Отправка запускается с собственной солью, приём — после чтения ответа; CTR продолжается через padding и DATA.
- **Фрейминг в DATA**: `len(2 LE)` + данные + `CRC32(4)`. CRC считается от данных, весь кадр вместе с длиной шифруется stream_xor.
- **Очереди**: `tx_queue` обслуживает общий `stcp_tx_queue_cb`; `rx_queue` получает отдельные копии расшифрованных сообщений.
- **Асинхронность**: все операции через `UASYNC` — сокет в неблокирующем режиме. Поддержка частичной отправки (send_offset) и EAGAIN/WOULDBLOCK.
- **Обработка ошибок**: CRC mismatch, несовпадение ожидаемого ключа сервера, ошибки шифрования или сети закрывают conn через `stcp_conn_do_close` и вызывают close_cb.
- **Время и владение**: timeout_ms>0 задаёт интервал подключения/handshake, иначе используется 10 с.
  `inst` нужен только при запуске для выбора padding и временной базы REALITY. ETCP_CONN заимствован для чтения текущих эпохи/флага при handshake.

## 3. API

### Структура `struct stcp_client`
Содержит встроенный `struct stcp_conn`, ожидаемый ключ пира, callbacks, UASYNC и снимки параметров внешних транспортов.

### `stcp_client_connect()`
Возвращает владеющий handle либо NULL при отказе запуска. При прямом TCP разрешает
адрес через getaddrinfo, создаёт неблокирующий сокет, при необходимости привязывает
его к local_addr и проверяет SO_ERROR после connect. При SOCKS5 ждёт завершения CONNECT.
Затем выполняет REALITY, если задан, и STCP: выводит общий ключ и отправляет handshake.
Для обычного DATA нужен ready_cb; для пробы используется ping_cb и флаг PING.

### `stcp_client_destroy()`
Отменяет незавершённый SOCKS5 dial или закрывает conn через `stcp_conn_do_close`.
Освобождение handle после close откладывается через uasync; встроенный conn отдельно не освобождается.
После destroy handle больше не используют. NULL допустим.

### `stcp_client_get_conn()`
Возвращает заимствованный встроенный `struct stcp_conn *`, также до готовности DATA
и после закрытия I/O. Для cli==NULL возвращает NULL.

### Коллбэки
- **`stcp_ready_cb`**: вызывается при успешном handshake (переход в `STCP_STATE_DATA`).
- **`stcp_close_cb`**: вызывается при закрытии или ошибке соединения, получает код ошибки.

`stcp_ping_send` создаёт автоматически освобождаемую пробу. Callback вызывается один
раз с успехом и RTT в 0.1 мс либо с `(0, 0, arg)` при ошибке. Возвращённый handle
можно использовать для ранней отмены через destroy; после завершения пробы он недействителен.

### Внутренние функции
- **`client_derive_session()`** — вычисляет session_key через ECDH. Потоки инициализируются после получения соответствующих солей.
- **`client_send_handshake()`** — формирует и отправляет handshake-пакет (salt + обфусцированный pubkey + зашифрованный padding+CRC + padding).
- **`client_hs_cb()`** — проверяет ожидаемый ключ сервера, расшифровывает тело ответа и проверяет CRC/размер padding.
- **`client_hs_padding_cb()`** — продвигает приёмный CTR через padding, завершает таймер handshake и сообщает DATA либо результат PING.
- **`stcp_recv_try()` / `stcp_recv_set()`** — общий поэтапный приём handshake и DATA.
- **`stcp_frame_encrypt()`** — формирует и шифрует целиком `len(2 LE) + data + CRC32(4 LE)`.
- **`stcp_frame_decrypt()`** — дешифрует тело с CRC без поля длины; используется для handshake.
