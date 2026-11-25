/*
 * chat_admin.c — передача канального приватного ключа (прав админа) другому узлу
 *
 * Владелец канала передаёт оба ключа (ed25519_privkey + x25519_privkey) выбранному
 * мемберу через etcp_router с ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT.
 * Получатель верифицирует ключи (вывод pubkey == сохранённые pubkey канала) и импортирует.
 */

#include "chat_admin.h"
#include "chat_core_priv.h"
#include "chat_event.h"
#include "group_policy.h"
#include "member_sync.h"
#include "merkle_sync.h"

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
#include "../../lib/debug_config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>

#define CA_ID "chat_admin"
static void admin_key_recv_cb(struct ETCP_CONN* conn, struct ll_entry* entry);

/* ─── ed25519 pubkey из ed25519 privkey (проверка переданной пары) ─── */

static int ed25519_pub_from_priv(const uint8_t priv[32], uint8_t pub[32]) {
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, priv, 32);
    if (!pkey) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: Ed25519 private key initialization failed", CA_ID); return -1; }
    size_t plen = 32;
    int ok = EVP_PKEY_get_raw_public_key(pkey, pub, &plen) == 1 && plen == 32;
    EVP_PKEY_free(pkey);
    if (!ok) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: Ed25519 public key derivation failed bytes=%zu", CA_ID, plen);
    return ok ? 0 : -1;
}

struct admin_job {
    struct member_sync_write_context context; /* Собственная identity/seed, SQL не обращается к instance. */
    char channel[65];
    uint64_t group;
    uint8_t peer[32]; /* Ed25519 адресата или проверенного отправителя. */
    uint8_t ed_private[32], x_private[32]; /* Вход import либо выход READ, очистить после done. */
    struct group_policy_update* admission;
    int importing, changes;
    struct UTUN_INSTANCE* inst; /* Только uasync guard/done/cleanup. */
    uint64_t core_generation, group_generation, registration;
};

static int admin_current(const struct admin_job* job) {
    struct UTUN_INSTANCE* inst = job->inst;
    struct TOPO_GROUP* group = inst->topo_groups ? topo_groups_find(inst->topo_groups, job->group) : NULL;
    return !inst->destroying && inst->group_started && chat_core_generation(inst) == job->core_generation &&
        inst->router_bindings.callbacks[ETCP_RT_ID_ADMIN_KEY] == admin_key_recv_cb &&
        inst->chat_admin_generation == job->registration && group && !group->stopping && group->generation == job->group_generation &&
        topo_group_chat_key_allowed(inst, job->group, job->peer);
}
static int admin_guard(void* data) {
    struct admin_job* job = data;
    if (!admin_current(job)) return 0;
    job->context.now = ntp_time_get_us(job->inst);
    return 1;
}

static int admin_channel_read(sqlite3* db, struct admin_job* job, uint8_t x[32], uint8_t ed[32]) {
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT x25519_pubkey,ed25519_pubkey,x25519_privkey,ed25519_privkey"
        " FROM channels WHERE channel_id=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, job->channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        uint8_t* outputs[] = {x, ed, job->x_private, job->ed_private};
        rc = SQLITE_OK;
        for (unsigned i = 0; i < (job->importing ? 2u : 4u); i++) {
            const void* value = sqlite3_column_blob(st, (int)i);
            if (sqlite3_column_type(st, (int)i) != SQLITE_BLOB || sqlite3_column_bytes(st, (int)i) != 32 || !value) {
                rc = sqlite3_errcode(db) == SQLITE_NOMEM ? SQLITE_NOMEM : i >= 2 ? SQLITE_AUTH : SQLITE_CORRUPT; break;
            }
            memcpy(outputs[i], value, 32);
        }
    } else if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND;
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    return rc;
}

