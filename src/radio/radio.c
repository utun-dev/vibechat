// radio.c — рация (PTT walkie-talkie): мультикаст голоса по CHAT-группе.
//
// Флудит Opus-кадры (ETCP_ID_RADIO) всем подписчикам канала — узлам с
// TOPO_FLAG_RADIO в NODEINFO. Пакет несёт hop_list (узлы, через которые он уже
// прошёл) для защиты от циклов и форвардится только в те ветви, за которыми есть
// хотя бы один подписчик (RPF-обрезание по BGP-путям). Кадры нумеруются от 0,
// последний кадр передачи помечается FIN.
//
// Приём: если мы подписаны (group->radio_active) — доставляем кадр в frame_fn
// (аудио-движок radio_audio). Независимо от подписки форвардим дальше.
// Все функции — в uasync-потоке.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/u_async.h"
#include "../lib/db_async.h"
#include "../lib/ll_queue.h"
#include "../transport_layer/etcp_api.h"
#include "../transport_layer/etcp.h"
#include "../routing_layer/topo_node.h"
#include "../routing_layer/topo_group.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../chat/chat_event.h"
#include "../chat/group_policy.h"
#include "../chat/chat_setting.h"
#include "../ntp_time.h"
#include "../stream/stream.h"
#include "radio.h"
#include "radio_proto.h"

#define RADIO_ID "radio"

#define RADIO_MAX_TALKERS 16  /* максимум одновременно говорящих источников */

#define RADIO_DEDUP_MAX    128    /* ёмкость кэша дедупликации (≈2.5с @ 50 кадров/с) */
#define RADIO_DEDUP_TTL_TB 20000  /* 2с — окно дедупликации (совпадает с таймаутом источника) */
#define RADIO_TALKER_TIMEOUT_TB 20000  /* 2с без кадров — считаем передачу завершённой (FIN потерян/пир отвалился) */

/* Короткий кэш транзита, не удостоверяющий автора; tb — момент прихода. */
struct radio_seen {
    uint8_t src[RADIO_KEY_SIZE];
    uint64_t timestamp;
    uint64_t tb;
};

/* Последняя проверенная пачка автора; сохраняется между PTT/FIN и выключением прослушивания. */
struct radio_author_time {
    struct radio_author_time* next;
    uint8_t src[RADIO_KEY_SIZE];
    uint64_t timestamp;
};

struct radio_mute_sql {
    char channel[65];
    uint8_t ed[32]; /* Ed25519 участника для WRITE. */
    int write; /* 0 — READ списка, 1 — WRITE с последующим снимком списка. */
    int muted; /* 0/1 либо 2=toggle; после work результат 0/1. */
    uint8_t (*members)[32];
    int count;
};
struct radio_mute_task {
    struct radio_mute_sql sql; /* Worker получает только этот самостоятельный SQL-образ. */
    struct radio_mute_task* next;
    struct radio_ctx* ctx; /* Слабая ссылка: destroy отсоединяет все запросы. */
    radio_mute_done_fn done;
    void* arg;
};

struct radio_ctx {
    struct TOPO_GROUP* group;
    uint16_t stream_id;          /* id текущей передачи (talk-burst) */
    uint16_t seq;                /* seq следующего исходящего кадра */
    uint64_t tx_timestamp;       /* последний подписанный timestamp, не сбрасывается при новом PTT */
    struct radio_author_time* author_times; /* проверенные timestamps авторов, до уничтожения группы */
    int      talking;            /* 1 = идёт передача (PTT зажата) */
    uint8_t  burst_target, burst_count; /* Настройка текущей передачи и число накопленных кадров. */
    uint8_t  burst_body[RADIO_MAX_BODY]; /* Записи length:u16 + Opus, принадлежат uasync. */
    size_t   burst_bytes;         /* Занятый префикс burst_body. */
    void*    burst_timer;         /* Deadline неполной пачки; отменяется до освобождения ctx. */
    uint32_t c_tx_frames;        /* отправлено кадров */
    uint32_t c_rx_frames;        /* доставлено локально (мы подписаны) */
    uint32_t c_fwd_frames;       /* отправлено пачек соседям (DATA/FIN) */
    uint32_t c_loop_dropped;     /* дропнуто из-за цикла (self в hop_list) */
    uint32_t c_nosub_dropped;    /* не отправлено: за соседом нет подписчиков */
    uint32_t c_bad_dropped;      /* дропнуто некорректных пакетов */
    uint32_t c_auth_dropped;     /* подписчик отклонил автора/подпись до изменения состояния */
    uint32_t c_dup_dropped;      /* повторы/опоздавшие пачки автора либо дубли транзита */
    struct radio_talker { uint8_t src[RADIO_KEY_SIZE]; uint16_t stream; uint64_t last_tb; } talkers[RADIO_MAX_TALKERS];  /* активные говорящие */
    void*    talker_timer;        /* периодический sweep зависших говорящих (self-rearming) */
    struct radio_seen seen[RADIO_DEDUP_MAX];  /* кэш дедупликации */
    int      seen_count;
    uint8_t (*muted_members)[32];    /* локальный кэш, принадлежит uasync-потоку */
    int muted_count;
    int muted_loaded;
    int listen_requested;
    struct radio_mute_task* mutes;
};

struct radio_instance {
    radio_frame_fn frame_fn;
    void*          frame_arg;
    radio_mute_fn  mute_fn;
    void*          mute_arg;
};

static void radio_talker_timer_cb(void* arg);

static struct radio_ctx* radio_of(struct TOPO_GROUP* group) {
    return group ? group->radio : NULL;
}

static struct radio_instance* radio_inst(struct UTUN_INSTANCE* inst) {
    return inst ? inst->radio : NULL;
}

int radio_init(struct TOPO_GROUP* group) {
    if (!group) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: init null group", RADIO_ID); return -1; }
    if (group->radio) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: already initialized grp=%016llx", RADIO_ID, (unsigned long long)group->group_id); return -1; }

    struct radio_ctx* ctx = u_calloc(1, sizeof(struct radio_ctx));
    if (!ctx) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: alloc failed", RADIO_ID); return -1; }
    ctx->group = group;
    ctx->stream_id = (uint16_t)(get_time_tb() & 0xFFFF);
    ctx->talker_timer = uasync_set_timeout(group->instance->ua, RADIO_TALKER_TIMEOUT_TB, ctx, radio_talker_timer_cb, "radio_talker");
    if (!ctx->talker_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: init talker_timer failed grp=%016llx", RADIO_ID, (unsigned long long)group->group_id);
        u_free(ctx); return -1;
    }
    group->radio = ctx;

    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "%s: init ok grp=%016llx", RADIO_ID, (unsigned long long)group->group_id);
    return 0;
}

void radio_destroy(struct TOPO_GROUP* group) {
    if (!group || !group->radio) return;
    struct radio_ctx* ctx = group->radio;
    while (ctx->mutes) {
        struct radio_mute_task* task = ctx->mutes; ctx->mutes = task->next; task->ctx = NULL;
    }
    if (ctx->burst_timer) uasync_cancel_timeout(group->instance->ua, ctx->burst_timer);
    if (ctx->burst_count) DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio: destroy discards pending batch group=%llx frames=%u",
                                   (unsigned long long)group->group_id, ctx->burst_count);
    if (ctx->talker_timer) { uasync_cancel_timeout(group->instance->ua, ctx->talker_timer); ctx->talker_timer = NULL; }
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "%s: destroy grp=%016llx (tx=%u rx=%u fwd=%u loop=%u nosub=%u bad=%u auth=%u dup=%u)",
               RADIO_ID, (unsigned long long)group->group_id,
               ctx->c_tx_frames, ctx->c_rx_frames, ctx->c_fwd_frames,
               ctx->c_loop_dropped, ctx->c_nosub_dropped, ctx->c_bad_dropped, ctx->c_auth_dropped, ctx->c_dup_dropped);
    u_free(ctx->muted_members);
    while (ctx->author_times) {
        struct radio_author_time* next = ctx->author_times->next;
        u_free(ctx->author_times); ctx->author_times = next;
    }
    u_free(ctx);
    group->radio = NULL;
}

