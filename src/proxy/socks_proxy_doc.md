# socks_proxy

## 1. Назначение
Модуль реализует локальные SOCKS5 CONNECT, HTTP/1.0–1.1 forward proxy и HTTP CONNECT. Разрешает доменное имя назначения и передаёт TCP через ETCP к `tcp_proxy_server` на exit node. Обычный HTTP разбирается до границы каждого запроса и ответа; CONNECT и согласованный Upgrade переходят в сырой туннель.

Файлы:
- `src/proxy/socks_proxy.c` — реализация
- `src/proxy/socks_proxy.h` — интерфейс
- `src/proxy/http_proxy.c/h` — потоковый HTTP codec, независимый от DNS, TCP и ETCP

## 2. Как пользоваться

### Инициализация
```c
struct socks_proxy_conn* conns = NULL;    // связный список активных соединений
int conn_count = 0;                       // счётчик соединений
uint32_t next_stream_id = 0;              // монотонный генератор stream_id
struct listen_ctx* listen = socks_proxy_init_listen(
    ua, "127.0.0.1:1080", &conns, &conn_count, &next_stream_id,
    inst, via_ed, is_http);
```
`is_http=0` — SOCKS5, `is_http=1` — HTTP CONNECT/прокси.

### Приём ETCP-данных от exit node
Внешний dispatcher (`tcp_proxy_client`) проверяет peer/group и вызывает `socks_proxy_handle_etcp()`
при получении пакетов `ETCP_RT_ID_TCP_PROXY_CLIENT` (0x08). Запросы уходят в `ETCP_RT_ID_TCP_PROXY_SERVER` (0x04):
```c
int handled = socks_proxy_handle_etcp(&conns, &conn_count, stream_id, subcmd, data, data_len);
// handled=1 — stream_id найден (включая игнорирование команды), 0 — stream_id не найден
```
Поддерживаемые subcmd: `CONNECTED`, `DATA`, `WINDOW`, `CLOSE`, `ERROR`, `FIN`.

### Поиск соединения
```c
struct socks_proxy_conn* c = socks_proxy_find_conn(conns, stream_id);
// обход связного списка, O(N)
```

### Завершение
```c
socks_proxy_close_listen(ua, listen, &sock_out);  // слушатель
socks_proxy_conn_free_all(&conns, &conn_count);  // все соединения
```
`inst/ua/conns/count/next_stream_id` заимствуются и должны жить до закрытия listener и всех клиентов.
Закрытие listener сохраняет активных клиентов. Вызовы и callbacks выполняются в uasync владельца.
Адрес listener — IPv4:port; порт 0 выбирает ОС, фактический порт возвращает `socks_proxy_get_port()`.
При создании через `tcp_proxy_client_create` каждый включённый listener обязателен:
пустой/неверный адрес или ошибка nonblocking, bind или listen откатывают создание всего клиента. При accept нужны неблокирующий сокет и резерв для отложенного освобождения. Отказ выделения маркера CLOSE использует этот резерв.

### Ключевые нюансы
- **Backpressure:** DATA ждёт CONNECTED и кредита `proxy_flow`. Неотправленные байты сохраняются
  в `tx_buf/tx_len` и отправляются порциями до 4096. При доступном окне ставится router send-ready
  waiter; safety-net 500 мс возобновляет отправку с обычным force=0. Чтение tcp_io ограничено его очередью.
  HUP сохраняет high/low-water и ручную остановку чтения; временно снятая регистрация fd
  восстанавливается после освобождения очереди. Запись не зависит от паузы чтения.
- **Несколько соединений:** управляются через связный список `struct socks_proxy_conn*` + счётчик. В HTTP keep-alive каждое сообщение получает новый upstream stream; клиентский TCP-сокет сохраняется. Pipelining обрабатывается последовательно, включая запросы к разным адресатам.
- **stream_id** генерируется монотонно (`++next_stream_id`), передаётся в заголовке ETCP-пакетов для мультиплексирования.