static int admin_work(sqlite3* db, void* data) {
    struct admin_job* job = data; uint8_t x[32], ed[32], check[32]; int rc = SQLITE_OK;
    const uint8_t* participants[] = {job->context.public_key, job->peer};
    for (unsigned i = 0; i < 2; i++) {
        enum ms_author_status status = member_sync_verify_author(db, job->channel, participants[i], &rc);
        if (status != MS_AUTHOR_ACTIVE) return status == MS_AUTHOR_ERROR ? rc : SQLITE_AUTH;
    }
    rc = admin_channel_read(db, job, x, ed); if (rc) return rc;
    if (ed25519_pub_from_priv(job->ed_private, check) || memcmp(check, ed, 32) ||
        sc_compute_public_key_from_private(job->x_private, check) != SC_OK || memcmp(check, x, 32)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: channel key mismatch ch=%s importing=%d", CA_ID, job->channel, job->importing);
        return SQLITE_AUTH;
    }
    if (!job->importing) return SQLITE_OK;
    sqlite3_stmt* st = NULL;
    rc = sqlite3_prepare_v2(db, "UPDATE channels SET x25519_privkey=?, ed25519_privkey=? WHERE channel_id=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, job->x_private, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, job->ed_private, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_text(st, 3, job->channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) rc = sqlite3_changes(db) == 1 ? SQLITE_OK : SQLITE_CONSTRAINT;
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (!rc) rc = chat_core_save_member_write(db, &job->context, job->channel, job->context.public_key, NULL,
        "{\"admin\":\"yes\"}", &job->admission, &job->changes);
    return rc;
}

static void admin_done(void* data, const struct db_async_result* result) {
    struct admin_job* job = data;
    if (result->status) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: database failed ch=%s peer=%s import=%d status=%d rc=%d: %s",
            CA_ID, job->channel, sc_public_key_hex(job->peer, (char[65]){0}), job->importing, result->status, result->sqlite_code, result->message); return;
    }
    if (!admin_current(job)) { DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "%s: completed after owner stop ch=%s", CA_ID, job->channel); return; }
    if (job->importing) {
        if (job->admission) { struct group_policy_update* update = job->admission; job->admission = NULL; group_policy_publish(job->inst, update); }
        if (admin_current(job) && job->changes)
            member_sync_record_committed(job->inst, job->channel, job->context.public_key, (unsigned)job->changes);
        if (!admin_current(job)) return;
        uint8_t event[1 + sizeof(job->channel) + 32]; size_t length = strlen(job->channel); event[0] = (uint8_t)length;
        memcpy(event + 1, job->channel, length); memcpy(event + 1 + length, job->peer, 32);
        chat_event_post(job->inst, CHAT_EVT_ADMIN_KEY_RECEIVED, event, (int)length + 33);
        DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "%s: keys and admin role committed ch=%s from=%s", CA_ID,
            job->channel, sc_public_key_hex(job->peer, (char[65]){0}));
    } else {
        struct ll_entry* entry = ll_alloc_lldgram(1 + CHAT_ADMIN_KEY_PAYLOAD_SIZE);
        if (!entry) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: send packet allocation failed ch=%s", CA_ID, job->channel); return; }
        entry->dgram[0] = ETCP_RT_ID_ADMIN_KEY;
        memcpy(entry->dgram + 1, job->ed_private, 32); memcpy(entry->dgram + 33, job->x_private, 32);
        entry->len = 1 + CHAT_ADMIN_KEY_PAYLOAD_SIZE;
        int rc = etcp_route_send_key(job->inst, job->group, job->peer, entry, 1, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT);
        if (rc) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: encrypted key send failed ch=%s peer=%s rc=%d",
            CA_ID, job->channel, sc_public_key_hex(job->peer, (char[65]){0}), rc);
        else DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "%s: encrypted keys sent ch=%s peer=%s", CA_ID,
            job->channel, sc_public_key_hex(job->peer, (char[65]){0}));
    }
}
static void admin_free(void* data) {
    struct admin_job* job = data; group_policy_discard(job->admission);
    OPENSSL_cleanse(job->context.private_key, 32); OPENSSL_cleanse(job->ed_private, 32); OPENSSL_cleanse(job->x_private, 32); u_free(job);
}
static void admin_submit(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t peer[32], const uint8_t* ed, const uint8_t* x) {
    struct TOPO_GROUP* group = inst->topo_groups ? topo_groups_find(inst->topo_groups, group_id) : NULL;
    if (!chat_core_generation(inst) || !group || group->stopping || !inst->chat_admin_generation ||
        inst->router_bindings.callbacks[ETCP_RT_ID_ADMIN_KEY] != admin_key_recv_cb ||
        !topo_group_chat_key_allowed(inst, group_id, peer)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "%s: inactive or unauthorized request group=%llu peer=%s", CA_ID,
            (unsigned long long)group_id, sc_public_key_hex(peer, (char[65]){0})); return;
    }
    struct admin_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: cannot reserve key request group=%llu", CA_ID, (unsigned long long)group_id); return; }
    job->inst = inst; job->group = group_id; memcpy(job->peer, peer, 32); job->importing = ed != NULL;
    job->registration = inst->chat_admin_generation; job->core_generation = chat_core_generation(inst); job->group_generation = group->generation;
    strcpy(job->channel, group->channel_id);
    if (ed) { memcpy(job->ed_private, ed, 32); memcpy(job->x_private, x, 32); }
    job->context.self = inst->node_id;
    memcpy(job->context.public_key, inst->my_ed25519_pubkey, 32); memcpy(job->context.private_key, inst->my_ed25519_privkey, 32);
    db_async_run(inst->db_worker, &(struct db_async_task){.name=ed ? "admin-key-import" : "admin-key-export",
        .mode=ed ? DB_ASYNC_WRITE : DB_ASYNC_READ, .data=job, .guard=admin_guard, .work=admin_work, .done=admin_done, .cleanup=admin_free});
}

