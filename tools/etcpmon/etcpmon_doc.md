# etcpmon

## 1. Назначение

Windows GUI монитор для ETCP соединений uTun. Подключается по обычному IPv4 TCP к control_server внутри uTun
и отображает метрики соединений, линков, TUN, роутера и уровней отладки. Все сетевые вызовы и callbacks
выполняются в GUI-потоке: WM_TIMER установлен на 10 мс, автоматический запрос метрик — раз в 100 мс
при отсутствии предыдущего запроса в pending. Частота полученных сэмплов зависит от ответов сервера.

Connections и локальные сокеты опрашиваются раз в секунду, даже при пустом списке соединений.
Выбор сохраняется по ID узла при изменении порядка и имён. При удалении выбранного соединения
выбирается первое доступное; пустой список или отключение очищает метрики и график.
Одновременно ожидается не более одного ответа на автоматический запрос метрик. Таймаут ответа
на запрос с seq_id — 5 секунд; причина отключения записывается в `etcpmon.log`.

## 2. Как пользоваться

### Типовой сценарий

1. В конфиге uTun настроить `[control]` (без `allow` сервер отклоняет все подключения):

   ```ini
   [control]
   ip=127.0.0.1
   port=9090
   allow=127.0.0.1/32
   ```
2. Запустить uTun → он поднимает TCP control server на указанном порту
3. Запустить `etcpmon.exe` (Windows, MinGW/MSYS2 сборка)
4. Ввести числовой IPv4 и порт сервера, нажать Connect. При создании окна уже начинается попытка
   подключения к `192.168.40.1:9091`; для смены адреса сначала нажать Disconnect.
5. Первое доступное соединение выбирается автоматически; другое можно выбрать в Connections.

Канал управления не шифруется и не проверяет ключи ETCP; `allow` фильтрует только IPv4 источника.
Для удалённого управления требуется доверенный защищённый канал. DNS-имена и IPv6 клиент не принимает.

### Что отображает

**ETCP Connection:**
- RTT last / avg10 и jitter (wire: 0.1 мс, GUI: мс), bytes sent, retransmissions, ACKs, unacked bytes, optimal inflight, links count
- Поле RTT Avg100 в окне есть, но соответствующего значения в протоколе нет: оно не заполняется
- RX/TX дубликаты, таймеры (retrans/ack_resp), WaitAck состояние (suspended/cb/timer)
- ID Sequence: next_tx_id, last_rx_id, last_delivered_id, rx_ack_till
- Счётчики ошибок: reinit, reset, pkt_format_errors

**TUN / Routing:**
- Bytes/packets read/written, ошибки чтения/записи
- Routed/dropped пакеты, TUN очереди (InQ/OutQ packets/bytes)
- Таблица маршрутов: total, local, learned, BGP senders, BGP nodes

**Links:**
- Статус (UP/DOWN), encrypt/decrypt/send/recv ошибки
- Total encrypted/decrypted bytes, bandwidth (Kbps), NAT changes
- RTT last, поле с подписью avg10 (фактически BBR minRTT, 0 без BBR), transmit time, keepalive counters (sent/recv)
- Inflight bytes/packets/limit, таймеры (init/keepalive/shaper)

**BBR per link:**
- mode (STARTUP/DRAIN/PROBE_BW/PROBE_RTT), cycle idx, full_bw/loss_in_round флаги
- pacing_rate (bytes/sec), min_rtt (us), bw_hi/bw_lo (bytes/sec), inflight_hi/lo, pacing_gain

**Router Congestion (агрегированные):**
- total_conns, total_inflight, total_send_q, total_recv_q (счётчики записей очередей, не байтов)
- pkts sent/recv, send err, ACK sent/recv, dup_dropped, oob_dropped, stale_ack, sign_fail
- RTT max/avg, jitter max, minRTT min

**Графики BBR (scrolling canvas, 860 сэмплов, 1px=1sample):**
- Inflight Bytes, Cwnd (inflight_lim), minRTT, bwHi, Pacing Rate, bwLo, inflight Hi, BBR Mode
- История первого линка, до 860 сэмплов; если галочек нет, отображаются первые четыре канала
- Вертикальная линия курсора с показом значений в блоках под графиком

**Очереди (Queues & Errors):**
- InQ, InSend, WaitAck, AckQ, RecvQ, OutQ — bytes/packets
- Нормализатор: input/output pkts/bytes, alloc/logic errors, frag_size, data_ptr/size, cumulative totals
- ACK Debug: hit_inf, hit_sndq, miss, link_wait
- Системные ресурсы: active_timeouts, busy_memory_blocks

**Debug Levels (правая колонка):**
- Глобальный уровень: NONE/ERROR/WARN/INFO/DEBUG/TRACE (радиокнопки)
- Per-category уровни с метками категорий
- Radio-кнопки отправляют CMD_SET_DEBUG_CONFIG на сервер при клике

**Action:**
- Текстовое поле + кнопка Send отправляют CMD_ACTION (до 31 байта текста + NUL)
- Сервер распознаёт префиксы `nodes` (текстовый дамп узлов) и `nat` (перезапуск NAT/STUN-проверок)
- `nodes` возвращает текст в отдельное окно Consolas, `nat` — подтверждение запуска проверки
- Имена сравниваются целиком. Неизвестная команда или недоступная служба возвращают `RSP_ERROR`
  с исходным seq; GUI показывает ошибку и сохраняет TCP-соединение

### Сборка

