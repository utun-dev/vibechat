// call — управление P2P аудио/видеозвонком одного узла.
// Начать вызов через start, ответить через accept/decline, завершить через hangup.
// Состояния и результаты приходят событиями CHAT_EVT_CALL_* из chat_event.h.
// Аудио подключается через call_audio.h, видеотрек — через call_video_transport.h или call_video_port.h.
//
// Модуль оперирует парой (group_id, Ed25519): звонок можно инициировать из
// CHAT-группы или из DM (group_id — группа маршрутизации, как в dm_core).
// Сигналинг, аудио и видео идут через etcp_router с обязательными ROUTE_CRYPTO_SIGN|ENCRYPT; соединение
// оптимизируется через параллельные пробы conn_mgr (DIRECT/REVERSE/INDIRECT).
// Оба узла должны оставаться допущенными участниками группы доставки. При отзыве доступа
// или остановке группы звонок завершается, очереди медиа очищаются. Группа активного звонка
// не меняется; новый звонок из DM заново выбирает доступную общую группу через dm_call_route.
//
// Все функции, кроме явно отмеченных, — в uasync-потоке. GUI/headless вызывают через trampoline
// (gui_bridge_post_uasync_fn / uasync_post).
#ifndef CALL_H
#define CALL_H

#include <stdint.h>
#include "call_jitter.h"
#include "call_clock.h"

struct UTUN_INSTANCE;

#ifdef __cplusplus
extern "C" {
#endif

// ── жизненный цикл ──

/* Зарегистрировать сигналинг/медиа в роутере и создать реестр сессий после dm_core_init.
 * inst/ua/роутер должны существовать. 0 = успех (повтор идемпотентен), -1 = ошибка с откатом. */
int  call_init(struct UTUN_INSTANCE* inst);

/* Остановить сессии, отменить таймеры/CM/waiters и снять сервисы роутера перед dm_core_destroy.
 * Сначала закрыть call_headless. Без отправки завершающих команд и событий ENDED; NULL/повтор — no-op.
 * Регистрация call_audio_ops у ядра сохраняется, DSP и устройства frontend здесь не освобождаются. */
void call_destroy(struct UTUN_INSTANCE* inst);

/* uasync: после изменения участников/остановки CHAT-группы завершить затронутые звонки
 * с CONNECT_FAIL, отменить их медиа/CM/повторы отозванным пирам. Остальные группы не затрагивает. */
void call_revalidate_members(struct UTUN_INSTANCE* inst, uint64_t group);

// ── сигналинг (uasync-поток) ──

/* Caller: ch_id — группа либо "dm:<conv_id>" (BGP-группа выбирается при запуске).
 * peer — Ed25519 устройства. В личной беседе NULL или нули выбирают устройство из удостоверения собеседника.
 * out_call_id — ненулевой уникальный uint64, генерит вызывающий.
 * При любом незавершённом обычном звонке/приглашении рации новый ID получает ENDED/BUSY без создания сессии.
 * Уже зарегистрированный ID, включая ENDED до очистки, отклоняется без событий и изменения текущего звонка.
 * После ENDED новый уникальный ID разрешён; результат запуска приходит через CHAT_EVT_CALL_*. */
void call_start  (struct UTUN_INSTANCE* inst, const char* ch_id, const uint8_t peer[32], uint64_t out_call_id);

/* Вызвать все доступные устройства той же проверенной devgroup, что peer, в CHAT-канале.
 * READ не блокирует uasync; сессия сразу занимает линию, отмена/stop не ждут ответа БД.
 * Список фиксируется после READ; peer может быть offline. Остальные условия как у call_start. */
void call_start_devices(struct UTUN_INSTANCE* inst, const char* ch_id, const uint8_t peer[32], uint64_t call_id);
/* Uasync: devices=0 — один peer из RAM, иначе проверенная devgroup через READ worker.
 * После READ применяются текущие допуск/маршруты CHAT, без self; anchor может быть offline.
 * done обязателен, ровно один раз: count>=0 либо -1, при ошибке nodes=NULL; capacity=1..65535, без усечения.
 * nodes — Ed25519 в лексикографическом порядке, заимствованы до возврата done. RAM/отказ могут завершиться синхронно.
 * inst/arg живут до done/закрытия worker; caller перепроверяет свой call_id/запрос при доставке. */
typedef void (*call_targets_done_fn)(void* arg, int count, const uint8_t (*nodes)[32]);
void call_targets_read(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], int devices, unsigned capacity,
    call_targets_done_fn done, void* arg);

