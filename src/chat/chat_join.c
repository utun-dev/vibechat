/* Локальные приглашения, сквозной JOIN и пересылка по BGP. Контракт в chat_join.h. */

#include "chat_join.h"
#include "../db_changes.h"
#include "member_sync.h"
#include "chat_event.h"
#include "chat_sync.h"
#include "group_policy.h"
#include "invite_build.h"
#include "chat_core.h"
#include "merkle_sync.h"
#include "../../lib/db_async.h"

#include "../utun_instance.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../routing_layer/topo_group.h"
#include "../routing_layer/etcp_router.h"
#include "../routing_layer/route_crypto.h"
#include "../transport_layer/etcp_api.h"
#include "../transport_layer/etcp.h"
#include "../transport_layer/secure_channel.h"
#include "../ntp_time.h"
#include "../../lib/mem.h"
#include "../../lib/ll_queue.h"
#include "../../lib/u_async.h"
#include "../../lib/debug_config.h"
#include "../../lib/platform_compat.h"

#include <string.h>
#include <stdlib.h>
#include <openssl/crypto.h>

#define CJ_ID "chat_join"

/* ── Локальные ключи приглашений ── */

struct join_key_entry {
    struct join_key_entry* next;
    uint64_t join_key;
    uint64_t channel_id;
    uint64_t expiry_tb;    /* timebase (0.1ms) истечения */
    uint64_t window_request; /* Владелец ключа окна; 0 для приглашений со сроком. */
};

static uint64_t _now_tb(void) { return get_time_tb(); }

/* Ключ действует, пока локальный узел допущен и имеет право приглашать. */
static int inviter_allowed(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t inviter[32]) {
    const char* channel = topo_groups_channel(inst->topo_groups, group);
    if (!inst->join_stopping && topo_group_chat_key_allowed(inst, group, inviter) &&
        group_policy_can_invite(inst, channel, inviter) == 1) return 1;
    DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "%s: invitation access denied ch=%llu inviter=%s", CJ_ID,
               (unsigned long long)group, sc_public_key_hex(inviter, (char[65]){0}));
    return 0;
}

static void keys_revalidate(struct UTUN_INSTANCE* inst, uint64_t group) {
    uint64_t now = _now_tb();
    struct join_key_entry** p = &inst->join_keys;
    while (*p) {
        struct join_key_entry* key = *p;
        int expired = key->expiry_tb <= now;
        if (expired || ((!group || key->channel_id == group) && !inviter_allowed(inst, key->channel_id, inst->my_ed25519_pubkey))) {
            struct join_key_entry* r = *p;
            *p = r->next;
            DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "%s: join-key removed ch=%llu inviter=%s reason=%s",
                       CJ_ID, (unsigned long long)r->channel_id, sc_public_key_hex(inst->my_ed25519_pubkey, (char[65]){0}),
                       expired ? "expired" : "access revoked");
            u_free(r);
        } else {
            p = &(*p)->next;
        }
    }
}

/* Оконный ключ занимает один слот таблицы и сохраняется до отзыва или потери доступа. */
static int _keys_put(struct UTUN_INSTANCE* inst, uint64_t channel_id, uint64_t join_key, uint64_t window_request) {
    if (!inst || inst->join_stopping || !channel_id || channel_id > INT64_MAX || !join_key) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "%s: invalid key registration", CJ_ID);
        return -1;
    }
    const uint8_t* inviter_id = inst->my_ed25519_pubkey;
    keys_revalidate(inst, channel_id);
    if (!inviter_allowed(inst, channel_id, inviter_id)) return -1;
    /* Список упорядочен по последней регистрации, в том числе при одинаковом expiry_tb. */
    struct join_key_entry** oldest = NULL;
    struct join_key_entry* entry = NULL;
    unsigned count = 0;
    for (struct join_key_entry** link = &inst->join_keys; *link; link = &(*link)->next) {
        struct join_key_entry* key = *link;
        if (key->join_key == join_key) {
            if (key->channel_id == channel_id && key->window_request == window_request) {
                entry = key; *link = key->next;
                break;
            }
            DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "%s: invitation token already in use ch=%llu registered_ch=%llu", CJ_ID,
                       (unsigned long long)channel_id, (unsigned long long)key->channel_id);
            return -1;
        }
        if (window_request && key->window_request) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: window key already published request=%llu", CJ_ID,
                (unsigned long long)key->window_request);
            return -1;
        }
        if (!key->window_request) oldest = link;
        count++;
    }
    const int refresh = entry != NULL;
    if (!entry && count >= JOIN_KEYS_PER_INVITER) {
        entry = *oldest; *oldest = entry->next;
        DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "%s: oldest join-key replaced ch=%llu inviter=%s limit=%u", CJ_ID,
            (unsigned long long)entry->channel_id, sc_public_key_hex(inviter_id, (char[65]){0}), JOIN_KEYS_PER_INVITER);
    }
    if (!entry) {
        entry = u_malloc(sizeof(*entry));
        if (!entry) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: join-key alloc failed", CJ_ID); return -1; }
    }
    entry->join_key = join_key; entry->channel_id = channel_id;
    entry->window_request = window_request;
    entry->expiry_tb = window_request ? UINT64_MAX : _now_tb() + (uint64_t)JOIN_KEY_TTL_SECONDS * 10000ULL;
    entry->next = inst->join_keys; inst->join_keys = entry;
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "%s: join-key %s ch=%llu inviter=%s window_request=%llu ttl_s=%u", CJ_ID,
        refresh ? "refreshed" : "stored", (unsigned long long)channel_id, sc_public_key_hex(inviter_id, (char[65]){0}),
        (unsigned long long)window_request, window_request ? 0 : JOIN_KEY_TTL_SECONDS);
    return 0;
}

