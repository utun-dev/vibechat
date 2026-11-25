#include <string.h>

#include "../../lib/debug_config.h"
#include "stream_metrics.h"

/* Единственная корзина секундного интервала; old данные не переживают оборот окна. */
static struct stream_metric_bucket *metrics_bucket(struct stream_metrics *m, uint64_t now_ms) {
    uint64_t second = now_ms / 1000;
    struct stream_metric_bucket *bucket = &m->buckets[second % STREAM_METRIC_SECONDS];
    if (!bucket->valid || bucket->second != second) {
        memset(bucket, 0, sizeof(*bucket));
        bucket->second = second;
        bucket->valid = 1;
    }
    return bucket;
}

/* Переполнение вычисленного отставания не может превращать длительный stall в нулевую задержку. */
static uint64_t metrics_add(uint64_t first, uint64_t second) {
    return second > UINT64_MAX - first ? UINT64_MAX : first + second;
}

/* Фиксировать одну остановку только после отсутствия нового блока сверх ожидаемого интервала + 500 мс. */
static void metrics_detect_stop(struct stream_metrics *m, uint64_t now_ms) {
    if (!m->initialized || m->ended || m->stopped) return;
    uint64_t threshold = metrics_add(m->expected_interval_ms, STREAM_LATE_MS);
    uint64_t elapsed = now_ms - m->last_receive_ms;
    if (elapsed <= threshold) return;
    uint64_t started = metrics_add(m->last_receive_ms, metrics_add(threshold, 1));
    metrics_bucket(m, started)->stops++;
    m->stopped = 1;
    DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "subflow stopped after seq=%llu elapsed=%llums expected=%llums lag=%llums",
               (unsigned long long)m->last_seq, (unsigned long long)elapsed,
               (unsigned long long)m->expected_interval_ms, (unsigned long long)m->lag_ms);
}

/* Начальная ожидаемая пауза — source flush 100 мс; после замеров используется фактический темп источника. */
void stream_metrics_init(struct stream_metrics *m) {
    if (!m) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "metrics init null context"); return; }
    memset(m, 0, sizeof(*m));
    m->expected_interval_ms = STREAM_SOURCE_FLUSH_TB / 10;
}

/* Дельты относительно предшествующего блока исключают смещение часов и учитывают отставание по всей цепочке. */
int stream_metrics_observe(struct stream_metrics *m, uint64_t seq, uint16_t timestamp_ms, uint8_t flags,
                           uint32_t bytes, uint64_t now_ms) {
    if (!m || seq == UINT64_MAX || !bytes || (flags & ~STREAM_BLOCK_TIME_RESET)) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "metrics observe invalid seq=%llu bytes=%u flags=%u", (unsigned long long)seq, bytes, flags);
        return STREAM_ERR_ARGUMENT;
    }
    if (m->ended || (m->initialized && now_ms < m->last_receive_ms)) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "metrics state/clock invalid seq=%llu now=%llu previous=%llu ended=%d",
                   (unsigned long long)seq, (unsigned long long)now_ms, (unsigned long long)m->last_receive_ms, m->ended);
        return STREAM_ERR_STATE;
    }
    if (m->initialized && seq <= m->last_seq) return STREAM_DUPLICATE;
    metrics_detect_stop(m, now_ms);
    uint64_t receive_delta = m->initialized ? now_ms - m->last_receive_ms : 0;
    int rebase = !m->initialized || (flags & STREAM_BLOCK_TIME_RESET) || seq != m->last_seq + 1
                 || receive_delta >= STREAM_TIMESTAMP_PERIOD_MS;
    uint16_t source_delta = (uint16_t)(timestamp_ms - m->last_timestamp_ms);
    struct stream_metric_bucket *bucket = metrics_bucket(m, now_ms);
    bucket->blocks++;
    if (rebase) {
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "metrics rebase seq=%llu previous=%llu timestamp=%u/%u receive_delta=%llu flags=%u",
                    (unsigned long long)seq, (unsigned long long)m->last_seq, m->last_timestamp_ms, timestamp_ms,
                    (unsigned long long)receive_delta, flags);
        m->lag_ms = 0;
        m->expected_interval_ms = STREAM_SOURCE_FLUSH_TB / 10;
    } else {
        if (receive_delta >= source_delta) m->lag_ms = metrics_add(m->lag_ms, receive_delta - source_delta);
        else m->lag_ms = source_delta - receive_delta >= m->lag_ms ? 0 : m->lag_ms - (source_delta - receive_delta);
        bucket->bytes += bytes;
        bucket->source_ms += source_delta;
        bucket->receive_ms += receive_delta;
        bucket->intervals++;
        if (bucket->source_ms) {
            m->expected_interval_ms = bucket->source_ms / bucket->intervals;
            if (!m->expected_interval_ms) m->expected_interval_ms = 1;
        }
        if (m->lag_ms > STREAM_LATE_MS) bucket->late_blocks++;
        DEBUG_TRACE(DEBUG_CATEGORY_STREAM, "metrics seq=%llu source_delta=%u receive_delta=%llu lag=%llu expected=%llu",
                    (unsigned long long)seq, source_delta, (unsigned long long)receive_delta,
                    (unsigned long long)m->lag_ms, (unsigned long long)m->expected_interval_ms);
    }
    m->last_seq = seq;
    m->last_timestamp_ms = timestamp_ms;
    m->last_receive_ms = now_ms;
    m->initialized = 1;
    m->stopped = 0;
    return STREAM_OK;
}