/* ─── приём переданного ключа ───
 * payload: [ed25519_priv:32][x25519_priv:32]. Группу аутентифицирует Router.
 */
static void admin_key_recv_cb(struct ETCP_CONN* conn, struct ll_entry* entry) {
    if (!entry) return;
    if (conn && entry->dgram && entry->len == ROUTER_SVC_PAYLOAD_OFF) {
        queue_dgram_free(entry); queue_entry_free(entry); return; // У сервиса нет собственного состояния соединения.
    }
    if (!conn || !entry->dgram || entry->len != ROUTER_SVC_PAYLOAD_OFF + CHAT_ADMIN_KEY_PAYLOAD_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "%s: recv — invalid delivery len=%u", CA_ID, entry->len);
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    struct UTUN_INSTANCE* inst = conn->instance;
    if (!inst) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: recv — no instance", CA_ID);
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    struct chat_core_ctx* cc = CC(inst);
    if (!cc || !cc->initialized || (!cc->db && !db_async_can_write(inst->db_worker))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: recv — not initialized", CA_ID);
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    if (!db_async_can_write(inst->db_worker) && !sqlite3_get_autocommit(cc->db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: recv — import rejected inside external transaction", CA_ID);
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }

    const uint8_t* d = entry->dgram;
    const uint8_t* from_ed = d + ROUTER_SVC_SRC_ED_OFF;

    const uint8_t* payload = d + ROUTER_SVC_PAYLOAD_OFF;
    uint64_t group_id; memcpy(&group_id, d + ROUTER_SVC_GROUP_OFF, 8);
    const uint8_t* ed_priv = payload;
    const uint8_t* x_priv = payload + 32;
    if (!topo_group_chat_key_allowed(inst, group_id, from_ed)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "%s: key import denied group=%llu sender=%s", CA_ID,
            (unsigned long long)group_id, sc_public_key_hex(from_ed, (char[65]){0}));
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    if (db_async_can_write(inst->db_worker)) {
        admin_submit(inst, group_id, from_ed, ed_priv, x_priv);
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }

    char ch_id[65]; strcpy(ch_id, topo_groups_channel(inst->topo_groups, group_id));

    /* проверить, что ключи соответствуют публичным ключам канала (защита от подлога) */
    uint8_t ch_x_pub[32], ch_ed_pub[32];
    if (topo_node_sqlite_channel_get(cc->db, ch_id, NULL, 0, NULL, ch_x_pub, ch_ed_pub, NULL) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: recv — no channel ch=%s from %s, drop",
                    CA_ID, ch_id, sc_public_key_hex(from_ed, (char[65]){0}));
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }

    uint8_t ed_pub_check[32], x_pub_check[32];
    if (ed25519_pub_from_priv(ed_priv, ed_pub_check) != 0
        || memcmp(ed_pub_check, ch_ed_pub, 32) != 0
        || sc_compute_public_key_from_private(x_priv, x_pub_check) != SC_OK
        || memcmp(x_pub_check, ch_x_pub, 32) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: recv — key mismatch for ch=%s from %s, drop",
                    CA_ID, ch_id, sc_public_key_hex(from_ed, (char[65]){0}));
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }

    if (topo_node_sqlite_channel_set_privs(cc->db, ch_id, x_priv, ed_priv) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: recv — set_privs FAILED ch=%s from %s",
                    CA_ID, ch_id, sc_public_key_hex(from_ed, (char[65]){0}));
        if (!sqlite3_get_autocommit(cc->db)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: recv — rollback left transaction open ch=%s; restart required", CA_ID, ch_id);
            inst->running = 0;
        }
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }

    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "%s: received admin key ch=%s from %s — imported, now co-owner",
               CA_ID, ch_id, sc_public_key_hex(from_ed, (char[65]){0}));

    /* Другие узлы проверяют право приглашать по подписанной роли, а не по наличию нашего приватного ключа. */
    struct chat_member_tags* tags = chat_member_tags_load(inst, ch_id, inst->my_ed25519_pubkey);
    if (!tags || chat_member_tags_set(tags, "admin", "yes") || chat_member_tags_commit(inst, tags))
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: co-owner key imported, publishing admin role failed ch=%s", CA_ID, ch_id);
    chat_member_tags_free(tags);

    uint8_t evt[97];
    uint8_t cl = (uint8_t)strlen(ch_id);
    evt[0] = cl; memcpy(evt + 1, ch_id, cl);
    memcpy(evt + 1 + cl, from_ed, 32);
    chat_event_post(inst, CHAT_EVT_ADMIN_KEY_RECEIVED, evt, 1 + cl + 32);

    queue_dgram_free(entry); queue_entry_free(entry);
}

