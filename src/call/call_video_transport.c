#include "call_video_transport.h"
#include "call_priv.h"
#include "../chat/chat_setting.h"
#include "../utun_instance.h"
#include "../routing_layer/etcp_router.h"
#include "../routing_layer/route_crypto.h"
#include "../routing_layer/topo_group.h"
#include "../transport_layer/etcp.h"
#include "../../lib/mem.h"
#include "../../lib/u_async.h"
#include "../../lib/debug_config.h"
#include <string.h>

enum { VIDEO_CAPS = 1, VIDEO_KEY_REQUEST = 2, VIDEO_CAPS_ACK = 3, VIDEO_DECODE_OK = 4, VIDEO_DECODE_REJECT = 5 };
enum { VIDEO_TX_FRAMES = 32, VIDEO_TX_SPAN_US = 500000, VIDEO_ACK_FRAMES = ROUTER_MAX_SEND_Q_PACKETS + ROUTER_MAX_INFLIGHT };
struct video_tx_frame {
    struct call_video_frame frame;
    uint8_t *data;
    uint64_t started_tb;
    unsigned budget_yields;
    int router_retry;
};
/* Данные уже принадлежат роутеру. Граница ACK относится к текущей эпохе канала. */
struct video_tx_ack {
    uint64_t frame_id, started_tb;
    int64_t pts_us;
    uint32_t size, end_seq;
    uint16_t fps;
};
struct call_video_transport {
    struct call_session *session;
    struct call_video_caps local, peer;
    struct call_video_ops ops;
    void *arg, *timer, *flush_pending;
    struct queue_waiter_handle send_waiter; // Одноразовое событие освобождения router send_q.
    struct call_video_receiver *receiver;
    struct video_tx_frame tx[VIDEO_TX_FRAMES];
    unsigned tx_count, tx_bytes;
    struct video_tx_ack awaiting_ack[VIDEO_ACK_FRAMES];
    unsigned ack_head, ack_count;
    struct call_video_format tx_last_format;
    uint64_t tx_last_id, key_request_tb;
    uint64_t local_revision, peer_revision, acknowledged_revision, caps_sent_tb;
    int64_t tx_last_pts;
    int have_peer, need_key, key_request_pending, key_requested;
    struct call_video_rate rate;
    struct route_tx_meter production;
    struct call_video_format rate_format;
    uint64_t rate_request, produced_id, route_local_epoch, route_peer_epoch, rate_writable_us, rate_writable_since_us;
    uint32_t applied_bps, maximum_bps, range_minimum_bps, range_maximum_bps;
    int rate_pending, admit;
    const char *router_wait;
    uint64_t received_generation, decode_generation;
    int decode_result, decode_pending, tx_rejected;
    uint64_t report_us;
    unsigned offered, accepted, transport_busy, key_wait;
    uint64_t flush_scheduled_us, flush_wait_max_us;
    unsigned flush_yields, flush_runs, flush_net_wait;
};

