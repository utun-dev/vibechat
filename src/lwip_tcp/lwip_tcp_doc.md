# lwip_tcp

## 1. Назначение

Встроенный lwIP TCP стек, адаптированный для uTun. Полноценная реализация RFC 793 (TCP) с минимальным набором опций (только MSS), context-based архитектурой, single-threaded исполнением и интеграцией с event loop uasync. Не использует глобальных переменных lwIP — весь контекст изолирован в `struct lwip_tcp_ctx` (один экземпляр на TCP-прокси).

Модуль состоит из 4 файлов:
- `lwip_tcp.c` — ядро TCP: жизненный цикл, таймеры, выделение PCB, управление состояниями
- `lwip_tcp_out.c` — отправка сегментов: `tcp_write`, `tcp_output`, `tcp_enqueue_flags`, ретрансмиссии, RST, ACK, zero-window probe
- `lwip_tcp_in.c` — приём сегментов: `lwip_tcp_input` → разбор заголовка, маршрутизация по PCB, `tcp_process`/`tcp_receive`, OOSEQ, ACK-обработка
- `lwip_pbuf.c` — упрощённый packet buffer (PBUF_RAM), используемый как хранилище данных сегментов
- `lwip_tcp_opts.h` — жёстко заданные константы конфигурации
- `lwip_tcp_priv.h` — внутренний API: wire-формат TCP заголовка, структура сегмента, макросы событий, функции для `_in` и `_out`

Ключевые свойства:
- **Context-based:** весь TCP-стек инкапсулирован в `struct lwip_tcp_ctx`, никаких глобальных переменных
- **Single-threaded:** все операции в одном потоке, управляемом через `uasync`
- **uasync-driven:** таймер `tcp_tmr_cb` каждые `tmr_interval_ms` (по умолчанию 62.5ms = TCP_TMR_INTERVAL/4, ускоритель) вызывает `tcp_fasttmr` и каждый нечётный вызов — `tcp_slowtmr`
- **Output callback:** `ctx->output(pbuf, src_ip, dst_ip)` — вызывается когда TCP хочет отправить IP-пакет. Пользователь сам реализует инкапсуляцию в IP-заголовок и передачу
- **Memory pools:** пулы для `tcp_pcb`, `tcp_pcb_listen`, `tcp_seg` (выделение за O(1) из предварительно аллоцированной памяти)
- **Trace ring buffer:** 384 записи для отладки поведения TCP (поддерживает дамп)

## 2. Как пользоваться

### Типовой сценарий работы

```
lwip_tcp_init(ua, output_fn, arg)   — инициализация контекста
  └→ tcp_new(ctx)                   — создание TCP PCB
     └→ tcp_bind(pcb, ip, port)     — бинд локального адреса/порта
        └→ tcp_listen(pcb)          — перевод в LISTEN (сервер)
        └→ tcp_connect(pcb, ip, port, cb)  — исходящее соединение (клиент)

lwip_tcp_input(ctx, pbuf, src_ip, dst_ip) — подача входящего IP-пакета
  → разбор TCP-заголовка, диспетчеризация по активным PCB

tcp_write(pcb, data, len, flags)    — отправка данных
  → данные попадают в pcb->unsent (очередь на отправку)

tcp_output(pcb)                     — отправка unsent сегментов через output callback
  → соблюдение окна перегрузки (cwnd) и окна приёмника (snd_wnd)
  → алгоритм Nagle (можно отключить через tcp_nagle_disable)

tcp_close(pcb) / tcp_abort(pcb)     — закрытие соединения
lwip_tcp_destroy(ctx)               — полная очистка контекста
```

### Таймеры

При создании контекста lwIP резервирует таймер и его место в heap. Ошибка резервирования завершает инициализацию.
Каждые `tmr_interval_ms` обработчик `tcp_tmr_cb` выставляет следующий срок в той же записи и запускает обработку TCP.

- **Fast timer** (`tcp_fasttmr`): каждый вызов. Delayed ACK (250ms), close-pending retry, повторная подача refused_data
- **Slow timer** (`tcp_slowtmr`): каждый нечётный вызов (каждые 2×tmr_interval_ms = 125ms по умолчанию). Ретрансмиссия (RTO, backoff), persist timer (zero-window probe), таймауты FIN_WAIT_2/SYN_RCVD/LAST_ACK, очистка TIME_WAIT, OOSEQ timeout, poll-события

