#include <string.h>

#include "../../lib/debug_config.h"
#include "../transport_layer/etcp_api.h"
#include "stream_wire.h"

/* Заголовок одинаков у raw и router payload; cmd raw сохраняется внутри routed-кодограммы. */
int stream_wire_parse(const uint8_t *wire, size_t length, struct stream_message *m) {
    if (!wire || !m || length < STREAM_WIRE_HEADER || wire[0] != ETCP_ID_STREAM || wire[1] != STREAM_WIRE_VERSION
        || wire[2] < STREAM_CMD_INFO_REQ || wire[2] > STREAM_CMD_STATS) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream wire header invalid bytes=%zu", length); return STREAM_ERR_FORMAT;
    }
    struct stream_message result = {.command = wire[2], .subblock = wire[3], .payload = wire + STREAM_WIRE_HEADER,
        .length = length - STREAM_WIRE_HEADER};
    memcpy(result.descriptor.group_key, wire + STREAM_WIRE_GROUP_OFF, SC_PUBKEY_SIZE);
    result.descriptor.source = stream_address_read(wire + 36);
    result.descriptor.source_epoch = stream_read_be(wire + 68, 8);
    result.sender_epoch = stream_read_be(wire + 76, 8); result.receiver_epoch = stream_read_be(wire + 84, 8);
    result.handle = stream_read_be(wire + 92, 8);
    if (!stream_address_set(stream_address_read(result.descriptor.group_key)) || !stream_address_set(result.descriptor.source) || !result.descriptor.source_epoch || !result.sender_epoch) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream wire zero key/epoch command=%u", result.command); return STREAM_ERR_FORMAT;
    }
    *m = result; return STREAM_OK;
}

void stream_wire_header(uint8_t *wire, const struct stream_message *m) {
    wire[0] = ETCP_ID_STREAM; wire[1] = STREAM_WIRE_VERSION; wire[2] = m->command; wire[3] = m->subblock;
    memcpy(wire + STREAM_WIRE_GROUP_OFF, m->descriptor.group_key, SC_PUBKEY_SIZE);
    memcpy(wire + 36, m->descriptor.source.ed, SC_PUBKEY_SIZE);
    stream_write_be(wire + 68, m->descriptor.source_epoch, 8); stream_write_be(wire + 76, m->sender_epoch, 8);
    stream_write_be(wire + 84, m->receiver_epoch, 8); stream_write_be(wire + 92, m->handle, 8);
}

/* Подсказка не меняет подписанные RS-байты и не используется как доверенный media index. */
void stream_frame_timing_write(uint8_t *wire, const struct stream_frame_timing *t) {
    memset(wire, 0, STREAM_FRAME_TIMING_SIZE);
    stream_write_be(wire, t->first_frame, 8); stream_write_be(wire + 8, t->last_frame, 8);
    stream_write_be(wire + 16, t->first_pts, 8); stream_write_be(wire + 24, t->last_pts, 8);
    stream_write_be(wire + 32, t->period_us, 4); stream_write_be(wire + 36, t->started, 4);
    stream_write_be(wire + 40, t->audio_us, 4);
    wire[44] = t->valid | (t->video << 1) | (t->continuation << 2);
}

int stream_frame_timing_read(const uint8_t *wire, size_t length, struct stream_frame_timing *out) {
    if (!wire || !out || length < STREAM_FRAME_TIMING_SIZE || (wire[44] & ~7u) || wire[45] || wire[46] || wire[47]) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream frame timing invalid header bytes=%zu", length); return STREAM_ERR_FORMAT;
    }
    struct stream_frame_timing t = {.first_frame = stream_read_be(wire, 8), .last_frame = stream_read_be(wire + 8, 8),
        .first_pts = stream_read_be(wire + 16, 8), .last_pts = stream_read_be(wire + 24, 8),
        .period_us = (uint32_t)stream_read_be(wire + 32, 4), .started = (uint32_t)stream_read_be(wire + 36, 4),
        .audio_us = (uint32_t)stream_read_be(wire + 40, 4), .valid = wire[44] & 1,
        .video = (wire[44] >> 1) & 1, .continuation = (wire[44] >> 2) & 1};
    if ((!t.valid && (t.video || t.continuation || t.audio_us || t.period_us || t.started || t.first_frame || t.last_frame
                     || t.first_pts || t.last_pts))
        || (!t.video && (t.continuation || t.started || t.first_frame || t.last_frame || t.first_pts || t.last_pts))
        || (t.video && (t.last_frame < t.first_frame || t.last_pts < t.first_pts || t.last_pts > INT64_MAX
                       || t.started > 16 || (!t.started && !t.continuation))) || t.audio_us > 320000) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream frame timing invalid values frames=%u video=%u continuation=%u audio_us=%u",
                   t.started, t.video, t.continuation, t.audio_us); return STREAM_ERR_FORMAT;
    }
    *out = t; return STREAM_OK;
}