static struct join_key_entry* _keys_find(struct UTUN_INSTANCE* inst, uint64_t channel_id, uint64_t join_key) {
    keys_revalidate(inst, channel_id);
    for (struct join_key_entry* e = inst->join_keys; e; e = e->next)
        if (e->channel_id == channel_id && e->join_key == join_key) return e;
    return NULL;
}

static void _keys_free_all(struct UTUN_INSTANCE* inst) {
    struct join_key_entry* e = inst->join_keys;
    while (e) { struct join_key_entry* n = e->next; u_free(e); e = n; }
    inst->join_keys = NULL;
}

/* ── Сериализация member-блока ── */

int chat_join_serialize_member(const struct join_member_data* m,
                               uint8_t* out, size_t out_sz, size_t* out_len) {
    if (!m || !out || !out_len || !m->x25519 || !m->ed25519) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat_join: invalid member serialization arguments"); return -1;
    }
    size_t ul = strnlen(m->userinfo, sizeof(m->userinfo));
    if (ul > MS_USERINFO_MAX) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "chat_join: profile exceeds limit bytes=%zu", ul); return -1;
    }
    size_t need = JOIN_MEMBER_FIXED_SIZE + ul;
    if (need > out_sz) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat_join: member buffer too small need=%zu capacity=%zu", need, out_sz); return -1;
    }
    size_t off = 0;
    memcpy(out + off, m->x25519, 32); off += 32;
    memcpy(out + off, m->ed25519, 32); off += 32;
    if (m->join_sig) memcpy(out + off, m->join_sig, 64); else memset(out + off, 0, 64); off += 64;
    uint64_t join_ts = htobe64(m->join_ts);
    memcpy(out + off, &join_ts, 8); off += 8;
    if (m->update_sig) memcpy(out + off, m->update_sig, 64); else memset(out + off, 0, 64); off += 64;
    uint64_t update_ts = htobe64(m->update_ts);
    memcpy(out + off, &update_ts, 8); off += 8;
    out[off++] = (uint8_t)(ul >> 8); out[off++] = (uint8_t)ul;
    if (ul) memcpy(out + off, m->userinfo, ul);
    off += ul;
    *out_len = off;
    return 0;
}

int chat_join_parse_member(const uint8_t* data, size_t len, struct join_member_data* output) {
    if (!data || !output || len < JOIN_MEMBER_FIXED_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "chat_join: invalid/short member block bytes=%zu", len); return -1;
    }
    struct join_member_data parsed, *m = &parsed;
    memset(m, 0, sizeof(*m));
    size_t off = 0;
    m->x25519 = data + off; off += 32;
    m->ed25519 = data + off; off += 32;
    m->join_sig = data + off; off += 64;
    memcpy(&m->join_ts, data + off, 8); off += 8; m->join_ts = be64toh(m->join_ts);
    m->update_sig = data + off; off += 64;
    memcpy(&m->update_ts, data + off, 8); off += 8; m->update_ts = be64toh(m->update_ts);
    size_t ul = ((size_t)data[off] << 8) | data[off + 1]; off += 2;
    if (ul > MS_USERINFO_MAX || off + ul != len || memchr(data + off, 0, ul)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "chat_join: invalid profile length bytes=%zu payload=%zu", ul, len); return -1;
    }
    memcpy(m->userinfo, data + off, ul);
    m->userinfo[ul] = '\0';
    *output = parsed;
    return 0;
}

/* ── Обработка на инвайтере: верификация + добавление мембера + подпись дерева ── */

/* Поля конверта уже сверены. Проверяем собственную подпись и профиль. */
static int join_validate_member(sqlite3* db, const char* channel, const struct join_member_data* m, uint64_t node) {
    const uint8_t zero[64] = {0};
    if (!memcmp(m->join_sig, zero, sizeof(zero))) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "E2E JOIN missing member signature ch=%s", channel);
        return SQLITE_AUTH;
    }
    struct ms_member_rec record = {.node_id=node, .x25519=m->x25519, .ed25519=m->ed25519,
        .join_sig=m->join_sig, .join_ts=m->join_ts, .update_sig=m->update_sig, .update_ts=m->update_ts, .userinfo=m->userinfo};
    int code = SQLITE_OK;
    if (member_sync_verify_local_record_ex(db, channel, &record, &code) != 1) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "E2E JOIN member signature/profile invalid ch=%s node=%016llx rc=%d",
            channel, (unsigned long long)node, code);
        return code ? code : SQLITE_AUTH;
    }
    return SQLITE_OK;
}

/* Запись J уже доставлена сквозным каналом. Проверяем ключ транспорта, JOIN и профиль. */
static int join_validate_request(sqlite3* db, const char* channel, const uint8_t guest[32],
    const uint8_t* request, size_t size, struct join_member_data* member) {
    if (chat_join_parse_member(request, size, member) || memcmp(member->ed25519, guest, 32) ||
        sc_identity_check("JOIN guest record", member->x25519, guest) != SC_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN guest record identity mismatch ch=%s guest=%s size=%zu",
            channel, sc_public_key_hex(guest, (char[65]){0}), size);
        return SQLITE_AUTH;
    }
    int code = join_validate_member(db, channel, member, sc_derive_node_id_from_pubkey(member->x25519));
    if (code == SQLITE_AUTH || code == SQLITE_MISMATCH)
        db_changes_warn(db, NULL, channel, "JOIN request", "member signature or profile", guest, member->join_sig, NULL);
    return code;
}