`lwip_tcp_set_timer(ctx, interval_ms, rto_min_ms, rto_max_ms)` позволяет настроить интервал и лимиты RTO.

### Ключевые концепции

**4 списка PCB в контексте:**
- `bound_pcbs` — привязанные к порту, но без активного соединения (CLOSED после bind)
- `listen_pcbs` — слушающие (LISTEN), тип `tcp_pcb_listen` (облегчённая структура)
- `active_pcbs` — активные соединения (SYN_SENT..LAST_ACK)
- `tw_pcbs` — TIME_WAIT (ожидание 2×MSL)

**10 TCP состояний (enum tcp_state):**
```
CLOSED → LISTEN (bind + listen)
CLOSED → SYN_SENT → SYN_RCVD → ESTABLISHED → FIN_WAIT_1 → FIN_WAIT_2 → TIME_WAIT → CLOSED
                                             → CLOSE_WAIT → LAST_ACK → CLOSED
                           → CLOSING → TIME_WAIT → CLOSED
```

**Send buffer и очереди сегментов:**
- `snd_buf` — максимальный объём данных, который можно принять от приложения через `tcp_write` (16×MSS = 23360 байт)
- `unsent` — очередь сегментов, готовых к отправке (не отправлены)
- `unacked` — отправленные, но ещё не подтверждённые сегменты
- `ooseq` — out-of-order сегменты, ожидающие сборки
- `snd_queuelen` — количество pbuf'ов в unsent (лимит: 64 = 4×SND_BUF/MSS)

**RTT estimation (Van Jacobson):**
- `rttest` — время отправки сегмента для текущего замера RTT
- `sa`, `sv` — сглаженное среднее RTT и вариация (scaled, /8 и /4)
- `rto` — retransmission timeout (в единицах slow timer = 250ms)
- `rtime` — счётчик тиков с момента последней отправки

**Алгоритм Nagle:**
- Включён по умолчанию (`TF_NODELAY` не установлен)
- Объединяет маленькие сегменты, если есть неподтверждённые данные
- Отключается: `tcp_nagle_disable(pcb)` → устанавливает `TF_NODELAY`

**Congestion control (NewReno-like):**
- `cwnd` — congestion window, инициализируется 1 сегментом (slow start)
- `ssthresh` — порог slow start/congestion avoidance
- `dupacks` — счётчик дублирующихся ACK (fast retransmit при 3)
- `TF_INFR` — флаг «в процессе fast recovery»

**TCP опции:** поддерживается только MSS (Maximum Segment Size). Window Scale, Timestamps, SACK отключены.

### Приём данных из сети

Внешний код получает IP-пакет (после IP-заголовка), формирует `struct pbuf*` с TCP-сегментом и вызывает:
```c
lwip_tcp_input(ctx, p, src_ip, dst_ip);
```
Функция разбирает TCP-заголовок, находит PCB по (src_ip, src_port, dst_ip, dst_port) и передаёт управление `tcp_process()` / `tcp_receive()`. При получении данных вызывается callback `pcb->recv(arg, pcb, pbuf, err)`.

### Отправка данных в сеть

TCP вызывает `ctx->output(arg, pbuf, src_ip, dst_ip)` когда нужно отправить IP-пакет. Пользователь реализует:
1. Предварительное выделение места под IP-заголовок (pbuf_header)
2. Заполнение IP-заголовка
3. Расчёт IP checksum (`tcp_ip_checksum`)
4. Отправку через транспортный уровень (ETCP, raw socket и т.п.)

### Memory pools

Контекст создаёт 3 memory pool'а:
- `pcb_pool` — `sizeof(tcp_pcb)` элементов для активных соединений
- `pcb_listen_pool` — `sizeof(tcp_pcb_listen)` для слушающих PCB
- `seg_pool` — `sizeof(tcp_seg)` для сегментов

При нехватке PCB в пуле (`tcp_alloc`) последовательно пытается освободить память:
1. Обработать pending close (`tcp_handle_closepend`)
2. Убить самый старый TIME_WAIT PCB (`tcp_kill_timewait`)
3. Убить LAST_ACK PCB (`tcp_kill_state`)
4. Убить CLOSING PCB (`tcp_kill_state`)
5. Убить наименее приоритетный активный PCB (`tcp_kill_prio`)

