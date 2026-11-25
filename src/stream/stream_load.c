#include <string.h>

#include "../../lib/debug_config.h"
#include "stream_load.h"

void stream_load_init(struct stream_load *load, uint64_t now) {
    if (!load) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "load init null context"); return; }
    memset(load, 0, sizeof(*load)); load->previous_ms = now; load->revision = load->previous_revision = 1;
}

/* Нулевые/переполняющиеся изменения не могут скрыть нарушение баланса. */
int stream_load_add(struct stream_load *load, uint64_t packets, uint64_t bytes) {
    if (!load || !packets || !bytes || packets > UINT64_MAX - load->arrivals.packets || bytes > UINT64_MAX - load->arrivals.bytes
        || packets > UINT64_MAX - load->queued.packets || bytes > UINT64_MAX - load->queued.bytes) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "load add invalid/overflow packets=%llu bytes=%llu",
                    (unsigned long long)packets, (unsigned long long)bytes); return STREAM_ERR_ARGUMENT;
    }
    load->arrivals.packets += packets; load->arrivals.bytes += bytes;
    load->queued.packets += packets; load->queued.bytes += bytes; return STREAM_OK;
}

/* Удаления X не считаются выходом D и не уменьшают исходную оценку избытка A-D. */
int stream_load_remove(struct stream_load *load, uint64_t packets, uint64_t bytes, int sent) {
    if (!load || !packets || !bytes || packets > load->queued.packets || bytes > load->queued.bytes) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "load remove unaccounted packets=%llu bytes=%llu queued=%llu/%llu",
                    (unsigned long long)packets, (unsigned long long)bytes,
                    (unsigned long long)(load ? load->queued.packets : 0), (unsigned long long)(load ? load->queued.bytes : 0));
        return STREAM_ERR_STATE;
    }
    struct stream_load_counts *target = sent ? &load->sent : &load->removed;
    if (packets > UINT64_MAX - target->packets || bytes > UINT64_MAX - target->bytes) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "load remove counters overflow"); return STREAM_ERR_STATE;
    }
    target->packets += packets; target->bytes += bytes;
    load->queued.packets -= packets; load->queued.bytes -= bytes; return STREAM_OK;
}

void stream_load_changed(struct stream_load *load) {
    if (!load) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "load change null context"); return; }
    load->revision++; load->candidate = 0; load->snapshot.growth_rate = 0;
}

/* Сохраняем только законченные интервалы, без округления фактического времени по числу тиков. */
int stream_load_poll(struct stream_load *load, uint64_t now, uint64_t age, struct stream_load_snapshot *snapshot) {
    if (!load || !snapshot || now < load->previous_ms) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "load poll invalid context/clock now=%llu", (unsigned long long)now); return STREAM_ERR_ARGUMENT;
    }
    uint64_t elapsed = now - load->previous_ms;
    int result = STREAM_WAIT;
    if (elapsed >= STREAM_LOAD_PERIOD_MS) {
        struct stream_load_interval *entry = &load->history[load->head];
        *entry = (struct stream_load_interval){.begin_ms = load->previous_ms, .end_ms = now,
            .revision = load->previous_revision == load->revision ? load->revision : 0,
            .arrivals = {load->arrivals.packets - load->previous_a.packets, load->arrivals.bytes - load->previous_a.bytes},
            .sent = {load->sent.packets - load->previous_d.packets, load->sent.bytes - load->previous_d.bytes},
            .removed = {load->removed.packets - load->previous_x.packets, load->removed.bytes - load->previous_x.bytes}};
        load->last_d = entry->sent.bytes;
        load->head = (load->head + 1) % STREAM_LOAD_HISTORY;
        if (load->count < STREAM_LOAD_HISTORY) load->count++;
        uint64_t a = 0, d = 0, time = 0, grow_a = 0, grow_d = 0, grow_time = 0;
        for (unsigned i = 0; i < load->count; i++) {
            const struct stream_load_interval *e = &load->history[i];
            uint64_t span = e->end_ms - e->begin_ms;
            uint64_t cutoff = now > 10000 ? now - 10000 : 0;
            if (e->end_ms <= cutoff) continue;
            uint64_t overlap = e->end_ms - (e->begin_ms > cutoff ? e->begin_ms : cutoff);
            a += e->arrivals.bytes * overlap / span; d += e->sent.bytes * overlap / span; time += overlap;
            cutoff = now > 1000 ? now - 1000 : 0;
            if (e->end_ms <= cutoff || e->revision != load->revision) continue;
            overlap = e->end_ms - (e->begin_ms > cutoff ? e->begin_ms : cutoff);
            grow_a += e->arrivals.bytes * overlap / span; grow_d += e->sent.bytes * overlap / span; grow_time += overlap;
        }
        load->snapshot.input_rate = time ? a * 1000 / time : 0;
        load->snapshot.output_rate = time ? d * 1000 / time : 0;
        load->snapshot.growth_rate = grow_time && grow_a > grow_d ? (grow_a - grow_d) * 1000 / grow_time : 0;
        load->snapshot.interval_ms = elapsed; load->snapshot.valid = 1;
        load->previous_a = load->arrivals; load->previous_d = load->sent; load->previous_x = load->removed;
        load->previous_ms = now; load->previous_revision = load->revision; result = STREAM_OK;
    }
    int pressure = load->snapshot.valid && load->queued.bytes && age > STREAM_LOAD_PERIOD_MS
                   && (load->snapshot.growth_rate || !load->last_d);
    if (!pressure) load->candidate = 0;
    else if (!load->candidate) { load->candidate = 1; load->since_ms = now; }
    load->snapshot.arrivals = load->arrivals; load->snapshot.sent = load->sent; load->snapshot.removed = load->removed;
    load->snapshot.queued = load->queued; load->snapshot.rejected = load->rejected; load->snapshot.oldest_age_ms = age;
    load->snapshot.pressure = pressure; load->snapshot.confirmed = load->candidate && now - load->since_ms >= STREAM_LOAD_CONFIRM_MS;
    *snapshot = load->snapshot; return result;
}