size_t stream_telemetry_write(uint8_t *wire, const struct stream_telemetry *t) {
    wire[0] = t->count;
    for (unsigned i = 0; i < t->count; i++) {
        const struct stream_telemetry_row *r = &t->rows[i]; uint8_t *p = wire + 1 + i * STREAM_TELEMETRY_ROW_SIZE;
        memcpy(p, r->node.ed, SC_PUBKEY_SIZE); stream_write_be(p + 32, r->buffered_us, 8); stream_write_be(p + 40, r->sample, 8);
        stream_write_be(p + 48, r->frames, 4); stream_write_be(p + 52, r->age_ms, 4); p[56] = r->flags;
    }
    return 1 + t->count * STREAM_TELEMETRY_ROW_SIZE;
}

int stream_telemetry_read(const uint8_t *wire, size_t length, struct stream_telemetry *out) {
    if (!wire || !out || !length || !wire[0] || wire[0] > STREAM_PATH_MAX
        || length != 1u + wire[0] * STREAM_TELEMETRY_ROW_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream telemetry invalid size=%zu", length); return STREAM_ERR_FORMAT;
    }
    struct stream_telemetry t = {.count = wire[0]};
    for (unsigned i = 0; i < t.count; i++) {
        const uint8_t *p = wire + 1 + i * STREAM_TELEMETRY_ROW_SIZE;
        struct stream_telemetry_row *r = &t.rows[i];
        *r = (struct stream_telemetry_row){.node = stream_address_read(p), .buffered_us = stream_read_be(p + 32, 8),
            .sample = stream_read_be(p + 40, 8), .frames = (uint32_t)stream_read_be(p + 48, 4),
            .age_ms = (uint32_t)stream_read_be(p + 52, 4), .flags = p[56]};
        if (!stream_address_set(r->node) || r->buffered_us > INT64_MAX || (r->flags & ~7u)
            || (!(r->flags & STREAM_TELEMETRY_KNOWN) && (r->buffered_us || r->frames))) {
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream telemetry invalid row=%u flags=%u", i, r->flags); return STREAM_ERR_FORMAT;
        }
        for (unsigned j = 0; j < i; j++) if (stream_address_equal(t.rows[j].node, r->node)) {
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream telemetry repeated node row=%u", i); return STREAM_ERR_FORMAT;
        }
    }
    *out = t; return STREAM_OK;
}

void stream_telemetry_age(struct stream_telemetry *t, uint64_t now) {
    uint64_t elapsed = now >= t->received_ms ? now - t->received_ms : 0;
    for (unsigned i = 0; i < t->count; i++) {
        struct stream_telemetry_row *r = &t->rows[i];
        r->age_ms = elapsed > UINT32_MAX - r->age_ms ? UINT32_MAX : r->age_ms + (uint32_t)elapsed;
        if (r->age_ms >= STREAM_TELEMETRY_STALE_MS) r->flags |= STREAM_TELEMETRY_STALE;
    }
    t->received_ms = now;
}

uint64_t stream_telemetry_total(const struct stream_telemetry *t) {
    uint64_t total = 0;
    for (unsigned i = 0; i < t->count; i++) {
        uint64_t value = t->rows[i].buffered_us;
        total = value > UINT64_MAX - total ? UINT64_MAX : total + value;
    }
    return total;
}

/* История блока проверяется отдельно от BGP-пути; получатель не должен уже встречаться в ней. */
int stream_path_validate(const struct stream_path *path, struct stream_address source, struct stream_address sender, struct stream_address forbidden) {
    if (!path || !path->count || path->count > STREAM_PATH_MAX || !stream_address_equal(path->nodes[0], source)
        || !stream_address_equal(path->nodes[path->count - 1], sender)) goto invalid;
    for (unsigned i = 0; i < path->count; i++) {
        if (!stream_address_set(path->nodes[i]) || stream_address_equal(path->nodes[i], forbidden)) goto invalid;
        for (unsigned j = 0; j < i; j++) if (stream_address_equal(path->nodes[i], path->nodes[j])) goto invalid;
    }
    return STREAM_OK;
invalid:
    DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream path invalid/loop source=%s sender=%s receiver=%s count=%u",
               sc_public_key_hex(source.ed, (char[65]){0}), sc_public_key_hex(sender.ed, (char[65]){0}),
               sc_public_key_hex(forbidden.ed, (char[65]){0}), path ? path->count : 0);
    return STREAM_ERR_FORMAT;
}