/* ── сборка payload ── */

/* Пишет payload рации (начиная с полного адреса группы) в buf, возвращает длину. */
static size_t radio_build(uint8_t* buf, const uint8_t group_key[32], const uint8_t src[RADIO_KEY_SIZE], uint16_t stream,
                          uint16_t seq, uint64_t timestamp, uint8_t flags, const uint8_t* hops, uint8_t hop_count,
                          uint8_t count, const uint8_t* body, size_t body_len) {
    size_t off = 0;
    memcpy(buf + off, group_key, 32); off += 32;
    memcpy(buf + off, src, RADIO_KEY_SIZE); off += RADIO_KEY_SIZE;
    memcpy(buf + off, &stream, 2); off += 2;
    memcpy(buf + off, &seq, 2); off += 2;
    memcpy(buf + off, &timestamp, 8); off += 8;
    buf[off++] = flags;
    buf[off++] = hop_count;
    buf[off++] = count;
    if (hop_count) { memcpy(buf + off, hops, (size_t)hop_count * RADIO_KEY_SIZE); off += (size_t)hop_count * RADIO_KEY_SIZE; }
    if (body_len) { memcpy(buf + off, body, body_len); off += body_len; }
    return off;
}

/* ── отправка ── */

/* Каноническое тело подписи: ID протокола, неизменяемый заголовок и вся пачка, без пути. */
static size_t radio_signed_data(uint8_t out[RADIO_HDR_SIZE + RADIO_MAX_BODY], const uint8_t* payload,
        const uint8_t* body, size_t body_len) {
    out[0] = ETCP_ID_RADIO; memcpy(out + 1, payload, RADIO_OFF_HOPS);
    out[1 + RADIO_OFF_HOPS] = payload[RADIO_OFF_COUNT];
    if (body_len) memcpy(out + RADIO_HDR_SIZE, body, body_len);
    return RADIO_HDR_SIZE + body_len;
}

/* Caller оставляет 64 байта после len; подписываем один раз перед рассылкой всем соседям. */
static int radio_sign(struct radio_ctx* ctx, uint8_t* payload, size_t len) {
    struct UTUN_INSTANCE* inst = ctx->group->instance;
    if (ctx->tx_timestamp == UINT64_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio: outgoing timestamp exhausted group=%llx", (unsigned long long)ctx->group->group_id);
        return -1;
    }
    int64_t now = ntp_time_get_us(inst);
    uint64_t timestamp = now > 0 ? (uint64_t)now : 0;
    if (timestamp <= ctx->tx_timestamp) timestamp = ctx->tx_timestamp + 1;
    memcpy(payload + RADIO_OFF_TIMESTAMP, &timestamp, 8);
    uint8_t signed_data[RADIO_HDR_SIZE + RADIO_MAX_BODY];
    size_t body_off = RADIO_HDR_SIZE + (size_t)payload[RADIO_OFF_HOPS] * RADIO_KEY_SIZE;
    size_t size = radio_signed_data(signed_data, payload, payload + body_off, len - body_off);
    int rc = sc_ed25519_sign(inst->my_ed25519_privkey, signed_data, size, payload + len);
    if (rc != SC_OK) DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio: author signing failed node=%llx bytes=%zu rc=%d",
        (unsigned long long)inst->node_id, len, rc);
    else {
        ctx->tx_timestamp = timestamp;
        DEBUG_TRACE(DEBUG_CATEGORY_RADIO, "radio: signed group=%llx timestamp=%llu clock_us=%lld bytes=%zu",
            (unsigned long long)ctx->group->group_id, (unsigned long long)timestamp, (long long)now, len);
    }
    return rc == SC_OK ? 0 : -1;
}

/* Вызывается только локальным подписчиком; транзит сохраняет исходное удостоверение пакета. */
static int radio_verify_author(struct TOPO_GROUP* group, const uint8_t src[RADIO_KEY_SIZE], uint8_t x[32], const uint8_t* payload,
        const uint8_t* body, size_t body_len, const uint8_t signature[RADIO_SIGNATURE_SIZE]) {
    uint8_t signed_data[RADIO_HDR_SIZE + RADIO_MAX_BODY];
    int rc = sc_identity_public_x("radio", src, x) || !group_policy_member_key_allowed(group->instance, group->channel_id, x, src, 0);
    if (!rc) {
        size_t size = radio_signed_data(signed_data, payload, body, body_len);
        rc = sc_ed25519_verify(src, signed_data, size, signature);
    }
    if (rc) {
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio: author rejected group=%llx src=%s bytes=%zu rc=%d",
            (unsigned long long)group->group_id, sc_public_key_hex(src, (char[65]){0}), body_len, rc); return -1;
    }
    DEBUG_TRACE(DEBUG_CATEGORY_RADIO, "radio: author verified group=%llx src=%s bytes=%zu",
        (unsigned long long)group->group_id, sc_public_key_hex(src, (char[65]){0}), body_len);
    return 0;
}

/* статистика отправки: раз в секунду — сколько кадров реально ушло в etcp_send и сколько упало. */
static uint64_t s_send_stats_tb = 0;
static uint32_t s_send_ok = 0;
static uint32_t s_send_fail = 0;

static void radio_send_to(struct TOPO_GROUP* group, struct ETCP_CONN* conn,
                          const uint8_t* payload, size_t payload_len) {
    struct ll_entry* ee = queue_entry_new(0);
    if (!ee) {
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: send: queue_entry_new OOM", RADIO_ID);
        return;
    }
    uint8_t* pkt = u_malloc(1 + payload_len);
    if (!pkt) {
        queue_entry_free(ee);
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: send: u_malloc OOM len=%zu", RADIO_ID, payload_len);
        return;
    }
    pkt[0] = ETCP_ID_RADIO;
    memcpy(pkt + 1, payload, payload_len);
    ee->dgram = pkt;
    ee->len = (size_t)(1 + payload_len);
    if (etcp_send(conn, ee) != 0) {
        u_free(pkt); queue_entry_free(ee);
        s_send_fail++;
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: send: etcp_send FAILED conn=%s len=%zu",
                   RADIO_ID, conn ? conn->log_name : "?", (size_t)(1 + payload_len));
    } else {
        s_send_ok++;
    }

    uint64_t now = get_time_tb();
    if (s_send_stats_tb == 0 || now - s_send_stats_tb >= 10000) {   /* 1с (tb 0.1мс) */
        DEBUG_INFO(DEBUG_CATEGORY_RADIO, "%s: send stats ok=%u/s fail=%u", RADIO_ID, s_send_ok, s_send_fail);
        s_send_stats_tb = now;
        s_send_ok = 0;
        s_send_fail = 0;
    }
}