```cmd
cd tools\etcpmon
build.bat          # через MSYS2 UCRT64 (рекомендуется)
```
```bash
bash build.sh      # Unix/MinGW
make               # MinGW Makefile
make check         # WinAPI/Winsock регрессии списка, выбора и переподключения
```

На Linux с MinGW и Wine из каталога `tools/etcpmon`:

```bash
CC=x86_64-w64-mingw32-gcc bash build.sh
make CC=x86_64-w64-mingw32-gcc TEST_RUNNER=wine check
wine etcpmon.exe
```

Серверная часть (control_server) встроена в utun, собирается стандартным `./configure && make`.

### Горячие клавиши

- Tab в поле Server/Port — переключение между полями
- Enter в поле Server/Port — Connect, если отключён; Escape — Disconnect
- Enter в поле Action — отправка команды

## 3. API / Структура

### Компоненты

| Файл | Назначение |
|------|-----------|
| `etcpmon_main.c` | WinMain: инициализация GUI, создание окна, message loop |
| `etcpmon_client.h/c` | TCP клиент (Winsock2): connect, send/recv, парсинг протокола, history, pending requests с коллбэками |
| `etcpmon_gui.h/c` | WinAPI GUI: заданный размер окна 1250×1400, контролы, таймер, callbacks, тултипы |
| `etcpmon_graph.h/c` | Real-time график: 8 каналов, off-screen буфер, пересчёт min/max, курсор, обновление channel values |
| `etcpmon_protocol.h` | Бинарный протокол, общие структуры, shared между клиентом и сервером |

### Протокол (бинарный, TCP)

Формат: `[size:2][type:1][seq_id:1][payload...]`

`size` включает 4-байтовый заголовок, диапазон 4..32768. Все структуры packed(1), целые передаются
в порядке байтов хоста. Константа VERSION=3 не передаётся по сети: клиенту и серверу нужны одинаковые
форматы структур и порядок байтов. TCP-чтение может содержать часть кадра или несколько кадров.
`seq=0` не создаёт ожидания в клиенте; `seq=1..255` сопоставляет ответ с pending по одному номеру.
SELECT_CONN, SET_DEBUG_CONFIG и DISCONNECT не подтверждаются. GUI отправляет списки с seq=0,
а GET_DEBUG_CONFIG, автоопрос метрик и Action — с ненулевым seq и таймаутом.

**Клиент → Сервер:**
- `0x01` CMD_LIST_CONN — запрос списка соединений (без payload)
- `0x02` CMD_SELECT_CONN — выбор соединения: `peer_node_id` (uint64_t)
- `0x03` CMD_GET_METRICS — запрос метрик для выбранного соединения
- `0x04` CMD_DISCONNECT — отключение
- `0x05` CMD_LIST_SOCKETS — запрос списка локальных сокетов
- `0x06` CMD_ACTION — текстовая команда (32 байта)
- `0x07` CMD_GET_DEBUG_CONFIG — запрос текущих debug levels
- `0x08` CMD_SET_DEBUG_CONFIG — установка debug levels: `[global_level:1][cat_count:1][levels:N]`
- `0x09` CMD_SUBSCRIBE_NODES — подписка на изменения узлов (topo)

**Сервер → Клиент:**
- `0x81` RSP_CONN_LIST: `count(1) + N × {peer_node_id(8) + name(32)}`
- `0x82` RSP_METRICS: `etcp_metrics + tun_metrics + router_metrics + N × link_metrics`
- `0x83` RSP_SOCKET_LIST: `count(1) + N × socket_info`
- `0x84` RSP_DEBUG_CONFIG: `global_level(1) + cat_count(1) + names_csv(NUL-term) + levels[N]`
- `0x85` RSP_NODE_INFO — префикс `01 04` + `topo_node_serialize()` для UTUN с подсетями, seq=0
- `0x86` RSP_NODE_INFO_END — конец списка узлов с seq запроса подписки
- `0x88` RSP_NODE_REMOVED — узел удалён: `node_id(8)`
- `0x89` RSP_ACTION_RESULT — результат команды: `code(1) + text(NUL-term)`
- `0xFF` RSP_ERROR: `error_code(1) + message(NUL-term)`

Подписку на узлы поддерживает сервер, но GUI etcpmon её не запрашивает и эти уведомления не отображает.

### Очереди, таймауты и единицы

- Все команды клиента копируются в общий TX FIFO (до 64 КиБ/256 кадров). Short send и WSAEWOULDBLOCK
  сохраняют смещение для следующего прохода. Переполнение не принимает новую команду и возвращает ошибку.
- Pending регистрируется вместе с кадром, срок ответа 5 секунд начинается после его полной отправки.
  Тип и payload ответа проверяются до снятия pending/callback; `RSP_ERROR` завершает любой запрос.
- Connect и отсутствие прогресса TX также ограничены 5 секундами. Ответы на списки с seq=0 отдельно
  не отслеживаются; EOF/фатальная ошибка/таймаут закрывают соединение с диагностикой.
- Сервер держит один готовый кадр до 32 КиБ. Чтение и разбор запросов ждут его отправки через
  отменяемый deferred waiter. Для начального списка узлов хранятся ID, сериализация выполняется порционно.
- Live-изменения UTUN объединяются по ID (до 256); переполнение отключает подписчика с WARN.
  Серверный таймаут 30 секунд во время TX продлевается только положительным send, иначе — командами.
- История хранит байты, байты/с и minRTT в мс. При выводе используются B/KiB/MiB и B/s/KiB/s/MiB/s,
  включая строки BBR. Сравнение идёт с текстом самого контрола, поэтому очистка/reconnect не скрывают прежние значения.