## 3. API

### Структуры

**`struct socks_proxy_conn`** — одно прокси-соединение:
- `tc` — TCP-соединение с клиентом (`tcp_conn`)
- `stream_id` — уникальный идентификатор потока в ETCP
- `dest_ip[4]`, `dest_port` — целевой адрес (куда хочет подключиться клиент)
- `state` — текущее состояние (SOCKS_STATE_* или HTTP_STATE_*)
- `http` — отдельное состояние HTTP-транзакции и таймер; NULL для SOCKS5
- `next_stream_id` — заимствованный генератор идентификаторов последующих HTTP-транзакций
- `rem_closed`, `fin_remote`, `close_sent`, `close_pending` — флаги состояния закрытия
- `buf[16384]`, `buf_len` — непрочитанный хвост входного блока; HTTP headers хранятся в codec
- `tx_buf`, `tx_len`, `tx_waiter` — бэкпрессур: отложенные данные и waiter handle
- `bytes_to_client`, `bytes_from_exit` — счётчики переданных байт (диагностика)
- `head`, `count` — указатели на голову списка и счётчик (для самоудаления)

**`struct listen_ctx`** (непрозрачный) — контекст слушающего сокета:
- `listen_sock`, `socket_id` — сокет и uasync handle
- `conns`, `conn_count`, `next_stream_id` — общий список соединений

### Основные функции

| Функция | Описание |
|---------|----------|
| `socks_proxy_init_listen(ua, addr, conns, count, next_sid, inst, via_node, is_http)` | Создаёт IPv4 listener, возвращает собственный ctx или NULL. Не регистрирует router callback. |
| `socks_proxy_close_listen(ua, ctx, sock_out)` | Закрывает слушающий сокет, освобождает ctx. |
| `socks_proxy_get_port(ctx)` | Фактический порт listener, 0 при ошибке. |
| `socks_proxy_handle_etcp(head, count, stream_id, subcmd, data, len)` | Диспетчер ETCP-сообщений от exit node. Обрабатывает DATA (→write_queue клиенту), CLOSE/ERROR (→push_close), FIN (→push_fin). |
| `socks_proxy_find_conn(head, stream_id)` | Поиск соединения по stream_id в связном списке. |
| `socks_proxy_conn_free(c)` | Удаляет из списка, отменяет DNS/control retry/DATA timer/waiter, освобождает TCP/HTTP/buffers. Активный HTTP upstream отменяется; pending CLOSE/ERROR получает последнюю попытку. |
| `socks_proxy_conn_free_all(head, count)` | Освобождает все соединения в списке. |

### Внутренние функции

| Функция | Описание |
|---------|----------|
| `send_msg(inst, group_id, dst, subcmd, sid, data, len, force)` | Через общий proxy_send копирует payload и отправляет в TOPO_GROUP_UTUN; group_id аргумент игнорируется, mode=0. |
| `send_connect(c)` | Отправляет CONNECT (dest_ip[4]+dest_port[2]) на exit node. |
| `send_data(c, data, len)` | Отправляет DATA (релей пользовательского трафика). |
| `send_close(c)` | Попытка CLOSE с force=1; pending повторяется при free. ERROR отправляется через send_msg, FIN — через proxy_flow_flush. |
| `write_to_client(c, data, len)` | Запись данных в `tc->write_queue` (отправка клиенту через TCP). |
| `process_socks_greeting(c)` | Парсинг SOCKS5 GREETING: ver=5, выбор метода 0x00 (no auth), ответ 05 00. |
| `process_socks_request(c)` | CONNECT к IPv4 или DNS-имени; IPv6 отклоняется с REP=8. Успешный ответ отправляет socks_connected только после CONNECTED. |
| `http_read(c)`, `http_receive(c)` | Потоковый разбор клиентского запроса и upstream ответа через HTTP codec. |
| `http_finish(c)` | Завершение/отмена upstream, ожидание доставки ответа клиенту и переход к следующему запросу. |
| `http_fail(c)`, `http_deadline(c)` | Ошибки и тайм-ауты: собственный HTTP error до final headers, закрытие оборванного сообщения после них. |
| `on_accept_cb(sock, arg)` | Callback accept: создаёт `socks_proxy_conn`, `tcp_conn_create`, вешает read callback. |
| `on_read_cb(q, arg)` | Callback чтения из TCP: handshake-парсинг или релей данных через ETCP с бэкпрессуром. |
| `tx_waiter_cb(q, arg)` | Callback бэкпрессура: повторная отправка отложенных данных при готовности ETCP. |
| `on_fin_cb`, `on_error_cb`, `on_closed_cb` | FIN ждёт исходящих DATA; ошибка отправляет ERROR, cleanup из TCP callbacks откладывается через call_soon. HTTP вне туннеля использует границы транзакций. |

