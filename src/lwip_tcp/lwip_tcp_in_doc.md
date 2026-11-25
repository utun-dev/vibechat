# lwip_tcp_in

## 1. Назначение

Обработка входящих TCP-сегментов — адаптированный код из lwIP 2.1.x `tcp_in.c` для uTun. Все `#if`-блоки убраны, глобальный IP lwIP заменён явными параметрами `src_ip`/`dst_ip`. Каждый `lwip_tcp_ctx` обслуживается своим потоком uasync.

Модуль реализует:
- Демультиплексирование входящего сегмента к PCB (по quad: src_ip, dst_ip, sport, dport)
- Полный TCP state machine на 11 состояний (SYN_SENT→SYN_RCVD→ESTABLISHED→...→TIME_WAIT)
- Поддержку simultaneous open (SYN_SENT + SYN+ACK), RST, SYN flood в ESTABLISHED
- Обработку ACK: window update, dupack detection/fast retransmit, congestion control (slow start / congestion avoidance), RTT estimation
- Реассемблинг данных: вставка не-по-порядку сегментов в сортированный OOSEQ-список, слияние при заполнении пробелов
- Congestion control: Reno вариант (slow start, congestion avoidance, fast retransmit, fast recovery через TF_INFR), cwnd при dupack >3 инкрементируется, при ack>lastack — выход из fast recovery (сброс cwnd в ssthresh)
- Защиту от вышедших за rcv_wnd данных (обрезание seg.len, обрезание FIN/SYN)
- Обработку delayed close: отложенное закрытие PCB после отправки всех данных

## 2. Как пользоваться

Внутренний модуль, главная точка входа — `lwip_tcp_input()` (вызывается из стека lwIP при получении IP-пакета с протоколом TCP). Передаётся контекст `lwip_tcp_ctx`, pbuf, src_ip, dst_ip.

**Ключевые концепции:**

- `lwip_tcp_input()` (стр.120): парсинг TCP-заголовка, демультиплексирование по active_pcbs→tw_pcbs→listen_pcbs, вызов `tcp_process()`, отправка RST если нет получателя. Продвигает найденный PCB в начало списка (LRU).

- `tcp_process()` (стр.452): полный state machine — разбор комбинаций флагов (RST, SYN, ACK, FIN) в соответствии с состоянием PCB. Для каждого состояния своя логика: SYN_SENT ждёт SYN+ACK с корректным ackno, SYN_RCVD ждёт ACK (и обрабатывает retransmit SYN), ESTABLISHED/CLOSE_WAIT вызывают `tcp_receive()`, FIN_WAIT_1/2, CLOSING, LAST_ACK обрабатывают закрытие.

- `tcp_receive()` (стр.703): основная логика ACK и данных:
  - **Window update** (стр.710–720): обновление `snd_wnd`, трекинг `snd_wl1`/`snd_wl2` для защиты от старых ACK
  - **Dupack detection** (стр.723–738): dupack — это ACK с ackno == lastack, tcplen==0, и snd_wnd не изменился. При dupack: инкремент счётчика, fast retransmit при >=3, инкремент cwnd при >3 (inflating)
  - **ACK новых данных** (стр.739–806): обновление `lastack`, освобождение подтверждённых сегментов из unacked/unsent, congestion control (slow start при cwnd < ssthresh: cwnd += min(acked, 2*MSS) с учётом TF_RTO, congestion avoidance при cwnd >= ssthresh: cwnd += MSS когда bytes_acked >= cwnd), обновление snd_buf, выход из TF_RTO
  - **RTT estimation** (стр.798–807): по методу Van Jacobson (sa/sv/rto), только если `rttest` установлен (был измеряемый сегмент) и ackno > rtseq
  - **Reassembly** (стр.810–1001): если seqno in-window, но не in-order — создание/вставка в OOSEQ-список с автоматическим слиянием перекрывающихся сегментов и удалением полностью покрытых. Если seqno == rcv_nxt — приём данных, продвижение rcv_nxt, вытаскивание из ooseq примыкающих сегментов (стр.899–922)
  - **PU-ACK** (стр.924/997/1000/1005): отправка ACK (delayed или immediately) в ответ на данные или out-of-window пакеты

- `tcp_listen_input()` (стр.373): обработка входящего SYN для listen PCB — создание нового PCB, установка ISS, регистрация в active_pcbs, применение проверенного MSS, отправка SYN+ACK.

- `tcp_timewait_input()` (стр.428): обработка пакетов для TIME_WAIT PCB — ответ RST на SYN в окне, продление таймера на FIN, ACK на данные.

- `tcp_parse_options()` проверяет длины опций до поиска PCB. Опции могут пересекать границы pbuf. MSS применяется при получении SYN или принятого SYN+ACK и ограничивается локальным MSS.

- **OOSEQ** (стр.647–672, 925–994): сортированный список out-of-order сегментов. `tcp_oos_insert_segment()` вставляет новый сегмент, сливая/обрезая соседние при перекрытии. FIN-флаг переносится на поглощающий сегмент. При вставке в середину списка предшествующий сегмент может обрезаться справа.

