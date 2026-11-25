# lwip_tcp_out

## 1. Назначение
Исходящая сторона lwIP TCP стека: сегментация данных в MSS-чанки, планирование передачи, контроль перегрузки, ретрансмиссия и отправка управляющих сегментов (SYN, FIN, RST, ACK, zero-window probe). Адаптирован из lwIP для uTun — убраны timestamps, SACK, WND_SCALE, checksum-on-copy.

## 2. Как пользоваться
Внутренний модуль — вызывается из `lwip_tcp.c` (таймеры, API), `lwip_tcp_in.c` (fast retransmit, SACK-обработка).

### Ключевые концепции

#### tcp_write() — сегментация и постановка в unsent
Пишет данные в выходной буфер PCB. Фазы:
1. **Дописывание в хвост last_unsent**: если последний unsent сегмент < MSS, добавляем цепочку pbuf к нему (copy/zero-copy, extend).
2. **Новые сегменты**: оставшиеся данные режутся на MSS-чанки, каждому создаётся `struct tcp_seg` через `tcp_create_segment`.
3. **Commit**: конкатенация к хвосту `pcb->unsent`, обновление `snd_lbb`, `snd_buf`, `snd_queuelen`. PSH-флаг ставится на последний сегмент, если не задан `TCP_WRITE_FLAG_MORE`.
4. Лимит `snd_queuelen` ≤ `TCP_SND_QUEUELEN` (4 * SND_BUF/MSS = 64). При превышении — LERR_MEM.
5. MSS адаптивен: если `snd_wnd_max` даёт меньшее окно, MSS снижается до `min(MSS, snd_wnd_max/2)`.

#### tcp_output() — планирование и отправка
Ограничения:
- **cwnd**: `min(cwnd, snd_wnd)` — окно перегрузки
- **Нейгл**: `tcp_do_output_nagle()` — запрещает отправку маленьких сегментов если есть unacked данные и сегмент < MSS (отключается `TF_NODELAY` или при `TF_INFR`).
- **Zero-window persist**: если всё ограничено snd_wnd (а не cwnd) и unacked пуст — взводится `persist_backoff` для последующей отправки zero-window probe.
- Отправленные data-сегменты (TCP_TCPLEN > 0) перемещаются из `unsent` в `unacked` с сортировкой по seqno.
- Пустые сегменты (например, чистый ACK) освобождаются после отправки.
- Обновляется `snd_nxt`.

#### tcp_enqueue_flags() — управляющие сегменты
Создаёт сегмент с заданными флагами (SYN, FIN, RST), ставит MSS-опцию для SYN. Инкрементирует `snd_lbb` для SYN/FIN (потребление номера последовательности).

#### tcp_send_fin() — завершение соединения
Если есть unsent данные — вешает FIN на хвостовой сегмент. Иначе создаёт новый FIN-сегмент через `tcp_enqueue_flags`.

#### tcp_split_unsent_seg() — дробление unsent
Отрезает хвост сегмента (например, для retransmit при потере). Копирует remainder в новый pbuf, создаёт новый `tcp_seg`, переносит флаги (PSH/FIN) на остаток. Вставляет после оригинального сегмента.

#### Ретрансмиссия

| Функция | Назначение |
|---------|-----------|
| `tcp_rexmit_rto_prepare()` | Переносит весь `unacked` обратно в `unsent`, ставит `TF_RTO`, сбрасывает `rttest` |
| `tcp_rexmit_rto_commit()` | Инкрементирует `nrtx`, дёргает `tcp_output()` |
| `tcp_rexmit_rto()` | prepare + commit — полный RTO retransmit |
| `tcp_rexmit()` | Перемещает 1-й unacked сегмент обратно в unsent (быстрая ретрансмиссия). Инкрементирует `nrtx`, сбрасывает `rttest` |
| `tcp_rexmit_fast()` | При 3 dupack: ретрансмитит 1-й unacked через `tcp_rexmit()`, затем `ssthresh = max(cwnd/2, 2*MSS)`, `cwnd = ssthresh + 3*MSS`, ставит `TF_INFR` |

#### tcp_zero_window_probe() — зонд при нулевом окне
Отправляет 1 байт данных из первого unsent сегмента (или FIN если сегмент уже пустой FIN). Инкрементирует `persist_probe`, `snd_nxt` сдвигается на 1.

#### Управляющие сегменты

| Функция | Назначение |
|---------|-----------|
| `tcp_rst()` | RST+ACK сегмент с указанными seqno/ackno, IP/портами |
| `tcp_send_empty_ack()` | Пустой ACK с `rcv_ann_wnd` — подтверждение приёма |
| `tcp_keepalive()` | Заглушка (keepalive отключён) |