## 4. Протокол

### SOCKS5 flow (is_http=0)
```
Клиент → uTun:         [ver=5][nmethods][methods...]
uTun → Клиент:         [05][00]                              (GREETING: no auth)
Клиент → uTun:         [05][01][00][atyp][addr...][port]    (CONNECT REQUEST)
uTun → exit (ETCP):    TCP_PROXY CONNECT[dest_ip+port]
exit → uTun (ETCP):    CONNECTED                            (реальный connect завершён)
uTun → Клиент:         [05][00][00][01][0.0.0.0][0]         (ответ ок)
Клиент ↔ uTun ↔ exit:  RELAY данных через ETCP DATA
```

### HTTP CONNECT flow (is_http=1)
```
Клиент → uTun:         CONNECT host:port HTTP/1.1\r\nHost: host:port\r\n\r\n
uTun → exit (ETCP):    TCP_PROXY CONNECT[dest_ip+port]
exit → uTun (ETCP):    CONNECTED
uTun → Клиент:         HTTP/1.1 200 Connection Established\r\n\r\n
Клиент ↔ uTun ↔ exit:  RELAY данных через ETCP DATA
```

### HTTP-прокси (GET/POST, is_http=1)
```
Клиент → uTun:         GET http://host:port/path HTTP/1.1\r\nHost: ...\r\n\r\n
uTun → exit (ETCP):    TCP_PROXY CONNECT[dest_ip+port]
exit → uTun (ETCP):    CONNECTED
uTun → exit (ETCP):    TCP_PROXY DATA[GET /path HTTP/1.1\r\n...]
exit → uTun:           HTTP headers/body через ETCP DATA, затем FIN
uTun → Клиент:         проверенный и реконструированный HTTP response
uTun → exit (ETCP):    CLOSE завершённого stream (после полного ответа, FIN нужен для тела до EOF)
Клиент → uTun:         следующий запрос → новый CONNECT с новым stream_id
```
URL преобразуется в origin-form без потери query; Host строится из absolute URL. Заголовки текущего соединения, включая поля из Connection, и Proxy-Authorization удаляются в обоих направлениях. Framing и Connection формируются заново, добавляется Via. Upstream обычного запроса получает Connection: close; frontend HTTP/1.1 сохраняет keep-alive для ответов с определённой длиной.

Codec поддерживает Content-Length, chunked с extensions/trailers, промежуточные ответы, HEAD/204/304, тела ответа до EOF и подтверждённый 101. Тело передаётся по мере поступления, без накопления целиком. HTTP/1.0 получает декодированное chunked тело и закрытие соединения. Неоднозначный framing (CL+TE, разные CL), obs-fold, неверные порты и управляющие байты отклоняются до отправки заголовков.

Фрагменты ответа дописываются в последний буфер очереди, если в нём достаточно места. После завершения HTTP-транзакции чтение следующего запроса возобновляется и при пустой входной очереди.