/* Есть ли за conn (не считая уже пройденных hop_list) хотя бы один подписчик рации. */
static int radio_neighbor_has_subscriber(struct TOPO_GROUP* group, struct ETCP_CONN* conn,
                                         const uint8_t* hop_list, uint8_t hop_count) {
    if (!group || !conn || !group->nodes) return 0;
    const uint8_t* self = group->instance->my_ed25519_pubkey;
    struct ll_entry* ne = group->nodes->head;
    while (ne) {
        struct TOPO_GROUP_NODE* nq = (struct TOPO_GROUP_NODE*)ne;
        if (nq->radio && memcmp(nq->ed25519, self, RADIO_KEY_SIZE)) {
            int visited = 0;
            for (int i = 0; i < hop_count; i++) if (!memcmp(hop_list + i * RADIO_KEY_SIZE, nq->ed25519, RADIO_KEY_SIZE)) { visited = 1; break; }
            if (!visited) {
                struct ll_entry* pe = nq->paths ? nq->paths->head : NULL;
                while (pe) {
                    struct TOPO_NODEPATH* p = (struct TOPO_NODEPATH*)pe;
                    if (p->conn == conn) return 1;
                    pe = pe->next;
                }
            }
        }
        ne = ne->next;
    }
    return 0;
}

/* Флудит payload (уже содержит group_key..hop_list..opus) всем соседям, кроме from_conn. */
static void radio_flood(struct TOPO_GROUP* group, struct ETCP_CONN* from_conn,
                        const uint8_t* payload, size_t payload_len,
                        const uint8_t* hop_list, uint8_t hop_count) {
    struct radio_ctx* ctx = radio_of(group);
    if (!ctx || !group->senders_list) return;
    struct ll_entry* se = group->senders_list->head;
    while (se) {
        struct TOPO_GROUP_CONN_ITEM* item = (struct TOPO_GROUP_CONN_ITEM*)se->data;
        if (item && item->conn && item->conn != from_conn && topo_group_data_allowed(group, item->conn)) {
            if (radio_neighbor_has_subscriber(group, item->conn, hop_list, hop_count)) {
                radio_send_to(group, item->conn, payload, payload_len);
                ctx->c_fwd_frames++;
            } else {
                ctx->c_nosub_dropped++;
            }
        }
        se = se->next;
    }
}

/* ── индикация говорящих ── */

static void radio_talk_event(struct TOPO_GROUP* group, const uint8_t author[RADIO_KEY_SIZE], uint16_t stream, uint8_t on) {
    const uint8_t* key = topo_group_wire_id(group);
    if (!key) { DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio talk event group key unavailable"); return; }
    uint8_t buf[32 + RADIO_KEY_SIZE + 2 + 1];
    size_t off = 0;
    memcpy(buf + off, key, 32); off += 32;
    memcpy(buf + off, author, RADIO_KEY_SIZE); off += RADIO_KEY_SIZE;
    memcpy(buf + off, &stream, 2); off += 2;
    buf[off++] = on;
    chat_event_post(group->instance, CHAT_EVT_RADIO_TALK, buf, (int)off);
    DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "%s: talk %s src=%s stream=%u",
                RADIO_ID, on ? "START" : "STOP", sc_public_key_hex(author, (char[65]){0}), stream);
}

/* зафиксировать начало передачи: пост on=1, если это новый поток источника */
static void radio_talker_on(struct radio_ctx* ctx, const uint8_t src[RADIO_KEY_SIZE], uint16_t stream) {
    uint64_t now = get_time_tb();
    for (int i = 0; i < RADIO_MAX_TALKERS; i++) {
        if (!memcmp(ctx->talkers[i].src, src, RADIO_KEY_SIZE)) {
            ctx->talkers[i].last_tb = now;
            if (ctx->talkers[i].stream != stream) {
                ctx->talkers[i].stream = stream;
                radio_talk_event(ctx->group, src, stream, 1);
            }
            return;
        }
    }
    for (int i = 0; i < RADIO_MAX_TALKERS; i++) {
        if (!ctx->talkers[i].last_tb) {
            memcpy(ctx->talkers[i].src, src, RADIO_KEY_SIZE);
            ctx->talkers[i].stream = stream;
            ctx->talkers[i].last_tb = now;
            radio_talk_event(ctx->group, src, stream, 1);
            return;
        }
    }
}

/* зафиксировать конец передачи: пост on=0, если источник был в таблице */
static void radio_talker_off(struct radio_ctx* ctx, const uint8_t src[RADIO_KEY_SIZE]) {
    for (int i = 0; i < RADIO_MAX_TALKERS; i++) {
        if (!memcmp(ctx->talkers[i].src, src, RADIO_KEY_SIZE)) {
            radio_talk_event(ctx->group, src, ctx->talkers[i].stream, 0);
            memset(&ctx->talkers[i], 0, sizeof(ctx->talkers[i]));
            return;
        }
    }
}

void radio_revalidate_members(struct TOPO_GROUP* group) {
    struct radio_ctx* ctx = radio_of(group);
    if (!ctx || group->group_type != TOPO_GROUP_TYPE_CHAT) return;
    struct UTUN_INSTANCE* inst = group->instance;
    int self_allowed = group_policy_member_key_allowed(inst, group->channel_id, inst->my_keys.public_key, inst->my_ed25519_pubkey, 0);
    if (!self_allowed) {
        if (ctx->talking || ctx->burst_count)
            DEBUG_INFO(DEBUG_CATEGORY_RADIO, "radio: revoke local transmission group=%llx stream=%u buffered=%u",
                       (unsigned long long)group->group_id, ctx->stream_id, ctx->burst_count);
        if (ctx->burst_timer) uasync_cancel_timeout(group->instance->ua, ctx->burst_timer);
        ctx->burst_timer = NULL; ctx->burst_count = 0; ctx->burst_bytes = 0; ctx->talking = 0;
    }
    for (unsigned i = 0; i < RADIO_MAX_TALKERS; i++) {
        if (!ctx->talkers[i].last_tb) continue;
        const uint8_t* source = ctx->talkers[i].src;
        uint8_t x[32];
        if (!self_allowed || sc_identity_public_x("radio talker", source, x) || !group_policy_member_key_allowed(inst, group->channel_id, x, source, 0)) {
            DEBUG_INFO(DEBUG_CATEGORY_RADIO, "radio: revoke talker group=%llx source=%s",
                       (unsigned long long)group->group_id, sc_public_key_hex(source, (char[65]){0}));
            radio_talker_off(ctx, source);
        }
    }
}

/* Снять говорящих, от которых дольше RADIO_TALKER_TIMEOUT_TB нет кадров:
 * FIN потерян/не был отправлен (пир отвалился, выключил рацию во время разговора).
 * Аналог таймаута источника в radio_audio.c — гарантирует, что индикация «говорит» гаснет. */
static void radio_talkers_sweep(struct radio_ctx* ctx) {
    uint64_t now = get_time_tb();
    for (int i = 0; i < RADIO_MAX_TALKERS; i++) {
        if (!ctx->talkers[i].last_tb) continue;
        if (now - ctx->talkers[i].last_tb > RADIO_TALKER_TIMEOUT_TB) {
            DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "%s: talker timeout src=%s (last frame %.0fms ago)",
                        RADIO_ID, sc_public_key_hex(ctx->talkers[i].src, (char[65]){0}),
                        (double)(now - ctx->talkers[i].last_tb) / 10.0);
            radio_talker_off(ctx, ctx->talkers[i].src);
        }
    }
}

static void radio_talker_timer_cb(void* arg) {
    struct radio_ctx* ctx = (struct radio_ctx*)arg;
    if (!ctx || !ctx->group) return;
    radio_talkers_sweep(ctx);
    ctx->talker_timer = uasync_set_timeout(ctx->group->instance->ua, RADIO_TALKER_TIMEOUT_TB, ctx, radio_talker_timer_cb, "radio_talker");
}

/* Дедупликация: вернуть 1, если (src,stream,seq) уже видели (дубль по другому пути),
 * иначе зафиксировать в кэше и вернуть 0. Лениво чистит просроченные записи. */
