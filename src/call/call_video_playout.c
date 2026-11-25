#include "call_video_playout.h"
#include "../../lib/debug_config.h"
#include "../../lib/u_async.h"
#include <string.h>
#include <time.h>
#include <limits.h>

#define VIDEO_AHEAD_US 40000
#define VIDEO_MAX_SPEED_PPM 1600000

int64_t call_video_playout_now_us(void) {
#ifdef _WIN32
    return (int64_t)get_time_us();
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
#endif
}

void call_video_playout_init(struct call_video_playout *p, uint64_t call_id) {
    memset(p, 0, sizeof(*p)); p->call_id = call_id; p->last_pts = p->head_pts = -1;
    p->interval_min = p->av_min = INT64_MAX; p->av_max = INT64_MIN;
    p->speed_ppm = 1000000;
    DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video playout init call=%llu presentation_clock=1 max_speed_ppm=%d ahead_us=%d",
        (unsigned long long)call_id, VIDEO_MAX_SPEED_PPM, VIDEO_AHEAD_US);
}

void call_video_playout_log_event(const struct call_video_playout_event *event) {
    const struct call_video_playout *p = &event->state;
    const struct call_video_playout_frame *f = &event->frame;
    int64_t now = event->now_us;
    if (event->kind == CALL_VIDEO_LOG_CLOCK) {
        DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video playout clock call=%llu valid=%d pts=%lld",
            (unsigned long long)p->call_id, p->audio_valid, (long long)f->pts_us); return;
    }
    if (event->kind == CALL_VIDEO_LOG_ORDER) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "video playout invalid/order call=%llu pts=%lld last=%lld received=%lld now=%lld interval_us=%u",
            (unsigned long long)p->call_id, (long long)f->pts_us, (long long)p->last_pts,
            (long long)f->received_us, (long long)now, f->interval_us); return;
    }
    if (event->kind == CALL_VIDEO_LOG_GAP) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL,
            "video source gap call=%llu pts_delta_us=%lld interval_us=%u av_us=%lld display_step_us=%lld; no extra gap wait",
            (unsigned long long)p->call_id, (long long)(f->pts_us - p->last_pts), f->interval_us,
            (long long)p->pending_av, (long long)((int64_t)f->interval_us * 1000000 / p->speed_ppm)); return;
    }
    if (event->kind == CALL_VIDEO_LOG_ACK) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "video presentation ACK rejected call=%llu pts=%lld expected=%lld awaiting=%d now=%lld selected=%lld",
            (unsigned long long)p->call_id, (long long)f->pts_us, (long long)p->pending.pts_us, p->awaiting,
            (long long)now, (long long)p->selected_us); return;
    }
    if (event->kind == CALL_VIDEO_LOG_RECOVERY) {
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "video display catchup call=%llu lag_us=%lld recovered_us=%lld window_us=%lld queued=%u reserve=%u; decoded presentation only",
            (unsigned long long)p->call_id, (long long)-p->pending_av, (long long)(p->recovery_lag + p->pending_av),
            (long long)(now - p->recovery_us), p->queued, p->reserve); return;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CALL,
        "video playout call=%llu interval_us=%lld shown=%u speed_ppm=%u drop_order=%u drop_display=%u "
        "cadence_us=%lld..%lld gap_pts_us=%lld gap_rx_us=%lld gap_dispatch_us=%lld "
        "av_us=%lld..%lld dispatch_max_us=%lld data_shift_us=%lld audio_wait=%u clock_wait=%u source_gaps=%u source_gap_max_us=%lld "
        "queued=%u reserve=%u awaiting=%d lead_us=%lld",
        (unsigned long long)p->call_id, (long long)(now - p->report_us), p->shown, p->speed_ppm, p->order, p->late,
        (long long)(p->interval_min == INT64_MAX ? 0 : p->interval_min), (long long)p->interval_max,
        (long long)p->gap_pts, (long long)p->gap_received, (long long)p->gap_dispatch,
        (long long)(p->shown ? p->av_min : 0), (long long)(p->shown ? p->av_max : 0),
        (long long)p->dispatch_max, (long long)p->data_shift, p->audio_wait, p->clock_wait,
        p->source_gaps, (long long)p->source_gap_max, p->queued, p->reserve, p->awaiting, (long long)p->lead_us);
}

