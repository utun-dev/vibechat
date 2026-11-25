/* call_video_port — обмен готовыми видеокадрами между платформенными workers и ядром звонка.
 * Attach подключает ядро; для активного call_id: open → send/pull/status → close. Перед отправкой настроить encoder через format.
 * Attach/detach — uasync; остальные API потокобезопасны. До detach остановить пользователей порта
 * и снять transport callbacks через close с обработкой команды либо остановку сессии звонка.
 * Send копирует кадры; pull передаёт буфер вызывающему, освобождение — u_free(). Команды исполняются в uasync.
 * В процессе один порт: TX до восьми кадров по 2 МиБ, RX до восьми кадров и 4 МиБ суммарно. */
#ifndef CALL_VIDEO_PORT_H
#define CALL_VIDEO_PORT_H
#include "call_video.h"
struct UTUN_INSTANCE;
#ifdef __cplusplus
extern "C" {
#endif
struct call_video_port_status {
    struct call_video_caps peer; // принятые возможности кодеков пира
    uint64_t key_serial, rate_generation, rate_request, configured_generation; // счётчик запросов KEY, ID команды битрейта и настроенное поколение
    uint32_t rate_bps; // запрошенный кодеру битрейт, бит/с; результат передать через applied
    uint32_t minimum_bps, maximum_bps; // действующий диапазон регулятора, бит/с
    int have_peer, error; // возможности пира получены / настройка завершилась ошибкой
    int can_encode; // Новые raw входы допустимы; готовый TX всё равно сохраняется и повторяется.
    int tx_idle; // Mailbox свободен; новый формат не обгонит кадры прежнего поколения в mailbox.
    uint64_t decoded_generation; // поколение, для которого удалённый decoder сообщил результат
    int decoded_accepted; // 1 — удалённый decoder принял поколение, 0 — отказ
    unsigned rx_frames; // Полные encoded кадры порта; pending decoder/GUI учитывает RX worker.
};
// ── Привязка к ядру ──

// Подключить единственное ядро к порту; 0/-1. Повторный attach, даже того же instance, — ошибка.
int call_video_port_attach(struct UTUN_INSTANCE *inst);

// Отменить ожидающий post и освободить очереди; выполнять до уничтожения ядра.
void call_video_port_detach(struct UTUN_INSTANCE *inst);

// ── Открытие и закрытие видеотрека ──

// Скопировать CAPS и запланировать настройку; 0/-1. Active-call проверяется асинхронно.
// Повтор open того же id сохраняет RX/TX/счётчики/wake; другой id очищает их.
int call_video_port_open(uint64_t id, const struct call_video_caps *caps);

// Закрыть текущий id и очистить его очереди; чужой id игнорируется.
void call_video_port_close(uint64_t id);

// ── Состояние и уведомления ──

// Скопировать текущее состояние; 0 — снимок получен, -1 — нет активного id/ядра или out.
int call_video_port_status(uint64_t id, struct call_video_port_status *out);

/* Неблокирующий накопительный счётчик полных принятых RX кадров текущего звонка.
 * Pull не уменьшает его. Для общего запаса вычитаются показанные/отклонённые кадры.
 * 0 для закрытого/другого звонка; новый open после остановки readers начинает с нуля. */
uint64_t call_video_port_received(uint64_t id);

/* Уведомление RX без обращения к port API из callback; вызывается под mutex порта.
 * 1 — установлен, 0 — звонок ещё не открыт/уже закрыт. NULL снимает callback до освобождения arg. */
int call_video_port_set_wake(uint64_t id, void (*wake)(void *), void *arg);

// ── Настройка кодера и исходящие кадры ──

// Передать формат/битрейт открытого encoder; 0 — команда принята, -1 — отказ.
// Перед сменой формата владелец при необходимости ждёт tx_idle; сама команда не ждёт drain.
// Итог проверять по status.error/configured_generation; 0 не подтверждает настройку транспорта.
int call_video_port_format(uint64_t id, const struct call_video_format *format, uint32_t bps);

// Подтвердить команду status.rate_generation/rate_request: bps — применённый битрейт, 0 — отказ encoder. 0/-1.
int call_video_port_applied(uint64_t id, uint64_t generation, uint64_t request, uint32_t bps);

#define CALL_VIDEO_PORT_TX_FRAMES 8u
#define CALL_VIDEO_PORT_TX_BYTES (CALL_VIDEO_PORT_TX_FRAMES * CALL_VIDEO_MAX_FRAME)
/* Атомарно копирует готовую пачку одного звонка/формата с возрастающими frame_id/PTS.
 * 1 — принята целиком, 0 — mailbox занят,
 * -1 — ошибка; на 0/-1 ни один кадр не принят. count: 1..CALL_VIDEO_PORT_TX_FRAMES.
 * Входные кадры всегда остаются у вызывающего. При 0 повторить ту же пачку после освобождения порта.
 * 1 подтверждает копирование в mailbox; transport проверяет формат/цепочку позже.
 * Transport busy сохраняет непринятый хвост, ошибка освобождает его и увеличивает key_serial. */
int call_video_port_send(const struct call_video_frame *frames, unsigned count);

// ── Входящие кадры ──

// Забрать старейший полный RX-кадр: 1 — получен, out->data освободить через u_free; 0 — кадра нет, out не меняется.
int call_video_port_pull(uint64_t id, struct call_video_frame *out);

// ── Управление качеством и восстановлением ──

// Запланировать запрос нового KEY удалённому encoder после потери опорных кадров.
void call_video_port_request_key(uint64_t id);

// Задать диапазон битрейта в бит/с: 300000..4000000, кратно 1000; 0 — команда принята, -1 — отказ.
int call_video_port_range(uint64_t id, uint32_t minimum_bps, uint32_t maximum_bps);

// Сообщить результат запуска decoder поколения generation: accepted=0/1; 0 — команда принята, -1 — отказ.
int call_video_port_decoded(uint64_t id, uint64_t generation, int accepted);

#ifdef __cplusplus
}
#endif
#endif