static int radio_seen_dup(struct radio_ctx* ctx, const uint8_t src[RADIO_KEY_SIZE], uint64_t timestamp, uint64_t now) {
    int free_slot = -1, oldest = -1;
    uint64_t oldest_tb = 0;
    for (int i = 0; i < ctx->seen_count; i++) {
        struct radio_seen* s = &ctx->seen[i];
        if (s->tb && now - s->tb > RADIO_DEDUP_TTL_TB) { s->tb = 0; }
        if (!s->tb) { if (free_slot < 0) free_slot = i; continue; }
        if (!memcmp(s->src, src, RADIO_KEY_SIZE) && s->timestamp == timestamp) return 1;
        if (oldest < 0 || s->tb < oldest_tb) { oldest = i; oldest_tb = s->tb; }
    }
    if (free_slot < 0) {
        if (ctx->seen_count < RADIO_DEDUP_MAX) free_slot = ctx->seen_count++;
        else free_slot = oldest;
    }
    memcpy(ctx->seen[free_slot].src, src, RADIO_KEY_SIZE);
    ctx->seen[free_slot].timestamp = timestamp;
    ctx->seen[free_slot].tb = now;
    return 0;
}

/* Только после проверки подписи: 0 — новая пачка, 1 — повтор/опоздание, -1 — нет памяти. */
static int radio_accept_timestamp(struct radio_ctx* ctx, const uint8_t src[RADIO_KEY_SIZE], uint64_t timestamp) {
    struct radio_author_time* author = ctx->author_times;
    while (author && memcmp(author->src, src, RADIO_KEY_SIZE)) author = author->next;
    if (author && timestamp <= author->timestamp) {
        DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "radio: old batch group=%llx src=%s timestamp=%llu last=%llu",
            (unsigned long long)ctx->group->group_id, sc_public_key_hex(src, (char[65]){0}),
            (unsigned long long)timestamp, (unsigned long long)author->timestamp);
        return 1;
    }
    if (!author) {
        author = u_calloc(1, sizeof(*author));
        if (!author) {
            DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio: author timestamp allocation failed group=%llx src=%s",
                (unsigned long long)ctx->group->group_id, sc_public_key_hex(src, (char[65]){0})); return -1;
        }
        memcpy(author->src, src, RADIO_KEY_SIZE); author->next = ctx->author_times; ctx->author_times = author;
    }
    author->timestamp = timestamp;
    DEBUG_TRACE(DEBUG_CATEGORY_RADIO, "radio: new batch group=%llx src=%s timestamp=%llu",
        (unsigned long long)ctx->group->group_id, sc_public_key_hex(src, (char[65]){0}), (unsigned long long)timestamp);
    return 0;
}

/* ── приём ── */

static void radio_recv(struct TOPO_GROUP* group, struct ETCP_CONN* from_conn,
                       const uint8_t* payload, size_t payload_len) {
    struct radio_ctx* ctx = radio_of(group);
    if (!ctx || !from_conn || payload_len < RADIO_HDR_SIZE) return;
    if (!topo_group_data_allowed(group, from_conn)) return;

    const uint8_t* p = payload;
    uint64_t group_id = group->group_id, timestamp;
    const uint8_t* src = p + RADIO_OFF_SRC;
    uint16_t stream, seq;
    uint8_t flags, hop_count;
    memcpy(&stream, p + RADIO_OFF_STREAM, 2);
    memcpy(&seq, p + RADIO_OFF_SEQ, 2);
    memcpy(&timestamp, p + RADIO_OFF_TIMESTAMP, 8);
    flags = p[RADIO_OFF_FLAGS];
    hop_count = p[RADIO_OFF_HOPS];

    if (!hop_count || hop_count > RADIO_MAX_HOPS
        || payload_len < RADIO_HDR_SIZE + (size_t)hop_count * RADIO_KEY_SIZE + RADIO_SIGNATURE_SIZE) {
        ctx->c_bad_dropped++;
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: bad pkt grp=%016llx hops=%u len=%zu from=%s",
                   RADIO_ID, (unsigned long long)group_id, hop_count, payload_len, from_conn->log_name);
        return;
    }
    const uint8_t* hops = p + RADIO_OFF_HOPLIST;
    const uint8_t* body = p + RADIO_OFF_HOPLIST + (size_t)hop_count * RADIO_KEY_SIZE;
    size_t body_len = payload_len - RADIO_OFF_HOPLIST - (size_t)hop_count * RADIO_KEY_SIZE - RADIO_SIGNATURE_SIZE;
    const uint8_t* signature = body + body_len;
    uint8_t count = p[RADIO_OFF_COUNT];
    const uint8_t* frames[RADIO_MAX_BURST];
    uint16_t lengths[RADIO_MAX_BURST];
    size_t offset = 0;
    /* Проверить всю пачку до изменения дедупликации, индикации и выдачи первого кадра. */
    if ((flags == RADIO_FLAG_FIN && (count || body_len))
        || (flags != RADIO_FLAG_FIN && (flags || !count || count > RADIO_MAX_BURST))) goto malformed;
    for (unsigned i = 0; i < count; i++) {
        if (body_len - offset < 2) goto malformed;
        memcpy(&lengths[i], body + offset, 2); offset += 2;
        if (!lengths[i] || lengths[i] > RADIO_MAX_OPUS || lengths[i] > body_len - offset) goto malformed;
        frames[i] = body + offset; offset += lengths[i];
    }
    if (offset != body_len) goto malformed;
    const uint8_t zero[RADIO_KEY_SIZE] = {0};
    if (!timestamp || memcmp(hops, src, RADIO_KEY_SIZE) ||
        memcmp(hops + (hop_count - 1) * RADIO_KEY_SIZE, from_conn->peer_ed25519_pubkey, RADIO_KEY_SIZE)) goto malformed;
    for (unsigned i = 0; i < hop_count; i++) {
        const uint8_t* hop = hops + i * RADIO_KEY_SIZE;
        if (!memcmp(hop, zero, RADIO_KEY_SIZE)) goto malformed;
        for (unsigned j = 0; j < i; j++) if (!memcmp(hops + j * RADIO_KEY_SIZE, hop, RADIO_KEY_SIZE)) goto malformed;
    }

    uint8_t x[32];
    if (group->radio_active && radio_verify_author(group, src, x, payload, body, body_len, signature)) {
        ctx->c_auth_dropped++; return;
    }

    const uint8_t* self = group->instance->my_ed25519_pubkey;

    /* цикл: мы уже проходили этот пакет */
    for (int i = 0; i < hop_count; i++) {
        if (!memcmp(hops + i * RADIO_KEY_SIZE, self, RADIO_KEY_SIZE)) {
            ctx->c_loop_dropped++;
            DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "%s: loop drop src=%s stream=%u seq=%u", RADIO_ID, sc_public_key_hex(src, (char[65]){0}), stream, seq);
            return;
        }
    }

    /* Подписчик помнит автора до закрытия группы; чистый транзит использует короткий кэш. */
    int duplicate = group->radio_active ? radio_accept_timestamp(ctx, src, timestamp)
                                       : radio_seen_dup(ctx, src, timestamp, get_time_tb());
    if (duplicate) {
        if (duplicate > 0) ctx->c_dup_dropped++;
        DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "%s: dup drop src=%s stream=%u seq=%u", RADIO_ID, sc_public_key_hex(src, (char[65]){0}), stream, seq);
        return;
    }

    /* начало передачи (первый кадр burst): видно и на транзитных узлах (коло), не только у подписчиков */
    if (seq == 0) {
        DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "%s: recv burst src=%s stream=%u fin=%u bytes=%zu sub=%d",
                    RADIO_ID, sc_public_key_hex(src, (char[65]){0}), stream, (flags & RADIO_FLAG_FIN) ? 1 : 0,
                    body_len, group->radio_active ? 1 : 0);
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "radio: RX batch src=%s stream=%u seq=%u count=%u bytes=%zu fin=%u",
                sc_public_key_hex(src, (char[65]){0}), stream, seq, count, body_len, flags);

    /* доставка локально, если мы подписаны */
    if (group->radio_active && ctx->group) {
        struct radio_instance* ri = radio_inst(group->instance);
        if (ri && ri->frame_fn) {
            for (unsigned i = 0; i < count; i++) {
                if (!topo_group_data_allowed(group, from_conn) || !group_policy_member_key_allowed(group->instance, group->channel_id, x, src, 0)) return;
                ri->frame_fn(group->instance, group_id, src, stream, (uint16_t)(seq + i), 0, frames[i], lengths[i], ri->frame_arg);
                ctx->c_rx_frames++;
            }
            if (!topo_group_data_allowed(group, from_conn) || !group_policy_member_key_allowed(group->instance, group->channel_id, x, src, 0)) return;
            if (flags & RADIO_FLAG_FIN) ri->frame_fn(group->instance, group_id, src, stream, seq, 1, NULL, 0, ri->frame_arg);
            ctx->c_rx_frames += !!(flags & RADIO_FLAG_FIN);
        }
        /* индикация говорящих: старт/стоп передачи */
        if (flags & RADIO_FLAG_FIN) radio_talker_off(ctx, src);
        else radio_talker_on(ctx, src, stream);
    }

    /* форвардинг: новый hop_list = старый + self */
    uint8_t new_count = hop_count + 1;
    if (new_count > RADIO_MAX_HOPS) {
        DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "%s: max hops reached, no fwd src=%s", RADIO_ID, sc_public_key_hex(src, (char[65]){0}));
        return;
    }
    uint8_t new_hops[RADIO_MAX_HOPS * RADIO_KEY_SIZE];
    memcpy(new_hops, hops, (size_t)hop_count * RADIO_KEY_SIZE);
    memcpy(new_hops + hop_count * RADIO_KEY_SIZE, self, RADIO_KEY_SIZE);

    uint8_t out[RADIO_MAX_PACKET_SIZE];
    size_t out_len = radio_build(out, p + RADIO_OFF_GROUP, src, stream, seq, timestamp, flags, new_hops, new_count, count, body, body_len);
    memcpy(out + out_len, signature, RADIO_SIGNATURE_SIZE); out_len += RADIO_SIGNATURE_SIZE;
    radio_flood(group, from_conn, out, out_len, new_hops, new_count);
    return;