static int join_connect_status(int code) {
    if (code == SQLITE_OK) return 0;
    return code == SQLITE_AUTH ? CHAT_CONNECT_AUTH : CHAT_CONNECT_ERROR;
}

/* RAM/RO выполняет запрос синхронно и возвращает причину отказа для process_submit. */
static int join_process_ram(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t guest[32], uint64_t key,
    uint64_t attempt, const uint8_t* request, size_t request_len) {
    if (!inst || inst->join_stopping || db_async_can_write(inst->db_worker) || !group || group > INT64_MAX ||
        !guest || !key || !attempt || !request || request_len > JOIN_REQUEST_MAX_SIZE || request_len < JOIN_MEMBER_FIXED_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "E2E JOIN invalid/stopped/worker request bytes=%zu", request_len); return SQLITE_MISUSE;
    }
    const char* channel = topo_groups_channel(inst->topo_groups, group);
    if (!inviter_allowed(inst, group, inst->my_ed25519_pubkey) ||
        !chat_join_key_matches(inst, group, key, inst->my_ed25519_pubkey)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN access/key denied ch=%s guest=%s",
            channel, sc_public_key_hex(guest, (char[65]){0})); return SQLITE_AUTH;
    }
    uint8_t signature[64]; struct join_member_data member;
    sqlite3* db = inst->topo_sqlite_db;
    struct db_changes_origin source = db_changes_source(db, guest, 0, "JOIN request");
    int changed = 0;
    int result = join_validate_request(db, channel, guest, request, request_len, &member);
    db_changes_restore(db, source);
    uint8_t join_id[32];
    if (!result) result = member_sync_join_id_read(inst->topo_sqlite_db, channel, member.ed25519, member.x25519, join_id);
    if (!result && member_sync_sign_join(inst->my_ed25519_privkey, channel, inst->my_ed25519_pubkey,
            member.x25519, member.ed25519, join_id, member.join_ts, signature)) result = SQLITE_ERROR;
    if (!result) {
        struct ms_member_rec record = {.node_id=sc_derive_node_id_from_pubkey(member.x25519), .x25519=member.x25519,
            .ed25519=member.ed25519, .join_sig=member.join_sig, .join_ts=member.join_ts, .update_sig=member.update_sig,
            .update_ts=member.update_ts, .userinfo=member.userinfo, .signed_by=inst->my_ed25519_pubkey, .signature=signature};
        changed = member_sync_apply_record(inst, channel, guest, &record);
        if (changed < 0) result = SQLITE_ERROR;
    }
    if (!result && (changed & MS_APPLY_CHANGED)) chat_sync_join_result(inst, 0, group, member.x25519, CHAT_JOIN_OK);
    return result;
}

int chat_join_process_request(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t guest[32], uint64_t key,
    uint64_t attempt, const uint8_t* request, size_t request_len) {
    return join_connect_status(join_process_ram(inst, group, guest, key, attempt, request, request_len));
}