// Callee: ответить только RINGING и ждать SELECT инициатора; медиа разрешает последующее CALL_ACCEPTED.
void call_accept (struct UTUN_INSTANCE* inst, uint64_t call_id);

// Callee: отклонить существующую незавершённую сессию, события DECLINED и ENDED с причиной DECLINE.
void call_decline(struct UTUN_INSTANCE* inst, uint64_t call_id);

// Любая сторона: завершить существующую сессию; неизвестный ID/ENDED — no-op.
void call_hangup (struct UTUN_INSTANCE* inst, uint64_t call_id);

// 1 = есть обычная сессия не в ENDED, включая WAIT_DECISION; 0 = свободен/модуль не запущен. Только uasync.
int call_busy(struct UTUN_INSTANCE* inst);

// Caller в WAIT_DECISION: подтвердить force и повторить INVITE с прежним ID/deadline. Только uasync.
void call_continue(struct UTUN_INSTANCE* inst, uint64_t call_id);

// Независимые настройки входящих вызовов; запрет выбранного типа приоритетнее проверки занятости.
enum call_policy {
    CALL_POLICY_PUBLISH_PHONE = 1, CALL_POLICY_WARN_PHONE = 2, CALL_POLICY_REJECT_PHONE = 4,
    CALL_POLICY_BLOCK_CALLS = 8, CALL_POLICY_BLOCK_RADIO = 16
};
/* Живое состояние ОС: 0=неизвестно, 1=свободен, 2=занят; policy — маска call_policy.
 * Только uasync. Изменение статуса публикуется в BGP, если включён PUBLISH_PHONE. */
void call_set_phone_state(struct UTUN_INSTANCE* inst, int state, unsigned policy);

/* Флаги BGP PHONE_KNOWN/BUSY с учётом звонка приложения и разрешения публикации.
 * Desktop без звонка свободен; у mobile состояние ОС может быть неизвестно. Только uasync. */
uint8_t call_phone_status(struct UTUN_INSTANCE* inst);

// ── медиа (релей Opus-кадров, uasync-поток) ──

/* Отправить закодированный Opus-кадр пиру (оборачивает в CALL_MEDIA).
 * Только ACTIVE: len=1..985, opus/timing копируются до возврата; timing=NULL — без capture PTS.
 * seq/ts выставляются автоматически. 0 = принят в локальную FIFO, -1 = неверные данные/нет ACTIVE/ошибка памяти.
 * Результат не подтверждает доставку: FIFO750 удаляет старейший кадр при заполнении, роутер может отказать при drain. */
int call_send_media(struct UTUN_INSTANCE* inst, uint64_t call_id, const uint8_t* opus, int len,
                    const struct call_capture_time* timing);

/* Коллбэк приёма медиа: вызывается в uasync-потоке на каждый Opus-кадр пира.
 * Используется headless-аудиосокетом; opus заимствован до возврата callback. */
typedef void (*call_media_recv_fn)(struct UTUN_INSTANCE* inst, uint64_t call_id,
                                   const uint8_t* opus, int len, void* arg);

/* Один callback в текущем call_ctx, fn=NULL снимает; arg заимствован до замены/destroy.
 * Требует call_init; stop/start чата сбрасывает этот callback. Независим от сохраняемого у ядра audio_ops. */
void call_set_media_recv_cb(struct UTUN_INSTANCE* inst, call_media_recv_fn fn, void* arg);

/* ── аудио-движок (опционально, GUI-сборки) ──
 * Сервисный аудио-контур (codec + джиттер-буфер + тоны) регистрируется через
 * call_set_audio_ops. call.c зовёт on_media на каждый принятый медиа-кадр (с seq и временем подачи пачки в Router)
 * и get_stats для события CALL_STATS. Демон без аудио (headless) — ops не ставит. */

