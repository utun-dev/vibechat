#include <string.h>

#include "../../lib/debug_config.h"
#include "stream_playback_clock.h"

unsigned stream_playback_prefill_ms(int mode) { return mode == STREAM_PLAYBACK_LOW_LATENCY ? 250 : 750; }
int64_t stream_playback_ahead_us(int mode) { return mode == STREAM_PLAYBACK_LOW_LATENCY ? 500000 : 1000000; }

int stream_playback_clock_set_mode(struct stream_playback_clock *c, int mode) {
    if (!c || (mode != STREAM_PLAYBACK_LOW_LATENCY && mode != STREAM_PLAYBACK_LOW_GLITCHES)) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "playback invalid mode=%d", mode); return -1;
    }
    if (c->mode == mode) return 0;
    c->mode = mode; c->tempo_milli = 1000; c->ramp_remainder = 0;
    c->prefilling = mode == STREAM_PLAYBACK_LOW_GLITCHES;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "playback mode id=%llu mode=%s history_ms=%d extra_guard_ms=%d",
        (unsigned long long)c->id, mode ? "low_glitches" : "low_latency", mode ? 30000 : 200, mode ? 500 : 0);
    return 0;
}

int stream_playback_clock_can_present(struct stream_playback_clock *c, int64_t reserve, int64_t guard, int ended) {
    if (!c) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "playback prefill: null clock"); return 0; }
    if (ended || !c->mode || reserve >= guard + 500000) c->prefilling = 0;
    return !c->prefilling;
}

const char *stream_playback_reason_name(enum stream_playback_reason reason) {
    switch (reason) {
    case STREAM_PLAYBACK_STARVED: return "starved";
    case STREAM_PLAYBACK_HISTORY_WAIT: return "history";
    case STREAM_PLAYBACK_COOLDOWN: return "cooldown";
    case STREAM_PLAYBACK_RESERVE: return "reserve";
    case STREAM_PLAYBACK_CATCHUP: return "catchup";
    }
    DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "playback invalid diagnostic reason=%d", reason);
    return "invalid";
}

/* Новая временная шкала не наследует минимумы прежней позиции. */
void stream_playback_clock_reset(struct stream_playback_clock *c, uint64_t id) {
    if (!c) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "playback clock reset: null clock id=%llu", (unsigned long long)id); return; }
    memset(c, 0, sizeof(*c));
    c->id = id; c->tempo_milli = 1000; c->mode = STREAM_PLAYBACK_LOW_GLITCHES; c->prefilling = 1;
    for (unsigned i = 0; i < STREAM_PLAYBACK_HISTORY; i++) c->history[i].period = -1;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "playback clock reset id=%llu mode=low_glitches history=30s extra_guard=500ms tempo=1.0..1.6", (unsigned long long)id);
}