### Callbacks (на PCB)

| Callback | Тип | Когда вызывается |
|----------|-----|-----------------|
| `recv` | `tcp_recv_fn(arg, pcb, pbuf, err)` | Получены данные (pbuf ≠ NULL) или соединение закрыто (pbuf == NULL, err == LERR_OK) |
| `sent` | `tcp_sent_fn(arg, pcb, len)` | Данные подтверждены, освободилось место в snd_buf |
| `connected` | `tcp_connected_fn(arg, pcb, err)` | Завершено трёхстороннее рукопожатие (клиент) |
| `accept` | `tcp_accept_fn(arg, new_pcb, err)` | Входящее соединение (на listen_pcb) |
| `err` | `tcp_err_fn(arg, err)` | Асинхронная ошибка (RST, abort) |
| `poll` | `tcp_poll_fn(arg, pcb)` | Периодический опрос (интервал задаётся при `tcp_poll`) |

### Диагностика

- **Trace ring buffer**: `lwip_tcp_trace_record()` записывает кольцевые события (384 записи). `lwip_tcp_trace_dump()` выводит в консоль. Типы событий: 'S' (send), 'A' (ack), 'T' (timeout), 'R' (rexmit), 'F' (fast rexmit), 'K' (kill), 'X' (segment), 'D' (drop), 'L' (loss), 'P' (probe), 'M' (tmr), 'C' (close)
- **Stats**: счётчики ошибок выделения памяти (`pbuf_fails`, `seg_fails`, `pcb_fails`, `write_fails`, `enq_fails`, `rst_fails`, `ack_fails`, `probe_fails`, `split_fails`). `lwip_tcp_stats_dump()` / `lwip_tcp_stats_clear()`
- **Глобальный hook**: `g_tcp_trace` — функция обратного вызова для внешнего мониторинга (используется в тестах)

## 3. API

### Жизненный цикл контекста

| Функция | Описание |
|---------|----------|
| `lwip_tcp_init(ua, output, output_arg)` | Создать TCP-контекст: пулы памяти (pcb, pcb_listen, seg), запустить таймер. Возвращает `lwip_tcp_ctx*` или NULL при ошибке |
| `lwip_tcp_destroy(ctx)` | Уничтожить контекст: остановить таймер, освободить все PCB (через abort/close), уничтожить пулы |
| `lwip_tcp_set_timer(ctx, interval_ms, rto_min_ms, rto_max_ms)` | Перенастроить интервал таймера (мс) и границы RTO. rto_max_ms=0 — без ограничения |

### Входной трафик

| Функция | Описание |
|---------|----------|
| `lwip_tcp_input(ctx, p, src_ip, dst_ip)` | Подать входящий IP-пакет (после IP-заголовка). pbuf освобождается внутри. src_ip/dst_ip — host byte order |

### Управление PCB

| Функция | Описание |
|---------|----------|
| `tcp_new(ctx)` | Создать новый TCP PCB с приоритетом NORMAL (64) |
| `tcp_alloc(ctx, prio)` | Создать PCB с заданным приоритетом (1..127). При нехватке памяти агрессивно освобождает TIME_WAIT/LAST_ACK/CLOSING/низкоприоритетные PCB |
| `tcp_bind(pcb, ip, port)` | Привязать локальный адрес/порт. port=0 — автоназначение из диапазона [0xC000..0xFFFF]. Переводит в bound_pcbs |
| `tcp_listen(pcb)` | Перевести CLOSED→LISTEN. Старый PCB освобождается, создаётся `tcp_pcb_listen` |
| `tcp_connect(pcb, ip, port, connected)` | Инициировать исходящее соединение. Отправляет SYN, переводит в SYN_SENT. Вызывает `connected` при успехе |
| `tcp_close(pcb)` | Активное закрытие: устанавливает TF_RXCLOSED, отправляет FIN (SYN_RCVD/ESTABLISHED→FIN_WAIT_1, CLOSE_WAIT→LAST_ACK). LISTEN: освобождает PCB и потомков |
| `tcp_shutdown(pcb, shut_rx, shut_tx)` | Частичное закрытие. shut_rx: перестать принимать, shut_tx: отправить FIN |
| `tcp_abort(pcb)` | Принудительный разрыв: отправляет RST, освобождает все очереди, вызывает errf |
| `tcp_abandon(pcb, reset)` | Подобен abort, но reset=0 не отправляет RST |
| `tcp_recved(pcb, len)` | Уведомить TCP что приложение обработало len байт, увеличить rcv_wnd. При достаточном росте окна отправляет ACK |