malformed:
    ctx->c_bad_dropped++;
    DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio: malformed batch src=%s stream=%u seq=%u flags=%u count=%u bytes=%zu offset=%zu",
               sc_public_key_hex(src, (char[65]){0}), stream, seq, flags, count, body_len, offset);
}

static void radio_recv_dispatcher(struct ETCP_CONN* from_conn, struct ll_entry* entry) {
    if (!from_conn || !entry || !entry->dgram || entry->len < 1 + RADIO_KEY_SIZE + RADIO_HDR_SIZE + RADIO_SIGNATURE_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio: invalid receive entry bytes=%zu", entry ? (size_t)entry->len : 0);
        if (entry) { queue_dgram_free(entry); queue_entry_free(entry); }
        return;
    }
    struct UTUN_INSTANCE* instance = from_conn->instance;
    if (!instance) { queue_dgram_free(entry); queue_entry_free(entry); return; }

    uint8_t* dgram = entry->dgram;
    if (dgram[0] != ETCP_ID_RADIO) { queue_dgram_free(entry); queue_entry_free(entry); return; }

    const uint8_t* key = dgram + 1 + RADIO_OFF_GROUP;
    struct TOPO_GROUP* group = topo_groups_find_wire(instance->topo_groups, key);
    if (group) radio_recv(group, from_conn, dgram + 1, entry->len - 1);
    else DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio: unknown group key=%s peer=%s", sc_public_key_hex(key, (char[65]){0}), from_conn->log_name);
    queue_dgram_free(entry); queue_entry_free(entry);
}

int radio_init_instance(struct UTUN_INSTANCE* inst) {
    if (!inst) return -1;
    if (inst->radio) return 0;
    struct radio_instance* ri = u_calloc(1, sizeof(*ri));
    if (!ri) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: alloc instance failed", RADIO_ID); return -1; }
    inst->radio = ri;
    if (etcp_bind(inst, ETCP_ID_RADIO, radio_recv_dispatcher) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: etcp_bind(ETCP_ID_RADIO) failed", RADIO_ID);
        u_free(ri); inst->radio = NULL;
        return -1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "%s: instance initialized (bound ETCP_ID_RADIO)", RADIO_ID);
    return 0;
}

void radio_destroy_instance(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->radio) return;
    etcp_unbind(inst, ETCP_ID_RADIO);
    u_free(inst->radio);
    inst->radio = NULL;
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "%s: instance destroyed", RADIO_ID);
}

void radio_set_frame_cb(struct UTUN_INSTANCE* inst, radio_frame_fn fn, void* arg) {
    struct radio_instance* ri = radio_inst(inst);
    if (!ri) return;
    ri->frame_fn = fn;
    ri->frame_arg = arg;
}

void radio_set_mute_cb(struct UTUN_INSTANCE* inst, radio_mute_fn fn, void* arg) {
    struct radio_instance* ri = radio_inst(inst);
    if (!ri) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio: mute callback without instance"); return; }
    ri->mute_fn = fn; ri->mute_arg = arg;
}

/* Один SQL-снимок/WRITE без runtime. Выходной список готов до commit и принадлежит задаче. */
static int radio_mute_work(sqlite3* db, void* arg) {
    struct radio_mute_sql* query = arg; char sql[180]; sqlite3_stmt* st = NULL;
    const char* stage = "prepare"; int rc = SQLITE_OK;
    if (query->write) {
        if (query->muted == 2) {
            snprintf(sql, sizeof(sql), "SELECT radio_muted FROM \"peers_%s\" WHERE ed25519_pubkey=? AND record_state=0", query->channel);
            rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
            if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, query->ed, 32, SQLITE_STATIC);
            if (rc == SQLITE_OK) rc = sqlite3_step(st);
            if (rc != SQLITE_ROW) { stage = "toggle lookup"; if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND; goto error; }
            if (sqlite3_column_type(st, 0) != SQLITE_INTEGER || sqlite3_column_int(st, 0) < 0 || sqlite3_column_int(st, 0) > 1) {
                stage = "toggle value"; rc = SQLITE_CORRUPT; goto error;
            }
            query->muted = !sqlite3_column_int(st, 0);
            stage = "toggle finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
        }
        snprintf(sql, sizeof(sql), "UPDATE \"peers_%s\" SET radio_muted=? WHERE ed25519_pubkey=? AND record_state=0", query->channel);
        stage = "prepare update"; rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL); if (rc != SQLITE_OK) goto error;
        stage = "bind mute"; rc = sqlite3_bind_int(st, 1, query->muted); if (rc != SQLITE_OK) goto error;
        stage = "bind member"; rc = sqlite3_bind_blob(st, 2, query->ed, 32, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
        stage = "update"; rc = sqlite3_step(st); if (rc != SQLITE_DONE) goto error;
        stage = "changes"; if (sqlite3_changes(db) != 1) { rc = SQLITE_CONSTRAINT; goto error; }
        stage = "update finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    }
    snprintf(sql, sizeof(sql), "SELECT ed25519_pubkey FROM \"peers_%s\" WHERE record_state=0 AND radio_muted<>0", query->channel);
    stage = "prepare list"; rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL); if (rc != SQLITE_OK) goto error;
    size_t capacity = 0; stage = "read list";
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (query->count == 65535) { stage = "list limit"; rc = SQLITE_TOOBIG; goto error; }
        if (sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != 32) {
            stage = "member Ed25519"; rc = SQLITE_CORRUPT; goto error;
        }
        if ((size_t)query->count == capacity) {
            size_t next = capacity ? capacity * 2 : 16; if (next > 65535) next = 65535;
            uint8_t (*members)[32] = u_realloc(query->members, next * sizeof(*members));
            if (!members) { stage = "reserve list"; rc = SQLITE_NOMEM; goto error; }
            query->members = members; capacity = next;
        }
        const void* ed = sqlite3_column_blob(st, 0);
        if (!ed) { stage = "read member Ed25519"; rc = SQLITE_NOMEM; goto error; }
        memcpy(query->members[query->count++], ed, 32);
    }
    if (rc != SQLITE_DONE) goto error;
    stage = "list finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "radio mute SQL ch=%s member=%s muted=%d rows=%d", query->channel,
        sc_public_key_hex(query->ed, (char[65]){0}), query->muted, query->count); return SQLITE_OK;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio mute SQL failed ch=%s member=%s stage=%s rows=%d rc=%d: %s", query->channel,
        sc_public_key_hex(query->ed, (char[65]){0}), stage, query->count, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int final = sqlite3_finalize(st);
    if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio mute cleanup finalize rc=%d", final);
    return rc;
}