struct call_history_tx;
struct call_audio_ops {
    // uasync: Opus/PTS заимствованы; ts_ms — время пачки, packet_start=1 только у её первого кадра.
    void (*on_media)(struct UTUN_INSTANCE* inst, uint64_t call_id,
                     uint16_t seq, uint32_t ts_ms, const uint8_t* opus, int len, const struct call_capture_time* timing, int packet_start);
    /* uasync-поток: метрики (buffer_ms, tempo_x100=100 для 1x, накопительные dropped/underruns, range).
     * Выходы необязательны. 0 = снимок получен, -1 = нет активного DSP; реализация может потреблять события диагностики. */
    int (*get_stats)(uint64_t call_id, int* buffer_ms, int* tempo_x100,
                     uint32_t* dropped, uint32_t* underruns, struct call_jitter_range* range);
    /* uasync: применить opus_codec_preset (0/1/2 = 16/32/64 кбит/с) к текущему и следующим звонкам.
     * Необязательный callback; 0 = применено, -1 = ошибка, прежний битрейт сохранён. */
    int (*set_codec_preset)(struct UTUN_INSTANCE* inst, int preset);
    /* uasync: диагностическая копия сумм очередей, callback не меняет сетевое управление. */
    void (*record_tx)(uint64_t call_id, const struct call_history_tx* snapshot);
};
/* Только uasync: сохранить заимствованный ops у ядра (NULL снимает регистрацию), допустимо до запуска чата.
 * Объект живёт до снятия/уничтожения ядра. Stop/start чата сохраняет регистрацию, состояние звонка — нет. */
void call_set_audio_ops(struct UTUN_INSTANCE* inst, const struct call_audio_ops* ops);

/* Аргумент call_send_media_trampoline: heap-объект из u_malloc/u_calloc, callback освобождает его через u_free.
 * Постить в inst->ua; inst должен жить до выполнения. Не заменяет эпохи/лимит TX в call_audio.h.
 * len=1..256; timing передаётся в call_send_media как есть (без PTS задать timing.pts_us=INT64_MIN). */
struct call_media_arg {
    struct UTUN_INSTANCE* inst; // Заимствованное ядро, живёт до callback.
    uint64_t call_id; // ID ACTIVE-сессии на момент выполнения callback.
    uint16_t len; // Число заполненных байтов opus, 1..256.
    struct call_capture_time timing; // Capture PTS/epoch/codec delay, копия метаданных кадра.
    uint8_t  opus[256]; // Встроенная копия закодированного кадра.
};
void call_send_media_trampoline(void* arg);

/* Trampoline выполняются в uasync, вызывают одноимённый API и освобождают arg через u_free.
 * arg — соответствующий ненулевой heap-объект, inst живёт до callback. Постановка не удерживает ядро.
 * call_start_arg.ch_id должен содержать завершающий NUL; остальные действия используют call_id_arg. */

struct call_start_arg {
    struct UTUN_INSTANCE* inst; // Заимствованное ядро, живёт до callback.
    char     ch_id[65]; // Ed25519 группы в hex либо dm:<conv_id>, с завершающим NUL.
    uint8_t peer[32]; // Ed25519 собеседника; нулевой ключ допускается для личной беседы.
    uint8_t devices; // 1 — вызвать devgroup указанного устройства, 0 — только устройство.
    uint64_t call_id; // Новый уникальный ненулевой ID, генерирует вызывающий.
};

struct call_id_arg {
    struct UTUN_INSTANCE* inst; // Заимствованное ядро, живёт до callback.
    uint64_t call_id; // ID сессии для accept/decline/hangup/continue.
};

void call_start_trampoline  (void* arg);

void call_accept_trampoline (void* arg);

void call_decline_trampoline(void* arg);

void call_hangup_trampoline (void* arg);

void call_continue_trampoline(void* arg);

#ifdef __cplusplus
}
#endif
#endif /* CALL_H */