/* Скорость суммируется по объёму и времени, а не усреднением частных rates разных интервалов. */
int stream_metrics_poll(struct stream_metrics *m, uint64_t now_ms, struct stream_metric_snapshot *snapshot) {
    if (!m || !snapshot) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "metrics poll invalid arguments"); return STREAM_ERR_ARGUMENT; }
    if (m->initialized && now_ms < m->last_receive_ms) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "metrics poll clock regression now=%llu previous=%llu",
                   (unsigned long long)now_ms, (unsigned long long)m->last_receive_ms);
        return STREAM_ERR_STATE;
    }
    struct stream_metric_snapshot result = {0};
    if (!m->initialized) { *snapshot = result; return STREAM_WAIT; }
    metrics_detect_stop(m, now_ms);
    result.lag_ms = m->lag_ms;
    result.expected_interval_ms = m->expected_interval_ms;
    result.progress_age_ms = now_ms - m->last_receive_ms;
    result.stopped = m->stopped;
    if (!m->ended && now_ms - m->last_receive_ms > m->expected_interval_ms)
        result.lag_ms = metrics_add(result.lag_ms, now_ms - m->last_receive_ms - m->expected_interval_ms);
    uint64_t second = now_ms / 1000;
    uint64_t bytes = 0, source_ms = 0, receive_ms = 0;
    for (unsigned i = 0; i < STREAM_METRIC_SECONDS; i++) {
        const struct stream_metric_bucket *bucket = &m->buckets[i];
        if (!bucket->valid || bucket->second > second || second - bucket->second >= STREAM_METRIC_SECONDS) continue;
        result.blocks += bucket->blocks;
        result.late_blocks += bucket->late_blocks;
        result.stops += bucket->stops;
        bytes += bucket->bytes;
        source_ms += bucket->source_ms;
        receive_ms += bucket->receive_ms;
    }
    result.source_rate_valid = source_ms != 0;
    result.receive_rate_valid = receive_ms != 0;
    if (source_ms) result.source_rate_bytes_sec = bytes * 1000 / source_ms;
    if (receive_ms) result.receive_rate_bytes_sec = bytes * 1000 / receive_ms;
    *snapshot = result;
    return STREAM_OK;
}

/* END останавливает ожидание будущих данных, история качества остаётся доступной. */
void stream_metrics_end(struct stream_metrics *m) {
    if (!m) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "metrics end null context"); return; }
    m->ended = 1;
    m->stopped = 0;
    DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "metrics ended at seq=%llu", (unsigned long long)m->last_seq);
}
