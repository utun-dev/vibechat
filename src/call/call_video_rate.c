#include "call_video_rate.h"
#include "../../lib/debug_config.h"
#include <string.h>
#include <stdlib.h>

#define WINDOW_US 500000u
#define MAX_BITRATE 100000000u

/* Обе границы проверяются до записи: частично применённого диапазона не бывает. */
int call_video_rate_parse_range(const char *text, uint32_t *minimum_bps, uint32_t *maximum_bps) {
    char *end = NULL;
    if (!text || !minimum_bps || !maximum_bps || *text < '0' || *text > '9') goto invalid;
    unsigned long minimum = strtoul(text, &end, 10);
    if (*end != ':' || end[1] < '0' || end[1] > '9') goto invalid;
    unsigned long maximum = strtoul(end + 1, &end, 10);
    if (*end || minimum < CALL_VIDEO_RATE_MIN_BPS / 1000 || minimum > maximum
        || maximum > CALL_VIDEO_RATE_MAX_BPS / 1000) goto invalid;
    *minimum_bps = (uint32_t)minimum * 1000; *maximum_bps = (uint32_t)maximum * 1000;
    return 0;
invalid:
    DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video bitrate range invalid value='%s' expected=300..4000 min:max", text ? text : "(null)");
    return -1;
}

void call_video_rate_init(struct call_video_rate *rate, uint64_t now_us) {
    if (!rate) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video rate init without state"); return; }
    memset(rate, 0, sizeof(*rate));
    rate->last_us = rate->next_change_us = rate->last_log_us = now_us;
    rate->initialized = 1;
    DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video rate reset time=%llu", (unsigned long long)now_us);
}

/* Период реакции не короче измерительного окна и двух RTT; uint64 исключает overflow RTT*2. */
static uint64_t feedback_interval(const struct call_video_rate_sample *s) {
    uint64_t rtt = (uint64_t)s->rtt_us * 2;
    return rtt > WINDOW_US ? rtt : WINDOW_US;
}

