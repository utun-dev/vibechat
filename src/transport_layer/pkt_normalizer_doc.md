# Packet Normalizer (фрагментация/дефрагментация)

## 1. Назначение
Модуль адаптирует кодограммы сервисов к передаче через ETCP-канал с ограниченным MTU. Содержимое кодограммы непрозрачно для normalizer. Состоит из двух половин:

- **Packer** (упаковщик) — разбивает входящие пакеты на фрагменты размером `frag_size` и помещает их в `etcp->input_queue`. Фрагменты собираются в общий буфер пачками для экономии накладных расходов, сброс по таймеру (call_soon) при пустой входной очереди.
- **Unpacker** (распаковщик) — собирает фрагменты из `etcp->output_queue` обратно в исходные пакеты по заголовку длины (2 байта LE), помещает результат в `pn->output` (который читает ETCP router/клиент).

Каждое ETCP-соединение владеет своим экземпляром `PKTNORM`.

## 2. Как пользоваться

### Создание и привязка
`pn_init()` вызывается автоматически при создании `ETCP_CONN`. Клиентский код получает нормализатор через `etcp->normalizer` и использует:

- **Отправка:** положить пакет в `pn->input` (очередь `pn_input`). Packer сам разобьёт и отправит.
- **Приём:** читать собранные пакеты из `pn->output` (очередь `pn_output`). На эту очередь ETCP вешает свой callback `etcp_int_recv`.

```c
// Создание (внутри etcp_connection_create):
etcp->normalizer = pn_init(etcp);
etcp->send_input_q = etcp->normalizer->input; // единая точка входа для отправки

// Отправка кодограммы cmd || payload через публичный API:
struct ll_entry* entry = ll_alloc_lldgram(len);
if (!entry) return -1;
memcpy(entry->dgram, data, len);
entry->len = len;
if (etcp_send(etcp, entry) < 0) {
    queue_dgram_free(entry); queue_entry_free(entry);
    return -1;
}

// Приём (через callback на pn->output):
// queue_set_callback(pn->output, etcp_int_recv, etcp);
```

### Формат фрагментов
Поток содержит конкатенацию кодограмм, каждая предварена 2-байтовым LE-заголовком длины:
```
[packet1_len:2 LE][packet1_data][packet2_len:2 LE][packet2_data]...
```
Поток разделяется на фрагменты ETCP: данные одной кодограммы могут продолжаться в следующем фрагменте. Заголовок длины packer записывает целиком; максимально допустимая длина кодограммы вместе с cmd — `PKTNORM_MAX_DGRAM_SIZE` (16 384 байта). Два байта длины записываются отдельно.

### Сброс состояния
Владелец вызывает `pn_reset()` при сбросе ETCP-потока. Он отменяет flush/waiter, очищает обе очереди normalizer и частичные буферы без выделения памяти. Сам `pn_reset()` не запускает handshake.

Неверный заголовок/длина и OOM после принятия данных вызывают `etcp_conn_fatal_reinit()`: новая локальная эпоха, очистка старого потока и согласование с пиром при сохранённых физических линках. Старые кодограммы не воспроизводятся; после согласования владельцы получают REINIT и восстанавливают операции своего протокола. До сохранения RX DATA в `recv_q` OOM обрабатывает сам ETCP: отбрасывает фрагмент без ACK.

Normalizer не удерживает отдельную OOM-очередь или retry-таймер. После reset/close из пользовательского callback обработчик прекращает старый разбор; извлечённый им фрагмент остаётся его собственностью до однократного освобождения. Счётчик активных callbacks откладывает освобождение normalizer при закрытии.

## 3. API

### Структуры

**`struct PKTNORM`** — основная структура пары packer/unpacker.