### Отправка данных

| Функция | Описание |
|---------|----------|
| `tcp_write(pcb, data, len, apiflags)` | Записать данные в send buffer. apiflags: `TCP_WRITE_FLAG_COPY` (копировать данные), `TCP_WRITE_FLAG_MORE` (будут ещё данные). При ошибке памяти выставляет TF_NAGLEMEMERR |
| `tcp_output(pcb)` | Отправить все готовые сегменты из unsent. Соблюдает cwnd и snd_wnd. При установленном TF_ACK_NOW сначала отправляет ACK. Копирует данные из input_pcb (если в процессе обработки входа) |
| `tcp_send_empty_ack(pcb)` | Отправить чистый ACK (без данных) |
| `tcp_send_fin(pcb)` | Добавить FIN к последнему unsent-сегменту или создать отдельный сегмент |
| `tcp_enqueue_flags(pcb, flags)` | Поставить в очередь сегмент с флагами (SYN/FIN) и MSS-опцией |

### Ретрансмиссия

| Функция | Описание |
|---------|----------|
| `tcp_rexmit_rto_prepare(pcb)` | Подготовка к RTO-ретрансмиссии: переместить все unacked→unsent. Выставляет TF_RTO |
| `tcp_rexmit_rto_commit(pcb)` | Завершить RTO-ретрансмиссию: увеличить nrtx, записать trace, вызвать tcp_output |
| `tcp_rexmit_rto(pcb)` | Полный RTO-цикл: prepare + commit |
| `tcp_rexmit(pcb)` | Ретрансмиссия самого старого unacked сегмента (вставить обратно в unsent по порядку seqno) |
| `tcp_rexmit_fast(pcb)` | Fast retransmit (при 3 dupacks): переотправить первый unacked, уменьшить ssthresh/сwnd, выставить TF_INFR |
| `tcp_rexmit_seg(pcb, seg)` | Ретрансмит конкретного сегмента (из ooseq/unacked) |
| `tcp_zero_window_probe(pcb)` | Отправить 1-байтовый probe при нулевом окне приёмника (persist timer) |
| `tcp_split_unsent_seg(pcb, split)` | Разделить первый unsent сегмент на две части (для zero-window probe) |

### Внутренние (объявлены в priv.h)

| Функция | Описание |
|---------|----------|
| `tcp_fasttmr(ctx)` | Fast timer: обработка delayed ACK, close-pending, refused_data |
| `tcp_slowtmr(ctx)` | Slow timer: ретрансмиссия (RTO/backoff), persist timer, таймауты FIN_WAIT_2/SYN_RCVD/LAST_ACK, OOSEQ cleanup, poll, TIME_WAIT cleanup |
| `tcp_txnow(ctx)` | Экстренная отправка для PCB с флагом TF_NAGLEMEMERR |
| `tcp_rst(pcb, seqno, ackno, local_ip, remote_ip, local_port, remote_port)` | Отправить RST-сегмент |
| `tcp_process_refused_data(pcb)` | Повторно подать refused_data в callback recv |
| `tcp_pcb_purge(pcb)` | Очистить все очереди сегментов (unsent, unacked, ooseq, refused_data) |
| `tcp_pcb_remove(pcblist, pcb)` | Удалить PCB из списка, очистить, перевести в CLOSED |
| `tcp_seg_copy(seg)` | Копия сегмента с ref на pbuf (stub — используйте `tcp_seg_copy_with_ctx`) |
| `tcp_seg_copy_with_ctx(ctx, seg)` | Копия сегмента с явным контекстом для доступа к seg_pool |
| `tcp_segs_free(seg)` / `tcp_seg_free(seg)` | Освобождение цепочки/одного сегмента (stub — используйте локальные версии) |
| `tcp_free_ooseq(pcb)` | Очистить OOSEQ-очередь |
| `tcp_free(pcb)` | Освободить PCB в соответствующий пул (pcb_pool или pcb_listen_pool) |
| `tcp_next_iss(pcb)` | Сгенерировать Initial Sequence Number |
| `tcp_checksum(data, len)` | Расчёт TCP checksum |
| `tcp_ip_checksum(iph)` | Расчёт IP header checksum |

