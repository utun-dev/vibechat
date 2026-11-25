#include "ble_backend.h"
#include "../u_async.h"
#include "../mem.h"
#include "../debug_config.h"
#include <string.h>
#define CAT DEBUG_CATEGORY_DISCOVERY
#define HELLO 1
#define ACCEPT 2
#define FRAME 3
#define HEADER 7
#define FLOW_HEADER 2
#define CONTROL_HEADER 2
#define CONTROL_SIZE (2 * BLE_PUBLIC_KEY_SIZE)
#define FLOW_MIN_VALUE (FLOW_HEADER + HEADER + 1)
#define FLOW_SLOTS (BLE_TX_WINDOW_BYTES / FLOW_MIN_VALUE)
#define DEADLINE_US 15000000ULL
#define STATS_US 5000000ULL
struct ble_stats {
    uint64_t since, tx_bytes, rx_bytes, wait_us, max_wait_us;
    unsigned tx_frames, rx_frames, fragments, dropped, missing, ack_tx, ack_rx, max_flight, max_frames;
};
struct ble_channel_state {
    struct ll_queue *tx, *rx;
    struct ll_entry* current;
    uint64_t rx_deadline;
    uint16_t sequence, offset, rx_sequence, rx_size, rx_offset, dropped_sequence, rx_next;
    int blocked, dropping, rx_seen;
    uint8_t assembly[BLE_INVITE_FRAME_MAX];
};
struct ble_connection {
    struct ble_transport* owner;
    ble_link_id id;
    uint64_t peer, deadline, submitted;
    struct ble_stats stats;
    struct ble_endpoint endpoint;
    struct ble_channel_state channels[BLE_CHANNEL_COUNT];
    unsigned int capacity; /* Безопасный TX budget; после reuse ACL backend может знать только минимум MTU23. */
    int rx_above_tx_logged;
    uint16_t pending_bytes;
    /* ID монотонен по модулю 65536; кольцо хранит размеры и границы кадров, собственных ретрансляций нет. */
    uint16_t tx_next, tx_acked, rx_last, ack_sent, flight_head, flight_count, flight_bytes;
    struct { uint16_t size; uint8_t frame_end; } flight[FLOW_SLOTS]; /* Последний фрагмент освобождает слот кадра. */
    unsigned tx_frames_pending; /* Принятые DATA, включая очередь и текущий кадр, до удалённого ACK конца. */
    uint64_t flight_deadline, ack_due, ack_completed;
    void* ack_timer;
    int rx_flow_seen, ack_dirty, pending_ack;
    enum ble_channel pending_channel, next_channel;
    int outgoing, opened, ready, pending, profile_only, profile_done, profile_validated;
    uint8_t peer_key[BLE_PUBLIC_KEY_SIZE];
    uint8_t control_tx[CONTROL_SIZE], control_rx[CONTROL_SIZE];
    uint8_t control_type, control_tx_offset, control_rx_offset;
};
struct ble_transport {
    struct UASYNC* ua;
    struct ble_config config;
    char adapter[64];
    struct ble_backend* backend;
    struct ble_advertising_power advertising_power;
    struct ble_connection links[BLE_LINKS_MAX];
    ble_link_id sequence;
    void* timer;
    int started, ready, reject_incoming, initial_advertisement_pending;
};
static uint16_t read16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void write16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static int key_empty(const uint8_t* key) { return !memcmp(key, (const uint8_t[BLE_PUBLIC_KEY_SIZE]){0}, BLE_PUBLIC_KEY_SIZE); }
/* Диагностический отпечаток зашифрованного кадра для сопоставления журналов двух узлов. */
static uint32_t frame_tag(const uint8_t* data, size_t len) {
    uint32_t tag = 2166136261u;
    for (size_t i = 0; i < len; i++) tag = (tag ^ data[i]) * 16777619u;
    return tag;
}
static void release_entry(struct ll_entry* e) { if (e) { queue_dgram_free(e); queue_entry_free(e); } }
static void pump(struct ble_transport* t, struct ble_connection* c);
static struct ble_connection* lookup(struct ble_transport* t, ble_link_id id) {
    if (t && id) for (int i = 0; i < BLE_LINKS_MAX; i++) if (t->links[i].id == id) return &t->links[i];
    return NULL;
}
/* Сводка раз в пять секунд и при закрытии: payload кадров, очереди и локальное ожидание GATT. */
static void log_stats(struct ble_connection* c, uint64_t now) {
    struct ble_stats* s = &c->stats;
    uint64_t elapsed = now > s->since ? now - s->since : 1;
    if (s->tx_frames || s->rx_frames || s->fragments || s->dropped || c->pending || c->flight_count) {
        DEBUG_DEBUG(DEBUG_CATEGORY_DEBUG, "BLE traffic link=%llu outgoing=%d elapsed_ms=%llu TX_Bps=%llu RX_Bps=%llu TX_frames=%u RX_frames=%u "
                    "fragments=%u drops=%u missing=%u wait_avg_us=%llu wait_max_us=%llu DATA_queue=%d INVITE_queue=%d pending_ms=%llu",
                    (unsigned long long)c->id, c->outgoing, (unsigned long long)(elapsed / 1000),
                    (unsigned long long)(s->tx_bytes * 1000000 / elapsed), (unsigned long long)(s->rx_bytes * 1000000 / elapsed),
                    s->tx_frames, s->rx_frames, s->fragments, s->dropped, s->missing,
                    (unsigned long long)(s->fragments ? s->wait_us / s->fragments : 0), (unsigned long long)s->max_wait_us,
                    queue_entry_count(c->channels[BLE_CHANNEL_DATA].tx) + !!c->channels[BLE_CHANNEL_DATA].current,
                    queue_entry_count(c->channels[BLE_CHANNEL_INVITE].tx) + !!c->channels[BLE_CHANNEL_INVITE].current,
                    (unsigned long long)(c->pending ? (now - c->submitted) / 1000 : 0));
        DEBUG_DEBUG(DEBUG_CATEGORY_DEBUG, "BLE flow link=%llu flight=%u/%u bytes=%u/%u peak_bytes=%u tx_next=%u acked=%u "
                    "rx_last=%u ACK_tx=%u ACK_rx=%u ack_dirty=%d ack_pending=%d frames=%u/%u peak_frames=%u",
                    (unsigned long long)c->id, c->flight_count, (unsigned)FLOW_SLOTS, c->flight_bytes,
                    BLE_TX_WINDOW_BYTES, s->max_flight, c->tx_next, c->tx_acked, c->rx_last,
                    s->ack_tx, s->ack_rx, c->ack_dirty, c->pending_ack && c->pending,
                    c->tx_frames_pending, c->owner->config.tx_window, s->max_frames);
    }
    memset(s, 0, sizeof(*s)); s->since = now;
}
int ble_get_outgoing(struct ble_transport* t, ble_link_id id, int* outgoing) {
    struct ble_connection* c = lookup(t, id);
    if (!c || !outgoing) { DEBUG_ERROR(CAT, "BLE direction: invalid link=%llu/output", (unsigned long long)id); return -1; }
    *outgoing = c->outgoing;
    return 0;
}
int ble_get_peer_key(struct ble_transport* t, ble_link_id id, uint8_t key[BLE_PUBLIC_KEY_SIZE]) {
    struct ble_connection* c = lookup(t, id);
    if (!key || !c || !c->ready) return BLE_NOT_READY;
    memcpy(key, c->peer_key, BLE_PUBLIC_KEY_SIZE);
    return BLE_OK;
}
static void close_link(struct ble_transport* t, struct ble_connection* c, const char* reason) {
    ble_link_id id = c->id;
    uint64_t peer = c->peer;
    uint64_t started = get_time_us();
    log_stats(c, get_time_us());
    DEBUG_INFO(CAT, "BLE closing link=%llu peer=%016llx reason=%s", (unsigned long long)id, (unsigned long long)peer, reason);
    DEBUG_DEBUG(DEBUG_CATEGORY_DEBUG,
                "BLE transport close begin link=%llu endpoint=%s outgoing=%d opened=%d ready=%d pending=%d profile_only=%d flight=%u/%u",
                (unsigned long long)id, c->endpoint.id, c->outgoing, c->opened, c->ready, c->pending, c->profile_only,
                c->flight_count, c->flight_bytes);
    if (t->backend) ble_backend_close(t->backend, id);
    DEBUG_DEBUG(DEBUG_CATEGORY_DEBUG, "BLE transport backend close returned link=%llu elapsed_us=%llu",
                (unsigned long long)id, (unsigned long long)(get_time_us() - started));
    if (c->ack_timer) uasync_cancel_timeout(t->ua, c->ack_timer);
    for (int i = 0; i < BLE_CHANNEL_COUNT; i++) {
        struct ble_channel_state* ch = &c->channels[i];
        release_entry(ch->current);
        queue_clear(ch->tx); queue_free(ch->tx);
        queue_clear(ch->rx); queue_free(ch->rx);
    }
    int profile_only = c->profile_only;
    memset(c, 0, sizeof(*c));
    DEBUG_INFO(CAT, "BLE closed link=%llu peer=%016llx reason=%s elapsed_us=%llu", (unsigned long long)id,
               (unsigned long long)peer, reason, (unsigned long long)(get_time_us() - started));
    if (!profile_only && t->config.callbacks.connection) t->config.callbacks.connection(t->config.arg, id, peer, BLE_CLOSED, reason);
}
uint64_t ble_backend_peer(struct ble_transport* t, ble_link_id id) {
    struct ble_connection* c = t ? lookup(t, id) : NULL; return c ? c->peer : 0;
}
static struct ble_connection* new_link(struct ble_transport* t, const struct ble_endpoint* ep, int outgoing, uint64_t peer, int profile_only) {
    if (!t || !t->started || !ep || !memchr(ep->id, 0, sizeof(ep->id)) || !ep->id[0]) {
        DEBUG_ERROR(CAT, "BLE invalid connection request"); return NULL;
    }
    for (int i = 0; i < BLE_LINKS_MAX; i++) if (t->links[i].id && !strcmp(t->links[i].endpoint.id, ep->id)) {
        /* Метаданные могут читаться через отдельный client handle поверх уже принадлежащего incoming ACL. */
        if ((profile_only && !t->links[i].outgoing) || (!outgoing && t->links[i].profile_only)) continue;
        DEBUG_WARN(CAT, "BLE duplicate connection endpoint=%s", ep->id); return NULL;
    }
    for (int i = 0; i < BLE_LINKS_MAX; i++) if (!t->links[i].id) {
        struct ble_connection* c = &t->links[i];
        int allocated = 1;
        for (int ch = 0; ch < BLE_CHANNEL_COUNT; ch++) {
            c->channels[ch].tx = queue_new(t->ua, 0, 0, 0, "ble_tx");
            c->channels[ch].rx = queue_new(t->ua, 0, 0, 0, "ble_rx");
            if (!c->channels[ch].tx || !c->channels[ch].rx) allocated = 0;
        }
        if (!allocated) {
            DEBUG_ERROR(CAT, "BLE queue allocation failed");
            for (int ch = 0; ch < BLE_CHANNEL_COUNT; ch++) { queue_free(c->channels[ch].tx); queue_free(c->channels[ch].rx); }
            memset(c, 0, sizeof(*c)); return NULL;
        }
        c->owner = t; c->id = ++t->sequence; c->peer = peer; c->endpoint = *ep; c->outgoing = outgoing; c->tx_next = 1;
        c->profile_only = profile_only;
        c->capacity = 20; c->stats.since = get_time_us(); c->deadline = c->stats.since + DEADLINE_US;
        DEBUG_INFO(CAT, "BLE connecting link=%llu endpoint=%s outgoing=%d", (unsigned long long)c->id, ep->id, outgoing);
        if (!profile_only && t->config.callbacks.connection) t->config.callbacks.connection(t->config.arg, c->id, peer, BLE_STARTING, "connecting");
        return c;
    }
    DEBUG_WARN(CAT, "BLE connection limit=%d", BLE_LINKS_MAX); return NULL;
}
/* writable отражает и свою очередь, и кредит удалённого приёмника. */
static void wake_writer(struct ble_transport* t, struct ble_connection* c, enum ble_channel channel) {
    struct ble_channel_state* ch = &c->channels[channel];
    if (!ch->blocked || !ble_channel_tx_ready(t, c->id, channel)) return;
    ch->blocked = 0;
    DEBUG_TRACE(CAT, "BLE TX resumed link=%llu channel=%d flight_bytes=%u", (unsigned long long)c->id, channel, c->flight_bytes);
    void (*writable)(void*, ble_link_id) = channel == BLE_CHANNEL_DATA ? t->config.callbacks.writable : t->config.callbacks.invite_writable;
    if (writable) writable(t->config.arg, c->id);
}
static void ack_timer(void* arg) {
    struct ble_connection* c = arg; c->ack_timer = NULL;
    c->ack_dirty = 1;
    pump(c->owner, c);
}
/* Свежий ID объединяем до 15ms, последний повторяем раз в 100ms даже при полном окне отправителя. */
static int schedule_ack(struct ble_transport* t, struct ble_connection* c, int fresh) {
    if (fresh && c->ack_dirty) return 1;
    if (fresh) c->ack_dirty = 1;
    uint64_t now = get_time_us();
    c->ack_due = c->ack_completed ? c->ack_completed + (c->ack_dirty ? BLE_ACK_INTERVAL_US : BLE_ACK_REPEAT_US) :
        now + BLE_ACK_INTERVAL_US;
    if (c->ack_timer) { uasync_cancel_timeout(t->ua, c->ack_timer); c->ack_timer = NULL; }
    if (c->pending && now >= c->ack_due) return 1;
    /* uasync использует усечённые TB: дополнительный TB исключает срабатывание раньше 15ms. */
    uint64_t delay = c->ack_due > now ? (c->ack_due - now + 199) / 100 : 0;
    c->ack_timer = uasync_set_timeout(t->ua, (int)delay, c, ack_timer, "ble_ack");
    if (c->ack_timer) return 1;
    DEBUG_ERROR(CAT, "BLE ACK timer allocation failed link=%llu rx_last=%u", (unsigned long long)c->id, c->rx_last);
    close_link(t, c, "ACK timer allocation failed"); return 0;
}
static void pump(struct ble_transport* t, struct ble_connection* c) {
    if (!c->opened || c->pending) return;
    uint8_t value[BLE_VALUE_MAX];
    size_t len;
    c->pending_bytes = 0;
    c->pending_ack = c->ack_dirty && get_time_us() >= c->ack_due;
    c->pending_channel = BLE_CHANNEL_DATA;
    if (c->pending_ack) { write16(value, c->rx_last); len = 2; }
    else if (c->control_type) {
        unsigned capacity = c->capacity, credit = BLE_TX_WINDOW_BYTES - c->flight_bytes;
        if (capacity > credit) capacity = credit;
        unsigned prefix = FLOW_HEADER + CONTROL_HEADER;
        if (capacity <= prefix || c->flight_count == FLOW_SLOTS) return;
        unsigned count = CONTROL_SIZE - c->control_tx_offset;
        if (count > capacity - prefix) count = capacity - prefix;
        value[FLOW_HEADER] = c->control_type; value[FLOW_HEADER + 1] = c->control_tx_offset;
        memcpy(value + prefix, c->control_tx + c->control_tx_offset, count);
        len = prefix + count; c->pending_bytes = count;
    }
    else {
        if (!c->ready) return;
        struct ble_channel_state* ch = NULL;
        for (int i = 0; i < BLE_CHANNEL_COUNT; i++) {
            enum ble_channel channel = (c->next_channel + i) % BLE_CHANNEL_COUNT;
            struct ble_channel_state* candidate = &c->channels[channel];
            if (channel == BLE_CHANNEL_DATA &&
                (BLE_TX_WINDOW_BYTES - c->flight_bytes < FLOW_MIN_VALUE || c->flight_count == FLOW_SLOTS)) {
                candidate->blocked = 1; continue;
            }
            if (!candidate->current) {
                candidate->current = queue_data_get(candidate->tx); candidate->offset = 0;
                if (candidate->current) ++candidate->sequence;
            }
            if (candidate->current) { ch = candidate; c->pending_channel = channel; break; }
        }
        if (!ch) return;
        c->next_channel = (c->pending_channel + 1) % BLE_CHANNEL_COUNT;
        unsigned prefix = c->pending_channel == BLE_CHANNEL_DATA ? FLOW_HEADER : 0;
        unsigned capacity = c->capacity;
        if (prefix && capacity > BLE_TX_WINDOW_BYTES - c->flight_bytes) capacity = BLE_TX_WINDOW_BYTES - c->flight_bytes;
        size_t n = ch->current->len - ch->offset;
        if (n > capacity - HEADER - prefix) n = capacity - HEADER - prefix;
        uint8_t* body = value + prefix;
        body[0] = FRAME; write16(body + 1, ch->sequence); write16(body + 3, ch->current->len); write16(body + 5, ch->offset);
        memcpy(body + HEADER, ch->current->dgram + ch->offset, n); len = prefix + HEADER + n; c->pending_bytes = n;
    }
    enum ble_wire_channel wire = c->pending_ack ? BLE_WIRE_ACK :
        c->pending_channel == BLE_CHANNEL_DATA ? BLE_WIRE_DATA : BLE_WIRE_INVITE;
    if (wire == BLE_WIRE_DATA) write16(value, c->tx_next);
    int result = ble_backend_send(t->backend, c->id, wire, value, len);
    if (result == BLE_OK) {
        c->pending = 1; c->submitted = get_time_us(); c->deadline = c->submitted + DEADLINE_US;
        if (wire == BLE_WIRE_ACK) {
            if (c->ack_timer) { uasync_cancel_timeout(t->ua, c->ack_timer); c->ack_timer = NULL; }
            c->ack_dirty = 0; c->stats.ack_tx++;
            DEBUG_TRACE(CAT, "BLE ACK TX link=%llu id=%u repeat=%d",
                        (unsigned long long)c->id, c->rx_last, c->ack_sent == c->rx_last);
            c->ack_sent = c->rx_last;
        } else if (wire == BLE_WIRE_DATA) {
            if (!c->flight_count) c->flight_deadline = c->submitted + DEADLINE_US;
            unsigned slot = (c->flight_head + c->flight_count++) % FLOW_SLOTS;
            struct ble_channel_state* ch = &c->channels[BLE_CHANNEL_DATA];
            c->flight[slot].size = len;
            c->flight[slot].frame_end = !c->control_type && ch->current && ch->offset + c->pending_bytes == ch->current->len;
            c->flight_bytes += len;
            if (c->flight_bytes > c->stats.max_flight) c->stats.max_flight = c->flight_bytes;
            DEBUG_TRACE(CAT, "BLE DATA TX link=%llu seq=%u type=%u bytes=%zu flight=%u/%u",
                        (unsigned long long)c->id, c->tx_next, value[FLOW_HEADER], len, c->flight_count, c->flight_bytes);
            c->tx_next++;
            if (!ble_tx_ready(t, c->id)) c->channels[BLE_CHANNEL_DATA].blocked = 1;
        }
    }
    else { DEBUG_ERROR(CAT, "BLE backend send failed link=%llu result=%d", (unsigned long long)c->id, result);
        close_link(t, c, "backend send failed"); }
}
static void tick(void* arg) {
    struct ble_transport* t = arg; t->timer = NULL;
    uint64_t now = get_time_us();
    for (int i = 0; i < BLE_LINKS_MAX; i++) {
        struct ble_connection* c = &t->links[i];
        if (!c->id) continue;
        if (c->profile_done) { close_link(t, c, "profile read complete"); continue; }
        if (now - c->stats.since >= STATS_US) log_stats(c, now);
        if (c->flight_count && now >= c->flight_deadline) {
            DEBUG_WARN(CAT, "BLE ACK timeout link=%llu acked=%u next=%u flight_packets=%u flight_bytes=%u",
                       (unsigned long long)c->id, c->tx_acked, c->tx_next, c->flight_count, c->flight_bytes);
            close_link(t, c, "ACK progress timeout"); continue;
        }
        if ((!c->ready || c->pending) && now >= c->deadline) {
            DEBUG_WARN(CAT, "BLE handshake/send timeout link=%llu pending=%d", (unsigned long long)c->id, c->pending);
            close_link(t, c, "operation timeout"); continue;
        }
        for (int channel = 0; channel < BLE_CHANNEL_COUNT; channel++) {
            struct ble_channel_state* ch = &c->channels[channel];
            if (ch->rx_size && now >= ch->rx_deadline) {
                DEBUG_WARN(CAT, "BLE frame timeout link=%llu received=%u expected=%u",
                           (unsigned long long)c->id, ch->rx_offset, ch->rx_size);
                ch->dropping = 1; ch->dropped_sequence = ch->rx_sequence;
                c->stats.dropped++;
                ch->rx_size = ch->rx_offset = 0;
            }
        }
    }
    t->timer = uasync_set_timeout(t->ua, 1000, t, tick, "ble_tick");
    if (!t->timer) { DEBUG_ERROR(CAT, "BLE timer allocation failed"); ble_backend_status(t, 0, "timer allocation failed"); }
}
struct ble_transport* ble_create(struct UASYNC* ua, const struct ble_config* config) {
    if (!ua || !config || !config->node_id || !config->discovery_id || key_empty(config->public_key) ||
        config->discovery_id(config->public_key) != config->node_id || config->tx_limit > BLE_QUEUE_MAX || config->tx_window > BLE_QUEUE_MAX ||
        config->initial_advertisement_size > BLE_BROADCAST_MAX || !memchr(config->name, 0, sizeof(config->name)) ||
        (config->adapter && strlen(config->adapter) >= 64)) {
        DEBUG_ERROR(CAT, "BLE create invalid configuration"); return NULL;
    }
    if (ble_name_validate(config->name)) return NULL;
    struct ble_transport* t = u_calloc(1, sizeof(*t));
    if (!t) { DEBUG_ERROR(CAT, "BLE create allocation failed"); return NULL; }
    t->ua = ua; t->config = *config;
    t->advertising_power = (struct ble_advertising_power){127, -1};
    if (!t->config.tx_limit) t->config.tx_limit = BLE_QUEUE_MAX;
    if (!t->config.tx_window) t->config.tx_window = BLE_TX_WINDOW_FRAMES;
    if (config->adapter) { strcpy(t->adapter, config->adapter); t->config.adapter = t->adapter; }
    return t;
}
int ble_start(struct ble_transport* t) {
    if (!t || t->started) { DEBUG_ERROR(CAT, "BLE invalid/repeated start"); return BLE_ERROR; }
    t->started = 1;
    t->initial_advertisement_pending = t->config.initial_advertisement_size != 0;
    t->backend = ble_backend_create(t, t->ua, &t->config);
    if (!t->backend) { t->started = 0; DEBUG_ERROR(CAT, "BLE backend creation failed"); return BLE_ERROR; }
    t->timer = uasync_set_timeout(t->ua, 1000, t, tick, "ble_tick");
    if (!t->timer) { DEBUG_ERROR(CAT, "BLE timer allocation failed"); ble_stop(t); return BLE_ERROR; }
    if (t->config.callbacks.status) t->config.callbacks.status(t->config.arg, BLE_STARTING, "registering BLE service");
    return BLE_OK;
}
void ble_stop(struct ble_transport* t) {
    if (!t || !t->started) return;
    t->started = t->ready = 0;
    if (t->timer) { uasync_cancel_timeout(t->ua, t->timer); t->timer = NULL; }
    for (int i = 0; i < BLE_LINKS_MAX; i++) if (t->links[i].id) close_link(t, &t->links[i], "transport stopped");
    ble_backend_destroy(t->backend); t->backend = NULL;
    if (t->config.callbacks.status) t->config.callbacks.status(t->config.arg, BLE_CLOSED, "stopped");
}
void ble_set_incoming(struct ble_transport* t, int allowed) {
    if (!t) { DEBUG_ERROR(CAT, "BLE incoming policy: null transport"); return; }
    t->reject_incoming = !allowed;
    DEBUG_INFO(CAT, "BLE incoming allowed=%d", !!allowed);
    if (!allowed) for (int i = 0; i < BLE_LINKS_MAX; i++)
        if (t->links[i].id && !t->links[i].outgoing) close_link(t, &t->links[i], "incoming disabled");
}
void ble_destroy(struct ble_transport* t) { if (t) { ble_stop(t); u_free(t); } }
const char* ble_backend_name(struct ble_transport* t) { return t->config.name; }
const uint8_t* ble_backend_public_key(struct ble_transport* t) { return t->config.public_key; }
int ble_backend_profile_only(struct ble_transport* t, ble_link_id id) {
    struct ble_connection* c = lookup(t, id); return c && c->profile_only;
}
int ble_set_name(struct ble_transport* t, const char* name) {
    if (!t || ble_name_validate(name)) { DEBUG_ERROR(CAT, "BLE set_name invalid transport/name"); return BLE_ERROR; }
    if (!strcmp(t->config.name, name)) return BLE_OK;
    if (t->backend && ble_backend_set_name(t->backend, name) != BLE_OK) {
        DEBUG_ERROR(CAT, "BLE NODE_NAME update refused"); return BLE_ERROR;
    }
    strcpy(t->config.name, name);
    DEBUG_INFO(CAT, "BLE NODE_NAME updated node=%016llx UTF8_bytes=%zu", (unsigned long long)t->config.node_id, strlen(name));
    return BLE_OK;
}
int ble_backend_profile(struct ble_transport* t, ble_link_id id, const uint8_t* node, size_t node_size,
                        const uint8_t* name, size_t name_size) {
    struct ble_connection* c = lookup(t, id);
    char text[BLE_NAME_MAX + 1];
    if (!c) { DEBUG_WARN(CAT, "BLE profile for retired link=%llu", (unsigned long long)id); return -1; }
    uint64_t peer = node && node_size == BLE_PUBLIC_KEY_SIZE && !key_empty(node) ? t->config.discovery_id(node) : 0;
    if (!c->outgoing || c->opened || !peer || peer != c->peer || !memcmp(node, t->config.public_key, BLE_PUBLIC_KEY_SIZE) ||
        (!key_empty(c->peer_key) && memcmp(node, c->peer_key, BLE_PUBLIC_KEY_SIZE)) ||
        name_size > BLE_NAME_MAX || (!name && name_size) ||
        (name_size && memchr(name, 0, name_size))) {
        DEBUG_ERROR(CAT, "BLE profile mismatch link=%llu expected_locator=%016llx received_locator=%016llx key_bytes=%zu name_bytes=%zu",
                    (unsigned long long)id, (unsigned long long)c->peer, (unsigned long long)peer, node_size, name_size);
        close_link(t, c, "GATT profile identity mismatch"); return -1;
    }
    if (name_size) memcpy(text, name, name_size);
    text[name_size] = 0;
    if (ble_name_validate(text)) { close_link(t, c, "invalid GATT NODE_NAME"); return -1; }
    memcpy(c->peer_key, node, BLE_PUBLIC_KEY_SIZE);
    c->profile_validated = 1;
    DEBUG_INFO(CAT, "BLE GATT profile read link=%llu node=%016llx name=%s UTF8_bytes=%zu",
               (unsigned long long)id, (unsigned long long)peer, text, name_size);
    c->profile_done = c->profile_only;
    int profile_only = c->profile_only;
    if (t->config.callbacks.profile) t->config.callbacks.profile(t->config.arg, &c->endpoint, c->peer_key, text);
    return profile_only ? 1 : 0;
}
int ble_advertise(struct ble_transport* t, const void* data, size_t len) {
    if (!t || !t->ready) { DEBUG_WARN(CAT, "BLE advertise before ready"); return BLE_NOT_READY; }
    if (len > BLE_BROADCAST_MAX || (len && !data)) { DEBUG_ERROR(CAT, "BLE invalid advertisement length=%zu", len); return BLE_ERROR; }
    return ble_backend_advertise(t->backend, data, len);
}
static ble_link_id connect_profile(struct ble_transport* t, const struct ble_endpoint* ep, uint64_t peer, const uint8_t* key) {
    if (!peer) { DEBUG_ERROR(CAT, "BLE connect requires expected peer service identity"); return 0; }
    if (!t || !t->ready) { DEBUG_WARN(CAT, "BLE connect before ready"); return 0; }
    if (!ep || !memchr(ep->id, 0, sizeof(ep->id))) { DEBUG_WARN(CAT, "BLE invalid connect endpoint"); return 0; }
    for (int i = 0; i < BLE_LINKS_MAX; i++) if (t->links[i].id && t->links[i].profile_only && !strcmp(t->links[i].endpoint.id, ep->id))
        close_link(t, &t->links[i], "ordinary connection requested");
    struct ble_connection* c = new_link(t, ep, 1, peer, 0);
    if (!c) return 0;
    if (key) memcpy(c->peer_key, key, BLE_PUBLIC_KEY_SIZE);
    ble_link_id id = c->id;
    if (ble_backend_connect(t->backend, id, ep) != BLE_OK) { close_link(t, c, "connect rejected"); return 0; }
    return id;
}
ble_link_id ble_connect(struct ble_transport* t, const struct ble_endpoint* ep, uint64_t peer) { return connect_profile(t, ep, peer, NULL); }
ble_link_id ble_connect_key(struct ble_transport* t, const struct ble_endpoint* ep, const uint8_t key[BLE_PUBLIC_KEY_SIZE]) {
    if (!t || !key || key_empty(key)) { DEBUG_WARN(CAT, "BLE connect missing full key"); return 0; }
    return connect_profile(t, ep, t->config.discovery_id(key), key);
}
int ble_read_profile(struct ble_transport* t, const struct ble_endpoint* ep, uint64_t peer) {
    if (!t || !t->ready || !peer) { DEBUG_WARN(CAT, "BLE profile query before ready/without node"); return BLE_NOT_READY; }
    if (!ep || !memchr(ep->id, 0, sizeof(ep->id))) { DEBUG_WARN(CAT, "BLE invalid profile endpoint"); return BLE_ERROR; }
    for (int i = 0; i < BLE_LINKS_MAX; i++)
        if (t->links[i].id && (t->links[i].profile_only ||
            (t->links[i].outgoing && !strcmp(t->links[i].endpoint.id, ep->id)))) return BLE_WOULD_BLOCK;
    struct ble_connection* c = new_link(t, ep, 1, peer, 1);
    if (!c) return BLE_ERROR;
    if (ble_backend_connect(t->backend, c->id, ep) != BLE_OK) { close_link(t, c, "profile connect refused"); return BLE_ERROR; }
    DEBUG_DEBUG(CAT, "BLE profile-only query started peer=%016llx endpoint=%s", (unsigned long long)peer, ep->id);
    return BLE_OK;
}
void ble_disconnect(struct ble_transport* t, ble_link_id id) {
    struct ble_connection* c = lookup(t, id);
    if (c) close_link(t, c, "API disconnect");
    else DEBUG_WARN(CAT, "BLE disconnect unknown link=%llu", (unsigned long long)id);
}
int ble_channel_tx_ready(struct ble_transport* t, ble_link_id id, enum ble_channel channel) {
    if ((unsigned)channel >= BLE_CHANNEL_COUNT) { DEBUG_ERROR(CAT, "BLE invalid channel=%d", channel); return 0; }
    struct ble_connection* c = lookup(t, id);
    struct ble_channel_state* ch = c ? &c->channels[channel] : NULL;
    return c && c->ready && (channel != BLE_CHANNEL_DATA ||
        (c->tx_frames_pending < t->config.tx_window &&
         BLE_TX_WINDOW_BYTES - c->flight_bytes >= FLOW_MIN_VALUE && c->flight_count < FLOW_SLOTS)) &&
        (unsigned)queue_entry_count(ch->tx) + !!ch->current < (channel == BLE_CHANNEL_DATA ? t->config.tx_limit : 1);
}
int ble_channel_tx_put(struct ble_transport* t, ble_link_id id, enum ble_channel channel, struct ll_entry* e) {
    if ((unsigned)channel >= BLE_CHANNEL_COUNT) { DEBUG_ERROR(CAT, "BLE invalid channel=%d", channel); return BLE_ERROR; }
    struct ble_connection* c = lookup(t, id);
    struct ble_channel_state* ch = c ? &c->channels[channel] : NULL;
    if (!c || !c->ready) { DEBUG_WARN(CAT, "BLE TX link not ready=%llu", (unsigned long long)id); return BLE_NOT_READY; }
    unsigned max_frame = channel == BLE_CHANNEL_INVITE ? BLE_INVITE_FRAME_MAX : BLE_FRAME_MAX;
    if (!e || e->owner || e->next || e->prev || !e->dgram || !e->len || e->len > max_frame || e->len > e->memlen) {
        DEBUG_ERROR(CAT, "BLE invalid TX frame link=%llu", (unsigned long long)id); return BLE_ERROR;
    }
    if (!ble_channel_tx_ready(t, id, channel)) {
        if (!ch->blocked) DEBUG_TRACE(CAT, "BLE TX paused link=%llu packets=%u", (unsigned long long)id,
                                     (channel == BLE_CHANNEL_DATA ? t->config.tx_limit : 1));
        ch->blocked = 1; return BLE_WOULD_BLOCK;
    }
    /* Без лимита/индекса/consumer put не имеет отказа после проверок и не вызывает чужой callback. */
    if (queue_data_put(ch->tx, e) != 0) { DEBUG_ERROR(CAT, "BLE TX queue invariant failed"); return BLE_ERROR; }
    if (channel == BLE_CHANNEL_DATA) {
        c->tx_frames_pending++;
        if (c->tx_frames_pending > c->stats.max_frames) c->stats.max_frames = c->tx_frames_pending;
        DEBUG_TRACE(CAT, "BLE frame queued link=%llu frames=%u/%u bytes=%u",
                    (unsigned long long)id, c->tx_frames_pending, t->config.tx_window, e->len);
    }
    if (!ble_channel_tx_ready(t, id, channel)) ch->blocked = 1;
    pump(t, c); return BLE_OK;
}
struct ll_entry* ble_channel_rx_get(struct ble_transport* t, ble_link_id id, enum ble_channel channel) {
    if ((unsigned)channel >= BLE_CHANNEL_COUNT) { DEBUG_ERROR(CAT, "BLE invalid channel=%d", channel); return 0; }
    struct ble_connection* c = lookup(t, id);
    struct ble_channel_state* ch = c ? &c->channels[channel] : NULL;
    return c ? queue_data_get(ch->rx) : NULL;
}
int ble_tx_ready(struct ble_transport* t, ble_link_id id) { return ble_channel_tx_ready(t, id, BLE_CHANNEL_DATA); }
int ble_tx_put(struct ble_transport* t, ble_link_id id, struct ll_entry* e) { return ble_channel_tx_put(t, id, BLE_CHANNEL_DATA, e); }
struct ll_entry* ble_rx_get(struct ble_transport* t, ble_link_id id) { return ble_channel_rx_get(t, id, BLE_CHANNEL_DATA); }
ble_link_id ble_backend_incoming(struct ble_transport* t, const struct ble_endpoint* ep) {
    if (t->reject_incoming) { DEBUG_INFO(CAT, "BLE incoming rejected by policy"); return 0; }
    struct ble_connection* c = new_link(t, ep, 0, 0, 0); return c ? c->id : 0;
}
void ble_backend_status(struct ble_transport* t, int ready, const char* reason) {
    if (ready && t->initial_advertisement_pending) {
        t->initial_advertisement_pending = 0;
        /* WinRT уже запустил эти байты до GATT и принимает повтор без перезапуска; остальные backend начинают здесь. */
        if (ble_backend_advertise(t->backend, t->config.initial_advertisement, t->config.initial_advertisement_size) != BLE_OK) {
            DEBUG_ERROR(CAT, "BLE initial advertisement failed bytes=%zu", t->config.initial_advertisement_size);
            ready = 0; reason = "initial BLE advertisement failed";
        }
    }
    t->ready = ready;
    if (!ready) {
        DEBUG_ERROR(CAT, "BLE radio failed: %s", reason);
        for (int i = 0; i < BLE_LINKS_MAX; i++) if (t->links[i].id) close_link(t, &t->links[i], reason);
    } else DEBUG_INFO(CAT, "BLE radio ready node=%016llx", (unsigned long long)t->config.node_id);
    if (t->config.callbacks.status) t->config.callbacks.status(t->config.arg, ready ? BLE_READY : BLE_CLOSED, reason);
}
void ble_backend_broadcast(struct ble_transport* t, const struct ble_endpoint* ep, const uint8_t* data, size_t len, int rssi) {
    if (len > BLE_BROADCAST_MAX) { DEBUG_WARN(CAT, "BLE oversized broadcast bytes=%zu", len); return; }
    if (t->config.callbacks.broadcast) t->config.callbacks.broadcast(t->config.arg, ep, data, len, rssi);
}
void ble_backend_advertising(struct ble_transport* t, enum ble_state state, const char* reason) {
    DEBUG_INFO(CAT, "BLE advertising state=%d reason=%s", state, reason);
    if (t->config.callbacks.advertising) t->config.callbacks.advertising(t->config.arg, state, reason);
}
void ble_backend_advertising_power(struct ble_transport* t, struct ble_advertising_power power) {
    t->advertising_power = power;
    DEBUG_INFO(CAT, "BLE advertising power capability max_dbm=%d (127=unknown) can_set=%d (-1=unknown)", power.max_dbm, power.can_set);
}
struct ble_advertising_power ble_get_advertising_power(const struct ble_transport* t) {
    return t && t->ready ? t->advertising_power : (struct ble_advertising_power){127, -1};
}
void ble_backend_opened(struct ble_transport* t, ble_link_id id, unsigned int mtu) {
    struct ble_connection* c = lookup(t, id);
    if (!c) return;
    if (c->profile_only) { DEBUG_ERROR(CAT, "BLE profile-only backend attempted data session"); close_link(t, c, "invalid profile session"); return; }
    if (c->opened || (c->outgoing && !c->profile_validated)) {
        DEBUG_WARN(CAT, "BLE opened without a new profile link=%llu outgoing=%d", (unsigned long long)id, c->outgoing);
        close_link(t, c, "missing profile or duplicate open"); return;
    }
    if (mtu < 23) { DEBUG_ERROR(CAT, "BLE invalid MTU=%u", mtu); close_link(t, c, "invalid MTU"); return; }
    c->capacity = mtu - 3; if (c->capacity > BLE_VALUE_MAX) c->capacity = BLE_VALUE_MAX;
    c->opened = 1;
    DEBUG_INFO(CAT, "BLE GATT open link=%llu mtu=%u fragment_payload=%u outgoing=%d DATA=without_response/notify INVITE=request/indicate tx_limit=%u",
               (unsigned long long)id, mtu, c->capacity - HEADER - FLOW_HEADER, c->outgoing, t->config.tx_limit);
    DEBUG_INFO(CAT, "BLE flow configured link=%llu window_frames=%u window_bytes=%u "
               "ACK=without_response/notify ACK_interval_us=%u ACK_repeat_us=%u id_bits=16",
               (unsigned long long)id, t->config.tx_window, BLE_TX_WINDOW_BYTES, BLE_ACK_INTERVAL_US, BLE_ACK_REPEAT_US);
    if (c->outgoing) {
        c->control_type = HELLO;
        memcpy(c->control_tx, t->config.public_key, BLE_PUBLIC_KEY_SIZE);
        memcpy(c->control_tx + BLE_PUBLIC_KEY_SIZE, c->peer_key, BLE_PUBLIC_KEY_SIZE);
    }
    pump(t, c);
}

