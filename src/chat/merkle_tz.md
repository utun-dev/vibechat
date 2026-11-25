# ТЗ: событийная синхронизация чата (db_sync + merkle_sync) вместо периодического опроса

> **Статус: историческое ТЗ прежней реализации, не описание текущего кода.**
> Актуальное описание протокола 4 — в `merkle_sync_doc.md`, `member_sync_doc.md` и `db_sync_arch.md`.
> Текущий merkle_sync использует два pull-прохода, один подготовленный пакет и waiter
> на сессию; out_q, broadcast/push_update, bg_check и отдельного WAL-таймера в нём нет.
> member_sync после commit уведомляет движок об изменении дерева вместо прямого push.
> Упомянутые ниже функции, строки, «текущее состояние» и критерии относятся к старому этапу.
> История теперь использует MESSAGES общего Merkle и отдельные PUSH/ACK; новые вставки и страницы
> фиксируются короткими транзакциями. Итоги сверки — в `../../doc/member_merkle_audit_findings.txt`, раздел13.
> Исторический текст ниже сохранён для контекста; его критерии не являются текущими требованиями.

## 1. Цель
После завершения первичной репликации синхронизация должна перейти в **событийный режим**:
любое изменение базы немедленно рассылается пирам (push), без периодического таймера-опроса.
Периодические таймеры остаются только на время первичного догона и детекции таймаутов.
Рассылка ведётся поштучно через **отдельную очередь на каждый пир** с автопередачей
в etcp-очередь и контролем congestion (backpressure).

## 2. Текущее состояние (контекст)

### db_sync (`src/chat/db_sync.c`)
- Событийный push **уже есть**: `db_record_insert_cascade` (стр. 623) при каждой записи шлёт
  `DB_MSG_PUSH` пирам с `sync_state >= 1` (SYNCING и SYNCED). Вызывается из
  `db_sync_insert_signed` (стр. 1780, локальная запись) и recv-обработчиков (стр. 1024, 1156).
- `db_sync_peer_check_cb` (стр. 1462) — периодический 5с (`DB_SYNC_PEER_CHECK_INTERVAL=5`):
  ищет несинхронных пиров (`sync_state==0`) → `db_sync_initiate_sync`; детект таймаута
  (`sync_state==1` завис → 0). **Перевзводится безусловно** (стр. 1470, 1545, 1627).
  Отмена в destroy (стр. 1644).
- `struct SI_PEER.sync_state`: 0=нужен sync, 1=идёт, 2=синхронизирован. `struct DB_SYNC.peer_check_timer`.
- Триггеры начала sync: `db_sync_on_conn_up` (стр. 1268), recv-обработчики (стр. 747, 817, 1212).

### merkle_sync (`src/chat/merkle_sync.c`)
- `_bg_timer_cb` (стр. 702) — периодический 5с (`MS_BG_INTERVAL_MS=5000`): round-robin
  `merkle_sync_bg_check` по каналам + WAL-checkpoint (`tick % 12`). **Перевзводится безусловно**
  (стр. 732). Отмена в destroy (стр. 767).
- Переход «корневой хеш совпал → synced» **реализован**: `_handle_hashes` (стр. 442-445):
  `differs==0 && level==0` → `_session_done(MT_OK)` → `SESS_SYNCED`.
- Рассылка: `merkle_sync_broadcast` (стр. 658) — **всем** сессиям (без проверки `SESS_SYNCED`),
  кроме `from_peer`; `merkle_sync_push_update` (стр. 618) — только `SESS_SYNCED` (лёгкие апдейты, online).
- Отправка: `_send_msg` (стр. 241) → `ms_find_conn_for_node` (стр. 231) → `etcp_send` →
  `queue_data_put(conn->send_input_q)` (при переполнении **дропает**, backpressure нет).
- `struct ms_session` (стр. 31): `ns`, `peer`, `sess_state`, `done_cb`, `pending`.
  `struct merkle_sync` (стр. 42): `sessions`, `bg_timer`.

### member_sync (`src/chat/member_sync.c`)
- `member_sync_put` (стр. 595) / `member_sync_del` (стр. 677): пишут в БД +
  `merkle_sync_recompute_path`. **Сами не рассылают.**
- `member_sync_set_online` (стр. 703): только пишет в БД; push делается вручную в
  `chat_sync.c` (стр. 402, 439).
- `member_sync_broadcast_one` (стр. 514): читает мембера + `merkle_sync_broadcast`.
  Вызывается вручную из `chat_core.c:673`, `chat_profile.c:77`; **НЕ** вызывается из
  `chat_channel.c:193` (self-add).
- `_member_apply_items` (apply_items, стр. ~425): `member_sync_put` поштучно + batch
  `merkle_sync_broadcast` (relay, стр. 440).

### Очереди / backpressure (`lib/ll_queue.h`)
- Есть: `queue_new`, `queue_set_callback`, `queue_set_threshold`, `queue_waiter_wait`,
  `queue_resume_callback`, `queue_data_put/get`, `queue_entry_count`.
- `conn->send_input_q` = вход normalizer'а (`pkt_normalizer.c:53`), порог `(0,0)`
  (ждать полного освобождения), `queue_set_waiter_defer(pn->input, 1)`.

## 3. Требования

### 3.1 db_sync — «work-driven» таймер
1. Хелпер `db_sync_has_pending_work(db)`: есть ли у любого активного инстанса пир с `sync_state != 2`.
2. Хелпер `db_sync_resume_peer_check(db)`: взвести `peer_check_timer`, если `NULL` и `db->enabled`.
3. `db_sync_peer_check_cb`: в начале `db->peer_check_timer = NULL`; в конце — взводить только
   если есть работа, иначе не взводить и логировать `INFO` «all peers synced — peer_check stopped».
   - Случай `!g` (нет default-группы, стр. 1468) — сохранить текущее поведение (перевзвод)
     или обработать отдельно (решить при реализации).