struct join_request_job {
    uint64_t group, key, attempt; char channel[65];
    struct member_sync_write_context context; /* Frozen SQL/crypto inputs. */
    uint8_t request[JOIN_REQUEST_MAX_SIZE]; size_t size;
    int changed; struct group_policy_update* admission;
    uint64_t peer_node;
    struct chat_join_request_result result; chat_join_done_fn done; void* arg;
    struct UTUN_INSTANCE* inst; uint64_t generation, common_generation, registration; /* Uasync only. */
};
static int join_request_current(void* data) {
    struct join_request_job* job = data; struct UTUN_INSTANCE* inst = job->inst;
    if (inst->destroying || !inst->group_started || inst->join_stopping || chat_core_generation(inst) != job->generation ||
        inst->group_generation != job->common_generation) return 0;
    struct TOPO_GROUP* group = inst->topo_groups ? topo_groups_find(inst->topo_groups, job->group) : NULL;
    return group && !group->stopping && group->group_type == TOPO_GROUP_TYPE_CHAT && group->generation == job->registration;
}
static int join_request_guard(void* data) {
    struct join_request_job* job = data;
    return join_request_current(job) && inviter_allowed(job->inst, job->group, job->context.public_key) &&
        chat_join_key_matches(job->inst, job->group, job->key, job->context.public_key);
}
static int join_request_write(sqlite3* db, void* data) {
    struct join_request_job* job = data; int allowed = group_policy_can_invite_read(db, job->channel, job->context.public_key);
    if (allowed < 0) return -allowed;
    if (!allowed) { job->result.status = CHAT_CONNECT_AUTH; return SQLITE_AUTH; }
    int code = SQLITE_OK;
    enum ms_author_status author = member_sync_verify_author(db, job->channel, job->context.public_key, &code);
    if (author == MS_AUTHOR_ERROR) return code ? code : SQLITE_ERROR;
    if (author != MS_AUTHOR_ACTIVE) return SQLITE_AUTH;
    uint8_t signature[64]; struct join_member_data member;
    struct db_changes_origin source = db_changes_source(db, job->result.node, job->peer_node, "JOIN request");
    code = join_validate_request(db, job->channel, job->result.node, job->request, job->size, &member);
    if (code) goto done;
    uint8_t join_id[32];
    code = member_sync_join_id_read(db, job->channel, member.ed25519, member.x25519, join_id);
    if (code) goto done;
    if (member_sync_sign_join(job->context.private_key, job->channel, job->context.public_key,
            member.x25519, member.ed25519, join_id, member.join_ts, signature)) {
        code = SQLITE_ERROR; goto done;
    }
    struct ms_member_rec record = {.node_id=sc_derive_node_id_from_pubkey(member.x25519), .x25519=member.x25519, .ed25519=member.ed25519,
        .join_sig=member.join_sig, .join_ts=member.join_ts, .update_sig=member.update_sig, .update_ts=member.update_ts,
        .userinfo=member.userinfo, .signed_by=job->context.public_key, .signature=signature};
    job->changed = member_sync_record_write(db, &job->context, job->channel, NULL, &record);
    if (job->changed < 0) { code = job->context.sqlite_code ? job->context.sqlite_code : SQLITE_ERROR; goto done; }
    int projected = group_policy_project(db, job->context.self, job->channel, &job->admission);
    if (projected) { code = -projected; goto done; }
    job->result.status = 0;
done:
    db_changes_restore(db, source);
    if (code) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN worker rejected ch=%s guest=%s attempt=%016llx rc=%d",
        job->channel, sc_public_key_hex(job->result.node, (char[65]){0}), (unsigned long long)job->attempt, code);
    return code;
}
static void join_request_done(void* data, const struct db_async_result* result) {
    struct join_request_job* job = data; int current = join_request_current(job);
    job->result.sqlite_code = result->sqlite_code; job->result.committed = !result->status;
    if (result->status) job->result.status = join_connect_status(result->sqlite_code ? result->sqlite_code : SQLITE_ERROR);
    else if (!current) { job->result.status = CHAT_CONNECT_ERROR; job->result.sqlite_code = SQLITE_ABORT; }
    if (!job->result.status) {
        struct group_policy_update* update = job->admission; job->admission = NULL; group_policy_publish(job->inst, update);
        if (join_request_current(job) && (job->changed & (MS_APPLY_CHANGED)))
            member_sync_record_committed(job->inst, job->channel, job->result.node, (unsigned)job->changed);
        if (join_request_current(job) && (job->changed & MS_APPLY_CHANGED)) {
            uint8_t x[32];
            if (sc_identity_public_x("JOIN result", job->result.node, x) == SC_OK)
                chat_sync_join_result(job->inst, 0, job->group, x, CHAT_JOIN_OK);
        }
        DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "JOIN committed ch=%s inviter=%s guest=%s attempt=%016llx changes=%d",
            job->channel, sc_public_key_hex(job->context.public_key, (char[65]){0}),
            sc_public_key_hex(job->result.node, (char[65]){0}), (unsigned long long)job->attempt, job->changed);
    } else DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "E2E JOIN completion ch=%s status=%d rc=%d committed=%d: %s",
        job->channel, job->result.status, job->result.sqlite_code, job->result.committed, result->message);
    if (job->done) job->done(job->arg, &job->result);
}
static void join_request_free(void* data) {
    struct join_request_job* job = data; group_policy_discard(job->admission); OPENSSL_cleanse(job, sizeof(*job)); u_free(job);
}
void chat_join_process_submit(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t guest[32], uint64_t key, uint64_t attempt,
    const uint8_t* request, size_t size, chat_join_done_fn done, void* arg) {
    struct chat_join_request_result result = {.status=CHAT_CONNECT_ERROR, .sqlite_code=SQLITE_MISUSE};
    if (guest) memcpy(result.node, guest, 32);
    struct TOPO_GROUP* topology = inst && inst->topo_groups ? topo_groups_find(inst->topo_groups, group) : NULL;
    if (!inst || inst->destroying || !inst->group_started || inst->join_stopping || !chat_core_is_initialized(inst) ||
        !topology || topology->stopping || topology->group_type != TOPO_GROUP_TYPE_CHAT || !key || !attempt || !guest ||
        !request || size > JOIN_REQUEST_MAX_SIZE || size < JOIN_MEMBER_FIXED_SIZE) goto refused;
    if (!db_async_can_write(inst->db_worker)) {
        result.sqlite_code = join_process_ram(inst, group, guest, key, attempt, request, size);
        result.status = join_connect_status(result.sqlite_code); result.committed = !result.status;
        if (done) done(arg, &result); return;
    }
    struct join_request_job* job = u_calloc(1, sizeof(*job));
    if (!job) { result.sqlite_code = SQLITE_NOMEM; goto refused; }
    job->inst = inst; job->group = group; job->key = key; job->attempt = attempt;
    uint8_t x[32];
    if (sc_identity_public_x("JOIN guest", guest, x) == SC_OK) job->peer_node = sc_derive_node_id_from_pubkey(x);
    job->size = size; memcpy(job->request, request, size);
    job->context.self = inst->node_id; job->context.now = ntp_time_get_us(inst);

    memcpy(job->context.public_key, inst->my_ed25519_pubkey, 32); memcpy(job->context.private_key, inst->my_ed25519_privkey, 32);
    job->generation = chat_core_generation(inst); job->common_generation = inst->group_generation; job->registration = topology->generation;
    strcpy(job->channel, topology->channel_id);
    job->result = result; job->done = done; job->arg = arg;
    db_async_run(inst->db_worker, &(struct db_async_task){.name="join-request", .mode=DB_ASYNC_WRITE,
        .guard=join_request_guard, .work=join_request_write, .done=join_request_done, .cleanup=join_request_free, .data=job}); return;
refused:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN submission refused ch=%llu guest=%s attempt=%016llx rc=%d bytes=%zu",
        (unsigned long long)group, sc_public_key_hex(guest, (char[65]){0}), (unsigned long long)attempt, result.sqlite_code, size);
    if (done) done(arg, &result);
}
void chat_join_revalidate_members(struct UTUN_INSTANCE* inst, uint64_t group) {
    if (inst) keys_revalidate(inst, group);
}