### Callback setters

| Функция | Описание |
|---------|----------|
| `tcp_arg(pcb, arg)` | Установить пользовательский аргумент для callbacks |
| `tcp_recv(pcb, recv)` | Установить callback приёма данных |
| `tcp_sent(pcb, sent)` | Установить callback подтверждения отправки |
| `tcp_err(pcb, errf)` | Установить callback асинхронной ошибки |
| `tcp_accept(pcb, accept)` | Установить callback входящего соединения (только для LISTEN) |
| `tcp_poll(pcb, poll, interval)` | Установить периодический опрос (интервал в единицах slow timer ~500ms) |

### Макросы-хелперы

| Макрос | Описание |
|--------|---------|
| `tcp_mss(pcb)` | Текущий MSS соединения |
| `tcp_sndbuf(pcb)` | Доступное место в send buffer |
| `tcp_sndqueuelen(pcb)` | Количество pbuf в очереди отправки |
| `tcp_nagle_disable(pcb)` / `tcp_nagle_enable(pcb)` | Отключить/включить алгоритм Nagle |
| `tcp_nagle_disabled(pcb)` | Проверить, отключён ли Nagle |
| `tcp_set_flags(pcb, f)` / `tcp_clear_flags(pcb, f)` / `tcp_is_flag_set(pcb, f)` | Операции с флагами PCB |

### Trace и диагностика

| Функция | Описание |
|---------|----------|
| `lwip_tcp_trace_record(ctx, event, seq, len, wnd, rto, rtime, state)` | Записать событие в ring buffer |
| `lwip_tcp_trace_dump(ctx)` | Вывести последние 50 событий trace в stdout |
| `lwip_tcp_trace_clear(ctx)` | Очистить trace buffer |
| `lwip_tcp_stats_dump(ctx)` | Вывести счётчики ошибок в stdout |
| `lwip_tcp_stats_clear(ctx)` | Сбросить счётчики ошибок |
| `tcp_debug_state_str(s)` | Получить строковое имя состояния |

## 4. Константы (lwip_tcp_opts.h)

### Размеры и буферы

| Константа | Значение | Описание |
|-----------|----------|----------|
| `TCP_MSS` | 1460 | Maximum Segment Size (MTU 1500 − IP(20) − TCP(20)) |
| `TCP_SND_BUF` | `16 * TCP_MSS` = 23360 | Send buffer (макс. данные от приложения) |
| `TCP_WND` | `8 * TCP_MSS` = 11680 | Receive window (объявляемый размер окна) |
| `TCP_SND_QUEUELEN` | `4 * TCP_SND_BUF / TCP_MSS` = 64 | Макс. количество pbuf в unsent |
| `TCP_SNDLOWAT` | `min(2*MSS, SND_BUF/2)` = 2920 | Нижняя граница snd_buf для callback sent |
| `INITIAL_MSS` | 536 | Начальный MSS (RFC 879) до согласования опций |

### Таймеры и таймауты

| Константа | Значение | Описание |
|-----------|----------|----------|
| `TCP_TMR_INTERVAL` | 250 ms | Базовый интервал таймера |
| `TCP_FAST_INTERVAL` | 250 ms | Fast timer interval |
| `TCP_SLOW_INTERVAL` | 500 ms (2×base) | Slow timer interval |
| `TCP_RTO_MIN_MS` | 3000 ms | Начальный RTO |
| `TCP_FIN_WAIT_TIMEOUT` | 20000 ms | Таймаут FIN_WAIT_2 (если установлен TF_RXCLOSED) |
| `TCP_SYN_RCVD_TIMEOUT` | 20000 ms | Таймаут SYN_RCVD |
| `TCP_MSL` | 60000 ms | Maximum Segment Lifetime (TIME_WAIT = 2×MSL = 120s) |
| `TCP_OOSEQ_TIMEOUT` | 6 × RTO | Таймаут очистки OOSEQ-очереди |

### Ретрансмиссия