/* Смещение обновляется по реально воспроизведённому материалу, а не по запрошенному темпу. */
int stream_playback_clock_update(struct stream_playback_clock *c, int64_t now, int64_t position,
                                 int64_t ready, int64_t guard, int64_t pending_gain,
                                 int starved, struct stream_playback_state *state) {
    if (!c || !state || now < 0 || position < 0 || ready < 0 || guard < 0 || guard > 60000000
        || pending_gain < 0 || pending_gain > 60000000 || (c->initialized && (now < c->last_us || position < c->last_position_us))) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "playback clock invalid id=%llu now=%lld pts=%lld ready=%lld guard=%lld pending=%lld",
                    c ? (unsigned long long)c->id : 0, (long long)now, (long long)position, (long long)ready,
                    (long long)guard, (long long)pending_gain);
        return -1;
    }
    int64_t elapsed = c->initialized ? now - c->last_us : 0;
    if (!c->initialized) { c->initialized = 1; c->first_us = c->last_bad_us = now; }
    else {
        int64_t advanced = position - c->last_position_us;
        if (advanced > elapsed * 2 + 1000000) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "playback clock position discontinuity id=%llu wall=%lld media=%lld; reset required",
                        (unsigned long long)c->id, (long long)elapsed, (long long)advanced);
            return -1;
        }
        c->correction_us += elapsed - advanced;
        if (elapsed > 1000000) {
            c->last_bad_us = now;
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "playback clock sampling gap id=%llu interval_us=%lld; restart observation window",
                       (unsigned long long)c->id, (long long)elapsed);
        }
    }
    if (starved || ready < position) {
        c->last_bad_us = now;
        if (!c->waiting) {
            c->pause_us = now; c->pauses++;
            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "playback waits id=%llu pts=%lld ready=%lld pauses=%llu",
                       (unsigned long long)c->id, (long long)position, (long long)ready, (unsigned long long)c->pauses);
        }
        c->waiting = 1; c->prefilling = c->mode == STREAM_PLAYBACK_LOW_GLITCHES;
    } else if (c->waiting) {
        c->last_bad_us = now; c->waiting = 0;
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "playback resumes id=%llu pause_us=%lld reserve_us=%lld correction_us=%lld",
                   (unsigned long long)c->id, (long long)(now - c->pause_us),
                   (long long)(ready - position), (long long)c->correction_us);
    }
    int64_t period = now / STREAM_PLAYBACK_BUCKET_US, reserve = ready - position;
    struct stream_playback_bucket *bucket = &c->history[period % STREAM_PLAYBACK_HISTORY];
    int64_t normalized = reserve - c->correction_us;
    if (bucket->period != period) { bucket->period = period; bucket->minimum_us = normalized; }
    else if (normalized < bucket->minimum_us) bucket->minimum_us = normalized;
    int64_t minimum = normalized;
    unsigned samples = 0;
    int64_t window = c->mode == STREAM_PLAYBACK_LOW_GLITCHES ? STREAM_PLAYBACK_HISTORY : STREAM_PLAYBACK_SHORT_HISTORY;
    for (unsigned i = 0; i < STREAM_PLAYBACK_HISTORY; i++) {
        const struct stream_playback_bucket *b = &c->history[i];
        if (b->period < 0 || b->period > period || period - b->period >= window) continue;
        if (b->minimum_us < minimum) minimum = b->minimum_us;
        if (period - b->period < STREAM_PLAYBACK_SHORT_HISTORY) samples++;
    }
    minimum += c->correction_us;
    int64_t stable = now - c->last_bad_us;
    int complete = samples == STREAM_PLAYBACK_SHORT_HISTORY && now - c->first_us >= STREAM_PLAYBACK_SETTLE_US;
    if (c->mode == STREAM_PLAYBACK_LOW_GLITCHES) guard += 500000;
    int64_t removable = minimum - guard - pending_gain;
    if (removable < 0) removable = 0;
    /* Уже подготовленный PCM и тормозной путь не должны съесть защищённый запас. */
    int excess = c->tempo_milli - 1000;
    int range = STREAM_PLAYBACK_MAX_TEMPO - 1000;
    int64_t braking = (int64_t)excess * excess * STREAM_PLAYBACK_RAMP_US / (2 * range * 1000);
    int target = 1000;
    if (complete && stable >= STREAM_PLAYBACK_SETTLE_US && !c->waiting && removable > braking) {
        int64_t allowance = removable - braking;
        int increase = allowance >= range * 2000 ? range : (int)(allowance / 2000);
        target += increase;
    }
    c->ramp_remainder += (elapsed > STREAM_PLAYBACK_RAMP_US ? STREAM_PLAYBACK_RAMP_US : elapsed) * range;
    int step = (int)(c->ramp_remainder / STREAM_PLAYBACK_RAMP_US);
    c->ramp_remainder %= STREAM_PLAYBACK_RAMP_US;
    if (target > c->tempo_milli) c->tempo_milli += target - c->tempo_milli < step ? target - c->tempo_milli : step;
    else c->tempo_milli -= c->tempo_milli - target < step ? c->tempo_milli - target : step;
    if (c->waiting) c->tempo_milli = 1000;
    struct stream_playback_state result = {.position_us = position, .reserve_us = reserve, .minimum_us = minimum,
        .correction_us = c->correction_us, .stable_us = stable, .removable_us = removable,
        .guard_us = guard, .pending_gain_us = pending_gain,
        .reason = c->waiting ? STREAM_PLAYBACK_STARVED : !complete ? STREAM_PLAYBACK_HISTORY_WAIT
            : stable < STREAM_PLAYBACK_SETTLE_US ? STREAM_PLAYBACK_COOLDOWN
            : removable <= braking ? STREAM_PLAYBACK_RESERVE : STREAM_PLAYBACK_CATCHUP,
        .tempo_milli = c->tempo_milli, .waiting = c->waiting, .history_ready = complete};
    if (!c->report_us || now - c->report_us >= 1000000) {
        DEBUG_TRACE(DEBUG_CATEGORY_VIDEO,
                    "playback reserve id=%llu pts=%lld ready=%lld reserve=%lld minimum=%lld correction=%lld "
                    "stable_ms=%lld history=%u tempo=%d pending_gain=%lld waiting=%d "
                    "guard_us=%lld removable_us=%lld braking_us=%lld reason=%s",
                    (unsigned long long)c->id, (long long)position, (long long)ready, (long long)reserve,
                    (long long)minimum, (long long)c->correction_us, (long long)(stable / 1000), samples,
                    c->tempo_milli, (long long)pending_gain, c->waiting,
                    (long long)guard, (long long)removable, (long long)braking, stream_playback_reason_name(result.reason));
        c->report_us = now;
    }
    c->last_us = now; c->last_position_us = position; *state = result;
    return 0;
}