- **Congestion control** (Reno): `cwnd` инициализируется как `min(4*MSS, max(2*MSS, 4380))`. Slow start: cwnd += min(acked, 2*MSS). Congestion avoidance: cwnd += MSS когда bytes_acked >= cwnd. Fast retransmit при 3 dupack. Fast recovery: TF_INFR включён, cwnd продолжает расти на MSS за dupack >3; выход при первом новом ACK (сброс cwnd = ssthresh).

## 3. API

### Внешние (вызываются извне модуля)

| Функция | Назначение |
|---|---|
| `lwip_tcp_input(ctx, p, src_ip, dst_ip)` | Главная точка входа: демультиплексирование + state machine + reassembly |

### Внутренние (статические, только в этом файле)

| Функция | Назначение |
|---|---|
| `tcp_process(in, pcb, peer_mss)` | State machine: обработка RST/SYN, переходы SYN_SENT→ESTABLISHED, SYN_RCVD→ESTABLISHED, FIN-переходы |
| `tcp_receive(in, pcb)` | ACK-обработка (window update, dupack, congestion control, RTT), приём/реассемблинг данных |
| `tcp_listen_input(in, lpcb, peer_mss)` | SYN на listen PCB: создание нового PCB, SYN+ACK |
| `tcp_timewait_input(in, pcb)` | Обработка пакетов в TIME_WAIT |
| `tcp_parse_options(p, len, local_mss)` | Проверяет границы каждой опции. Возвращает эффективный MSS, 0 при отсутствии MSS или −1 при ошибке. |
| `tcp_oos_insert_segment(in, pcb, cseg, seg)` | Вставка сегмента в OOSEQ с обрезанием/слиянием |
| `tcp_free_acked_segments(in, pcb, seg_list)` | Освобождение полностью подтверждённых сегментов из списка, накопление `recv_acked` |
| `tcp_input_delayed_close(in, pcb)` | Отложенное закрытие PCB при TF_CLOSED |

### Локальные хелперы

| Функция | Назначение |
|---|---|
| `pbuf_cat_local(h, t)` | Конкатенация двух pbuf — используется при слиянии OOSEQ с recv_data |
| `tcp_seg_copy_local(pcb, seg)` | Копирование tcp_seg через memcpy + pbuf_ref (из seg_pool) |
| `tcp_seg_free_local(pcb, seg)` | Освобождение tcp_seg: pbuf_free + возврат в seg_pool |
| `tcp_segs_free_local(pcb, seg)` | Освобождение цепочки tcp_seg |
| `pbuf_truncate(p, new_len)` | Урезание pbuf-цепочки справа до нужной длины |

### Состояние одного входящего сегмента

`lwip_tcp_input()` размещает `tcp_input_state` на стеке и передаёт его обработчикам. Вложенный ввод в другой стек сохраняет состояние внешнего сегмента.

- `inseg` — текущий обрабатываемый сегмент
- `tcphdr` — фиксированный заголовок сегмента
- `seqno`, `ackno` — номера последовательности (host byte order)
- `sport`, `dport` — порты (host byte order)
- `tcplen` — длина данных + SYN/FIN
- `flags` — флаги TCP-заголовка
- `recv_flags` — накопленные флаги обработки (TF_RESET, TF_CLOSED, TF_GOT_FIN)
- `recv_data` — собранные данные для передачи приложению через TCP_EVENT_RECV
- `recv_acked` — сумма подтверждённых байт для TCP_EVENT_SENT
- `src_ip`, `dst_ip` — IP-адреса пакета

## 4. Зависимости

- `lwip_tcp_priv.h` — заголовок TCP, макросы флагов/последовательностей, tcp_seg, события, таймеры
- `lwip_tcp.h` — PCB, контекст ctx, типы ошибок (LERR_*), состояния
- `lwip_tcp_opts.h` — TCP_MSS, TCP_WND, TCP_TMR_INTERVAL и прочие константы
- `lwip_pbuf.h` — pbuf API
- `lib/mem.h` — u_malloc/u_free
- `lib/debug_config.h` — DEBUG_ERROR/DEBUG_INFO
- `lib/memory_pool.h` — memory_pool_alloc/free для seg_pool
- Внешние функции из `lwip_tcp.c`: `tcp_update_rcv_ann_wnd()`, `tcp_abandon()`
- Внешние из `lwip_tcp_out.c`: `tcp_send_empty_ack()`, `tcp_enqueue_flags()`, `tcp_rst()`, `tcp_rexmit()`, `tcp_rexmit_rto()`, `tcp_rexmit_fast()`, `tcp_ack_now()`, `tcp_output()`, `tcp_alloc()`, `tcp_free()`, `tcp_pcb_purge()`, `tcp_pcb_remove()`, `tcp_process_refused_data()`, `tcp_abort()`
- `lwip_tcp_trace_record()` — запись в кольцевой буфер трассировки