| Константа | Значение | Описание |
|-----------|----------|----------|
| `TCP_MAXRTX` | 12 | Макс. количество ретрансмиссий (после — разрыв) |
| `TCP_SYNMAXRTX` | 6 | Макс. SYN-ретрансмиссий |
| `tcp_backoff[]` | `{1,2,3,4,5,6,7,7,7,7,7,7,7}` | Коэффициенты backoff для RTO (2^n, capped at 7) |
| `tcp_persist_backoff[]` | `{3,6,12,24,48,96,120}` | Backoff для persist timer (секунды, затем 2 минуты) |

### Keepalive (отключён)

| Константа | Значение |
|-----------|----------|
| `TCP_KEEPIDLE_DEFAULT` | 7200000 ms (2 часа) |
| `TCP_KEEPINTVL_DEFAULT` | 75000 ms |
| `TCP_KEEPCNT_DEFAULT` | 9 |

### Порты

| Константа | Значение | Описание |
|-----------|----------|----------|
| `TCP_LOCAL_PORT_RANGE_START` | 0xC000 (49152) | Начало диапазона эфемерных портов |
| `TCP_LOCAL_PORT_RANGE_END` | 0xFFFF (65535) | Конец диапазона |

### Приоритеты

| Константа | Значение |
|-----------|----------|
| `TCP_PRIO_MIN` | 1 |
| `TCP_PRIO_NORMAL` | 64 |
| `TCP_PRIO_MAX` | 127 |

При нехватке PCB в пуле, `tcp_alloc` убивает наименее приоритетные активные соединения. Низкий приоритет = ближе к `TCP_PRIO_MAX`.

### Отключённые фичи

| Константа | Значение | Комментарий |
|-----------|----------|-------------|
| `TCP_WND_SCALE` | 0 | Window scale option (не нужен в пределах малых окон) |
| `TCP_TIMESTAMPS` | 0 | TCP timestamps option |
| `TCP_SACK_OUT` | 0 | Selective ACK |
| `TCP_QUEUE_OOSEQ` | 1 | OOSEQ включён |
| `TCP_OVERSIZE` | 0 | Без pre-allocation размера pbuf |
| `TCP_LISTEN_BACKLOG` | 0 | Без backlog (ждём пока приложение заберёт) |
| `TCP_KEEPALIVE` | 0 | Keepalive отключён |
| `TCP_CHECKSUM_ON_COPY` | 0 | Без checksum-on-copy оптимизации |
| `TCP_CALCULATE_EFF_SEND_MSS` | 0 | Без расчёта эффективного MSS |

## 5. Структуры данных

### `struct lwip_tcp_ctx`

Основной контекст TCP-стека (один на инстанс TCP-прокси):

| Поле | Тип | Назначение |
|------|-----|-----------|
| `ua` | `UASYNC*` | Event loop для таймера |
| `output` | `tcp_output_fn` | Callback отправки IP-пакета |
| `output_arg` | `void*` | Аргумент для output callback |
| `timer` | `struct timeout_node*` | Резерв таймера uasync до уничтожения контекста |
| `bound_pcbs` | `tcp_pcb*` | Список PCB в CLOSED после bind |
| `listen_pcbs` | `tcp_pcb*` | Список слушающих PCB (LISTEN) |
| `active_pcbs` | `tcp_pcb*` | Список активных соединений |
| `tw_pcbs` | `tcp_pcb*` | Список TIME_WAIT |
| `pcb_pool` | `memory_pool*` | Пул для `tcp_pcb` |
| `pcb_listen_pool` | `memory_pool*` | Пул для `tcp_pcb_listen` |
| `seg_pool` | `memory_pool*` | Пул для `tcp_seg` |
| `tmr_interval_ms` | `uint16_t` | Интервал таймера (по умолчанию 62.5ms) |
| `rto_min_ms` | `uint16_t` | Минимальный RTO (по умолчанию 3000) |
| `rto_max_ms` | `uint16_t` | Максимальный RTO (0=без ограничений) |
| `ticks` | `uint32_t` | Счётчик slow timer тиков |
| `slowtmr_ctr` | `uint8_t` | Флаг защиты от двойной обработки PCB |
| `tmr_phase` | `uint8_t` | Фаза (чёт-нечет) для чередования slow/fast |
| `iss_seed` | `uint16_t` | Сид для генерации ISN |
| `ip_id` | `uint16_t` | Счётчик IP ID (для IP-заголовка) |
| `port_seed` | `uint16_t` | Сид для автоназначения портов |
| `trace_id` | `char` | Идентификатор контекста для trace ('A'/'B') |
| `trace` | `tcp_trace_buf` | Кольцевой буфер отладки (384 записи) |
| `stats` | struct | Счётчики ошибок (pbuf, seg, pcb, write, enq, rst, ack, probe, split fails) |