static uint64_t join_read_be64(const uint8_t* p) {
    uint64_t n; memcpy(&n, p, 8); return be64toh(n);
}

static void join_write_be64(uint8_t* p, uint64_t n) {
    n = htobe64(n); memcpy(p, &n, 8);
}

enum { JOIN_TO_INVITER = 1, JOIN_TO_GUEST };
enum {
    JOIN_RELAY_INVITER = 1,
    JOIN_RELAY_GUEST = JOIN_RELAY_INVITER + 32,
    JOIN_RELAY_ATTEMPT = JOIN_RELAY_GUEST + 32,
    JOIN_RELAY_HEADER = JOIN_RELAY_ATTEMPT + 8,
    JOIN_CIPHER_OVERHEAD = SC_NONCE_SIZE + SC_TAG_SIZE + SC_CRC32_SIZE
};
static const uint8_t join_relay_domain[16] = "utun-join-relay";
#define JOIN_RELAY_CONTEXT (sizeof(join_relay_domain) + 32 + JOIN_RELAY_HEADER)
#define JOIN_RELAY_MAX (JOIN_RELAY_HEADER + JOIN_RELAY_CONTEXT + 1 + CHAT_JOIN_BODY_MAX + JOIN_CIPHER_OVERHEAD)
#define JOIN_RELAY_IDLE_TB (60u * 10000u)
#define JOIN_RELAY_LIMIT 64u
_Static_assert(JOIN_RELAY_MAX + 1 <= ROUTER_SIGNED_ENCRYPTED_LIMIT, "JOIN relay frame must fit BGP");

struct join_relay {
    struct join_relay* next;
    struct chat_join_path path;
    uint64_t deadline;
};

/* Удаляет истёкшие и закрытые обратные пути. */
void chat_join_relay_closed(struct UTUN_INSTANCE* inst, const uint8_t* x, uint64_t handle) {
    struct join_relay** link = &inst->join_relays;
    uint64_t now = get_time_tb();
    while (*link) {
        struct join_relay* relay = *link;
        int closed = x ? !relay->path.handle && !memcmp(x, relay->path.hop_x, 32)
            : relay->path.handle && (!handle || handle == relay->path.handle);
        struct TOPO_GROUP* group = topo_groups_find(inst->topo_groups, relay->path.group);
        if (relay->deadline <= now || closed || inst->join_stopping || !group || group->stopping ||
            group->generation != relay->path.generation) {
            DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "JOIN relay removed attempt=%016llx group=%llu closed=%d",
                (unsigned long long)relay->path.attempt, (unsigned long long)relay->path.group, closed);
            *link = relay->next;
            u_free(relay);
        } else link = &relay->next;
    }
}

static int join_relay_parse(struct chat_join_path* path, const uint8_t* frame, size_t size) {
    if (size < JOIN_RELAY_HEADER + JOIN_RELAY_CONTEXT + 1 + JOIN_CIPHER_OVERHEAD || size > JOIN_RELAY_MAX ||
        (frame[0] != JOIN_TO_INVITER && frame[0] != JOIN_TO_GUEST) || !memcmp(frame + JOIN_RELAY_INVITER, frame + JOIN_RELAY_GUEST, 32) ||
        !(path->attempt = join_read_be64(frame + JOIN_RELAY_ATTEMPT))) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN relay invalid frame size=%zu", size);
        return -1;
    }
    memcpy(path->inviter, frame + JOIN_RELAY_INVITER, 32);
    memcpy(path->guest, frame + JOIN_RELAY_GUEST, 32);
    return 0;
}

static int join_relay_direct_send(struct UTUN_INSTANCE* inst, const struct chat_join_path* path, const uint8_t* data, size_t size) {
    struct ll_entry* e = ll_alloc_lldgram(size + 2);
    if (!e) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN direct frame allocation failed bytes=%zu", size);
        return -1;
    }
    e->len = size + 2;
    e->dgram[0] = ETCP_ID_CHAT_SYNC;
    e->dgram[1] = CS_MSG_JOIN_RELAY;
    memcpy(e->dgram + 2, data, size);
    int rc = -1;
    if (path->handle) {
        const struct chat_join_carrier* carrier = inst->join_carrier;
        if (carrier) rc = carrier->send(carrier->ctx, path->handle, e->dgram, e->len);
    } else {
        struct ETCP_CONN* conn = instance_find_conn_by_key(inst, path->hop_x);
        if (conn && conn->initialized && conn->links_up && !conn->close_requested && !etcp_send(conn, e)) return 0;
    }
    if (rc) DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN direct send unavailable attempt=%016llx peer=%s carrier=%llu",
        (unsigned long long)path->attempt, sc_public_key_hex(path->hop_x, (char[65]){0}), (unsigned long long)path->handle);
    queue_dgram_free(e);
    queue_entry_free(e);
    return rc;
}