/* Опубликовать готовую копию без аллокаций; старые READ не заменят более свежий commit. */
static void radio_mute_publish(struct radio_ctx* ctx, struct radio_mute_sql* query) {
    struct radio_mute_task** link = &ctx->mutes;
    while (*link) {
        struct radio_mute_task* task = *link;
        if (task->sql.write) { link = &task->next; continue; }
        *link = task->next; task->ctx = NULL;
    }
    u_free(ctx->muted_members); ctx->muted_members = query->members; query->members = NULL;
    ctx->muted_count = query->count; ctx->muted_loaded = 1;
    DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "radio mute snapshot committed ch=%s rows=%d", query->channel, query->count);
}

static int radio_mute_guard(void* arg) {
    struct radio_mute_task* task = arg;
    return task->ctx && !task->ctx->group->stopping;
}
static void radio_mute_cleanup(void* arg) {
    struct radio_mute_task* task = arg; u_free(task->sql.members); u_free(task);
}
static void radio_mute_done(void* arg, const struct db_async_result* outcome) {
    struct radio_mute_task* task = arg; struct radio_ctx* ctx = task->ctx; int result = -1;
    if (ctx) {
        struct radio_mute_task** link = &ctx->mutes; while (*link != task) link = &(*link)->next;
        *link = task->next; task->ctx = NULL;
    }
    if (outcome->status) DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio mute database ch=%s member=%s status=%d rc=%d: %s",
        task->sql.channel, sc_public_key_hex(task->sql.ed, (char[65]){0}), outcome->status, outcome->sqlite_code, outcome->message);
    else if (ctx && !ctx->group->stopping) {
        struct UTUN_INSTANCE* inst = ctx->group->instance; uint64_t group = ctx->group->group_id;
        radio_mute_publish(ctx, &task->sql); result = 0;
        int listen = ctx->listen_requested;
        if (listen) topo_group_set_radio(ctx->group, 1);
        if (task->sql.write) {
            DEBUG_INFO(DEBUG_CATEGORY_RADIO, "radio mute committed ch=%s member=%s muted=%d", task->sql.channel,
                sc_public_key_hex(task->sql.ed, (char[65]){0}), task->sql.muted);
            radio_refresh_member_mute(inst, group, task->sql.ed);
        }
    } else DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "radio mute completion detached ch=%s member=%s", task->sql.channel,
        sc_public_key_hex(task->sql.ed, (char[65]){0}));
    if (task->done) task->done(task->arg, result, result ? 0 : task->sql.muted);
}

/* Первый READ объединяется до завершения; ни worker, ни поздний ответ не владеют группой. */
static void radio_load_muted_members(struct radio_ctx* ctx) {
    for (struct radio_mute_task* task = ctx->mutes; task; task = task->next) if (!task->sql.write) return;
    struct radio_mute_task* task = u_calloc(1, sizeof(*task));
    if (!task) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio mute READ allocation failed"); return; }
    struct UTUN_INSTANCE* inst = ctx->group->instance;
    strcpy(task->sql.channel, ctx->group->channel_id);
    task->ctx = ctx; task->next = ctx->mutes; ctx->mutes = task;
    if (inst->db_worker) {
        db_async_run(inst->db_worker, &(struct db_async_task){.name="radio-mutes", .mode=DB_ASYNC_READ, .work=radio_mute_work,
            .data=&task->sql, .done=radio_mute_done, .cleanup=radio_mute_cleanup, .guard=radio_mute_guard}); return;
    }
    sqlite3* ram = inst->topo_sqlite_db; const char* file = ram ? sqlite3_db_filename(ram, "main") : NULL;
    struct db_async_result result = {.status=DB_ASYNC_CLOSED, .sqlite_code=SQLITE_ABORT};
    snprintf(result.message, sizeof(result.message), "radio mute READ has no asynchronous owner");
    if (file && !*file) {
        result.sqlite_code = radio_mute_work(ram, &task->sql); result.status = result.sqlite_code ? DB_ASYNC_SQL : DB_ASYNC_OK;
        snprintf(result.message, sizeof(result.message), "RAM radio mutes: %s", sqlite3_errstr(result.sqlite_code));
    }
    radio_mute_done(task, &result); radio_mute_cleanup(task);
}

int radio_member_muted(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t ed[32]) {
    if (!inst || !inst->topo_groups || !ed) return 0;
    struct radio_ctx* ctx = radio_of(topo_groups_find(inst->topo_groups, group_id));
    if (!ctx) return 0;
    for (int i = 0; i < ctx->muted_count; i++) if (!memcmp(ctx->muted_members[i], ed, 32)) return 1;
    return 0;
}

int radio_member_silenced(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t ed[32]) {
    return radio_member_muted(inst, group_id, ed) || stream_radio_muted(inst, group_id, ed);
}

void radio_refresh_member_mute(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t ed[32]) {
    struct radio_instance* ri = radio_inst(inst);
    if (ri && ri->mute_fn && ed) ri->mute_fn(inst, group_id, ed, radio_member_silenced(inst, group_id, ed), ri->mute_arg);
}

void radio_forget_member_mute(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t ed[32]) {
    if (!inst || !inst->topo_groups) return;
    struct radio_ctx* ctx = radio_of(topo_groups_find(inst->topo_groups, group_id));
    if (!ctx) return;
    struct radio_mute_task** link = &ctx->mutes;
    while (*link) {
        struct radio_mute_task* task = *link;
        if (task->sql.write) { link = &task->next; continue; }
        *link = task->next; task->ctx = NULL;
    }
    for (int i = 0; i < ctx->muted_count; i++) if (!memcmp(ctx->muted_members[i], ed, 32)) {
        ctx->muted_count--;
        if (i != ctx->muted_count) memcpy(ctx->muted_members[i], ctx->muted_members[ctx->muted_count], 32);
        DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "radio: forget removed member mute grp=%016llx member=%s",
                    (unsigned long long)group_id, sc_public_key_hex(ed, (char[65]){0}));
        break;
    }
    if (!ctx->muted_loaded && ctx->listen_requested) radio_load_muted_members(ctx);
}