### `struct tcp_pcb`

Protocol Control Block для активного соединения:

| Группа | Поля | Назначение |
|--------|------|-----------|
| **Идентификация** | `local_ip/port`, `remote_ip/port`, `ttl`, `tos` | Сетевые адреса |
| **Состояние** | `state`, `prio`, `flags` | TCP состояние, приоритет, флаги (TF_*) |
| **Таймеры** | `polltmr/intervall`, `last_timer`, `tmr`, `rtime` | Poll-опрос, защита от двойного вызова, timestamp, RTO-счётчик |
| **Приём** | `rcv_nxt`, `rcv_wnd`, `rcv_ann_wnd`, `rcv_ann_right_edge` | Следующий ожидаемый seq, окно приёма, объявленное окно и правый край |
| **RTT** | `rttest`, `rtseq`, `sa`, `sv`, `rto`, `nrtx` | Замер RTT, сглаженное среднее/вариация, timeout, счётчик ретрансмиссий |
| **Duplicate ACK** | `dupacks`, `lastack` | Счётчик дублирующих ACK, последний ACK'нутый seq |
| **Congestion** | `cwnd`, `ssthresh`, `rto_end`, `bytes_acked` | Congestion window, порог slow start, конец RTO-окна, подтверждённые байты |
| **Отправка** | `snd_nxt`, `snd_wl1/2`, `snd_lbb`, `snd_wnd/max` | Следующий seqno, окно отправки, last byte buffered, окно приёмника |
| **Буфер** | `snd_buf`, `snd_queuelen` | Доступно для записи, длина очереди |
| **Сегменты** | `unsent`, `unacked`, `ooseq` | Очереди исходящих/подтверждённых/out-of-order сегментов |
| **Данные** | `refused_data` | Отложенные данные (когда приложение не готово) |
| **Callbacks** | `sent`, `recv`, `connected`, `pollfn`, `errf` | Функции обратного вызова |
| **Keepalive** | `keep_idle` | Интервал idle до первого keepalive probe (отключён) |
| **Persist** | `persist_cnt/backoff/probe` | Счётчик, backoff множитель, счётчик probe |
| **Размер** | `mss` | Согласованный MSS (начальный 536) |

### `struct tcp_pcb_listen`

Упрощённый PCB для LISTEN (не содержит полей данных/сегментов/таймеров):

| Поле | Тип | Назначение |
|------|-----|-----------|
| `next` | `tcp_pcb*` | Указатель на следующий в списке |
| `ctx` | `lwip_tcp_ctx*` | Контекст |
| `callback_arg` | `void*` | Пользовательский аргумент |
| `state` | `tcp_state` | Всегда LISTEN |
| `prio` | `uint8_t` | Приоритет |
| `local_port` | `uint16_t` | Локальный порт |
| `local_ip` | `uint32_t` | Локальный IP (0 = ANY) |
| `accept` | `tcp_accept_fn` | Callback входящего соединения |

### `struct tcp_seg`

Узел очереди сегментов:

| Поле | Тип | Назначение |
|------|-----|-----------|
| `next` | `tcp_seg*` | Следующий сегмент в списке |
| `p` | `pbuf*` | Пакетный буфер с TCP-заголовком и данными |
| `len` | `uint16_t` | Длина данных в сегменте |
| `flags` | `uint8_t` | Флаги опций (TF_SEG_OPTS_MSS) |
| `tcphdr` | `tcp_hdr*` | Указатель на TCP-заголовок внутри pbuf |

Макрос `TCP_TCPLEN(seg)` = `seg->len + (FIN или SYN ? 1 : 0)` — занимаемое место в sequence space.

### `struct tcp_hdr`

Wire-формат TCP-заголовка (20 байт, packed):