int stream_path_parse(const uint8_t *wire, size_t length, struct stream_path *path, size_t *used) {
    if (!wire || !path || !used || !length || !wire[0] || wire[0] > STREAM_PATH_MAX || length < 1u + wire[0] * SC_PUBKEY_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream path size invalid bytes=%zu", length); return STREAM_ERR_FORMAT;
    }
    struct stream_path result = {.count = wire[0]};
    for (unsigned i = 0; i < result.count; i++) result.nodes[i] = stream_address_read(wire + 1 + i * SC_PUBKEY_SIZE);
    *path = result; *used = 1 + result.count * SC_PUBKEY_SIZE; return STREAM_OK;
}

size_t stream_path_write(uint8_t *wire, const struct stream_path *path) {
    wire[0] = path->count;
    for (unsigned i = 0; i < path->count; i++) memcpy(wire + 1 + i * SC_PUBKEY_SIZE, path->nodes[i].ed, SC_PUBKEY_SIZE);
    return 1 + path->count * SC_PUBKEY_SIZE;
}

/* Неизвестное измерение явно отличается от нулевого lag/rate; числа не сужаются до старых uint16 RTT. */
void stream_metric_write(uint8_t *wire, const struct stream_metric_snapshot *s, int valid) {
    wire[0] = valid ? 1 | (s->source_rate_valid ? 2 : 0) | (s->receive_rate_valid ? 4 : 0) | (s->stopped ? 8 : 0) : 0;
    stream_write_be(wire + 1, s->lag_ms, 8); stream_write_be(wire + 9, s->source_rate_bytes_sec, 8);
    stream_write_be(wire + 17, s->receive_rate_bytes_sec, 8); stream_write_be(wire + 25, s->blocks, 8);
    stream_write_be(wire + 33, s->late_blocks, 8); stream_write_be(wire + 41, s->stops, 8);
}

int stream_metric_read(const uint8_t *wire, struct stream_metric_snapshot *s, int *valid) {
    if (wire[0] & ~15u || (!(wire[0] & 1) && wire[0])) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream metric flags invalid=%u", wire[0]); return STREAM_ERR_FORMAT;
    }
    *s = (struct stream_metric_snapshot){.lag_ms = stream_read_be(wire + 1, 8), .source_rate_bytes_sec = stream_read_be(wire + 9, 8),
        .receive_rate_bytes_sec = stream_read_be(wire + 17, 8), .blocks = stream_read_be(wire + 25, 8),
        .late_blocks = stream_read_be(wire + 33, 8), .stops = stream_read_be(wire + 41, 8),
        .source_rate_valid = !!(wire[0] & 2), .receive_rate_valid = !!(wire[0] & 4), .stopped = !!(wire[0] & 8)};
    *valid = !!(wire[0] & 1); return STREAM_OK;
}

/* END имеет собственный domain и подписывает ключ запуска + последний индекс; UINT64_MAX означает пустой поток. */
int stream_end_sign(const struct stream_descriptor *d, uint64_t last, const uint8_t key[32], uint8_t wire[STREAM_END_SIZE]) {
    if (!d || !key || !wire || !stream_address_set(stream_address_read(d->group_key))) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream END sign invalid arguments"); return STREAM_ERR_ARGUMENT; }
    memcpy(wire, "stream_end", 10); memcpy(wire + 10, d->group_key, SC_PUBKEY_SIZE);
    memcpy(wire + 42, d->source.ed, SC_PUBKEY_SIZE); stream_write_be(wire + 74, d->source_epoch, 8);
    stream_write_be(wire + STREAM_END_LAST_OFF, last, 8);
    if (sc_ed25519_sign(key, wire, STREAM_END_SIGNED_SIZE, wire + STREAM_END_SIGNED_SIZE) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream END signature failed"); return STREAM_ERR_CRYPTO;
    }
    return STREAM_OK;
}

int stream_end_verify(const struct stream_descriptor *d, const uint8_t key[32], const uint8_t *wire, size_t length, uint64_t *last) {
    if (!d || !key || memcmp(key, d->source.ed, SC_PUBKEY_SIZE) || !wire || !last || length != STREAM_END_SIZE || memcmp(wire, "stream_end", 10)
        || memcmp(wire + 10, d->group_key, SC_PUBKEY_SIZE) || memcmp(wire + 42, d->source.ed, SC_PUBKEY_SIZE)
        || stream_read_be(wire + 74, 8) != d->source_epoch || sc_ed25519_verify(d->source.ed, wire, STREAM_END_SIGNED_SIZE, wire + STREAM_END_SIGNED_SIZE) != SC_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream END invalid signature/key bytes=%zu", length); return STREAM_ERR_SIGNATURE;
    }
    *last = stream_read_be(wire + STREAM_END_LAST_OFF, 8); return STREAM_OK;
}