#### Контрольные суммы
- `tcp_ip_checksum()` — IP-заголовок (20 байт, one's complement)
- `tcp_checksum()` — данные произвольной длины
- `tcp_pseudo_checksum()` — TCP псевдозаголовок (src_ip + dst_ip + proto + tcp_len + данные pbuf)

#### RTT tracking
`rttest`/`rtseq` взводятся в `tcp_output_segment()` при отправке данных если `rttest == 0` — запоминается время (`ticks`) и seqno. Сбрасывается при ретрансмиссии. Используется в `lwip_tcp_in.c` для обновления SRTT/RTO.

#### Выходной конвейер
Каждый сегмент, проходящий через `tcp_output_segment()`:
1. Заполняет ackno = `rcv_nxt`, wnd = `rcv_ann_wnd`
2. Записывает MSS-опцию если есть
3. Взводит `rttest`/`rtseq` если нужно
4. Сдвигает pbuf payload на TCP-заголовок
5. Вычисляет контрольную сумму TCP (псевдозаголовок)
6. Добавляет IP-заголовок (IPv4, vhl=0x45, ttl=64, вычисляет IP checksum)
7. Вызывает `ctx->output()` — коллбэк в tcp_proxy_client для отправки IP-пакета через ETCP
8. Снимает IP-заголовок обратно с pbuf

Проверка `tcp_output_segment_busy()` блокирует отправку если pbuf.ref != 1 (ещё читается в input).

#### Логирование и трассировка
- `lwip_tcp_trace_record()` — запись в ring-буфер событий: 'S'=send, 'X'=cwnd-limited, 'R'=RTO, 'F'=fast_rtx, 'P'=probe, 'C'=FIN
- `g_tcp_trace` — хук для внешнего наблюдателя (используется в тестах)
- `DEBUG_TRACE(DEBUG_CATEGORY_TRAFFIC, ...)` — детали отправки
- Статистика: `stats.write_fails`, `stats.enq_fails`, `stats.rst_fails`, `stats.ack_fails`, `stats.probe_fails`, `stats.split_fails`, `stats.seg_fails`, `stats.pbuf_fails`

## 3. API

### Публичные (объявлены в lwip_tcp.h / lwip_tcp_priv.h)

```c
err_t tcp_write(struct tcp_pcb *pcb, const void *data, uint16_t len, uint8_t apiflags);
```
Сегментация данных в unsent очередь. `apiflags`: `TCP_WRITE_FLAG_COPY` (копировать данные, иначе zero-copy), `TCP_WRITE_FLAG_MORE` (не ставить PSH на последний сегмент). Возвращает `LERR_OK` или `LERR_MEM`.

```c
err_t tcp_output(struct tcp_pcb *pcb);
```
Отправка unsent сегментов с учётом cwnd/snd_wnd и Нейгла. Перемещает отправленные в unacked.

```c
err_t tcp_enqueue_flags(struct tcp_pcb *pcb, uint8_t flags);
```
Постановка управляющего сегмента (SYN/FIN/RST) в unsent очередь. Для SYN добавляет MSS-опцию.

```c
err_t tcp_send_fin(struct tcp_pcb *pcb);
```
Добавляет FIN к последнему unsent сегменту или создаёт новый.

```c
void tcp_rst(struct tcp_pcb *pcb, uint32_t seqno, uint32_t ackno,
             uint32_t local_ip, uint32_t remote_ip,
             uint16_t local_port, uint16_t remote_port);
```
Отправка RST+ACK.

```c
err_t tcp_send_empty_ack(struct tcp_pcb *pcb);
```
Пустой ACK с текущим `rcv_ann_wnd`.

```c
err_t tcp_rexmit_rto_prepare(struct tcp_pcb *pcb);
void tcp_rexmit_rto_commit(struct tcp_pcb *pcb);
void tcp_rexmit_rto(struct tcp_pcb *pcb);
```
RTO ретрансмиссия: prepare (unacked→unsent), commit (nrtx++, tcp_output). `tcp_rexmit_rto()` — одно действие.

```c
err_t tcp_rexmit(struct tcp_pcb *pcb);
```
Перемещает первый unacked сегмент в unsent.

```c
void tcp_rexmit_fast(struct tcp_pcb *pcb);
```
Быстрая ретрансмиссия при 3 dupack: ssthresh/2, cwnd=ssthresh+3*MSS, tcp_rexmit.

```c
err_t tcp_zero_window_probe(struct tcp_pcb *pcb);
```
Отправка 1-байтового зонда из первого unsent сегмента.

```c
err_t tcp_keepalive(struct tcp_pcb *pcb);
```
Заглушка (keepalive отключён в opts).

```c
err_t tcp_split_unsent_seg(struct tcp_pcb *pcb, uint16_t split);
```
Дробление первого unsent сегмента на части по split байт.

```c
uint16_t tcp_ip_checksum(const struct ip_hdr *iph);
uint16_t tcp_checksum(const void *data, uint16_t len);
```
Вычисление контрольных сумм.

## 4. Зависимости
- `lwip_tcp.h` — типы PCB, pbuf, err_t, коллбэки, enum state
- `lwip_tcp_priv.h` — `struct tcp_seg`, макросы TCPH_*, SEQ, Nagle
- `lwip_tcp_opts.h` — TCP_MSS, TCP_SND_QUEUELEN, TCP_WND etc.
- `lwip_pbuf.h` — `pbuf_alloc`, `pbuf_free`, `pbuf_header`, `pbuf_copy_partial`, `pbuf_take`, `pbuf_clen`, `pbuf_cat`
- `lib/mem.h` — u_malloc/u_free
- `lib/memory_pool.h` — `memory_pool_alloc`/`memory_pool_free` для seg_pool
- `lib/debug_config.h` — DEBUG_ERROR, DEBUG_TRACE, DEBUG_CATEGORY_ALL, DEBUG_CATEGORY_TRAFFIC
