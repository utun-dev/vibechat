/* Внутренний реестр звонков: call.c владеет ctx/сессиями, video_transport и process
 * видеопорта обращаются к ним только из uasync. Workers используют публичный port API.
 * Сессия размещена в data внешнего ll_entry; sessions индексирует call_id, а не пару group/peer.
 * Find возвращает заимствованный указатель, в том числе на ENDED до удаления записи.
 * При ENDED аудио-TX/video останавливаются сразу; CM/запись могут ждать END_ACK/таймаут.
 * Обычное удаление снимает запись из реестра и освобождает entry через call_soon;
 * call_destroy отменяет callbacks и освобождает оставшиеся в реестре записи синхронно.
 * Не переносить указатели ctx/session в другой поток или отложенную команду: передавать call_id
 * и повторять find в uasync. Описание публичных действий/событий — call.h/call_proto.h. */
#ifndef CALL_PRIV_H
#define CALL_PRIV_H
#include "call.h"
#include "call_audio_batch.h"
#include "../routing_layer/etcp_router.h"
#include "../../lib/ll_queue.h"

/* IDLE — только создание; RINGING бывает у обеих ролей, WAIT_DECISION — ожидание force
 * при телефонной занятости; ENDED в реестре уже не занимает линию и не принимает медиа. */
enum call_state { CALL_ST_IDLE, CALL_ST_OUTGOING, CALL_ST_RINGING, CALL_ST_ACTIVE, CALL_ST_ENDED,
    CALL_ST_WAIT_DECISION, CALL_ST_ANSWERING, CALL_ST_SELECTING };
enum call_role { CALL_ROLE_CALLER, CALL_ROLE_CALLEE };
#define CALL_MAX_TARGETS 64
/* Состояние отдельного адресата: terminal/ack обслуживаются независимо от выбранного медиа-пира. */
struct call_target {
    uint64_t ended_tb;
    uint8_t ed[32]; /* Ключ независимого адресата. */
    uint8_t state, end_subcmd, reason, ack, no_route;
};
struct call_video_transport;
struct call_session {
    struct ll_entry ll;            /* Не используется; реальный queue entry находится перед s == entry->data. */
    uint64_t call_id, group_id, peer_node_id; /* Числовая проекция пира для локальных событий и диагностики. */
    uint8_t peer_ed[32]; /* До выбора — anchor, после выбора — медиа-пир. */
    uint8_t role, state, reason;   /* call_role/call_state; reason — CALL_REASON_* после перехода в ENDED. */
    unsigned target_count;         /* Ноль у callee; caller владеет снимком адресатов до удаления сессии. */
    void* targets_read;            /* Токен ожидающего READ; callback владеет памятью, сравнивает после find(call_id). */
    struct call_target targets[CALL_MAX_TARGETS];
    uint8_t selected;              /* Caller зафиксировал peer_ed после первого ACCEPT. */
    void *target_timer;            /* Независимые повторы отбоя проигравшим, также во время ACTIVE. */
    uint8_t force;                 /* вызывающий подтвердил обход телефонной занятости */
    uint64_t started_tb;           /* Начало попытки get_time_tb (100мкс); 60с отсчитываются отсюда, включая WAIT_DECISION. */
    uint8_t end_subcmd;            /* Финальная команда для повтора; 0 — нет своей команды либо уже принят END_ACK. */
    uint64_t end_started_tb;       /* Начало ENDED в tb; предел повтора своей команды — 20с. */
    struct call_ctx *ctx;          /* Заимствованный владелец, живёт до удаления сессии из реестра. */
    struct CONN_MGR_HANDLE *cm_handle; /* Собственный запрос оптимизации пути; ACTIVE не требует готового CM. */
    void *ring_timer, *invite_timer; /* Caller: 60с/повтор INVITE 300мс; callee: ring_timer — watchdog INVITE до 2с. */
    uint8_t no_route_strikes;      /* Последовательные INVITE ticks без маршрута; сбрасывается при наличии маршрута. */
    void *traffic_timer, *teardown_timer, *stats_timer; /* RX watchdog 20с, завершение/END retry, путь и ACTIVE-статистика 1с. */
    uint8_t connection_reported, connection_type; /* Было событие CONNECTION / последний router path.type (0..3). */
    uint8_t connection_next_hop[32]; /* Ed25519 последнего опубликованного next hop. */
    uint64_t last_media_tb; /* Время последнего RX; watchdog использует traffic_timer. */
    uint16_t tx_media_seq;         /* Следующий sequence аудиопакета с wrap uint16; расходуется до выделения TX entry. */
    struct call_audio_batch_control audio_batch; /* Прежний адаптивный выбор обязательного числа кадров. */
    void *audio_batch_timer; /* Снимок Router и пересчёт target каждые 50мс; не отправляет неполную пачку. */
    uint64_t buffer_log_us; /* Последняя сводка транспорта, монотонные мкс; период 200мс, без отдельного таймера. */
    uint32_t tx_batches, rx_batches; /* Число Router-кодограмм, отдельно от аудиокадров. */
    struct ll_queue *tx_q;         /* FIFO Opus с локальными seq/PTS/created_us в entry->data; до 750, overflow удаляет голову. */
    struct router_send_request audio_send; /* Слот отправки: до callback кадры остаются в tx_q. */
    uint32_t c_tx_frames, c_rx_frames, c_tx_dropped, c_rx_dropped; /* TX принят router / валидный RX / TX overflow / RX не ведётся. */
    uint64_t tx_wait_total_ms;     /* Сумма времени enqueue->принятие Router; сброс при отчёте stats. */
    uint32_t tx_wait_frames, tx_wait_max_ms; /* Число drained кадров/максимальное ожидание за тот же отчётный интервал. */
    struct call_video_transport *video; /* Собственное ленивое состояние видеотрека; cleanup обнуляет и освобождает его. */
};
struct call_closed { uint64_t id, group, until; uint8_t peer_ed[32]; }; /* Короткий запрет оживления после раннего отбоя. */
struct call_pm_read;
struct call_ctx {
    struct call_closed closed[256]; /* Только завершённые tuple; 70с, без вытеснения свежих записей. */
    struct UTUN_INSTANCE *inst;    /* Заимствованное ядро, владеющее ctx в inst->call; audio_ops хранится отдельно у ядра. */
    int initialized;              /* Сигналинг привязан; 1 также во время регистрации media services/отката call_init. */
    struct ll_queue *sessions;    /* Собственные queue entries с call_session в data и 8-байтовым индексом call_id. */
    struct call_pm_read* pm_read; /* Одна исходящая PM-попытка ждёт READ; destroy/hangup отсоединяют, callback освобождает. */
    uint32_t c_calls_started, c_calls_ended; /* Созданные исходящие / все переходы в ENDED; это не парные счётчики. */
    call_media_recv_fn media_recv_fn; /* Один необязательный headless RX callback, вызываемый в uasync. */
    void *media_recv_arg;          /* Заимствованный arg до замены callback/destroy; пересоздание ctx сбрасывает привязку. */
};
/* Найти call_id, включая ENDED: заимствованная сессия либо NULL (также при NULL ctx/sessions).
 * Только uasync; не проверяет state/group/peer. После удаления/destroy указатель использовать нельзя. */
struct call_session *call_session_find(struct call_ctx *ctx, uint64_t call_id);
#endif