static int join_relay_bgp_send(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t generation, const uint8_t ed[32],
    const uint8_t* frame, size_t size) {
    struct TOPO_GROUP* g = topo_groups_find(inst->topo_groups, group);
    if (!g || g->generation != generation || !topo_group_chat_key_allowed(inst, group, ed) ||
        !topo_node_best_path(topo_node_find_by_ed(g, ed))) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN relay BGP path unavailable group=%llu peer=%s attempt=%016llx",
            (unsigned long long)group, sc_public_key_hex(ed, (char[65]){0}), (unsigned long long)join_read_be64(frame + JOIN_RELAY_ATTEMPT));
        return -1;
    }
    struct ll_entry* e = ll_alloc_lldgram(size + 2);
    if (!e) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN relay BGP frame allocation failed bytes=%zu", size);
        return -1;
    }
    e->len = size + 2;
    e->dgram[0] = ETCP_RT_ID_JOIN;
    e->dgram[1] = JOIN_SUBCMD_RELAY;
    memcpy(e->dgram + 2, frame, size);
    int rc = etcp_route_send_key(inst, group, ed, e, 0, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT);
    if (rc) DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN relay BGP send failed group=%llu attempt=%016llx rc=%d",
        (unsigned long long)group, (unsigned long long)join_read_be64(frame + JOIN_RELAY_ATTEMPT), rc);
    return rc;
}

/* Использует существующие маршруты CHAT по полному ключу, независимо от канала приглашения. */
static struct TOPO_GROUP* join_relay_group(struct UTUN_INSTANCE* inst, const uint8_t ed[32]) {
    struct TOPO_GROUP* best = NULL;
    unsigned hops = UINT8_MAX;
    for (struct ll_entry* e = inst->topo_groups->group_list->head; e; e = e->next) {
        struct TOPO_GROUP* group = (struct TOPO_GROUP*)e;
        if (group->group_type != TOPO_GROUP_TYPE_CHAT || group->stopping || !topo_group_wire_id(group)) continue;
        struct TOPO_NODEPATH* path = topo_node_best_path(topo_node_find_by_ed(group, ed));
        if (!path || path->hop_count > hops || !topo_group_chat_key_allowed(inst, group->group_id, ed)) continue;
        if (!best || path->hop_count < hops || memcmp(topo_group_wire_id(group), topo_group_wire_id(best), 32) < 0) {
            best = group;
            hops = path->hop_count;
        }
    }
    return best;
}

/* В шифротексте повторяем открытый заголовок и связываем выбранный выход с попыткой. */
static void join_relay_context(uint8_t data[JOIN_RELAY_CONTEXT], const struct chat_join_path* path, const uint8_t* header) {
    memcpy(data, join_relay_domain, sizeof(join_relay_domain));
    memcpy(data + sizeof(join_relay_domain), path->exit_x, 32);
    memcpy(data + sizeof(join_relay_domain) + 32, header, JOIN_RELAY_HEADER);
}

static int join_relay_crypto(struct UTUN_INSTANCE* inst, sc_context_t* ctx, const uint8_t peer[32]) {
    uint8_t x[32];
    return sc_identity_public_x("JOIN endpoint", peer, x) != SC_OK || sc_init_ctx(ctx, &inst->my_keys) != SC_OK ||
        sc_set_peer_public_key(ctx, x, SC_PEER_PUBKEY_BIN) != SC_OK;
}

int chat_join_send(struct UTUN_INSTANCE* inst, const struct chat_join_path* path, uint8_t type,
    const uint8_t* data, size_t size) {
    int author = !memcmp(inst->my_ed25519_pubkey, path->inviter, 32);
    if (inst->join_stopping || !path->attempt || size > CHAT_JOIN_BODY_MAX ||
        (!author && memcmp(inst->my_ed25519_pubkey, path->guest, 32))) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN send invalid endpoint type=%u bytes=%zu", type, size);
        return -1;
    }
    uint8_t frame[JOIN_RELAY_MAX], plain[JOIN_RELAY_CONTEXT + 1 + CHAT_JOIN_BODY_MAX];
    frame[0] = author ? JOIN_TO_GUEST : JOIN_TO_INVITER;
    memcpy(frame + JOIN_RELAY_INVITER, path->inviter, 32);
    memcpy(frame + JOIN_RELAY_GUEST, path->guest, 32);
    join_write_be64(frame + JOIN_RELAY_ATTEMPT, path->attempt);
    join_relay_context(plain, path, frame);
    plain[JOIN_RELAY_CONTEXT] = type;
    if (size) memcpy(plain + JOIN_RELAY_CONTEXT + 1, data, size);
    sc_context_t crypto = {0};
    size_t sealed = 0;
    int failed = join_relay_crypto(inst, &crypto, author ? path->guest : path->inviter) ||
        sc_encrypt(&crypto, plain, JOIN_RELAY_CONTEXT + 1 + size, frame + JOIN_RELAY_HEADER, &sealed) != SC_OK;
    OPENSSL_cleanse(&crypto, sizeof(crypto));
    OPENSSL_cleanse(plain, JOIN_RELAY_CONTEXT + 1 + size);
    if (failed) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN seal failed attempt=%016llx type=%u", (unsigned long long)path->attempt, type);
        return -1;
    }
    DEBUG_TRACE(DEBUG_CATEGORY_CHAT_SYNC, "JOIN TX attempt=%016llx type=%u direction=%u bytes=%zu",
        (unsigned long long)path->attempt, type, frame[0], size);
    size_t length = JOIN_RELAY_HEADER + sealed;
    if (path->group) return join_relay_bgp_send(inst, path->group, path->generation, path->hop_ed, frame, length);
    return join_relay_direct_send(inst, path, frame, length);
}