4. Возобновление по событиям: в `db_sync_initiate_sync` (единая точка старта sync) вызвать
   `db_sync_resume_peer_check(si->db_sync)`. Это покрывает `db_sync_on_conn_up` и recv-обработчики.
5. Важно: таймер должен крутиться пока есть `sync_state==1` (иначе не будет детекта таймаута).

### 3.2 merkle_sync — событийный bg_check + отдельный WAL-таймер
1. Убрать периодический `_bg_timer_cb` (5с) с round-robin проверкой.
2. `merkle_sync_bg_check` (стр. 672) сделать **событийным**: пересчёт дерева выполняется сразу
   при записи/`apply_update` (через `merkle_sync_recompute_path`, который уже вызывается из
   `member_sync_put/del`). Периодической сверки нет.
3. WAL-checkpoint вынести в **отдельный редкий таймер** (~60с, локально, без сети).
   Хранить отдельный handle; отменять в destroy. (Интервал уточнить: 60с.)

### 3.3 Централизованное событие «изменение базы» (member_sync)
1. Единый путь «запись → пересчёт дерева → рассылка»:
   - `member_sync_put` / `member_sync_del`: после `merkle_sync_recompute_path` автоматически
     ставить изменение в очередь рассылки (п. 3.4), **кроме источника** (`from_peer`/self).
   - `member_sync_set_online`: автоматически `merkle_sync_push_update`.
2. Убрать ручные рассылки из `chat_core.c:673`, `chat_profile.c:77`, `chat_sync.c:402/439`,
   `member_sync_broadcast_one` (или оставить как узкий хелпер, но не требовать от caller'ов).
3. Receive-путь `_member_apply_items`: избежать двойной рассылки — либо `member_sync_put`
   рассылает сам (тогда убрать batch-relay стр. 440), либо оставить relay как единый batch и
   НЕ рассылать из `member_sync_put` при приёме. **Выбрать один механизм**
   (рекомендуется: put рассылает поштучно, relay убрать).

### 3.4 По-пировая очередь рассылки с congestion control (merkle_sync)
1. **Очередь на пира**: добавить `struct ll_queue* out_q` в `struct ms_session`
   (сессия = peer + ns; это и есть «по-пировая» на канал). Создать при создании сессии,
   освободить при удалении/деините. (Если нужен строгий per-peer независимо от канала —
   отдельная структура `ms_peer_out{peer, out_q, waiter}`; по умолчанию per-session.)
2. **Энкью**: `merkle_sync_broadcast` / централизованный путь вместо немедленного
   `_send_broadcast_data` кладёт **поштучно** каждый item в `out_q` каждой подходящей сессии
   (кроме `from_peer`). Item содержит ns + данные мембера.
3. **Автопередача**: `queue_set_callback(out_q, drain_cb, session)`. `drain_cb`:
   `queue_data_get(out_q)` → собрать wire-сообщение → `etcp_send(conn, entry)` →
   `queue_resume_callback(out_q)`.
4. **Congestion control (backpressure)**: не дропать при переполнении etcp-очереди. Перед
   `etcp_send` проверять занятость `conn->send_input_q`; при превышении порога —
   `queue_waiter_wait(conn->send_input_q, session->waiter, resume_drain_cb, session)` и
   приостановить drain; возобновить по колбэку порога. Порог/лимит (пакеты/байты) уточнить,
   ориентир — не дропать, но и не копить безлимитно.

## 4. Файлы/функции (ориентир)
- `src/chat/db_sync.c` (+`.h`): `db_sync_has_pending_work`, `db_sync_resume_peer_check`,
  правка `db_sync_peer_check_cb`, вызов resume в `db_sync_initiate_sync`,
  поле `peer_check_timer` уже есть.
- `src/chat/merkle_sync.c` (+`.h`): поле `out_q`/waiter в `struct ms_session`, `drain_cb`,
  правка `merkle_sync_broadcast`/`push_update`, замена `_bg_timer_cb` на событийный +
  отдельный WAL-таймер.
- `src/chat/member_sync.c` (+`.h`): автопередача из `member_sync_put/del/set_online`,
  убрать ручные `member_sync_broadcast_one` у caller'ов (`chat_core.c`, `chat_profile.c`,
  `chat_sync.c`, `chat_channel.c`).
- Учесть `#ifdef UTUN_HAVE_STANDBY` для standby-гейтинга не требуется после этого (таймеры сами
  останавливаются), но keepalive/прочие не трогаем.

## 5. Критерии приёмки
1. После полного догона (все пиры `sync_state==2` / все сессии `SESS_SYNCED`) в логе нет
   `db_sync_peer`/`ms_bg` каждые 5с.
2. Локальная запись (сообщение/мембер/online) рассылается всем подходящим пирам без участия
   caller'а; self-add при входе в канал тоже рассылается.
3. При переполнении etcp-очереди рассылка не дропает, а приостанавливается и возобновляется
   (backpressure).
4. После события «изменение базы» дерево merkle пересчитано, и изменение разошлось;
   «корневой хеш совпал → synced» работает как раньше.
5. Юнит/интеграционные проверки (при наличии) проходят; спам DEBUG/TRACE `sys` в standby
   отсутствует (при разумном `debug`-уровне).

## 6. Риски / открытые вопросы
- Гранулярность очереди: per-session (peer+ns) vs строго per-peer — уточнить.
- Значения порога backpressure и интервала WAL-таймера.
- Взаимодействие рассылки «даже во время sync» с протоколом merkle-сессии (broadcast идёт
  параллельно обмену хешами) — убедиться, что не ломает `pending`-машину сессии.