| Поле | Описание |
|------|----------|
| `input` | Входная очередь packer (`pn_input`) — сюда кладут пакеты для отправки |
| `output` | Выходная очередь unpacker (`pn_output`) — отсюда читают собранные пакеты |
| `frag_size` | Размер DATA-фрагмента: MTU минус IP/UDP overhead, `SC_ENCRYPT_OVERHEAD`, `ETCP_PAYLOAD_HEADER_SIZE` и `ACK_REZERV`; overhead учитывает IPv6-линки |
| `data` / `data_ptr` / `data_size` | Буфер-накопитель packer (из `data_pool`) |
| `recvpart` | Частично собранный пакет unpacker |
| `flush_timer` | Таймер сброса накопленного буфера (call_soon) |
| `input_waiter_handle` | Backpressure-ожидание на `etcp->input_queue` |
| `tx_wait_time` | Задержка сброса (по умолчанию 10, не используется — сброс через call_soon) |
| `alloc_errors` / `logic_errors` | Счётчики ошибок |
| `in_total_pkts` / `in_total_bytes` | Статистика входящих (в packer) |
| `out_total_pkts` / `out_total_bytes` | Статистика исходящих (из unpacker) |

### Функции

**`pn_init(etcp)`** — создаёт пару PKTNORM. Выделяет `input`/`output` очереди с порогом 0 (backpressure: ждать полного опустошения). Вешает callback-и: `packer_cb` на input, `pn_unpacker_cb` на `etcp->output_queue`, `etcp_int_recv` на output. Возвращает NULL при ошибке.

**`pn_deinit(pn)`** — освобождает все ресурсы: очереди, буферы, таймеры, waiter-ы. Вызывает `routing_del_conn()` (deprecated, no-op). Снимает callback с `etcp->output_queue` для предотвращения use-after-free.

**`pn_reset(pn)`** — отбрасывает `input`/`output`, буферы packer/unpacker, pending и recvpart; отменяет flush и waiter. Не выделяет память, допускает NULL; используется владельцем при сбросе потока. Обычные callbacks очередей сохраняются.

**`pn_unpacker_reset_state(pn)`** — сбрасывает только unpacker (recvpart).

`pn_packer_send` — вспомогательная функция тестов из .c, в API заголовка не объявлена.
Сервисы отправляют кодограммы через `etcp_send`, учитывая UP, backpressure и владение entry.

### Внутренние callback-и

**`packer_cb`** — callback на `pn->input`. Пока сессия согласуется или закрывается, обработку не запускает. Ждёт освобождения `etcp->input_queue` через `queue_waiter_wait`, затем вызывает `etcp_input_ready_cb`. Отказ ожидания вызывает fatal REINIT.

**`etcp_input_ready_cb`** — основной цикл packer. Извлекает пакет из `pn->input`, записывает 2-байтовый заголовок длины, копирует данные в буфер `pn->data`. При заполнении буфера (или если осталось <3 байт) вызывает `pn_send_to_etcp`. Если входная очередь пуста — ставит `flush_timer` через `uasync_call_soon` для сброса хвоста.

**`pn_flush_cb`** — таймерный callback, вызывает `pn_send_to_etcp` при простое.

**`pn_send_to_etcp(pn)`** — выделяет `ETCP_FRAGMENT` из `io_pool`, передаёт в него накопленный буфер `pn->data`, отправляет в `etcp->input_queue`. Указатель normalizer обнуляется до синхронного callback очереди. Возвращает 0 при успехе/пустом буфере, -1 при ошибке или прерывании сессии; ошибку выделения/постановки обрабатывает fatal REINIT.

**`pn_unpacker_cb`** — callback на `etcp->output_queue`. Извлекает фрагменты, парсит длины, копирует данные в `pn->recvpart`. Перед выдачей целой кодограммы в `pn->output` снимает `recvpart` и обновляет статистику. OOM, неверный заголовок или отказ очереди вызывают fatal REINIT. После каждого пользовательского callback проверяет reset_count/close и прекращает старый разбор при изменении сессии.

**`pn_buf_renew(pn)`** — выделяет буфер из `data_pool`, если `pn->data == NULL`; при нехватке места (<3 байт) сначала вызывает `pn_send_to_etcp`. Возвращает 0 при успехе, -1 при ошибке/прерывании. OOM вызывает fatal REINIT.

### Зависимости
- `etcp.h` / `etcp_api.h` — структуры `ETCP_CONN`, `ETCP_FRAGMENT`, `etcp_int_recv`
- `routing.h` — `routing_add_conn`/`routing_del_conn` (deprecated, no-op)
- `utun_instance.h` — `UTUN_INSTANCE` (data_pool, ua)
- `ll_queue.h` — очереди, waiter, threshold
- `u_async.h` — `uasync_call_soon` для flush-таймера
- `mem.h` / `memory_pool` — аллокация буферов и фрагментов