static void receive_control(struct ble_transport* t, struct ble_connection* c, const uint8_t* data, size_t len) {
    if (c->ready || data[0] != (c->outgoing ? ACCEPT : HELLO) || len <= CONTROL_HEADER ||
        data[1] != c->control_rx_offset || len - CONTROL_HEADER > CONTROL_SIZE - c->control_rx_offset) {
        DEBUG_WARN(CAT, "BLE invalid control fragment link=%llu type=%u bytes=%zu expected_offset=%u ready=%d outgoing=%d",
                   (unsigned long long)c->id, data[0], len, c->control_rx_offset, c->ready, c->outgoing);
        close_link(t, c, "invalid control fragment"); return;
    }
    size_t count = len - CONTROL_HEADER;
    memcpy(c->control_rx + c->control_rx_offset, data + CONTROL_HEADER, count);
    c->control_rx_offset += count;
    if (c->control_rx_offset != CONTROL_SIZE) return;
    const uint8_t* source = c->control_rx;
    const uint8_t* target = source + BLE_PUBLIC_KEY_SIZE;
    uint64_t peer = key_empty(source) ? 0 : t->config.discovery_id(source);
    if (!peer || !memcmp(source, t->config.public_key, BLE_PUBLIC_KEY_SIZE) ||
        memcmp(target, t->config.public_key, BLE_PUBLIC_KEY_SIZE) ||
        (c->outgoing && memcmp(source, c->peer_key, BLE_PUBLIC_KEY_SIZE))) {
        DEBUG_WARN(CAT, "BLE control key mismatch link=%llu type=%u selector=%016llx",
                   (unsigned long long)c->id, data[0], (unsigned long long)peer);
        close_link(t, c, "control key mismatch"); return;
    }
    if (t->config.callbacks.validate && !t->config.callbacks.validate(t->config.arg, c->id, peer)) {
        DEBUG_WARN(CAT, "BLE validation rejected link=%llu", (unsigned long long)c->id);
        close_link(t, c, "validation rejected"); return;
    }
    c->peer = peer; c->ready = 1;
    memcpy(c->peer_key, source, BLE_PUBLIC_KEY_SIZE);
    if (!c->outgoing) {
        c->control_type = ACCEPT;
        memcpy(c->control_tx, t->config.public_key, BLE_PUBLIC_KEY_SIZE);
        memcpy(c->control_tx + BLE_PUBLIC_KEY_SIZE, source, BLE_PUBLIC_KEY_SIZE);
    }
    DEBUG_INFO(CAT, "BLE session ready link=%llu selector=%016llx full address checked",
               (unsigned long long)c->id, (unsigned long long)peer);
    if (t->config.callbacks.connection) t->config.callbacks.connection(t->config.arg, c->id, peer, BLE_READY, "full addresses exchanged");
    pump(t, c);
}
void ble_backend_received(struct ble_transport* t, ble_link_id id, enum ble_wire_channel wire, const uint8_t* data, size_t len) {
    struct ble_connection* c = lookup(t, id);
    if (!c) return;
    if (!c->opened) {
        DEBUG_WARN(CAT, "BLE value before mandatory subscriptions link=%llu channel=%d bytes=%zu", (unsigned long long)id, wire, len);
        close_link(t, c, "value before subscriptions"); return;
    }
    if (!data || !len || len > BLE_VALUE_MAX || (unsigned)wire >= BLE_WIRE_COUNT) {
        DEBUG_WARN(CAT, "BLE invalid wire value link=%llu channel=%d bytes=%zu", (unsigned long long)id, wire, len);
        close_link(t, c, "invalid wire value"); return;
    }
    if (len > c->capacity && !c->rx_above_tx_logged) {
        c->rx_above_tx_logged = 1;
        DEBUG_INFO(CAT, "BLE RX exceeds conservative TX budget link=%llu bytes=%zu TX_capacity=%u RX_limit=%u",
                   (unsigned long long)id, len, c->capacity, BLE_VALUE_MAX);
    }
    if (wire == BLE_WIRE_ACK) {
        if (len != 2) {
            DEBUG_WARN(CAT, "BLE invalid ACK length link=%llu bytes=%zu", (unsigned long long)id, len);
            close_link(t, c, "invalid ACK length"); return;
        }
        uint16_t ack = read16(data), advance = (uint16_t)(ack - c->tx_acked);
        if (!advance || advance >= 0x8000) {
            DEBUG_TRACE(CAT, "BLE ACK RX link=%llu id=%u acked=%u flight=%u/%u ignored=stale",
                        (unsigned long long)id, ack, c->tx_acked, c->flight_count, c->flight_bytes); return;
        }
        if (advance > c->flight_count) {
            DEBUG_WARN(CAT, "BLE ACK outside flight link=%llu id=%u acked=%u next=%u count=%u",
                       (unsigned long long)id, ack, c->tx_acked, c->tx_next, c->flight_count);
            close_link(t, c, "ACK outside flight"); return;
        }
        unsigned bytes = 0, frames = 0;
        for (unsigned i = 0; i < advance; i++) {
            bytes += c->flight[c->flight_head].size; frames += c->flight[c->flight_head].frame_end;
            c->flight_head = (c->flight_head + 1) % FLOW_SLOTS;
        }
        c->tx_frames_pending -= frames;
        c->flight_count -= advance; c->flight_bytes -= bytes; c->tx_acked = ack; c->stats.ack_rx++;
        c->flight_deadline = c->flight_count ? get_time_us() + DEADLINE_US : 0;
        DEBUG_TRACE(CAT, "BLE ACK RX link=%llu id=%u released=%u/%u flight=%u/%u frames_released=%u frames=%u/%u",
                    (unsigned long long)id, ack, advance, bytes, c->flight_count, c->flight_bytes,
                    frames, c->tx_frames_pending, t->config.tx_window);
        wake_writer(t, c, BLE_CHANNEL_DATA); pump(t, c); return;
    }
    enum ble_channel channel = wire == BLE_WIRE_DATA ? BLE_CHANNEL_DATA : BLE_CHANNEL_INVITE;
    if (wire == BLE_WIRE_DATA) {
        if (len <= FLOW_HEADER) {
            DEBUG_WARN(CAT, "BLE missing DATA flow header link=%llu bytes=%zu", (unsigned long long)id, len);
            close_link(t, c, "invalid DATA flow header"); return;
        }
        uint16_t seq = read16(data), advance = (uint16_t)(seq - c->rx_last);
        DEBUG_TRACE(CAT, "BLE DATA RX link=%llu seq=%u type=%u bytes=%zu previous=%u advance=%u",
                    (unsigned long long)id, seq, data[FLOW_HEADER], len, c->rx_last, advance);
        if (!advance || advance >= 0x8000) {
            DEBUG_WARN(CAT, "BLE duplicate/stale DATA link=%llu id=%u last=%u", (unsigned long long)id, seq, c->rx_last);
            if (c->rx_flow_seen) schedule_ack(t, c, 1); return;
        }
        if (advance != 1) DEBUG_WARN(CAT, "BLE DATA flow gap link=%llu expected=%u received=%u missing=%u",
                                    (unsigned long long)id, (uint16_t)(c->rx_last + 1), seq, advance - 1);
        c->rx_last = seq; c->rx_flow_seen = 1;
        if (!schedule_ack(t, c, 1)) return;
        data += FLOW_HEADER; len -= FLOW_HEADER;
    }
    struct ble_channel_state* ch = &c->channels[channel];
    if (channel == BLE_CHANNEL_DATA && (data[0] == HELLO || data[0] == ACCEPT)) {
        receive_control(t, c, data, len);
        return;
    }
    if (!c->ready || len <= HEADER || data[0] != FRAME) {
        DEBUG_WARN(CAT, "BLE invalid value link=%llu channel=%d bytes=%zu ready=%d type=%u",
                   (unsigned long long)id, channel, len, c->ready, len ? data[0] : 0);
        c->stats.dropped++;
        ch->rx_size = ch->rx_offset = 0; return;
    }
    uint16_t seq = read16(data + 1), total = read16(data + 3), offset = read16(data + 5);
    size_t n = len - HEADER;
    if (offset && ch->dropping && ch->dropped_sequence == seq) {
        DEBUG_TRACE(CAT, "BLE discarded frame tail link=%llu seq=%u offset=%u", (unsigned long long)id, seq, offset); return;
    }
    if (!offset) ch->dropping = 0;
    unsigned max_frame = channel == BLE_CHANNEL_INVITE ? BLE_INVITE_FRAME_MAX : BLE_FRAME_MAX;
    if (!total || total > max_frame || offset > total || n > (size_t)(total - offset)) goto invalid;
    if (!offset) {
        uint16_t expected = ch->rx_seen ? ch->rx_next : 1;
        if (seq != expected) {
            uint16_t gap = (uint16_t)(seq - expected);
            DEBUG_WARN(CAT, "BLE RX sequence gap link=%llu channel=%d expected=%u received=%u missing=%u",
                       (unsigned long long)id, channel, expected, seq, gap < 0x8000 ? gap : 0);
            if (gap < 0x8000) c->stats.missing += gap;
        }
        ch->rx_seen = 1; ch->rx_next = (uint16_t)(seq + 1);
        if (ch->rx_size) {
            DEBUG_WARN(CAT, "BLE incomplete frame replaced link=%llu channel=%d seq=%u received=%u total=%u next=%u",
                       (unsigned long long)id, channel, ch->rx_sequence, ch->rx_offset, ch->rx_size, seq);
            c->stats.dropped++;
        }
        ch->rx_sequence = seq; ch->rx_size = total; ch->rx_offset = 0; ch->rx_deadline = get_time_us() + DEADLINE_US;
    }
    if (ch->rx_size != total || ch->rx_sequence != seq || ch->rx_offset != offset) goto invalid;
    memcpy(ch->assembly + offset, data + HEADER, n); ch->rx_offset += n;
    if (ch->rx_offset != total) return;
    ch->rx_size = ch->rx_offset = 0;
    if (queue_entry_count(ch->rx) >= BLE_QUEUE_MAX) {
        DEBUG_WARN(CAT, "BLE RX full: frame dropped link=%llu channel=%d", (unsigned long long)id, channel);
        c->stats.dropped++; return;
    }
    struct ll_entry* entry = ll_alloc_lldgram(total);
    if (!entry) { DEBUG_ERROR(CAT, "BLE RX allocation failed bytes=%u", total); c->stats.dropped++; return; }
    memcpy(entry->dgram, ch->assembly, total); entry->len = total;
    if (queue_data_put(ch->rx, entry) != 0) { DEBUG_ERROR(CAT, "BLE RX queue invariant failed"); return; }
    c->stats.rx_bytes += total; c->stats.rx_frames++;
    DEBUG_TRACE(CAT, "BLE wire RX link=%llu channel=%d seq=%u bytes=%u tag=%08x",
                (unsigned long long)id, channel, seq, total, frame_tag(entry->dgram, entry->len));
    void (*received)(void*, ble_link_id) = channel == BLE_CHANNEL_DATA ? t->config.callbacks.received : t->config.callbacks.invite_received;
    if (received) received(t->config.arg, id);
    return;
invalid:
    c->stats.dropped++;
    DEBUG_WARN(CAT, "BLE fragment dropped link=%llu channel=%d seq=%u offset=%u expected=%u total=%u bytes=%zu",
               (unsigned long long)id, channel, seq, offset, ch->rx_offset, total, n);
    ch->rx_size = ch->rx_offset = 0;
    ch->dropping = 1; ch->dropped_sequence = seq;
}
void ble_backend_sent(struct ble_transport* t, ble_link_id id, int success) {
    struct ble_connection* c = lookup(t, id);
    if (!c) return;
    if (!success || !c->pending) { DEBUG_WARN(CAT, "BLE send completion failed link=%llu success=%d pending=%d channel=%d bytes=%u",
        (unsigned long long)id, success, c->pending, c->pending_channel, c->pending_bytes);
        close_link(t, c, "send failed"); return; }
    enum ble_channel channel = c->pending_channel;
    uint64_t waited = get_time_us() - c->submitted;
    c->stats.fragments++; c->stats.wait_us += waited;
    if (waited > c->stats.max_wait_us) c->stats.max_wait_us = waited;
    c->pending = 0;
    if (c->pending_ack) {
        c->pending_ack = 0; c->ack_completed = get_time_us();
        DEBUG_TRACE(CAT, "BLE ACK sent link=%llu id=%u elapsed_us=%llu latest=%u dirty=%d",
                    (unsigned long long)id, c->ack_sent, (unsigned long long)waited, c->rx_last, c->ack_dirty);
        if (!schedule_ack(t, c, 0)) return;
        pump(t, c); return;
    }
    struct ble_channel_state* ch = &c->channels[channel];
    if (c->control_type) {
        c->control_tx_offset += c->pending_bytes;
        if (c->control_tx_offset == CONTROL_SIZE) c->control_type = 0;
    }
    else if (ch->current) {
        ch->offset += c->pending_bytes;
        if (ch->offset == ch->current->len) {
            c->stats.tx_bytes += ch->current->len; c->stats.tx_frames++;
            DEBUG_TRACE(CAT, "BLE wire TX link=%llu channel=%d seq=%u bytes=%u tag=%08x",
                        (unsigned long long)id, channel, ch->sequence, ch->current->len,
                        frame_tag(ch->current->dgram, ch->current->len));
            release_entry(ch->current); ch->current = NULL;
        }
    }
    wake_writer(t, c, channel);
    pump(t, c);
}
void ble_backend_closed(struct ble_transport* t, ble_link_id id, const char* reason) {
    struct ble_connection* c = lookup(t, id); if (c) close_link(t, c, reason);
}