static struct call_session *active(struct UTUN_INSTANCE *inst, uint64_t id) {
    struct call_session *s = inst ? call_session_find(inst->call, id) : NULL;
    return s && s->state == CALL_ST_ACTIVE ? s : NULL;
}
static void log_caps(uint64_t id, const char *side, const struct call_video_caps *caps) {
    for (unsigned i = 0; i < 2; i++) {
        const struct call_video_limit *tx = &caps->encode[i], *rx = &caps->decode[i];
        DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video %s limits call=%llu codec=%u "
            "encode=%ux%u@%u pps=%u level=%u bps=%u profiles=%u decode=%ux%u@%u pps=%u level=%u bps=%u profiles=%u",
            side, (unsigned long long)id, i + 1,
            tx->width, tx->height, tx->fps, tx->pixels_per_second, tx->level, tx->maximum_bps, tx->profiles,
            rx->width, rx->height, rx->fps, rx->pixels_per_second, rx->level, rx->maximum_bps, rx->profiles);
    }
}
static void tick(void *arg);
static const char *encode_pause(const struct call_video_transport *v) {
    if (v->tx_rejected) return "decoder_rejected";
    if (!v->have_peer || v->acknowledged_revision != v->local_revision) return "capabilities";
    if (v->router_wait) return v->router_wait;
    if (v->ack_count == VIDEO_ACK_FRAMES) return "ack_queue";
    if (v->tx_count == VIDEO_TX_FRAMES || v->tx_bytes >= CALL_VIDEO_MAX_FRAME
        || (v->tx_count && v->tx_last_pts - v->tx[0].frame.pts_us >= VIDEO_TX_SPAN_US)) return "frame_fifo";
    return v->admit ? NULL : "rate_admission";
}
/* Admission ограничивает новые входы encoder, а готовые кадры продолжают передаваться. */
static void writable(struct call_video_transport *v) {
    int can_encode = !encode_pause(v);
    if (v->rate_pending) {
        // Применение команды начинается на очередном входе кодера. Время паузы TX в таймаут не входит.
        uint64_t now = get_time_us();
        if (v->rate_writable_since_us) v->rate_writable_us += now - v->rate_writable_since_us;
        v->rate_writable_since_us = can_encode ? now : 0;
        if (can_encode && v->rate_writable_us > 12000000) {
            v->rate_pending = 0; v->applied_bps = 0; v->admit = 0; can_encode = 0;
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video encoder command timeout call=%llu generation=%llu request=%llu writable_us=%llu",
                        (unsigned long long)v->session->call_id, (unsigned long long)v->rate_format.generation,
                        (unsigned long long)v->rate_request, (unsigned long long)v->rate_writable_us);
        }
    }
    if (v->ops.writable) v->ops.writable(v->arg, can_encode);
}
static struct call_video_transport *ensure(struct call_session *s) {
    if (s->video) return s->video;
    struct call_video_transport *v = u_calloc(1, sizeof(*v));
    if (!v) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video transport allocation failed"); return NULL; }
    v->receiver = call_video_receiver_create();
    if (!v->receiver) { u_free(v); return NULL; }
    v->session = s; v->need_key = 1; v->tx_last_pts = -1;
    v->admit = 1;
    v->timer = uasync_set_timeout(s->ctx->inst->ua, 500, v, tick, "call_video");
    if (!v->timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video timer allocation failed");
        call_video_receiver_destroy(v->receiver); u_free(v); return NULL;
    }
    s->video = v; return v;
}
static int control_send(struct call_video_transport *v, uint8_t type) {
    struct call_session *s = v->session;
    uint8_t data[17 + CALL_VIDEO_CAPS_BYTES]; data[0] = type; memcpy(data + 1, &s->call_id, 8);
    uint64_t revision = type == VIDEO_CAPS ? v->local_revision : type == VIDEO_CAPS_ACK ? v->peer_revision : 0;
    if (type == VIDEO_DECODE_OK || type == VIDEO_DECODE_REJECT) revision = v->decode_generation;
    memcpy(data + 9, &revision, 8);
    size_t length = 17;
    if (type == VIDEO_CAPS) {
        if (call_video_caps_encode(&v->local, data + 17)) return -1;
        length += CALL_VIDEO_CAPS_BYTES;
    }
    struct ETCP_ROUTER_CONN *conn = etcp_router_conn_get_key(s->ctx->inst, s->group_id, s->peer_ed, ETCP_RT_ID_CALL_VIDEO_CONTROL);
    if (!conn || etcp_router_conn_send(conn, data, length, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT)) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "video control send failed call=%llu type=%u", (unsigned long long)s->call_id, type); return -1;
    }
    if (type == VIDEO_CAPS) v->caps_sent_tb = get_time_tb();
    return 0;
}
/* Сохраняем запрос до принятия надёжным control-каналом, даже если он пока заполнен. */
static void send_key_request(struct call_video_transport *v) {
    if (!v->key_request_pending) return;
    uint64_t now = get_time_tb();
    if (v->key_request_tb && now - v->key_request_tb < 5000) return;
    v->key_request_tb = now;
    if (control_send(v, VIDEO_KEY_REQUEST)) return;
    v->key_request_pending = 0; v->key_requested = 1;
    DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video KEY requested call=%llu", (unsigned long long)v->session->call_id);
}
/* Удаление головы сохраняет порядок и владение остальными кадрами. */
static void pop_tx(struct call_video_transport *v) {
    v->tx_bytes -= v->tx[0].frame.size; u_free(v->tx[0].data); --v->tx_count;
    memmove(v->tx, v->tx + 1, v->tx_count * sizeof(v->tx[0]));
}
/* call_soon остаётся в очереди с NULL callback до прохода uasync; v можно освободить сразу. */
static void cancel_flush(struct call_video_transport *v) {
    etcp_router_cancel_send_ready(&v->send_waiter);
    if (v->flush_pending && uasync_call_soon_cancel(v->session->ctx->inst->ua, v->flush_pending) != ERR_OK)
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video flush cancel failed call=%llu", (unsigned long long)v->session->call_id);
    v->flush_pending = NULL; v->flush_scheduled_us = 0;
}
static void discard_tx(struct call_video_transport *v, const char *reason) {
    DEBUG_WARN(DEBUG_CATEGORY_CALL, "video TX discard call=%llu frames=%u bytes=%u reason=%s",
               (unsigned long long)v->session->call_id, v->tx_count, v->tx_bytes, reason);
    cancel_flush(v);
    while (v->tx_count) pop_tx(v);
    v->need_key = 1;
    if (v->ops.need_key) v->ops.need_key(v->arg);
}
static uint64_t media_bytes(uint64_t bytes, uint64_t packets) {
    uint64_t headers = packets * CALL_VIDEO_HEADER_BYTES;
    return bytes >= headers ? bytes - headers : 0;
}
static unsigned unsent_bytes(const struct call_video_transport *v) {
    return v->tx_count ? v->tx_bytes - v->tx[0].frame.offset : 0;
}
static void sync_router_tx(struct call_video_transport *v, const struct ETCP_ROUTER_TX_STATS *net, uint64_t now) {
    int reset = v->route_local_epoch && (net->local_epoch != v->route_local_epoch
        || (v->route_peer_epoch && net->peer_epoch != v->route_peer_epoch));
    // Первый handshake подтверждает эпоху, сохраняя уже поставленные в send_q фрагменты.
    if (reset) {
        unsigned partial_offset = v->tx_count ? v->tx[0].frame.offset : 0;
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "video TX epoch ended call=%llu local=%llu peer=%llu next_local=%llu next_peer=%llu "
            "unconfirmed_frames=%u partial_offset=%u reason=%s",
            (unsigned long long)v->session->call_id, (unsigned long long)v->route_local_epoch, (unsigned long long)v->route_peer_epoch,
            (unsigned long long)net->local_epoch, (unsigned long long)net->peer_epoch, v->ack_count, partial_offset,
            net->local_epoch ? "router_reset" : "router_closed");
        // Роутер потерял старые очереди: хвост или зависимые кадры прежней цепочки продолжать нельзя.
        if (v->ack_count || partial_offset) discard_tx(v, "router_epoch_lost");
        v->ack_head = v->ack_count = 0;
        if (v->rate_format.generation) {
            call_video_rate_init(&v->rate, now); route_tx_meter_reset(&v->production, now);
        }
    }
    v->route_local_epoch = net->local_epoch; v->route_peer_epoch = net->peer_epoch;
    while (v->ack_count) {
        const struct video_tx_ack *acked = &v->awaiting_ack[v->ack_head];
        if ((int32_t)(net->ack_next_seq - acked->end_seq) < 0) break;
        DEBUG_TRACE(DEBUG_CATEGORY_CALL, "video TX frame acknowledged call=%llu frame=%llu pts=%lld bytes=%u end_seq=%u",
            (unsigned long long)v->session->call_id, (unsigned long long)acked->frame_id,
            (long long)acked->pts_us, acked->size, acked->end_seq);
        v->ack_head = (v->ack_head + 1) % VIDEO_ACK_FRAMES; --v->ack_count;
    }
}
static int router_tx_snapshot(struct call_video_transport *v, uint64_t now, struct ETCP_ROUTER_TX_STATS *net) {
    struct call_session *s = v->session;
    int result = etcp_router_tx_stats_key(s->ctx->inst, s->group_id, s->peer_ed, ETCP_RT_ID_CALL_VIDEO, now, net);
    if (result >= 0) sync_router_tx(v, net, now);
    return result;
}
static unsigned buffered_video_us(const struct call_video_transport *v) {
    unsigned duration = 0;
    for (unsigned i = 0; i < v->ack_count; ++i)
        duration += 1000000 / v->awaiting_ack[(v->ack_head + i) % VIDEO_ACK_FRAMES].fps;
    for (unsigned i = 0; i < v->tx_count; ++i) duration += 1000000 / v->tx[i].frame.format.fps;
    return duration;
}
static void report_tx(struct call_video_transport *v, const struct ETCP_ROUTER_TX_STATS *net, uint64_t now) {
    if (now - v->report_us < 1000000) return;
    v->report_us = now;
    if (!v->rate_format.generation && !v->offered && !v->tx_count && !v->ack_count) return;
    const char *pause = encode_pause(v);
    uint64_t oldest_tb = v->ack_count ? v->awaiting_ack[v->ack_head].started_tb : v->tx_count ? v->tx[0].started_tb : 0;
    DEBUG_DEBUG(DEBUG_CATEGORY_CALL,
        "video TX flow call=%llu offered=%u accepted=%u transport_busy=%u key_wait=%u pause=%s "
        "buffer_frames=%u fifo_frames=%u ack_frames=%u buffer_video_us=%u oldest_us=%llu "
        "fifo_unsent_bytes=%u router_queued_bytes=%llu router_inflight_bytes=%llu "
        "flush_yields=%u flush_runs=%u flush_wait_max_us=%llu flush_net_wait=%u",
        (unsigned long long)v->session->call_id, v->offered, v->accepted, v->transport_busy, v->key_wait, pause ? pause : "none",
        v->tx_count + v->ack_count, v->tx_count, v->ack_count, buffered_video_us(v),
        (unsigned long long)(oldest_tb ? (get_time_tb() - oldest_tb) * 100 : 0), unsent_bytes(v),
        (unsigned long long)media_bytes(net->queued_bytes, net->queued_packets),
        (unsigned long long)media_bytes(net->inflight_bytes, net->inflight_packets),
        v->flush_yields, v->flush_runs, v->flush_wait_max_us, v->flush_net_wait);
    v->offered = v->accepted = v->transport_busy = v->key_wait = 0;
    v->flush_yields = v->flush_runs = v->flush_net_wait = 0; v->flush_wait_max_us = 0;
}
/* Ограничиваем накопление полусекундой целевого потока плюс базовый RTT.
 * 64 KiB оставляем для всплеска IDR; абсолютный предел — защита памяти. */