/* ─── жизненный цикл ─── */

int chat_admin_init(struct UTUN_INSTANCE* inst) {
    if (!inst || inst->chat_admin_generation == UINT64_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: invalid instance/exhausted registration", CA_ID); return -1;
    }
    if (etcp_router_bind(inst, ETCP_RT_ID_ADMIN_KEY, admin_key_recv_cb) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: etcp_router_bind failed", CA_ID);
        return -1;
    }
    if (etcp_router_require_crypto(inst, ETCP_RT_ID_ADMIN_KEY, ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED) ||
        etcp_router_set_authorize(inst, ETCP_RT_ID_ADMIN_KEY, topo_group_chat_key_allowed)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: router access policy initialization failed", CA_ID);
        etcp_router_unbind(inst, ETCP_RT_ID_ADMIN_KEY); return -1;
    }
    inst->chat_admin_generation++;
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "%s: initialized generation=%llu", CA_ID, (unsigned long long)inst->chat_admin_generation);
    return 0;
}

void chat_admin_destroy(struct UTUN_INSTANCE* inst) {
    if (!inst) return;
    if (inst->chat_admin_generation < UINT64_MAX) inst->chat_admin_generation++;
    etcp_router_unbind(inst, ETCP_RT_ID_ADMIN_KEY);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "%s: destroyed", CA_ID);
}

/* ─── передача прав (вызывается из uasync-потока) ─── */

void chat_core_transfer_admin(struct UTUN_INSTANCE* inst, const char* ch_id, const uint8_t target_ed[32]) {
    if (inst && db_async_can_write(inst->db_worker)) {
        uint64_t group = topo_groups_channel_slot(inst->topo_groups, ch_id);
        if (!group || !target_ed) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: invalid transfer target/channel", CA_ID); return; }
        admin_submit(inst, group, target_ed, NULL, NULL); return;
    }
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized || !cc->db) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: transfer — not initialized", CA_ID);
        return;
    }
    if (!ch_id || !ch_id[0] || target_ed == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: transfer — invalid args ch=%s target=%s",
                    CA_ID, ch_id ? ch_id : "(null)", sc_public_key_hex(target_ed, (char[65]){0}));
        return;
    }

    uint64_t group_id = topo_groups_channel_slot(inst->topo_groups, ch_id);
    if (!group_id || !topo_group_chat_key_allowed(inst, group_id, target_ed)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "%s: transfer denied ch=%s target=%s", CA_ID,
                   ch_id, sc_public_key_hex(target_ed, (char[65]){0})); return;
    }
    uint8_t x_priv[32], ed_priv[32];
    if (topo_node_sqlite_channel_get_privs(cc->db, ch_id, x_priv, ed_priv) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: transfer — no channel privkey ch=%s (not owner?)", CA_ID, ch_id);
        return;
    }

    uint8_t payload[CHAT_ADMIN_KEY_PAYLOAD_SIZE];
    memcpy(payload, ed_priv, 32);
    memcpy(payload + 32, x_priv, 32);

    struct ll_entry* entry = queue_entry_new(0);
    if (!entry) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: transfer — queue_entry_new failed", CA_ID); return; }
    entry->dgram = u_malloc(1 + sizeof(payload));
    if (!entry->dgram) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: transfer — payload allocation failed", CA_ID); queue_entry_free(entry); return; }
    entry->dgram[0] = ETCP_RT_ID_ADMIN_KEY;
    memcpy(entry->dgram + 1, payload, sizeof(payload));
    entry->len = 1 + (uint16_t)sizeof(payload);

    int rc = etcp_route_send_key(inst, group_id, target_ed, entry, 1,
                             ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT);
    if (rc != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: transfer — etcp_route_send FAILED rc=%d ch=%s → %s",
                    CA_ID, rc, ch_id, sc_public_key_hex(target_ed, (char[65]){0}));
        return;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "%s: admin key sent ch=%s → %s (encrypted+signed)",
               CA_ID, ch_id, sc_public_key_hex(target_ed, (char[65]){0}));
}

void chat_core_transfer_admin_trampoline(void* arg) {
    struct chat_transfer_admin_req* req = (struct chat_transfer_admin_req*)arg;
    if (!req) return;
    chat_core_transfer_admin(req->inst, req->ch_id, req->target_ed);
    u_free(req);
}
