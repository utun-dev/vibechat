#include <pthread.h>
#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include "stream_audio_meter.h"

struct stream_audio_meter {
    pthread_mutex_t lock; /* Только счётчики; под lock нет аллокаций, вывода или вызовов ядра. */
    unsigned refs;
    int64_t ready_us;
    uint64_t observed, missing, starts, peak, current; /* Кадры выхода; starts — число эпизодов. */
};

struct stream_audio_meter *stream_audio_meter_create(void) {
    struct stream_audio_meter *m = u_calloc(1, sizeof(*m));
    if (!m) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "audio meter allocation failed"); return NULL; }
    int err = pthread_mutex_init(&m->lock, NULL);
    if (err) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "audio meter mutex failed error=%d", err); u_free(m); return NULL; }
    m->ready_us = -1; m->refs = 1;
    return m;
}

struct stream_audio_meter *stream_audio_meter_retain(struct stream_audio_meter *m) {
    if (m) { pthread_mutex_lock(&m->lock); m->refs++; pthread_mutex_unlock(&m->lock); }
    return m;
}

void stream_audio_meter_release(struct stream_audio_meter *m) {
    if (!m) return;
    pthread_mutex_lock(&m->lock);
    unsigned refs = --m->refs;
    pthread_mutex_unlock(&m->lock);
    if (!refs) { pthread_mutex_destroy(&m->lock); u_free(m); }
}

void stream_audio_meter_ready(struct stream_audio_meter *m, int64_t end_us) {
    if (!m) return;
    pthread_mutex_lock(&m->lock);
    m->ready_us = end_us;
    if (end_us < 0) m->current = 0;
    pthread_mutex_unlock(&m->lock);
}

void stream_audio_meter_reset(struct stream_audio_meter *m) {
    if (!m) return;
    pthread_mutex_lock(&m->lock); m->current = 0; pthread_mutex_unlock(&m->lock);
}

void stream_audio_meter_output(struct stream_audio_meter *m, int64_t fed_us, unsigned pcm, unsigned missing, int underrun) {
    if (!m) return;
    pthread_mutex_lock(&m->lock);
    if (m->ready_us >= 0) {
        m->observed += pcm + missing;
        if (pcm) m->current = 0;
        if (missing && (m->current || (underrun && fed_us >= m->ready_us))) {
            if (!m->current) m->starts++;
            m->current += missing; m->missing += missing;
            if (m->current > m->peak) m->peak = m->current;
        }
    }
    pthread_mutex_unlock(&m->lock);
}

void stream_audio_meter_collect(struct stream_audio_meter *m, struct stream_stat_sample *report, int active) {
    if (!m) return;
    pthread_mutex_lock(&m->lock);
    /* Округляем вверх: даже пропуск части одного callback виден в миллисекундах. */
    report->v[ST_AUDIO_OBSERVED] += (m->observed + 47) / 48;
    report->v[ST_PAUSE_MS] += (m->missing + 47) / 48;
    report->v[ST_PAUSES] += m->starts;
    uint64_t peak_ms = (m->peak + 47) / 48;
    if (peak_ms > report->v[ST_PAUSE_MAX]) report->v[ST_PAUSE_MAX] = peak_ms;
    if (active && m->observed && m->current) report->v[ST_PAUSED] = 1;
    m->observed = m->missing = m->starts = m->peak = 0;
    pthread_mutex_unlock(&m->lock);
}