static uint64_t debt_limit(const struct call_video_transport *v, uint32_t rtt_us) {
    uint64_t rtt = rtt_us < 2000000 ? rtt_us : 2000000;
    uint64_t limit = (uint64_t)(v->applied_bps ? v->applied_bps : 1000000) * (rtt + 500000) / 8000000;
    if (limit < 65536) limit = 65536;
    return limit < CALL_VIDEO_MAX_FRAME ? limit : CALL_VIDEO_MAX_FRAME;
}
static void update_rate(struct call_video_transport *v, const struct ETCP_ROUTER_TX_STATS *net, uint64_t now) {
    if (!v->rate_format.generation || !v->applied_bps) return;
    struct call_session *s = v->session;
    if (!v->have_peer || !call_video_format_supported(&v->local, &v->peer, &v->rate_format)) {
        if (v->admit) DEBUG_WARN(DEBUG_CATEGORY_CALL, "video rate mode no longer supported call=%llu generation=%llu; admission paused",
                                (unsigned long long)s->call_id, (unsigned long long)v->rate_format.generation);
        v->admit = 0; return;
    }
    uint32_t minimum, maximum;
    if (call_video_rate_parse_range(chat_setting_get(s->ctx->inst, "call_video_bitrate_range_kbps"), &minimum, &maximum)) {
        v->admit = 0; return;
    }
    if (maximum > v->maximum_bps) maximum = v->maximum_bps;
    if (minimum != v->range_minimum_bps || maximum != v->range_maximum_bps) {
        v->range_minimum_bps = minimum; v->range_maximum_bps = maximum;
        if (minimum > maximum)
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "video range unavailable call=%llu min=%u codec_cap=%u; admission paused",
                       (unsigned long long)s->call_id, minimum, v->maximum_bps);
        else DEBUG_INFO(DEBUG_CATEGORY_CALL, "video range applied call=%llu min=%u max=%u codec_cap=%u",
                        (unsigned long long)s->call_id, minimum, maximum, v->maximum_bps);
    }
    if (minimum > maximum) { v->admit = 0; return; }
    int found = net->local_epoch != 0;
    struct route_tx_window produced = {0}; route_tx_meter_window(&v->production, now, &produced);
    struct call_video_rate_sample sample = {0};
    sample.now_us = now; sample.window_end_us = now / ROUTE_TX_SAMPLE_US * ROUTE_TX_SAMPLE_US;
    sample.delivered_bytes = media_bytes(net->window.bytes, net->window.packets); sample.produced_bytes = produced.bytes;
    sample.debt_bytes = unsent_bytes(v) + media_bytes(net->queued_bytes + net->inflight_bytes,
        (uint64_t)net->queued_packets + net->inflight_packets);
    sample.applied_bps = v->applied_bps; sample.minimum_bps = minimum; sample.maximum_bps = maximum;
    sample.baseline_rtt_us = net->baseline_rtt_us; sample.rtt_us = net->rtt_us;
    sample.frame_interval_us = v->tx_last_format.fps ? 1000000 / v->tx_last_format.fps : 33333;
    sample.debt_limit_bytes = debt_limit(v, net->baseline_rtt_us);
    sample.valid = found && net->window.valid && produced.valid && net->window.end_us == produced.end_us;
    // Первый KEY должен создать логический канал; до этого нет транспортных измерений.
    sample.ready = found ? net->ready : 1;
    sample.no_route = found ? net->no_route : !etcp_router_peer_reachable(s->ctx->inst, s->group_id, s->peer_ed, ETCP_RT_ID_CALL_VIDEO);
    sample.receive_busy = net->receive_busy; sample.probing = net->probing;
    sample.command_pending = v->rate_pending;
    struct call_video_rate_result result;
    if (call_video_rate_step(&v->rate, &sample, &result)) { v->admit = 0; return; }
    v->admit = result.admit;
    if (result.target_bps != v->applied_bps && v->ops.bitrate) {
        v->rate_pending = 1; ++v->rate_request;
        v->rate_writable_us = v->rate_writable_since_us = 0;
        v->ops.bitrate(v->arg, v->rate_format.generation, v->rate_request, result.target_bps);
    }
}
static void flush(struct call_video_transport *v);
static void send_ready(struct ll_queue *queue, void *arg) {
    (void)queue;
    struct call_video_transport *v = arg; flush(v); writable(v);
}
/* Снимаем handle до callback: повторный бюджет будет поставлен в следующий batch uasync. */
static void continue_flush(void *arg) {
    struct call_video_transport *v = arg;
    v->flush_pending = NULL; v->flush_runs++;
    flush(v); writable(v);
}
static void flush(struct call_video_transport *v) {
    if (v->flush_scheduled_us) {
        uint64_t wait = get_time_us() - v->flush_scheduled_us;
        if (wait > v->flush_wait_max_us) v->flush_wait_max_us = wait;
    }
    // Другой источник flush мог успеть раньше отложенного вызова: оставляем только одно продолжение.
    cancel_flush(v);
    v->router_wait = NULL;
    if (!v->tx_count || v->acknowledged_revision != v->local_revision) return;
    struct call_session *s = v->session;
    struct ETCP_ROUTER_CONN *conn = NULL;
    uint8_t wire[CALL_VIDEO_HEADER_BYTES + CALL_VIDEO_FRAGMENT_BYTES];
    // Ограниченный burst одного вызова. Только budget ставит немедленное продолжение; backpressure — нет.
    for (unsigned budget = 0; v->tx_count && budget < 16; budget++) {
        if (!v->have_peer || !call_video_format_supported(&v->local, &v->peer, &v->tx[0].frame.format)) {
            discard_tx(v, "capabilities_changed"); return;
        }
        if (!conn) conn = etcp_router_conn_get_key(s->ctx->inst, s->group_id, s->peer_ed, ETCP_RT_ID_CALL_VIDEO);
        if (!conn) {
            struct video_tx_frame *head = &v->tx[0];
            if (!head->router_retry) DEBUG_WARN(DEBUG_CATEGORY_CALL, "video TX waiting for router call=%llu frame=%llu offset=%u",
                (unsigned long long)s->call_id, (unsigned long long)head->frame.frame_id, head->frame.offset);
            head->router_retry = 1; v->router_wait = "router_unavailable"; return;
        }
        struct ETCP_ROUTER_TX_STATS net;
        if (router_tx_snapshot(v, get_time_us(), &net)) { v->router_wait = "router_snapshot"; return; }
        if (!v->tx_count) return; // При сбросе эпохи sync_router_tx мог освободить старую цепочку.
        if (v->ack_count == VIDEO_ACK_FRAMES) return;
        if (media_bytes(net.queued_bytes + net.inflight_bytes, (uint64_t)net.queued_packets + net.inflight_packets)
            >= debt_limit(v, net.baseline_rtt_us)) { v->router_wait = "router_debt"; v->flush_net_wait++; return; }
        if (!etcp_router_conn_send_q_has_room(conn)
            || queue_entry_count(conn->send_q) >= ROUTER_MAX_SEND_Q_PACKETS) {
            v->router_wait = "router_queue"; v->flush_net_wait++;
            etcp_router_conn_on_send_ready(conn, &v->send_waiter, send_ready, v);
            return;
        }
        struct video_tx_frame *head = &v->tx[0];
        struct call_video_frame *f = &head->frame;
        uint32_t remaining = f->size - f->offset;
        f->length = remaining < CALL_VIDEO_FRAGMENT_BYTES ? remaining : CALL_VIDEO_FRAGMENT_BYTES;
        f->data = head->data + f->offset;
        size_t length;
        if (call_video_fragment_encode(f, wire, sizeof(wire), &length)) { discard_tx(v, "invalid_fragment"); return; }
        // send выделяет текущий tx_seq при копировании пакета в очередь uasync.
        uint32_t end_seq = conn->tx_seq + 1;
        if (etcp_router_conn_send(conn, wire, length, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT)) {
            if (!head->router_retry) DEBUG_WARN(DEBUG_CATEGORY_CALL, "video TX enqueue deferred call=%llu frame=%llu offset=%u",
                (unsigned long long)s->call_id, (unsigned long long)f->frame_id, f->offset);
            head->router_retry = 1; v->router_wait = "router_enqueue"; return;
        }
        if (head->router_retry) {
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "video TX enqueue resumed call=%llu frame=%llu offset=%u queue_age_us=%llu",
                (unsigned long long)s->call_id, (unsigned long long)f->frame_id, f->offset,
                (unsigned long long)((get_time_tb() - head->started_tb) * 100));
            head->router_retry = 0;
        }
        f->offset += f->length;
        if (f->offset == f->size) {
            unsigned index = (v->ack_head + v->ack_count) % VIDEO_ACK_FRAMES;
            v->awaiting_ack[index] = (struct video_tx_ack){.frame_id = f->frame_id, .started_tb = head->started_tb,
                .pts_us = f->pts_us, .size = f->size, .end_seq = end_seq, .fps = f->format.fps};
            ++v->ack_count;
            if (f->pts_us % 1000000 < 34000)
                DEBUG_DEBUG(DEBUG_CATEGORY_CALL,
                    "video timing stage=send call=%llu frame=%llu pts=%lld boot_us=%llu bytes=%u queue_age_us=%llu budget_yields=%u",
                    (unsigned long long)f->call_id, (unsigned long long)f->frame_id, (long long)f->pts_us,
                    (unsigned long long)get_time_us(), f->size, (unsigned long long)((get_time_tb() - head->started_tb) * 100),
                    head->budget_yields);
            pop_tx(v);
        }
    }
    if (v->tx_count && v->ack_count < VIDEO_ACK_FRAMES) {
        v->flush_pending = uasync_call_soon(s->ctx->inst->ua, v, continue_flush);
        if (v->flush_pending) {
            v->flush_scheduled_us = get_time_us(); v->flush_yields++; v->tx[0].budget_yields++;
        } else DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video flush continuation allocation failed call=%llu", (unsigned long long)s->call_id);
    }
}
static void tick(void *arg) {
    struct call_video_transport *v = arg;
    v->timer = NULL;
    // Отдельный control может обогнать ACCEPT: повторяем до подтверждения приложения.
    if (v->local_revision && v->acknowledged_revision != v->local_revision && get_time_tb() - v->caps_sent_tb >= 5000)
        control_send(v, VIDEO_CAPS);
    if (v->decode_pending && !control_send(v, v->decode_result ? VIDEO_DECODE_OK : VIDEO_DECODE_REJECT)) v->decode_pending = 0;
    send_key_request(v);
    struct ETCP_ROUTER_TX_STATS net;
    uint64_t now = get_time_us();
    if (router_tx_snapshot(v, now, &net) >= 0) {
        if (!v->tx_rejected) update_rate(v, &net, now);
        report_tx(v, &net, now);
    }
    flush(v);
    writable(v);
    v->timer = uasync_set_timeout(v->session->ctx->inst->ua, 500, v, tick, "call_video");
    if (!v->timer) DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video timer rearm failed call=%llu", (unsigned long long)v->session->call_id);
}
void call_video_transport_stop(struct call_session *s) {
    struct call_video_transport *v = s->video;
    if (!v) return;
    s->video = NULL;
    cancel_flush(v);
    if (v->timer) uasync_cancel_timeout(s->ctx->inst->ua, v->timer);
    call_video_receiver_destroy(v->receiver);
    while (v->tx_count) pop_tx(v);
    u_free(v);
    // Уже принятые роутером пакеты содержат call_id и не принадлежат следующему звонку.
    etcp_router_conn_resume_receive(etcp_router_conn_lookup(s->ctx->inst, s->group_id, s->peer_ed, ETCP_RT_ID_CALL_VIDEO));
}
int call_video_configure(struct UTUN_INSTANCE *inst, uint64_t id, const struct call_video_caps *caps,
                         const struct call_video_ops *ops, void *arg) {
    struct call_session *s = active(inst, id);
    if (!s || !call_video_caps_valid(caps) || (ops && (!ops->frame || !ops->receive_ready))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video configure invalid call/capabilities/callbacks id=%llu", (unsigned long long)id); return -1;
    }
    struct call_video_transport *v = ensure(s);
    if (!v) return -1;
    v->local = *caps; v->ops = ops ? *ops : (struct call_video_ops){0}; v->arg = arg;
    if (++v->local_revision == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video capabilities revision exhausted"); return -1;
    }
    if (!ops) {
        memset(&v->local, 0, sizeof(v->local));
        v->rate_format = (struct call_video_format){0}; v->applied_bps = 0; v->rate_pending = 0; v->admit = 0;
        if (v->tx_count) discard_tx(v, "platform_detached");
        call_video_receiver_reset(v->receiver);
        v->key_request_pending = v->key_requested = 0;
    }
    if (v->have_peer && v->ops.capabilities) v->ops.capabilities(v->arg, &v->peer);
    call_video_resume_receive(inst, id);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "video configured call=%llu encode=%u/%u decode=%u/%u", (unsigned long long)id,
               v->local.encode[0].width, v->local.encode[1].width, v->local.decode[0].width, v->local.decode[1].width);
    log_caps(id, "local", &v->local);
    return control_send(v, VIDEO_CAPS);
}
int call_video_send_frame(struct UTUN_INSTANCE *inst, const struct call_video_frame *f) {
    struct call_session *s = f ? active(inst, f->call_id) : NULL;
    struct call_video_transport *v = s ? s->video : NULL;
    if (!v || !call_video_format_valid(&f->format) || !f->data || !f->size || f->size > CALL_VIDEO_MAX_FRAME
        || f->offset || f->length != f->size || !f->frame_id || f->pts_us < 0 || f->pts_us > INT64_MAX / 2
        || (f->flags & ~CALL_VIDEO_KEY) || f->frame_id <= v->tx_last_id || f->pts_us <= v->tx_last_pts) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL,
            "video TX invalid active=%d call=%llu frame=%llu last_frame=%llu pts=%lld last_pts=%lld size=%u offset=%u length=%u format_valid=%d",
            v != NULL, (unsigned long long)(f ? f->call_id : 0), (unsigned long long)(f ? f->frame_id : 0),
            (unsigned long long)(v ? v->tx_last_id : 0), (long long)(f ? f->pts_us : -1), (long long)(v ? v->tx_last_pts : -1),
            f ? f->size : 0, f ? f->offset : 0, f ? f->length : 0, f && call_video_format_valid(&f->format)); return -1;
    }
    if (!v->have_peer || v->acknowledged_revision != v->local_revision) return 0;
    if (v->tx_rejected) return -1;
    if (!call_video_format_supported(&v->local, &v->peer, &f->format)) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "video TX unsupported format codec=%u level=%u bitrate_cap=%u profile=%u",
                   f->format.codec, f->format.level, f->format.maximum_bps, f->format.profile); return -1;
    }
    if (v->rate_format.generation) {
        if (!call_video_same_format(&f->format, &v->rate_format)) {
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "video TX differs from configured encoder generation/mode"); return -1;
        }
        if (f->frame_id > v->produced_id) {
            route_tx_meter_ack(&v->production, get_time_us(), f->size); v->produced_id = f->frame_id;
        }
    }
    v->offered++;
    flush(v);
    if (v->tx_count == VIDEO_TX_FRAMES || v->tx_bytes + f->size > CALL_VIDEO_MAX_FRAME
        || (v->tx_count && f->pts_us - v->tx[0].frame.pts_us > VIDEO_TX_SPAN_US)) {
        v->transport_busy++;
        return 0;
    }
    struct ETCP_ROUTER_TX_STATS net;
    if (etcp_router_tx_stats_key(inst, s->group_id, s->peer_ed, ETCP_RT_ID_CALL_VIDEO, get_time_us(), &net) == 0) {
        uint64_t debt = media_bytes(net.queued_bytes + net.inflight_bytes, (uint64_t)net.queued_packets + net.inflight_packets);
        if (debt + unsent_bytes(v) + f->size > CALL_VIDEO_MAX_FRAME) { v->transport_busy++; return 0; }
    }
    int changed = !call_video_same_format(&f->format, &v->tx_last_format);
    if (f->format.generation < v->tx_last_format.generation || (changed && f->format.generation == v->tx_last_format.generation)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video TX format changed without new generation"); return -1;
    }
    if (!(f->flags & CALL_VIDEO_KEY) && (v->need_key || changed || (v->tx_last_id && f->frame_id != v->tx_last_id + 1))) {
        if (!v->need_key || !v->key_wait) DEBUG_WARN(DEBUG_CATEGORY_CALL, "video TX skips dependent frame call=%llu frame=%llu last=%llu",
            (unsigned long long)f->call_id, (unsigned long long)f->frame_id, (unsigned long long)v->tx_last_id);
        if (!v->need_key && v->ops.need_key) v->ops.need_key(v->arg);
        v->need_key = 1; v->key_wait++;
        return CALL_VIDEO_SEND_NEEDS_KEY;
    }
    uint8_t *data = u_malloc(f->size);
    if (!data) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video TX allocation failed bytes=%u", f->size); v->need_key = 1; return -1; }
    memcpy(data, f->data, f->size);
    struct video_tx_frame *tail = &v->tx[v->tx_count++];
    tail->budget_yields = 0; tail->router_retry = 0;
    tail->frame = *f; tail->frame.data = tail->data = data; tail->started_tb = get_time_tb(); v->tx_bytes += f->size;
    v->tx_last_id = f->frame_id; v->tx_last_pts = f->pts_us; v->tx_last_format = f->format; v->need_key = 0;
    DEBUG_TRACE(DEBUG_CATEGORY_CALL, "video TX frame queued call=%llu frame=%llu pts=%lld bytes=%u key=%d fifo_frames=%u ack_frames=%u",
        (unsigned long long)f->call_id, (unsigned long long)f->frame_id, (long long)f->pts_us, f->size,
        !!(f->flags & CALL_VIDEO_KEY), v->tx_count, v->ack_count);
    v->accepted++; flush(v); writable(v); return 1;
}
int call_video_configure_rate(struct UTUN_INSTANCE *inst, uint64_t id, const struct call_video_format *format,
                              uint32_t applied, uint32_t maximum) {
    struct call_session *s = active(inst, id);
    struct call_video_transport *v = s ? s->video : NULL;
    uint64_t generation = format ? format->generation : 0;
    if (!v || !v->ops.bitrate || !v->have_peer || !call_video_format_supported(&v->local, &v->peer, format)
        || generation < v->rate_format.generation
        || (generation == v->rate_format.generation && !call_video_same_format(format, &v->rate_format))
        || applied < CALL_VIDEO_RATE_MIN_BPS || applied > format->maximum_bps
        || maximum < CALL_VIDEO_RATE_MIN_BPS || maximum > format->maximum_bps) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video rate configure invalid call=%llu generation=%llu bitrate=%u cap=%u",
                    (unsigned long long)id, (unsigned long long)generation, applied, maximum); return -1;
    }
    uint32_t range_minimum, range_maximum;
    if (call_video_rate_parse_range(chat_setting_get(inst, "call_video_bitrate_range_kbps"), &range_minimum, &range_maximum)) return -1;
    if (maximum < range_minimum) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video rate codec cap below requested minimum call=%llu min=%u cap=%u",
                    (unsigned long long)id, range_minimum, maximum); return -1;
    }
    if (v->rate_format.generation != generation) {
        v->rate_format = *format; v->rate_pending = 0; v->admit = 1; v->tx_rejected = 0;
        call_video_rate_init(&v->rate, get_time_us()); route_tx_meter_reset(&v->production, get_time_us());
    }
    v->applied_bps = applied; v->maximum_bps = maximum;
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "video rate configured call=%llu generation=%llu codec=%u level=%u applied=%u cap=%u envelope=%u profile=%u",
               (unsigned long long)id, (unsigned long long)generation, format->codec, format->level,
               applied, maximum, format->maximum_bps, format->profile); return 0;
}
int call_video_rate_applied(struct UTUN_INSTANCE *inst, uint64_t id, uint64_t generation, uint64_t request, uint32_t bps) {
    struct call_session *s = active(inst, id);
    struct call_video_transport *v = s ? s->video : NULL;
    if (!v || generation != v->rate_format.generation || request != v->rate_request || !v->rate_pending) {
        DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video rate stale completion call=%llu generation=%llu request=%llu",
                    (unsigned long long)id, (unsigned long long)generation, (unsigned long long)request); return -1;
    }
    if (bps && (bps < CALL_VIDEO_RATE_MIN_BPS || bps > v->rate_format.maximum_bps)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video rate invalid encoder result bps=%u", bps); return -1;
    }
    v->rate_pending = 0; v->applied_bps = bps;
    if (!bps) {
        v->admit = 0;
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video encoder rejected bitrate call=%llu generation=%llu request=%llu",
                    (unsigned long long)id, (unsigned long long)generation, (unsigned long long)request); return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video bitrate applied call=%llu generation=%llu request=%llu bps=%u",
                (unsigned long long)id, (unsigned long long)generation, (unsigned long long)request, bps); return 0;
}
void call_video_resume_receive(struct UTUN_INSTANCE *inst, uint64_t id) {
    struct call_session *s = active(inst, id);
    if (s) etcp_router_conn_resume_receive(etcp_router_conn_lookup(inst, s->group_id, s->peer_ed, ETCP_RT_ID_CALL_VIDEO));
}
void call_video_request_key(struct UTUN_INSTANCE *inst, uint64_t id) {
    struct call_session *s = active(inst, id);
    struct call_video_transport *v = s ? s->video : NULL;
    if (!v) return;
    v->key_request_pending = 1; send_key_request(v);
}
int call_video_decode_result(struct UTUN_INSTANCE *inst, uint64_t id, uint64_t generation, int accepted) {
    struct call_session *s = active(inst, id);
    struct call_video_transport *v = s ? s->video : NULL;
    if (!v || !generation || generation != v->received_generation || (accepted != 0 && accepted != 1)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video stale decoder result call=%llu generation=%llu",
            (unsigned long long)id, (unsigned long long)generation); return -1;
    }
    v->decode_generation = generation; v->decode_result = accepted; v->decode_pending = 1;
    if (!control_send(v, accepted ? VIDEO_DECODE_OK : VIDEO_DECODE_REJECT)) v->decode_pending = 0;
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "video decoder result call=%llu generation=%llu accepted=%d pending=%d",
        (unsigned long long)id, (unsigned long long)generation, accepted, v->decode_pending);
    return 0;
}
static int ready(const struct ETCP_ROUTER_CONN *route, const uint8_t *data, size_t length) {
    struct call_video_frame f;
    if (call_video_fragment_parse(data, length, &f)) return 1;
    struct call_session *s = active(route->inst, f.call_id);
    struct call_video_transport *v = s ? s->video : NULL;
    if (!v || s->group_id != route->group_id || memcmp(s->peer_ed, route->remote_ed, 32) || !v->ops.frame || f.offset) return 1;
    if (!v->have_peer || !call_video_format_supported(&v->peer, &v->local, &f.format)) return 1;
    return v->ops.receive_ready(v->arg, f.size);
}
static void receive(struct ETCP_CONN *conn, struct ll_entry *entry) {
    if (!entry || !entry->dgram || entry->len < ROUTER_SVC_PAYLOAD_OFF || !conn) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "video RX invalid router envelope");
        if (entry) { queue_dgram_free(entry); queue_entry_free(entry); } return;
    }
    const uint8_t *data = entry->dgram + ROUTER_SVC_PAYLOAD_OFF;
    size_t length = entry->len - ROUTER_SVC_PAYLOAD_OFF;
    if (!length) goto done; // Локальное уведомление роутера о закрытии, не медиапакет.
    if ((entry->dgram[ROUTER_SVC_FLAGS_OFF] & (ROUTER_FLAG_ENCRYPTED | ROUTER_FLAG_SIGNED))
        != (ROUTER_FLAG_ENCRYPTED | ROUTER_FLAG_SIGNED)) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "unprotected call video packet svc=%u flags=%02x bytes=%zu",
                   entry->dgram[0], entry->dgram[ROUTER_SVC_FLAGS_OFF], length);
        goto done;
    }
    uint64_t group, id = 0;
    const uint8_t* peer = entry->dgram + ROUTER_SVC_SRC_ED_OFF;
    memcpy(&group, entry->dgram + ROUTER_SVC_GROUP_OFF, 8);
    int control = entry->dgram[0] == ETCP_RT_ID_CALL_VIDEO_CONTROL;
    struct call_video_frame f;
    if (control) {
        if (length != 17 && length != 17 + CALL_VIDEO_CAPS_BYTES) goto invalid;
        memcpy(&id, data + 1, 8);
    } else {
        if (call_video_fragment_parse(data, length, &f)) goto done;
        id = f.call_id;
    }
    struct call_session *s = active(conn->instance, id);
    if (!s) goto done; // Уже завершённый звонок не держит router receive_q.
    if (s->group_id != group || memcmp(s->peer_ed, peer, 32)) goto invalid;
    struct call_video_transport *v = ensure(s);
    if (!v) goto done;
    if (control) {
        uint64_t revision; memcpy(&revision, data + 9, 8);
        if (data[0] == VIDEO_CAPS && length == 17 + CALL_VIDEO_CAPS_BYTES && revision) {
            struct call_video_caps caps;
            if (call_video_caps_parse(data + 17, CALL_VIDEO_CAPS_BYTES, &caps)) goto done;
            if (revision <= v->peer_revision) {
                if (revision == v->peer_revision) control_send(v, VIDEO_CAPS_ACK);
                goto done;
            }
            v->peer_revision = revision;
            v->peer = caps; v->have_peer = 1;
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "video peer capabilities call=%llu encode=%u/%u decode=%u/%u", (unsigned long long)id,
                       caps.encode[0].width, caps.encode[1].width, caps.decode[0].width, caps.decode[1].width);
            log_caps(id, "peer", &caps);
            if (v->ops.capabilities) v->ops.capabilities(v->arg, &v->peer);
            control_send(v, VIDEO_CAPS_ACK);
        } else if (data[0] == VIDEO_CAPS_ACK && length == 17 && revision && revision <= v->local_revision) {
            if (revision < v->local_revision) goto done;
            v->acknowledged_revision = revision;
            flush(v); writable(v);
            DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video capabilities acknowledged call=%llu revision=%llu",
                        (unsigned long long)id, (unsigned long long)revision);
        } else if (data[0] == VIDEO_KEY_REQUEST && length == 17 && !revision) {
            DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video peer requested KEY call=%llu retained_frames=%u retained_bytes=%u",
                        (unsigned long long)id, v->tx_count, v->tx_bytes);
            if (v->ops.need_key) v->ops.need_key(v->arg);
        } else if ((data[0] == VIDEO_DECODE_OK || data[0] == VIDEO_DECODE_REJECT) && length == 17 && revision) {
            if (revision != v->rate_format.generation) goto done;
            int accepted = data[0] == VIDEO_DECODE_OK;
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "video remote decoder result call=%llu generation=%llu accepted=%d",
                (unsigned long long)id, (unsigned long long)revision, accepted);
            if (!accepted) { v->tx_rejected = 1; v->admit = 0; if (v->tx_count) discard_tx(v, "decoder_rejected"); }
            if (v->ops.decoded) v->ops.decoded(v->arg, revision, accepted);
        } else goto invalid;
    } else if (v->ops.frame) {
        if (!v->have_peer || !call_video_format_supported(&v->peer, &v->local, &f.format)) goto invalid;
        struct call_video_frame complete;
        int result = call_video_receiver_push(v->receiver, &f, get_time_us(), &complete);
        if (result == 1) {
            if (complete.flags & CALL_VIDEO_KEY) v->key_request_pending = v->key_requested = 0;
            if (complete.pts_us % 1000000 < 34000)
                DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video timing stage=receive call=%llu frame=%llu pts=%lld boot_us=%llu bytes=%u",
                    (unsigned long long)id, (unsigned long long)complete.frame_id, (long long)complete.pts_us,
                    (unsigned long long)get_time_us(), complete.size);
            v->received_generation = complete.format.generation; v->ops.frame(v->arg, &complete);
        }
        else if (result < 0 || (!(f.flags & CALL_VIDEO_KEY) && call_video_receiver_needs_key(v->receiver)
            && (!v->key_requested || get_time_tb() - v->key_request_tb >= 20000)))
            call_video_request_key(conn->instance, id);
    }
    goto done;