ETCP credit возвращается за исходные принятые байты после опустошения клиентской write queue и write_buf. Частичный header/chunk marker остаётся charged до проверки и доставки результата; удалённые trailers также учитываются. Переход к новому stream разрешён только после доставки полного ответа и отправки CLOSE/ERROR старого stream. Синхронная доставка через loopback защищена счётчиком busy и флагом sending.

Лимиты: headers 32 KiB, start line и chunk/trailer line 8 KiB, не более 512 полей. Назначения — IPv4 или DNS-имя с IPv4-результатом; IPv6 отклоняется с 502 из-за формата общего proxy CONNECT. Поддерживаемый transfer coding — chunked.

Тайм-ауты: request/response headers 30 s, DNS/connect 15 s, отсутствие прогресса передачи и idle keep-alive 60 s. Заголовки имеют общий deadline, который не продлевается каждым байтом. Когда ответ с закрытием соединения полностью принят, для записи остатка клиенту отводится 10 с. CONNECT/Upgrade после установки туннеля используют обычный lifecycle TCP. Ошибки и смена транзакций логируются в категории proxy; credentials не выводятся.

ERROR на exit немедленно уничтожает сокет и отложенный upload; CLOSE сохраняет обычное завершение очереди.

SOCKS greeting принимает только предложенный метод 0 (no auth); иначе отправляется 05 FF.
Реализован только CONNECT, без BIND/UDP ASSOCIATE. DNS выполняется на ingress через
асинхронный A-resolver, выбирается первый адрес. Хвост совмещённых greeting/request/data сохраняется.
Ошибка DNS/connect до установления потока даёт SOCKS REP=4 либо HTTP 502.

В сыром туннеле FIN локального клиента ждёт tx_buf и read_queue, но не write_queue
противоположного направления. FIN от exit ставится в write_queue клиента после предыдущих DATA.
При CLOSE или после обмена FIN в обоих направлениях у сокета есть 10 с на запись оставшихся DATA.
Первый FIN сохраняет встречное направление. Повторная команда и частичная запись сохраняют исходный срок.
Если запись завершилась раньше, сокет закрывается сразу. По истечении срока клиент освобождает поток
и записывает объём остатка в лог proxy. После обоих FIN и фактического локального shutdown ставится close-маркер. WINDOW возвращает
кредит после опустошения write_queue и write_buf. Планировщик повторяет управляющие команды при освобождении выходной очереди.
При уничтожении потока ожидающий отправки CLOSE/ERROR переходит в очередь клиента.

## 5. Зависимости
- `proxy_protocol.h` — subcmd, размеры заголовков, proxy_flow и proxy_send
- `etcp_api.h` — `ETCP_RT_ID_TCP_PROXY_SERVER/CLIENT`
- `etcp_router.h` — `etcp_router_on_send_ready()`, `etcp_router_cancel_send_ready()`
- `tcp_io.h` / `tcp_conn` — TCP-соединения (`tcp_conn_create`, `tcp_conn_push_close`, `tcp_conn_push_fin`, write/read очереди)
- `u_async.h` — event loop, `uasync_add_socket_t`
- `ll_queue.h` — очереди (write_queue, read_queue), waiter механизм
- `memory_pool.h` — аллокация из пулов (`entry_pool`, `data_pool`)
- `utun_instance.h` — `struct UTUN_INSTANCE`

Нормативные размеры сообщений и порядок байтов — в [proxy_protocol.md](../../doc/proxy_protocol.md).
Контракты чистого HTTP codec (обнуление состояния, emit/source_len, used и EOF) — в [http_proxy.h](http_proxy.h).
`test_socks_http_proxy` проверяет 16 byte-exact загрузок и двусторонний TLS shutdown.
`UTUN_TEST_TLS_ABRUPT=1` отдельно воспроизводит обрыв TLS: реальные EPIPE/ENOTCONN
остаются ошибками TCP и диагностируются, уже доставленные ответы сохраняются.