static void log_event(struct call_video_playout *p, enum call_video_playout_event_kind kind,
    const struct call_video_playout_frame *frame, int64_t now) {
    struct call_video_playout_event event = {.kind = kind, .state = *p, .now_us = now};
    if (frame) event.frame = *frame;
    if (p->log_event) p->log_event(p->log_arg, &event);
    else call_video_playout_log_event(&event);
}

/* Сводка агрегирует штатные ожидания; timer лишь копирует событие в ограниченную очередь. */
static void report(struct call_video_playout *p, int64_t now) {
    if (!p->report_us) p->report_us = now;
    if (now - p->report_us < 1000000) return;
    log_event(p, CALL_VIDEO_LOG_REPORT, NULL, now);
    p->report_us = now; p->shown = p->order = p->late = p->audio_wait = p->clock_wait = 0;
    p->interval_min = p->av_min = INT64_MAX; p->av_max = INT64_MIN;
    p->interval_max = p->dispatch_max = p->data_shift = 0;
    p->gap_pts = p->gap_received = p->gap_dispatch = 0;
    p->source_gaps = 0; p->source_gap_max = 0;
}

enum call_video_playout_action call_video_playout_poll(struct call_video_playout *p,
    const struct call_video_playout_frame *f, int64_t now, const struct call_clock_snapshot *clock,
    unsigned queued, unsigned reserve) {
    int audio_valid = clock && clock->valid;
    int64_t audio = clock ? clock->pts_us : -1;
    p->queued = queued; p->reserve = reserve;
    report(p, now); p->deadline_us = now + 5000;
    if (p->audio_valid != !!audio_valid) {
        p->audio_valid = !!audio_valid;
        struct call_video_playout_frame changed = {.pts_us = audio};
        log_event(p, CALL_VIDEO_LOG_CLOCK, &changed, now);
    }
    if (!audio_valid) { p->head_pts = -1; p->recovery_us = 0; p->skipping = 0; p->clock_wait++; return CALL_VIDEO_PLAYOUT_WAIT; }
    // Пустой FIFO не означает разрыв часов; срок сохраняется до появления следующей головы.
    if (!f || p->awaiting) return CALL_VIDEO_PLAYOUT_WAIT;
    if (f->pts_us <= p->last_pts || f->received_us < 0 || f->received_us > now
        || f->interval_us < 16666 || f->interval_us > 1000000) {
        log_event(p, CALL_VIDEO_LOG_ORDER, f, now);
        p->order++; return CALL_VIDEO_PLAYOUT_DROP_ORDER;
    }
    // Небольшой рабочий запас сохраняем; ускорение снимает избыток всего конвейера, а не только decoded FIFO.
    int64_t av = f->pts_us - audio;
    uint32_t base = clock->rate_ppm > 1000000 ? clock->rate_ppm : 1000000;
    unsigned target_frames = (reserve < 8 ? reserve : 8) + 1;
    const int64_t working_lag = (int64_t)target_frames * f->interval_us + 50000;
    // Сначала даём ускорению 500 мс. Пропуск показа нужен лишь когда долг не сокращается
    // (например, вход 60 fps и экран 60 Hz). Небольшие паузы сохраняют все кадры.
    if (-av <= working_lag || queued <= target_frames + 1) {
        p->recovery_us = 0; p->skipping = 0;
    } else {
        if (!p->recovery_us) { p->recovery_us = now; p->recovery_lag = -av; }
        else if (now - p->recovery_us >= 500000) {
            if (p->recovery_lag + av < f->interval_us / 4 && !p->skipping) {
                p->skipping = 1; p->pending_av = av; log_event(p, CALL_VIDEO_LOG_RECOVERY, f, now);
            }
            p->recovery_us = now; p->recovery_lag = -av;
        }
    }
    if (p->skipping) {
        p->last_pts = f->pts_us; p->head_pts = -1; p->late++;
        return CALL_VIDEO_PLAYOUT_DROP_LATE; // Часы last_show/last_deadline меняются исключительно по ACK.
    }
    int64_t target = base;
    if (queued > target_frames) target += (int64_t)(queued - target_frames) * 100000;
    else if (queued < target_frames) target -= 20000; // До одного будущего кадра набираем запас без стартовой паузы.
    // Слабая A/V-коррекция видит скрытый сетевой долг, но не съедает рабочий запас.
    int64_t lag = -av - (int64_t)(target_frames + 1) * f->interval_us - 100000;
    if (lag > 0) target += lag > 1000000 ? 50000 : lag / 20;
    if (target > VIDEO_MAX_SPEED_PPM) target = VIDEO_MAX_SPEED_PPM;
    int64_t elapsed = p->rate_us ? now - p->rate_us : 0;
    if (elapsed > 100000) elapsed = 100000;
    if (elapsed > 0) p->speed_ppm = (uint32_t)((int64_t)p->speed_ppm + (target - p->speed_ppm) * elapsed / (200000 + elapsed));
    p->rate_us = now;
    int64_t delta = p->last_pts >= 0 ? f->pts_us - p->last_pts : 0;
    int64_t step = (int64_t)f->interval_us * 1000000 / p->speed_ppm;
    if (p->head_pts != f->pts_us) {
        p->head_pts = f->pts_us;
        p->head_show_us = p->anchored ? p->last_deadline + step : now;
        // После опоздания сохраняем все кадры, но не устраиваем мгновенный burst.
        int64_t earliest = p->last_show_us + (int64_t)f->interval_us * 1000000 / VIDEO_MAX_SPEED_PPM;
        if (p->anchored && p->head_show_us < earliest) p->head_show_us = earliest;
        p->head_due_us = p->head_show_us - (p->anchored ? p->lead_us : 0);
    }
    int64_t deadline = p->head_due_us;
    p->deadline_us = deadline;
    if (av > VIDEO_AHEAD_US) {
        p->deadline_us = now + 5000; p->audio_wait++;
        return CALL_VIDEO_PLAYOUT_WAIT;
    }
    if (deadline > now) return CALL_VIDEO_PLAYOUT_WAIT;
    if (delta > (int64_t)f->interval_us * 3) {
        p->pending_av = av;
        if (!p->source_gaps) log_event(p, CALL_VIDEO_LOG_GAP, f, now);
        p->source_gaps++;
        if (delta > p->source_gap_max) p->source_gap_max = delta;
    }
    p->last_pts = f->pts_us; p->pending = *f; p->awaiting = 1; p->selected_us = now; p->pending_av = av;
    if (now - deadline > p->dispatch_max) p->dispatch_max = now - deadline;
    return CALL_VIDEO_PLAYOUT_SHOW;
}