invalid:
    DEBUG_WARN(DEBUG_CATEGORY_CALL, "video RX invalid peer/group/control/format call=%llu peer=%s bytes=%zu",
               (unsigned long long)id, sc_public_key_hex(peer, (char[65]){0}), length);
done:
    queue_dgram_free(entry); queue_entry_free(entry);
}
int call_video_transport_bind(struct UTUN_INSTANCE *inst) {
    if (etcp_router_bind(inst, ETCP_RT_ID_CALL_VIDEO_CONTROL, receive)) return -1;
    if (etcp_router_bind(inst, ETCP_RT_ID_CALL_VIDEO, receive)) {
        etcp_router_unbind(inst, ETCP_RT_ID_CALL_VIDEO_CONTROL); return -1;
    }
    if (etcp_router_require_crypto(inst, ETCP_RT_ID_CALL_VIDEO_CONTROL, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT)
        || etcp_router_set_authorize(inst, ETCP_RT_ID_CALL_VIDEO_CONTROL, topo_group_chat_key_allowed)
        || etcp_router_require_crypto(inst, ETCP_RT_ID_CALL_VIDEO, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT)
        || etcp_router_set_authorize(inst, ETCP_RT_ID_CALL_VIDEO, topo_group_chat_key_allowed)) {
        etcp_router_unbind(inst, ETCP_RT_ID_CALL_VIDEO); etcp_router_unbind(inst, ETCP_RT_ID_CALL_VIDEO_CONTROL); return -1;
    }
    etcp_router_set_receive_ready(inst, ETCP_RT_ID_CALL_VIDEO, ready); return 0;
}
void call_video_transport_unbind(struct UTUN_INSTANCE *inst) {
    etcp_router_set_receive_ready(inst, ETCP_RT_ID_CALL_VIDEO, NULL);
    etcp_router_unbind(inst, ETCP_RT_ID_CALL_VIDEO); etcp_router_unbind(inst, ETCP_RT_ID_CALL_VIDEO_CONTROL);
}