/* Снижение ограничено вдвое за один feedback; рост — проба 5% с повторным измерением. */
int call_video_rate_step(struct call_video_rate *rate, const struct call_video_rate_sample *s,
                         struct call_video_rate_result *out) {
    if (!rate || !rate->initialized || !s || !out || s->now_us < rate->last_us
        || s->now_us > UINT64_MAX - (uint64_t)UINT32_MAX * 2 - WINDOW_US
        || s->window_end_us > s->now_us || s->now_us - s->window_end_us >= 50000
        || s->window_end_us % 50000 || !s->minimum_bps || s->minimum_bps > s->maximum_bps
        || s->maximum_bps > MAX_BITRATE || !s->applied_bps || s->applied_bps > MAX_BITRATE
        || !s->debt_limit_bytes || s->debt_limit_bytes > UINT32_MAX || s->debt_bytes > UINT32_MAX
        || s->delivered_bytes > UINT32_MAX || s->produced_bytes > UINT32_MAX
        || !s->frame_interval_us || s->frame_interval_us > 1000000) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video rate invalid sample time=%llu window=%llu applied=%u min=%u max=%u",
                    (unsigned long long)(s ? s->now_us : 0), (unsigned long long)(s ? s->window_end_us : 0),
                    s ? s->applied_bps : 0, s ? s->minimum_bps : 0, s ? s->maximum_bps : 0);
        return -1;
    }
    struct call_video_rate_result result = {.target_bps = s->applied_bps, .reason = CALL_VIDEO_RATE_HOLD,
        .admit = s->ready && !s->no_route && !s->receive_busy && s->debt_bytes < s->debt_limit_bytes,
        .delivery_bps = s->delivered_bytes * 16};
    uint64_t interval = feedback_interval(s);
    if (rate->applied_bps != s->applied_bps) {
        rate->applied_bps = s->applied_bps;
        rate->next_change_us = s->now_us + interval;
    }
    /* Запас RTT относится к inflight, два кадра — к пакетному выходу encoder. */
    uint64_t bounded_delivery = result.delivery_bps > MAX_BITRATE ? MAX_BITRATE : result.delivery_bps;
    uint64_t normal = bounded_delivery * ((uint64_t)s->baseline_rtt_us + 50000) / 8000000;
    normal += (uint64_t)s->applied_bps * s->frame_interval_us * 2 / 8000000;
    result.excess_bytes = s->debt_bytes > normal ? s->debt_bytes - normal : 0;
    if (!result.excess_bytes || !s->valid || s->probing) rate->high = 0;
    else if (!rate->high) { rate->high = 1; rate->high_since_us = s->now_us; }

    if (!s->ready || s->no_route || s->receive_busy) result.reason = CALL_VIDEO_RATE_BLOCKED;
    else if (!s->command_pending) {
        if (s->applied_bps > s->maximum_bps || s->applied_bps < s->minimum_bps) {
            result.target_bps = s->applied_bps > s->maximum_bps ? s->maximum_bps : s->minimum_bps;
            result.reason = CALL_VIDEO_RATE_LIMIT;
        } else if (s->valid && !s->probing && s->now_us >= rate->next_change_us) {
            int growing = s->debt_bytes > rate->previous_debt && s->produced_bytes > s->delivered_bytes * 105 / 100;
            int persistent = rate->high && s->now_us - rate->high_since_us >= WINDOW_US;
            if (result.excess_bytes && (growing || persistent)) {
                uint64_t capacity = result.delivery_bps * 95 / 100;
                /* Долг — интеграл дефицита. Целевой срок разгрузки 1с, без второго интегратора. */
                uint64_t drain = result.excess_bytes * 8;
                uint64_t candidate = capacity > drain ? capacity - drain : 0;
                uint32_t floor = s->applied_bps / 2;
                if (floor < s->minimum_bps) floor = s->minimum_bps;
                if (candidate < floor) candidate = floor;
                if (candidate < s->applied_bps * 95ull / 100 || (candidate == s->minimum_bps && candidate < s->applied_bps)) {
                    result.target_bps = (uint32_t)candidate; result.reason = CALL_VIDEO_RATE_DRAIN;
                }
            } else if (!result.excess_bytes && result.admit && s->produced_bytes && s->delivered_bytes
                       && s->debt_bytes <= rate->previous_debt + s->applied_bps / 160) {
                /* Source-limited сцена не доказывает предел канала: допускаем небольшую пробу. */
                uint64_t candidate = s->applied_bps * 105ull / 100;
                if (candidate > s->maximum_bps) candidate = s->maximum_bps;
                if (candidate > s->applied_bps) {
                    result.target_bps = (uint32_t)candidate; result.reason = CALL_VIDEO_RATE_PROBE;
                }
            }
        }
    }
    if (result.target_bps != s->applied_bps) {
        rate->next_change_us = s->now_us + interval;
        DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video rate request reason=%d from=%u to=%u ack_bps=%llu debt=%llu excess=%llu",
                    result.reason, s->applied_bps, result.target_bps, (unsigned long long)result.delivery_bps,
                    (unsigned long long)s->debt_bytes, (unsigned long long)result.excess_bytes);
    }
    if (s->now_us - rate->last_log_us >= 1000000) {
        rate->last_log_us = s->now_us;
        DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video rate sample applied=%u ack_bps=%llu produced=%llu debt=%llu excess=%llu"
                    " valid=%d admit=%d pending=%d probe=%d reason=%d",
                    s->applied_bps, (unsigned long long)result.delivery_bps, (unsigned long long)s->produced_bytes,
                    (unsigned long long)s->debt_bytes, (unsigned long long)result.excess_bytes, s->valid, result.admit,
                    s->command_pending, s->probing, result.reason);
    }
    rate->previous_debt = s->debt_bytes; rate->last_us = s->now_us;
    *out = result;
    return 0;
}
