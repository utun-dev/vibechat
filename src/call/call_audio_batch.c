#include "call_audio_batch.h"
#include "../routing_layer/etcp_router.h"
#include "../../lib/debug_config.h"
#include <string.h>

static int valid_header(const struct call_pkt_media *h) {
    return h->subcmd == CALL_SUBCMD_MEDIA && h->call_id && h->track_id == CALL_TRACK_AUDIO
        && h->count && h->count <= CALL_AUDIO_BATCH_MAX && h->codec_delay_samples <= CALL_OPUS_SAMPLE_RATE;
}
static int valid_frame(const struct call_pkt_media *h, const struct call_pkt_audio_frame *f) {
    return f->length && f->length <= CALL_AUDIO_MAX_OPUS && (f->capture_pts_us == INT64_MIN
        || (f->capture_pts_us >= 0 && f->capture_pts_us <= INT64_MAX / 2 && h->capture_epoch));
}
int call_audio_batch_encode(const struct call_audio_batch_view *batch, uint8_t *out, size_t capacity, size_t *length) {
    if (!batch || !out || !length || !valid_header(&batch->header)) goto invalid;
    size_t total = CALL_MEDIA_HDR_SIZE;
    for (unsigned i = 0; i < batch->header.count; i++) {
        if (!batch->frames[i].opus || !valid_frame(&batch->header, &batch->frames[i].header)) goto invalid;
        total += CALL_AUDIO_FRAME_HDR_SIZE + batch->frames[i].header.length;
    }
    if (total > capacity || total > CALL_MEDIA_MAX_BYTES) goto invalid;
    memcpy(out, &batch->header, CALL_MEDIA_HDR_SIZE);
    size_t offset = CALL_MEDIA_HDR_SIZE;
    for (unsigned i = 0; i < batch->header.count; i++) {
        const struct call_audio_frame_view *f = &batch->frames[i];
        memcpy(out + offset, &f->header, CALL_AUDIO_FRAME_HDR_SIZE); offset += CALL_AUDIO_FRAME_HDR_SIZE;
        memcpy(out + offset, f->opus, f->header.length); offset += f->header.length;
    }
    *length = total;
    return 0;
invalid:
    DEBUG_WARN(DEBUG_CATEGORY_CALL, "audio batch encode invalid fields/capacity=%zu", capacity); return -1;
}
int call_audio_batch_parse(const uint8_t *wire, size_t length, struct call_audio_batch_view *out) {
    if (!wire || !out || length < CALL_MEDIA_HDR_SIZE || length > CALL_MEDIA_MAX_BYTES) goto invalid;
    struct call_audio_batch_view result = {0};
    memcpy(&result.header, wire, CALL_MEDIA_HDR_SIZE);
    if (!valid_header(&result.header)) goto invalid;
    size_t offset = CALL_MEDIA_HDR_SIZE;
    for (unsigned i = 0; i < result.header.count; i++) {
        struct call_audio_frame_view *f = &result.frames[i];
        if (length - offset < CALL_AUDIO_FRAME_HDR_SIZE) goto invalid;
        memcpy(&f->header, wire + offset, CALL_AUDIO_FRAME_HDR_SIZE); offset += CALL_AUDIO_FRAME_HDR_SIZE;
        if (!valid_frame(&result.header, &f->header) || f->header.length > length - offset) goto invalid;
        f->opus = wire + offset; offset += f->header.length;
    }
    if (offset != length) goto invalid;
    *out = result;
    return 0;
invalid:
    DEBUG_WARN(DEBUG_CATEGORY_CALL, "audio batch RX malformed bytes=%zu", length); return -1;
}

void call_audio_batch_init(struct call_audio_batch_control *c, uint64_t now) {
    if (!c) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "audio batch reset without state"); return; }
    *c = (struct call_audio_batch_control){.target = 4, .last_us = now, .probe_backoff_us = 30000000};
}
static void rollback_probe(struct call_audio_batch_control *c, uint64_t now, const char *reason) {
    DEBUG_WARN(DEBUG_CATEGORY_CALL,
        "audio batch probe rollback target=%u->%u reason=%s delay_us=%llu reference_us=%llu next_probe_ms=%llu",
        c->target, c->probe_previous, reason, (unsigned long long)c->delay_us, (unsigned long long)c->reference_us,
        (unsigned long long)(c->probe_backoff_us / 1000));
    c->target = c->probe_previous; c->probe_previous = 0;
    c->probe_after_us = now + c->probe_backoff_us;
    if (c->probe_backoff_us < 120000000) c->probe_backoff_us *= 2;
    c->next_change_us = now + 750000;
    c->reference_us = c->delay_us;
    c->high = c->low = 0;
}