| Поле | Размер | Описание |
|------|--------|----------|
| `src` | `uint16_t` | Source port |
| `dest` | `uint16_t` | Destination port |
| `seqno` | `uint32_t` | Sequence number (network byte order) |
| `ackno` | `uint32_t` | Acknowledgement number |
| `_hdrlen_rsvd_flags` | `uint16_t` | Data offset (4 бита), reserved (6 бит), flags (6 бит) |
| `wnd` | `uint16_t` | Window size |
| `chksum` | `uint16_t` | Checksum |
| `urgp` | `uint16_t` | Urgent pointer |

Макросы для доступа: `TCPH_HDRLEN(hdr)`, `TCPH_FLAGS(hdr)`, `TCPH_HDRLEN_SET(hdr, len)`, `TCPH_FLAGS_SET(hdr, flags)`, `TCPH_SET_FLAG/UNSET_FLAG(hdr, flags)`.

Флаги: `TCP_FIN`(0x01), `TCP_SYN`(0x02), `TCP_RST`(0x04), `TCP_PSH`(0x08), `TCP_ACK`(0x10), `TCP_URG`(0x20), `TCP_ECE`(0x40), `TCP_CWR`(0x80).

### `struct tcp_trace_entry` / `struct tcp_trace_buf`

Кольцевой буфер отладки: 384 записи, каждая содержит ticks, event type, seq, len, wnd, rto, rtime, ctx_id, state.

## 6. Диагностические события трассировки (trace event types)

| Код | Значение | Где записывается |
|-----|----------|-----------------|
| `'S'` | Отправка сегмента (seq, len, wnd) | `tcp_output_segment()` в out.c |
| `'X'` | Отправка non-data сегмента (seq, len, cwnd) | `tcp_output_segment()` в out.c |
| `'A'` | Обработка ACK (ack, acked, cwnd) | `tcp_receive()` в in.c |
| `'D'` | Drop (seq, len) — дубликат или старый сегмент | `tcp_receive()` в in.c |
| `'r'` | Состояние RTO-счётчика (rtime, rto, state) | `tcp_receive()` в in.c |
| `'T'` | RTO timeout (rto, rtime) | `tcp_slowtmr()` в tcp.c |
| `'R'` | RTO retransmit commit (seq, len, rto) | `tcp_rexmit_rto_commit()` в out.c |
| `'F'` | Fast retransmit (seq, len, ssthresh) | `tcp_rexmit_fast()` в out.c |
| `'C'` | Close (state) | `tcp_close_shutdown()` в out.c/tcp.c |
| `'L'` | Loss simulation (drop len) | сетевой эмулятор |
| `'M'` | Timer tick | `tcp_tmr_cb()` в tcp.c |
| `'K'` | Kill PCB в slowtmr (state, nrtx, port) | `tcp_slowtmr()` в tcp.c |
| `'Y'` | Kill PCB в tcp_kill_* (seq, nrtx) | `tcp_kill_prio/state/timewait()` в tcp.c |
| `'Z'` | Abandon PCB (state, reset_flag, port) | `tcp_abandon()` в tcp.c, in.c |
| `'E'` | Error (err_code) | обработка ошибок в in.c |
| `'P'` | Zero-window probe (rto) | `tcp_zero_window_probe()` в out.c |

## 7. Зависимости

- `lib/mem.h` — `u_malloc`/`u_calloc`/`u_free`/`u_strdup` (для выделения `lwip_tcp_ctx`)
- `lib/debug_config.h` — `DEBUG_ERROR`, `DEBUG_WARN`, `DEBUG_INFO`, `DEBUG_DEBUG`, `DEBUG_TRACE`, категории `DEBUG_CATEGORY_ALL`/`DEBUG_CATEGORY_GENERAL`
- `lib/memory_pool.h` — `memory_pool_init/destroy/alloc/free/is_freed` (пулы PCB, сегментов)
- `lib/u_async.h` — `uasync_set_timeout`/`uasync_cancel_timeout`, `get_time_tb()`
- `lwip_pbuf.h` — `pbuf_alloc/free/ref/header/chain/dechain/clen/copy_partial/take/get_contiguous`
- `<string.h>` — `memcpy`, `memset`
- `<arpa/inet.h>` / `<winsock2.h>` — `ntohl`/`ntohs`/`htonl`/`htons` (сетевой порядок байт)