/* Временный единственный legacy writer и RAM-тесты используют ту же SQL-операцию, что worker. */
static int radio_mute_legacy(struct UTUN_INSTANCE* inst, struct radio_mute_sql* query) {
    sqlite3* db = inst->topo_sqlite_db;
    const char* file = db ? sqlite3_db_filename(db, "main") : NULL;
    if (!db || !sqlite3_get_autocommit(db) || db_async_can_write(inst->db_worker) || (!inst->db_worker && file && *file)) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio mute legacy writer unavailable/external transaction ch=%s", query->channel);
        return SQLITE_MISUSE;
    }
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE /* radio mute */", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto error;
    rc = radio_mute_work(db, query); if (rc != SQLITE_OK) goto error;
    rc = sqlite3_exec(db, "COMMIT /* radio mute */", NULL, NULL, NULL); if (rc == SQLITE_OK) return rc;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio mute legacy transaction ch=%s rc=%d: %s", query->channel, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    if (!sqlite3_get_autocommit(db)) {
        int rollback = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        if (rollback != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio: local mute rollback rc=%d: %s", rollback, sqlite3_errmsg(db));
        if (!sqlite3_get_autocommit(db)) {
            DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio: local mute transaction remains open; requesting owner stop/DB close");
            utun_instance_stop(inst);
        }
    }
    return rc;
}

int radio_set_member_muted(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t ed[32], int muted) {
    struct TOPO_GROUP* group = inst && inst->topo_groups ? topo_groups_find(inst->topo_groups, group_id) : NULL;
    struct radio_ctx* ctx = radio_of(group);
    if (!ctx || group->stopping || group->group_type != TOPO_GROUP_TYPE_CHAT || !ed || !memcmp(ed, (uint8_t[32]){0}, 32) || !memcmp(ed, inst->my_ed25519_pubkey, 32) ||
        (muted != 0 && muted != 1)) {
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio: invalid local mute grp=%llx member=%s muted=%d",
            (unsigned long long)group_id, sc_public_key_hex(ed, (char[65]){0}), muted); return -1;
    }
    struct radio_mute_sql query = {.write=1, .muted=muted};
    memcpy(query.ed, ed, 32);
    strcpy(query.channel, topo_groups_channel(inst->topo_groups, group_id));
    int rc = radio_mute_legacy(inst, &query);
    if (rc == SQLITE_OK) {
        radio_mute_publish(ctx, &query);
        DEBUG_INFO(DEBUG_CATEGORY_RADIO, "radio mute committed ch=%s member=%s muted=%d", query.channel,
            sc_public_key_hex(ed, (char[65]){0}), muted);
        if (ctx->listen_requested) topo_group_set_radio(group, 1);
        radio_refresh_member_mute(inst, group_id, ed);
    }
    u_free(query.members); return rc == SQLITE_OK ? 0 : -1;
}

void radio_member_mute_save(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t ed[32], int muted,
    radio_mute_done_fn done, void* arg) {
    if (!done) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio mute requires done callback"); return; }
    struct TOPO_GROUP* group = inst && inst->topo_groups ? topo_groups_find(inst->topo_groups, group_id) : NULL;
    struct radio_ctx* ctx = radio_of(group);
    if (!ctx || group->stopping || group->group_type != TOPO_GROUP_TYPE_CHAT || !ed || !memcmp(ed, (uint8_t[32]){0}, 32) || !memcmp(ed, inst->my_ed25519_pubkey, 32) ||
        muted < 0 || muted > 2) {
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio mute invalid group/node/value"); done(arg, -1, 0); return;
    }
    struct radio_mute_task* task = u_calloc(1, sizeof(*task));
    if (!task) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio mute WRITE allocation failed"); done(arg, -1, 0); return; }
    strcpy(task->sql.channel, topo_groups_channel(inst->topo_groups, group_id));
    memcpy(task->sql.ed, ed, 32); task->sql.write = 1; task->sql.muted = muted; task->ctx = ctx; task->done = done; task->arg = arg;
    task->next = ctx->mutes; ctx->mutes = task;
    if (db_async_can_write(inst->db_worker)) {
        db_async_run(inst->db_worker, &(struct db_async_task){.name="radio-mute-save", .mode=DB_ASYNC_WRITE, .work=radio_mute_work,
            .data=&task->sql, .done=radio_mute_done, .cleanup=radio_mute_cleanup, .guard=radio_mute_guard}); return;
    }
    int rc = radio_mute_legacy(inst, &task->sql);
    struct db_async_result result = {.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK, .sqlite_code=rc};
    snprintf(result.message, sizeof(result.message), "legacy radio mute transaction: %s", sqlite3_errstr(rc));
    radio_mute_done(task, &result); radio_mute_cleanup(task);
}

/* ── подписка (вкл/выкл прослушивание) ── */

void radio_set_active(struct UTUN_INSTANCE* inst, uint64_t group_id, int on) {
    if (!inst || !inst->topo_groups) return;
    struct TOPO_GROUP* group = topo_groups_find(inst->topo_groups, group_id);
    if (!group) { DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: set_active group 0x%016llx not found", RADIO_ID, (unsigned long long)group_id); return; }
    struct radio_ctx* ctx = radio_of(group);
    if (ctx) ctx->listen_requested = !!on;
    if (on && group->group_type == TOPO_GROUP_TYPE_CHAT) {
        if (!ctx) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio subscription without context"); return; }
        if (!ctx->muted_loaded) { radio_load_muted_members(ctx); return; }
    }
    if (!on) {
        /* выключение прослушивания: снять всех активных говорящих — индикация должна погаснуть
         * сразу, а не дожидаться FIN/таймаута */
        struct radio_ctx* ctx = radio_of(group);
        if (ctx) {
            for (int i = 0; i < RADIO_MAX_TALKERS; i++)
                if (ctx->talkers[i].last_tb) radio_talker_off(ctx, ctx->talkers[i].src);
        }
    }
    topo_group_set_radio(group, on);
}

int radio_is_active(struct UTUN_INSTANCE* inst, uint64_t group_id) {
    if (!inst || !inst->topo_groups) return 0;
    struct TOPO_GROUP* group = topo_groups_find(inst->topo_groups, group_id);
    return group ? group->radio_active : 0;
}

int radio_start_pending(struct UTUN_INSTANCE* inst, uint64_t group_id) {
    struct radio_ctx* ctx = radio_of(inst && inst->topo_groups ? topo_groups_find(inst->topo_groups, group_id) : NULL);
    if (!ctx || !ctx->listen_requested || ctx->muted_loaded) return 0;
    for (struct radio_mute_task* task = ctx->mutes; task; task = task->next) if (!task->sql.write) return 1;
    return 0;
}

int radio_subscribers(struct UTUN_INSTANCE* inst, uint64_t group_id) {
    if (!inst || !inst->topo_groups) return 0;
    struct TOPO_GROUP* group = topo_groups_find(inst->topo_groups, group_id);
    return group ? topo_group_radio_subscribers(group) : 0;
}

/* Число повторов/опоздавших пачек и дублей транзита — для диагностики/тестов. */
uint32_t radio_dup_dropped(struct UTUN_INSTANCE* inst, uint64_t group_id) {
    if (!inst || !inst->topo_groups) return 0;
    struct TOPO_GROUP* group = topo_groups_find(inst->topo_groups, group_id);
    struct radio_ctx* ctx = radio_of(group);
    return ctx ? ctx->c_dup_dropped : 0;
}

/* ── передача (PTT) ── */

/* Отправить накопленные кадры одной кодограммой; новый кадр не продлевает deadline. */
static void radio_flush(struct radio_ctx* ctx, const char* reason) {
    if (ctx->burst_timer) {
        uasync_cancel_timeout(ctx->group->instance->ua, ctx->burst_timer); ctx->burst_timer = NULL;
    }
    if (!ctx->burst_count) return;
    if (!topo_group_data_allowed(ctx->group, NULL)) {
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio: discard revoked batch group=%llx frames=%u",
                   (unsigned long long)ctx->group->group_id, ctx->burst_count);
        radio_revalidate_members(ctx->group); return;
    }
    const uint8_t* key = topo_group_wire_id(ctx->group);
    if (!key) {
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio: discard batch without group key ch=%s frames=%u", ctx->group->channel_id, ctx->burst_count);
        ctx->burst_count = 0; ctx->burst_bytes = 0; return;
    }
    const uint8_t* self = ctx->group->instance->my_ed25519_pubkey;
    uint8_t out[RADIO_HDR_SIZE + RADIO_KEY_SIZE + RADIO_MAX_BODY + RADIO_SIGNATURE_SIZE];
    uint16_t first_seq = (uint16_t)(ctx->seq - ctx->burst_count);
    size_t len = radio_build(out, key, self, ctx->stream_id, first_seq, 0, 0, self, 1,
                            ctx->burst_count, ctx->burst_body, ctx->burst_bytes);
    DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "radio: TX batch group=%llx stream=%u seq=%u count=%u bytes=%zu reason=%s",
                (unsigned long long)ctx->group->group_id, ctx->stream_id, first_seq, ctx->burst_count, len, reason);
    ctx->burst_count = 0; ctx->burst_bytes = 0;
    if (!radio_sign(ctx, out, len)) radio_flood(ctx->group, NULL, out, len + RADIO_SIGNATURE_SIZE, self, 1);
}

