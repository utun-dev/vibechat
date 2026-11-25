/* Общий таймер показа видео, один владелец — worker показа. Изображениями не владеет.
 * poll вызывается для первого готового decoded кадра. queued — все ещё не показанные полные кадры.
 * DROP_ORDER/DROP_LATE освобождают decoded кадр, SHOW удерживает его до presented; WAIT сохраняет голову.
 * Сам модуль очереди не меняет. Encoded входы не пропускаются.
 * received_us ставится на входе RX port, до ожидания worker; все локальные времена — MONOTONIC, не BOOTTIME.
 * SHOW резервирует один кадр до presented(pts, фактическое время); только ACK продвигает часы.
 * Короткое опустошение сохраняет расписание. Темп плавно зависит от суммарного запаса кадров.
 * Невалидные аудиочасы запрещают новый показ, сохраняя decoder/очередь и порядок PTS. */
#ifndef CALL_VIDEO_PLAYOUT_H
#define CALL_VIDEO_PLAYOUT_H
#include <stdint.h>
#include "call_clock.h"
#ifdef __cplusplus
extern "C" {
#endif

struct call_video_playout_frame {
    int64_t pts_us, received_us; // Capture PTS отправителя и локальное время полной RX-сборки, мкс.
    uint32_t interval_us; // 1000000/format.fps; разрыв capture PTS не создаёт повторную паузу.
};
enum call_video_playout_action {
    CALL_VIDEO_PLAYOUT_WAIT, CALL_VIDEO_PLAYOUT_SHOW, CALL_VIDEO_PLAYOUT_DROP_ORDER, CALL_VIDEO_PLAYOUT_DROP_LATE
};
struct call_video_playout_event;
struct call_video_playout {
    uint64_t call_id; // Для диагностики; не фильтрует входные кадры по звонку.
    int anchored, audio_valid; // Есть фаза подтверждённого показа / последний clock был valid.
    int64_t last_pts, last_deadline, deadline_us; // Последний выбранный/пропущенный PTS, фаза и срок следующего poll.
    int64_t report_us, last_show_us, interval_min, interval_max, av_min, av_max, dispatch_max, data_shift;
    int64_t last_show_pts, last_received, gap_pts, gap_received, gap_dispatch;
    uint32_t speed_ppm; // Темп аудио + коррекция отставания, максимум 1.6x.
    unsigned shown, order, late, audio_wait, clock_wait;
    int64_t recovery_us, recovery_lag; // Окно проверки: ускоренный показ действительно уменьшает отставание?
    int skipping; // Только показ decoded кадров при устойчивом долге, который не удаётся сократить темпом.
    unsigned source_gaps;
    int64_t source_gap_max; // Большие разрывы capture PTS, уже прошедшие до показа.
    struct call_video_playout_frame pending; // Единственный выбранный кадр до подтверждения потребителем.
    int awaiting; // Есть SHOW без соответствующего presented; новые SHOW запрещены.
    int64_t selected_us, pending_av, head_pts, head_due_us, lead_us, rate_us;
    int64_t head_show_us; // Плановая презентация головы; GUI ACK корректирует фазу плавно.
    unsigned queued, reserve; // reserve — будущие кадры сверх головы, включая необходимый pipeline backend.
    // NULL пишет DEBUG напрямую; timer устанавливает неблокирующий callback с копированием события.
    void (*log_event)(void *, const struct call_video_playout_event *);
    void *log_arg;
};
enum call_video_playout_event_kind {
    CALL_VIDEO_LOG_REPORT, CALL_VIDEO_LOG_CLOCK, CALL_VIDEO_LOG_ORDER, CALL_VIDEO_LOG_GAP, CALL_VIDEO_LOG_ACK, CALL_VIDEO_LOG_RECOVERY
};
struct call_video_playout_event {
    enum call_video_playout_event_kind kind;
    struct call_video_playout state; // Снимок принадлежит событию, указатели logger не разыменовываются reporter.
    struct call_video_playout_frame frame;
    int64_t now_us;
};
/* Reporter вне timer: форматирует и выводит сохранённое событие через DEBUG категории call. */
void call_video_playout_log_event(const struct call_video_playout_event *event);

/* Локальное MONOTONIC время, мкс (Windows использует get_time_us); не capture PTS удалённого узла. */
int64_t call_video_playout_now_us(void);
/* Обнулить состояние и logger, задать id. p обязателен; вызывать до первого poll/после остановки владельца. */
void call_video_playout_init(struct call_video_playout *p, uint64_t call_id);
/* frame==NULL обновляет состояние без снятия временной привязки.
 * deadline_us — абсолютный срок при WAIT, проверять аудиочасы не реже чем раз в 5мс.
 * queued учитывает encoded, decoder, decoded и GUI каждый ровно один раз; reserve обычно 1.
 * p обязателен, now_us — неубывающий MONOTONIC; NULL/invalid audio означает WAIT.
 * Некорректный PTS/received/interval даёт DROP_ORDER только при valid audio и отсутствии awaiting.
 * source_epoch не участвует: коррекция аудио не означает разрыв видеокодека. */
enum call_video_playout_action call_video_playout_poll(struct call_video_playout *p,
    const struct call_video_playout_frame *frame, int64_t now_us, const struct call_clock_snapshot *audio,
    unsigned queued, unsigned reserve);
/* Подтверждение выбранного PTS в MONOTONIC, не раньше выбора. Повторный/чужой ACK логируется и игнорируется. */
void call_video_playout_presented(struct call_video_playout *p, int64_t pts_us, int64_t now_us);
#ifdef __cplusplus
}
#endif
#endif