static void join_relay_open(struct UTUN_INSTANCE* inst, const struct chat_join_path* path, const uint8_t* frame, size_t size) {
    uint8_t plain[JOIN_RELAY_CONTEXT + 1 + CHAT_JOIN_BODY_MAX + SC_CRC32_SIZE], expected[JOIN_RELAY_CONTEXT];
    const uint8_t* sender = frame[0] == JOIN_TO_INVITER ? path->guest : path->inviter;
    sc_context_t crypto = {0};
    size_t opened = 0;
    join_relay_context(expected, path, frame);
    if (join_relay_crypto(inst, &crypto, sender) ||
        sc_decrypt(&crypto, frame + JOIN_RELAY_HEADER, size - JOIN_RELAY_HEADER, plain, &opened) != SC_OK ||
        opened <= sizeof(expected) || CRYPTO_memcmp(expected, plain, sizeof(expected))) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN authentication failed attempt=%016llx sender=%s direction=%u",
            (unsigned long long)path->attempt, sc_public_key_hex(sender, (char[65]){0}), frame[0]);
    } else {
        chat_sync_join_receive(inst, path, plain[sizeof(expected)], plain + sizeof(expected) + 1, opened - sizeof(expected) - 1);
    }
    OPENSSL_cleanse(&crypto, sizeof(crypto));
    OPENSSL_cleanse(plain, sizeof(plain));
}

void chat_join_receive_direct(struct UTUN_INSTANCE* inst, const struct chat_join_route* route, const uint8_t* frame, size_t size) {
    struct chat_join_path path = {.handle = route->handle};
    if (inst->join_stopping || join_relay_parse(&path, frame, size)) return;
    memcpy(path.hop_x, route->public_key, 32);
    if (frame[0] == JOIN_TO_GUEST) {
        if (memcmp(path.guest, inst->my_ed25519_pubkey, 32)) {
            DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN relay response addressed to another guest attempt=%016llx", (unsigned long long)path.attempt);
            return;
        }
        memcpy(path.exit_x, path.hop_x, 32);
        join_relay_open(inst, &path, frame, size);
        return;
    }
    if (sc_identity_check("JOIN guest transport", path.hop_x, path.guest) != SC_OK) return;
    if (!memcmp(path.inviter, inst->my_ed25519_pubkey, 32)) {
        memcpy(path.exit_x, inst->my_keys.public_key, 32);
        join_relay_open(inst, &path, frame, size);
        return;
    }
    chat_join_relay_closed(inst, NULL, UINT64_MAX);
    unsigned count = 0;
    struct join_relay* relay = inst->join_relays;
    while (relay && (relay->path.attempt != path.attempt || memcmp(relay->path.guest, path.guest, 32) ||
        memcmp(relay->path.inviter, path.inviter, 32))) {
        count++;
        relay = relay->next;
    }
    if (!relay) {
        struct TOPO_GROUP* group = count < JOIN_RELAY_LIMIT ? join_relay_group(inst, path.inviter) : NULL;
        if (!group) {
            DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN relay unavailable attempt=%016llx inviter=%s guest=%s active=%u",
                (unsigned long long)path.attempt, sc_public_key_hex(path.inviter, (char[65]){0}),
                sc_public_key_hex(path.guest, (char[65]){0}), count);
            return;
        }
        relay = u_malloc(sizeof(*relay));
        if (!relay) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN relay allocation failed");
            return;
        }
        path.group = group->group_id;
        path.generation = group->generation;
        relay->path = path;
        relay->next = inst->join_relays;
        inst->join_relays = relay;
        DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "JOIN relay route group=%s attempt=%016llx inviter=%s guest=%s carrier=%llu",
            group->channel_id, (unsigned long long)path.attempt, sc_public_key_hex(path.inviter, (char[65]){0}),
            sc_public_key_hex(path.guest, (char[65]){0}), (unsigned long long)path.handle);
    }
    if (relay->path.handle != path.handle || memcmp(relay->path.hop_x, path.hop_x, 32)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN relay path changed attempt=%016llx", (unsigned long long)path.attempt);
        return;
    }
    relay->deadline = get_time_tb() + JOIN_RELAY_IDLE_TB;
    join_relay_bgp_send(inst, relay->path.group, relay->path.generation, path.inviter, frame, size);
}

static void join_relay_receive_bgp(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t from[32],
    const uint8_t* frame, size_t size) {
    struct TOPO_GROUP* g = topo_groups_find(inst->topo_groups, group);
    struct chat_join_path path = {.group = group};
    if (!g || g->stopping || inst->join_stopping || join_relay_parse(&path, frame, size)) return;
    path.generation = g->generation;
    if (frame[0] == JOIN_TO_INVITER) {
        if (memcmp(path.inviter, inst->my_ed25519_pubkey, 32) || !memcmp(from, path.guest, 32) ||
            sc_identity_public_x("JOIN exit", from, path.exit_x) != SC_OK) {
            DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN routed endpoint mismatch attempt=%016llx", (unsigned long long)path.attempt);
            return;
        }
        memcpy(path.hop_ed, from, 32);
        join_relay_open(inst, &path, frame, size);
        return;
    }
    chat_join_relay_closed(inst, NULL, UINT64_MAX);
    for (struct join_relay* relay = inst->join_relays; relay; relay = relay->next) {
        const struct chat_join_path* saved = &relay->path;
        if (saved->group == group && saved->generation == path.generation && saved->attempt == path.attempt &&
            !memcmp(path.inviter, from, 32) && !memcmp(saved->inviter, from, 32) && !memcmp(saved->guest, path.guest, 32)) {
            relay->deadline = get_time_tb() + JOIN_RELAY_IDLE_TB;
            join_relay_direct_send(inst, saved, frame, size);
            return;
        }
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "JOIN relay response without route group=%llu attempt=%016llx",
        (unsigned long long)group, (unsigned long long)path.attempt);
}