/* Кадры перестали поступать: отправить хвост, не дожидаясь END. */
static void radio_burst_timeout(void* arg) {
    struct radio_ctx* ctx = arg;
    ctx->burst_timer = NULL;
    radio_flush(ctx, "deadline");
}

int radio_talk_begin(struct UTUN_INSTANCE* inst, uint64_t group_id) {
    if (!inst || !inst->topo_groups) return -1;
    struct TOPO_GROUP* group = topo_groups_find(inst->topo_groups, group_id);
    if (!group) { DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: talk_begin group 0x%016llx not found", RADIO_ID, (unsigned long long)group_id); return -1; }
    struct radio_ctx* ctx = radio_of(group);
    if (!ctx) return -1;
    if (!topo_group_data_allowed(group, NULL)) { radio_revalidate_members(group); return -1; }
    if (ctx->talking) {
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio: BEGIN closes unfinished stream=%u", ctx->stream_id);
        radio_talk_end(inst, group_id);
    }
    ctx->stream_id = (uint16_t)(ctx->stream_id + 1);
    ctx->seq = 0;
    ctx->talking = 1;
    ctx->burst_target = (uint8_t)chat_setting_get_int(inst, "radio_burst_frames", 4);
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "%s: talk_begin grp=%016llx stream=%u burst_frames=%u deadline_ms=%u",
               RADIO_ID, (unsigned long long)group_id, ctx->stream_id, ctx->burst_target, ctx->burst_target * 20);
    return 0;
}

int radio_talk_send(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t* opus, int len) {
    if (!inst || !inst->topo_groups || !opus || len <= 0 || len > RADIO_MAX_OPUS) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio: invalid send frame len=%d", len); return -1;
    }
    struct TOPO_GROUP* group = topo_groups_find(inst->topo_groups, group_id);
    struct radio_ctx* ctx = radio_of(group);
    if (!ctx) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio: send group unavailable group=%llx", (unsigned long long)group_id); return -1; }
    if (!topo_group_data_allowed(group, NULL)) { radio_revalidate_members(group); return -1; }
    if (!ctx->talking) {
        /* burst не открыт: хвостовой кадр, пришедший после radio_talk_end (гонка релиза PTT) —
         * не переоткрываем передачу, иначе получится burst без FIN и «говорит» зависнет у пиров. */
        DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "%s: send dropped: no open burst grp=%016llx", RADIO_ID, (unsigned long long)group_id);
        return 0;
    }

    uint16_t size = (uint16_t)len;
    memcpy(ctx->burst_body + ctx->burst_bytes, &size, 2); ctx->burst_bytes += 2;
    memcpy(ctx->burst_body + ctx->burst_bytes, opus, (size_t)len); ctx->burst_bytes += (size_t)len;
    ctx->burst_count++;
    ctx->seq++;
    ctx->c_tx_frames++;
    if (ctx->burst_count == ctx->burst_target) radio_flush(ctx, "full");
    else if (!ctx->burst_timer) {
        ctx->burst_timer = uasync_set_timeout(inst->ua, ctx->burst_target * 200, ctx, radio_burst_timeout, "radio_burst");
        if (!ctx->burst_timer) {
            DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio: batch timer allocation failed; flushing frames=%u", ctx->burst_count);
            radio_flush(ctx, "timer_failed");
        }
    }
    return 0;
}

int radio_talk_end(struct UTUN_INSTANCE* inst, uint64_t group_id) {
    if (!inst || !inst->topo_groups) return -1;
    struct TOPO_GROUP* group = topo_groups_find(inst->topo_groups, group_id);
    if (!group) return -1;
    struct radio_ctx* ctx = radio_of(group);
    if (!ctx) return -1;
    if (!topo_group_data_allowed(group, NULL)) { radio_revalidate_members(group); return -1; }
    if (!ctx->talking) return 0;

    radio_flush(ctx, "end");
    const uint8_t* key = topo_group_wire_id(group);
    ctx->talking = 0;
    if (!key) { DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio: FIN without group key ch=%s", group->channel_id); return -1; }
    const uint8_t* self = inst->my_ed25519_pubkey;
    uint8_t out[RADIO_HDR_SIZE + RADIO_KEY_SIZE + RADIO_SIGNATURE_SIZE];
    size_t out_len = radio_build(out, key, self, ctx->stream_id, ctx->seq,
                                 0, RADIO_FLAG_FIN, self, 1, 0, NULL, 0);
    ctx->seq++;
    ctx->c_tx_frames++;

    DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "%s: talk_end grp=%016llx stream=%u frames=%u",
                RADIO_ID, (unsigned long long)group_id, ctx->stream_id, ctx->c_tx_frames);
    if (radio_sign(ctx, out, out_len)) return -1;
    radio_flood(group, NULL, out, out_len + RADIO_SIGNATURE_SIZE, self, 1);
    return 0;
}

/* ── trampoline-функции (GUI/аудио-поток → uasync) ── */

void radio_talk_begin_trampoline(void* arg) {
    struct radio_talk_arg* a = (struct radio_talk_arg*)arg;
    radio_talk_begin(a->inst, a->group_id);
    u_free(a);
}

void radio_talk_end_trampoline(void* arg) {
    struct radio_talk_arg* a = (struct radio_talk_arg*)arg;
    radio_talk_end(a->inst, a->group_id);
    u_free(a);
}

void radio_talk_send_trampoline(void* arg) {
    struct radio_media_arg* a = (struct radio_media_arg*)arg;
    radio_talk_send(a->inst, a->group_id, a->opus, a->len);
    u_free(a);
}

void radio_set_active_trampoline(void* arg) {
    struct radio_set_active_arg* a = (struct radio_set_active_arg*)arg;
    radio_set_active(a->inst, a->group_id, a->on);
    u_free(a);
}