int call_audio_batch_step(struct call_audio_batch_control *c, uint64_t now, const struct ETCP_ROUTER_TX_STATS *net,
                         const struct call_audio_batch_input *input) {
    if (!c || !c->target || now < c->last_us || (net && net->last_ack_us > now)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "audio batch invalid control sample time=%llu", (unsigned long long)now); return -1;
    }
    unsigned previous = c->target;
    uint64_t path_node = input ? input->path_node_id : 0, path_epoch = input ? input->path_epoch : 0;
    if ((!net && (c->local_epoch || c->peer_epoch || c->feedback_received))
        || (net && (net->local_epoch != c->local_epoch || net->peer_epoch != c->peer_epoch))
        || path_node != c->path_node_id || path_epoch != c->path_epoch) {
        call_audio_batch_init(c, now);
        if (net) { c->local_epoch = net->local_epoch; c->peer_epoch = net->peer_epoch; }
        c->path_node_id = path_node; c->path_epoch = path_epoch;
        DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "audio batch reset router epoch=%llx/%llx path=%llx/%llx previous=%u",
            (unsigned long long)c->local_epoch, (unsigned long long)c->peer_epoch,
            (unsigned long long)path_node, (unsigned long long)path_epoch, previous);
    }
    c->last_us = now;
    c->delay_us = net ? net->delivery_rtt_us : 0;
    if (net && net->queued_age_us > c->delay_us) c->delay_us = net->queued_age_us;
    if (net && net->inflight_age_us > c->delay_us) c->delay_us = net->inflight_age_us;
    uint64_t interval = net ? net->delivery_rtt_us : 250000;
    if (interval < 250000) interval = 250000;
    if (interval > 750000) interval = 750000;
    uint64_t fresh_us = net && net->delivery_rtt_us > 1000000 ? net->delivery_rtt_us * 2 : 2000000;
    int measured = net && net->ready && !net->no_route && !net->receive_busy && !net->probing
        && net->acked_packets && now - net->last_ack_us <= fresh_us;
    int usable = measured && net->window.valid;
    if (usable && !c->feedback_received) { c->reference_us = c->delay_us; c->feedback_received = 1; }
    uint64_t local_wait = 0, accumulation = (uint64_t)c->target * CALL_OPUS_FRAME_MS * 1000;
    if (input && input->frames >= c->target && input->oldest_us > accumulation)
        local_wait = input->oldest_us - accumulation;
    // База не следует за растущим EWMA. После увеличения пачки новую порцию роста считаем от нового уровня.
    if (usable && !c->probe_previous && c->delay_us < c->reference_us) c->reference_us = c->delay_us;
    int pressure = net && (local_wait > 100000 || net->queued_age_us > 100000 || c->delay_us > c->reference_us + 50000);
    const char *reason = "hold";
    if (!usable) {
        if (c->probe_previous) { rollback_probe(c, now, "feedback_unavailable"); reason = "probe_interrupted"; }
        c->high = c->low = 0;
    } else if (pressure) {
        c->low = 0;
        if (!c->high) { c->high = 1; c->high_since_us = now; }
        uint64_t hold = c->probe_previous ? 100000 : 200000;
        if (now >= c->next_change_us && now - c->high_since_us >= hold
            && net->acked_packets > c->changed_ack_packets) {
            if (c->probe_previous) { rollback_probe(c, now, "delivery_pressure"); reason = "probe_failed"; }
            else if (c->target < CALL_AUDIO_BATCH_MAX) {
                c->target++; c->next_change_us = now + interval; c->reference_us = c->delay_us;
                c->high = 0; reason = "queue_pressure";
            }
            c->changed_ack_packets = net->acked_packets;
        }
    } else if (!net->queued_packets && !local_wait && c->delay_us <= c->reference_us + 20000) {
        c->high = 0;
        if (!c->low) { c->low = 1; c->low_since_us = now; }
        if (c->probe_previous) {
            if (now - c->low_since_us >= 5000000 && net->acked_packets > c->changed_ack_packets) {
                DEBUG_INFO(DEBUG_CATEGORY_CALL, "audio batch probe accepted target=%u previous=%u delay_us=%llu reference_us=%llu",
                    c->target, c->probe_previous, (unsigned long long)c->delay_us, (unsigned long long)c->reference_us);
                c->probe_previous = 0; c->reference_us = c->delay_us; c->low = 0;
            }
        }
        if (!c->probe_previous && c->low && c->target > 1 && now >= c->next_change_us && now >= c->probe_after_us
            && now - c->low_since_us >= 10000000
            && net->acked_packets > c->changed_ack_packets) {
            c->probe_previous = c->target; c->target--; c->reference_us = c->delay_us;
            c->next_change_us = now;
            c->low = 0; c->changed_ack_packets = net->acked_packets; reason = "probe_smaller";
        }
    } else c->high = c->low = 0;
    if (c->target != previous) DEBUG_INFO(DEBUG_CATEGORY_CALL,
        "audio batch target=%u->%u reason=%s delay_us=%llu reference_us=%llu router_rtt_us=%llu "
        "queued_us=%llu flight_us=%llu local_wait_us=%llu probe_previous=%u",
        previous, c->target, reason, (unsigned long long)c->delay_us, (unsigned long long)c->reference_us,
        (unsigned long long)(net ? net->delivery_rtt_us : 0), (unsigned long long)(net ? net->queued_age_us : 0),
        (unsigned long long)(net ? net->inflight_age_us : 0), (unsigned long long)local_wait, c->probe_previous);
    return 0;
}