/* Пакеты имеют фиксированную длину. Разные домены исключают отражение запроса
 * как ответа или как подписываемого JOIN_REQUEST при той же паре X25519. */
static void join_recv_cb(struct ETCP_CONN* conn, struct ll_entry* entry) {
    if (!entry || !conn || !entry->dgram || entry->len < ROUTER_SVC_PAYLOAD_OFF) {
        if (entry) { queue_dgram_free(entry); queue_entry_free(entry); }
        return;
    }
    struct UTUN_INSTANCE* inst = conn->instance;
    if (!inst) { queue_dgram_free(entry); queue_entry_free(entry); return; }

    const uint8_t* d = entry->dgram;
    const uint8_t* from_ed = d + ROUTER_SVC_SRC_ED_OFF;
    uint64_t group_id;  memcpy(&group_id,  d + ROUTER_SVC_GROUP_OFF, 8);
    if (entry->len == ROUTER_SVC_PAYLOAD_OFF) {
        queue_dgram_free(entry); queue_entry_free(entry);
        chat_join_revalidate_members(inst, group_id);
        return;
    }
    uint8_t required = ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED;
    if (memcmp(d + ROUTER_SVC_SRC_ED_OFF, inst->my_ed25519_pubkey, 32) && (d[ROUTER_SVC_FLAGS_OFF] & required) != required) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "%s: unprotected join packet rejected from=%s ch=%llu", CJ_ID,
                   sc_public_key_hex(from_ed, (char[65]){0}), (unsigned long long)group_id);
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    const uint8_t* payload = d + ROUTER_SVC_PAYLOAD_OFF;
    size_t plen = entry->len - ROUTER_SVC_PAYLOAD_OFF;
    uint8_t subcmd = payload[0];

    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "%s: recv subcmd=%02x from=%s ch=%llu len=%zu",
                CJ_ID, subcmd, sc_public_key_hex(from_ed, (char[65]){0}), (unsigned long long)group_id, plen);

    if (subcmd == JOIN_SUBCMD_RELAY) {
        join_relay_receive_bgp(inst, group_id, from_ed, payload + 1, plen - 1);
    } else {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN unknown BGP frame subcmd=%u from=%s",
            subcmd, sc_public_key_hex(from_ed, (char[65]){0}));
    }

    queue_dgram_free(entry); queue_entry_free(entry);
}

/* ── Жизненный цикл ── */

int chat_join_init(struct UTUN_INSTANCE* inst) {
    if (!inst) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: init without instance", CJ_ID); return -1; }
    inst->join_stopping = 0;
    if (etcp_router_bind(inst, ETCP_RT_ID_JOIN, join_recv_cb) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: etcp_router_bind failed", CJ_ID);
        return -1;
    }
    if (etcp_router_require_crypto(inst, ETCP_RT_ID_JOIN, ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED) ||
        etcp_router_set_authorize(inst, ETCP_RT_ID_JOIN, topo_group_chat_key_allowed)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: router policy initialization failed", CJ_ID);
        chat_join_destroy(inst); return -1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "%s: initialized", CJ_ID);
    return 0;
}

void chat_join_destroy(struct UTUN_INSTANCE* inst) {
    if (!inst) return;
    inst->join_stopping = 1;
    chat_join_relay_closed(inst, NULL, 0);
    chat_invite_build_stop(inst);
    if (inst->router_bindings.callbacks[ETCP_RT_ID_JOIN]) etcp_router_unbind(inst, ETCP_RT_ID_JOIN);
    _keys_free_all(inst);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "%s: destroyed", CJ_ID);
}

/* ── Публичные операции ── */

int chat_join_store_local_key(struct UTUN_INSTANCE* inst, uint64_t channel_id, uint64_t join_key) {
    return _keys_put(inst, channel_id, join_key, 0);
}

int chat_join_store_window_key(struct UTUN_INSTANCE* inst, uint64_t channel, uint64_t key, uint64_t request) {
    if (!request) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: missing window invitation owner", CJ_ID); return -1; }
    return _keys_put(inst, channel, key, request);
}

void chat_join_revoke_window_key(struct UTUN_INSTANCE* inst, uint64_t request) {
    for (struct join_key_entry** link = &inst->join_keys; *link; link = &(*link)->next) {
        struct join_key_entry* key = *link;
        if (!key->window_request || (request && key->window_request != request)) continue;
        *link = key->next;
        DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "%s: window key revoked ch=%llu request=%llu", CJ_ID,
            (unsigned long long)key->channel_id, (unsigned long long)key->window_request);
        u_free(key);
        return;
    }
}

const uint8_t* chat_join_lookup_inviter(struct UTUN_INSTANCE* inst, uint64_t channel_id, uint64_t join_key) {
    if (!inst || !channel_id || channel_id > INT64_MAX || !join_key) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: invalid key lookup", CJ_ID); return 0;
    }
    struct join_key_entry* e = _keys_find(inst, channel_id, join_key);
    return e ? inst->my_ed25519_pubkey : NULL;
}

uint64_t chat_join_key_group(struct UTUN_INSTANCE* inst, uint64_t join_key) {
    if (!inst || !join_key) return 0;
    uint64_t now = _now_tb();
    for (const struct join_key_entry* e = inst->join_keys; e; e = e->next)
        if (e->join_key == join_key && e->expiry_tb > now) return e->channel_id;
    return 0;
}

int chat_join_key_matches(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t key, const uint8_t ed[32]) {
    const uint8_t* registered = chat_join_lookup_inviter(inst, group, key);
    return ed && registered && !memcmp(registered, ed, 32);
}