void call_video_playout_presented(struct call_video_playout *p, int64_t pts, int64_t now) {
    if (!p->awaiting || p->pending.pts_us != pts || now < p->selected_us) {
        struct call_video_playout_frame rejected = {.pts_us = pts};
        log_event(p, CALL_VIDEO_LOG_ACK, &rejected, now); return;
    }
    const struct call_video_playout_frame *f = &p->pending;
    int64_t av = p->pending_av, lead = now - p->selected_us;
    // GUI freeze не становится постоянным опережением dispatch для последующих кадров.
    // ACK TextureView/композитора может занимать целый период экрана. Его надо вычесть
    // из dispatch следующего кадра; иначе один ACK на 60 Hz искусственно снижает FPS.
    if (lead <= f->interval_us + f->interval_us / 4 + 1000) p->lead_us = (p->lead_us * 7 + lead) / 8;
    int64_t error = now - p->head_show_us;
    if (!p->anchored || error > f->interval_us / 2 || error < -(int64_t)f->interval_us / 2) p->last_deadline = now;
    else {
        int64_t correction = error / 8;
        if (correction > 1000) correction = 1000;
        if (correction < -1000) correction = -1000;
        p->last_deadline = p->head_show_us + correction;
    }
    p->awaiting = 0; p->anchored = 1; p->head_pts = -1;
    {
        if (av < p->av_min) p->av_min = av;
        if (av > p->av_max) p->av_max = av;
        if (p->last_show_us) {
            int64_t gap = now - p->last_show_us;
            if (gap < p->interval_min) p->interval_min = gap;
            if (gap > p->interval_max) {
                p->interval_max = gap; p->gap_pts = f->pts_us - p->last_show_pts;
                p->gap_received = f->received_us - p->last_received; p->gap_dispatch = lead;
            }
        }
        p->last_show_us = now; p->last_show_pts = f->pts_us; p->last_received = f->received_us; p->shown++;
    }
}
