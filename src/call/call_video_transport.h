/* Видеотранспорт звонка: uasync-поток, отдельные control/audio/video router channels.
 * Callback не вызывает изменяющий API звонка реентерабельно; команды worker возвращает через post.
 * Callback копирует нужные данные до возврата. При stop callbacks больше не вызываются. */
#ifndef CALL_VIDEO_TRANSPORT_H
#define CALL_VIDEO_TRANSPORT_H
#include "call_video.h"
#include "call_video_rate.h"
struct UTUN_INSTANCE;
struct call_session;
#ifdef __cplusplus
extern "C" {
#endif
struct call_video_ops {
    // peer/frame заимствованы на время callback; frame — полный собранный encoded кадр.
    void (*capabilities)(void *arg, const struct call_video_caps *peer);
    // 1 — есть место для полного кадра bytes, 0 — router удерживает первый фрагмент/ACK до resume.
    int (*receive_ready)(void *arg, uint32_t bytes);
    void (*frame)(void *arg, const struct call_video_frame *frame);
    void (*need_key)(void *arg);
    void (*bitrate)(void *arg, uint64_t generation, uint64_t request, uint32_t bps);
    void (*decoded)(void *arg, uint64_t generation, int accepted);
    void (*writable)(void *arg, int admit); // Разрешение новых входов encoder; также будит retry сохранённой пачки.
};
/* Настройка активного звонка, включая receive-only. При ops обязательны frame/receive_ready;
 * NULL ops снимает callbacks, обнуляет локальные CAPS/регулятор и очищает TX/частичную сборку RX.
 * caps обязательны даже при NULL ops; отражают только реально проверенные профили.
 * 0 — CAPS поставлены в router, -1 — ошибка; при отказе отправки настройка уже может быть применена.
 * CAPS повторяются до прикладного CAPS_ACK; успешный configure ещё не разрешает TX. */
int call_video_configure(struct UTUN_INSTANCE *inst, uint64_t call_id, const struct call_video_caps *caps,
                         const struct call_video_ops *ops, void *arg);
enum { CALL_VIDEO_SEND_NEEDS_KEY = -2 };
/* Копирует один полный кадр. 1 принят, 0 временная занятость, -1 неверный формат/сессия.
 * CALL_VIDEO_SEND_NEEDS_KEY — пропустить зависимый кадр и проверить следующий кадр из очереди.
 * После 0 producer сохраняет кадр и повторяет с теми же ID/PTS, не запрашивая KEY.
 * FIFO видеоканала: до 32 кадров, 500мс PTS и CALL_VIDEO_MAX_FRAME байт суммарно.
 * При заторе роутера подача новых входов приостанавливается, очередь и позиция отправки сохраняются. */
int call_video_send_frame(struct UTUN_INSTANCE *inst, const struct call_video_frame *frame);
/* Повторить доставку router RX после освобождения места платформой; чужой/закрытый id игнорируется. */
void call_video_resume_receive(struct UTUN_INSTANCE *inst, uint64_t call_id);
/* Decoder потерял references. Запрос отправляется не чаще раза в 500мс и сохраняется до постановки в control-канал. */
void call_video_request_key(struct UTUN_INSTANCE *inst, uint64_t call_id);
/* Результат последнего полностью полученного поколения, accepted=0/1, 0/-1.
 * Отправка control при необходимости повторяется timer, отдельно от потери references. */
int call_video_decode_result(struct UTUN_INSTANCE *inst, uint64_t call_id, uint64_t generation, int accepted);
/* После открытия encoder; format — согласованный и фактически проверенный режим.
 * maximum_bps — дополнительный предел регулятора, не выше format.maximum_bps;
 * applied_bps может быть выше maximum_bps (нужно снижение), но не выше envelope.
 * Пользовательский min:max берётся из call_video_bitrate_range_kbps каждые 50мс.
 * Верхняя граница ограничивается codec cap; при отсутствии пересечения admission приостановлен.
 * Новый codec cap требует повторной настройки; сама смена диапазона не сбрасывает команду в работе.
 * Callback bitrate только ставит команду worker в очередь. Подтверждение — отдельным post.
 * applied=0 в rate_applied означает отказ encoder, выключает регулятор до configure_rate. */
int call_video_configure_rate(struct UTUN_INSTANCE *inst, uint64_t call_id, const struct call_video_format *format,
                              uint32_t applied_bps, uint32_t maximum_bps);
/* 0 — ненулевой результат принят; -1 — stale/неверный результат либо обработанный отказ applied_bps=0. */
int call_video_rate_applied(struct UTUN_INSTANCE *inst, uint64_t call_id, uint64_t generation,
                            uint64_t request, uint32_t applied_bps);

/* Только call.c: bind 0/-1 регистрирует два router-сервиса; unbind снимает их.
 * Stop отменяет timer/продолжение/send waiter и освобождает transport сессии. */
int call_video_transport_bind(struct UTUN_INSTANCE *inst);
void call_video_transport_unbind(struct UTUN_INSTANCE *inst);
void call_video_transport_stop(struct call_session *session);
#ifdef __cplusplus
}
#endif
#endif
