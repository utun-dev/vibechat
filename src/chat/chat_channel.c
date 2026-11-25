/*
 * chat_channel.c — управление каналами: ensure_ready, create, connect
 *
 * Вынесено из chat_core.c для уменьшения размера модуля.
 */

#include "chat_core_priv.h"
#include "chat_channel.h"
#include "../group/group_protocol.h"
#include "../kanban/kanban_sql.h"
#include "chat_attachment.h"
#include "group_avatar.h"
#include "group_policy.h"
#include "group_info.h"
#include "chat_event.h"
#include "chat_member.h"
#include "chat_feedback.h"
#include "chat_setting.h"

#include "../utun_instance.h"
#include "../ntp_time.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../routing_layer/topo_group.h"
#include "../routing_layer/topo_group_connect.h"
#include "member_sync.h"
#include "merkle_sync.h"
#include "chat_sync.h"
#include "../transport_layer/secure_channel.h"
#include "../media_delivery/media_delivery.h"
#include "../media_delivery/media_index.h"
#include "../member_files/member_files.h"
#include "../../lib/mem.h"
#include "../../lib/ll_queue.h"
#include "../../lib/db_async.h"
#include "../../lib/platform_compat.h"
#include "../transport_layer/etcp.h"
#include "../transport_layer/etcp_connections.h"

#define OPENSSL_API_COMPAT 0x10100000L
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

#include <errno.h>
#include <stdlib.h>

int chat_channel_valid(const char* text) {
    return text && strnlen(text, 65) == 64 && strspn(text, "0123456789abcdef") == 64 &&
        strspn(text + 16, "0") != 48;
}

uint64_t chat_numeric_id(const char* text) {
    if (!text) return 0;
    size_t length = strnlen(text, 20);
    if (!length || length == 20 || text[0] == '0' || strspn(text, "0123456789") != length) return 0;
    uint64_t id = strtoull(text, NULL, 10); return id <= INT64_MAX ? id : 0;
}

static int typed_description(const char* channel, const char* name, const char* type, const uint8_t owner[32],
    const uint8_t x[32], const uint8_t ed[32], uint8_t* out, size_t capacity) {
    char json[GROUP_INFO_MAX + 1];
    if (group_info_initial_typed(name, type, json)) return -1;
    return group_info_message(channel, json, owner, x, ed, out, capacity);
}

/* Создание по имени подписывает тот же JSON, который сохраняет channel_put. */
int chat_channel_description_data(const char* channel, const char* name, const uint8_t owner[32],
    const uint8_t x25519[32], const uint8_t ed25519[32], uint8_t* out, size_t capacity) {
    char json[GROUP_INFO_MAX + 1];
    if (group_info_initial(name, json)) return -1;
    return group_info_message(channel, json, owner, x25519, ed25519, out, capacity);
}

/* ─── подготовка инфраструктуры канала (db_sync instance) ─── */

int chat_channel_activate(struct UTUN_INSTANCE* inst, const char* channel, const struct group_protocol_prepared* prepared) {
    if (!inst || !chat_core_is_initialized(inst) || !inst->topo_groups || !chat_channel_valid(channel) || !prepared) {
        DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "channel activation invalid owner/channel/snapshot"); return -1;
    }
    struct TOPO_GROUP* group = topo_groups_find_channel(inst->topo_groups, channel);
    int created = !group;
    if (!group) group = topo_groups_create_channel(inst->topo_groups, channel);
    if (!group || member_sync_subscribe_group(inst, group) || group_protocol_activate(inst, channel, prepared)) {
        DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "channel activation failed ch=%s created=%d", channel, created);
        if (created && group) topo_groups_remove_group(inst->topo_groups, group->group_id);
        return -1;
    }
    return 0;
}

int chat_core_ensure_channel_ready(struct UTUN_INSTANCE* inst, const char* ch_id) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized || !chat_channel_valid(ch_id) || !inst->topo_groups) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "channel preparation without active chat/topology"); return -1;
    }
    if (db_async_can_write(inst->db_worker)) {
        struct TOPO_GROUP* existing = topo_groups_find_channel(inst->topo_groups, ch_id);
        if (existing && existing->group_type == TOPO_GROUP_TYPE_CHAT && !existing->stopping) return 0;
        DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "channel preparation requires async submit ch=%s", ch_id); return -1;
    }
    if (topo_node_sqlite_member_local_init(cc->db, ch_id) < 0) return -1;
    struct TOPO_GROUP* group = topo_groups_find_channel(inst->topo_groups, ch_id);
    int created_group = !group;
    if (!group) group = topo_groups_create_channel(inst->topo_groups, ch_id);
    if (!group || member_sync_subscribe_group(inst, group) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "channel group preparation failed ch=%s", ch_id);
        if (group && created_group) topo_groups_remove_group(inst->topo_groups, group->group_id);
        return -1;
    }
    if (group_protocol_prepare(inst, ch_id)) {
        DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "group content preparation failed ch=%s", ch_id);
        if (created_group) topo_groups_remove_group(inst->topo_groups, group->group_id);
        return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_GROUP, "group prepared ch=%s new_group=%d", ch_id, created_group);
    return 0;
}

/* ─── создание канала ─── */

int chat_channel_check_public_keys(const char* channel, const uint8_t x[32], const uint8_t ed[32]) {
    int rc = sc_identity_check(channel, x, ed);
    if (rc == SC_OK) {
        if (chat_channel_valid(channel) && !strcmp(channel, sc_public_key_hex(ed, (char[65]){0}))) return SQLITE_OK;
        DEBUG_WARN(DEBUG_CATEGORY_GROUP, "group identity does not match channel address ch=%s key=%s",
            channel, sc_public_key_hex(ed, (char[65]){0})); return SQLITE_AUTH;
    }
    return rc == SC_ERR_AUTH_FAILED ? SQLITE_AUTH : rc == SC_ERR_INVALID_ARG ? SQLITE_MISUSE : SQLITE_ERROR;
}

/* Проверить оба публичных ключа и транспортный секрет по исходному seed. */
static int channel_creation_keys(const struct chat_channel_create* req) {
    uint8_t x[32], secret[32], ed[32];
    int rc = sc_identity_from_seed(req->ed25519_privkey, x, secret, ed);
    int matches = !rc && !CRYPTO_memcmp(secret, req->x25519_privkey, 32) &&
        !memcmp(x, req->x25519_pubkey, 32) && !memcmp(ed, req->ed25519_pubkey, 32);
    OPENSSL_cleanse(secret, sizeof(secret));
    if (matches) return chat_channel_check_public_keys(req->channel_id, x, ed);
    DEBUG_WARN(DEBUG_CATEGORY_GROUP, "group identity does not match its seed ch=%s crypto=%d", req->channel_id, rc);
    return rc && rc != SC_ERR_AUTH_FAILED ? SQLITE_ERROR : SQLITE_AUTH;
}

int chat_core_create_channel(struct UTUN_INSTANCE* inst, const struct chat_channel_create* req) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized || !cc->db || db_async_can_write(inst->db_worker) || !inst->topo_groups || !req || memcmp(req->owner_node_id, inst->my_ed25519_pubkey, 32) ||
        strnlen(req->name, sizeof(req->name)) == sizeof(req->name) ||
        strnlen(req->group_type, sizeof(req->group_type)) == sizeof(req->group_type)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "channel create: invalid context/owner/name"); return -1;
    }
    if (!sqlite3_get_autocommit(cc->db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "channel create: external transaction rejected"); return -1;
    }
    uint8_t description[CHAT_CHANNEL_DESCRIPTION_MAX];
    const char* type = req->group_type[0] ? req->group_type : "chat";
    int bytes = typed_description(req->channel_id, req->name, type, req->owner_node_id,
        req->x25519_pubkey, req->ed25519_pubkey, description, sizeof(description));
    if (bytes < 0 || channel_creation_keys(req)) return -1;
    if (sc_ed25519_verify(req->ed25519_pubkey, description, (size_t)bytes, req->signature) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "channel create: invalid description signature ch=%s", req->channel_id); return -1;
    }
    if (topo_groups_find_channel(inst->topo_groups, req->channel_id)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "channel create: group ID already in use ch=%s", req->channel_id); return -1;
    }
    time_t now = ntp_time_get_seconds(inst);
    if (now <= 0) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "channel create: invalid join time ch=%s", req->channel_id); return -1; }
    uint64_t timestamp = (uint64_t)now;
    uint8_t join[64], root[64], update[64], body[MS_USERINFO_MAX + 73]; char profile[MS_USERINFO_MAX + 1];
    bytes = member_sync_build_join_msg(req->x25519_pubkey, req->ed25519_pubkey, inst->my_ed25519_pubkey,
        inst->my_keys.public_key, body, sizeof(body));
    uint8_t join_id[32];
    if (bytes < 0 || member_sync_join_id(body, join_id) || sc_ed25519_sign(inst->my_ed25519_privkey, body, (size_t)bytes, join) != SC_OK ||
        member_sync_sign_join(req->ed25519_privkey, req->channel_id, NULL, inst->my_keys.public_key, inst->my_ed25519_pubkey, join_id, timestamp, root) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "channel create: root JOIN/invitation signing failed ch=%s", req->channel_id); return -1;
    }
    if (chat_core_name_userinfo(inst->name, profile, sizeof(profile))) return -1;
    bytes = member_sync_build_update_msg(join_id, timestamp, profile, body, sizeof(body));
    if (bytes < 0 || sc_ed25519_sign(inst->my_ed25519_privkey, body, (size_t)bytes, update) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "channel create: root profile signing failed ch=%s", req->channel_id); return -1;
    }
    sqlite3_stmt* st = NULL; const char* stage = "begin";
    struct group_policy_update* admission = NULL;
    int rc = sqlite3_exec(cc->db, "BEGIN IMMEDIATE /* channel creation */", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "channel create: begin ch=%s rc=%d: %s", req->channel_id, rc, sqlite3_errmsg(cc->db)); return -1;
    }
    stage = "lookup channel";
    rc = sqlite3_prepare_v2(cc->db, "SELECT 1 FROM channels WHERE channel_id=?", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, req->channel_id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) { stage = "channel already exists"; rc = SQLITE_CONSTRAINT; goto error; }
    if (rc != SQLITE_DONE) goto error;
    stage = "lookup finalize"; rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto error;
    stage = "store description";
    char initial[GROUP_INFO_MAX + 1];
    if (group_info_initial_typed(req->name, type, initial) ||
        topo_node_sqlite_channel_put_description(cc->db, req->channel_id, initial, req->owner_node_id,
        req->x25519_pubkey, req->x25519_privkey, req->ed25519_pubkey, req->ed25519_privkey, req->signature)) {
        rc = SQLITE_ERROR; goto error;
    }
    stage = "content schema";
    if (group_protocol_schema(inst, req->channel_id)) { rc = SQLITE_ERROR; goto error; }
    stage = "store root member";
    int changes = member_sync_put(inst, req->channel_id, inst->my_keys.public_key, inst->my_ed25519_pubkey,
        join, timestamp, update, timestamp, profile, NULL, NULL, 0, 0, root);
    if (changes < 0 || !(changes & MS_APPLY_CHANGED)) { rc = SQLITE_ERROR; goto error; }
    stage = "verify root trust";
    if (group_policy_project(cc->db, inst->node_id, req->channel_id, &admission)) { rc = SQLITE_ERROR; goto error; }
    stage = "verify stored root";
    if (member_sync_verify_author(inst->topo_sqlite_db, req->channel_id, inst->my_ed25519_pubkey, &rc) != MS_AUTHOR_ACTIVE) {
        if (!rc) rc = SQLITE_CONSTRAINT;
        goto error;
    }
    stage = "commit"; rc = sqlite3_exec(cc->db, "COMMIT /* channel creation */", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto error;

    group_policy_publish(inst, admission);

    int ready = !chat_core_ensure_channel_ready(inst, req->channel_id);
    if (!ready) DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "channel create: committed ch=%s, runtime activation deferred; retry preparation", req->channel_id);
    member_sync_record_committed(inst, req->channel_id, inst->my_ed25519_pubkey, (unsigned)changes);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "channel created: ch=%s name=%s root=%016llx committed=1 runtime_ready=%d",
        req->channel_id, req->name, (unsigned long long)inst->node_id, ready);
    uint8_t event[65]; size_t length = strlen(req->channel_id); event[0] = (uint8_t)length;
    memcpy(event + 1, req->channel_id, length); chat_event_post(inst, CHAT_EVT_CHANNEL_UPDATED, event, (int)length + 1);
    return 0;
error:
    group_policy_discard(admission);
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "channel create: ch=%s stage=%s rc=%d (%s) db=%s",
        req->channel_id, stage, rc, sqlite3_errstr(rc), sqlite3_errmsg(cc->db));
    rc = sqlite3_finalize(st);
    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "channel create: cleanup finalize ch=%s rc=%d", req->channel_id, rc);
    if (!sqlite3_get_autocommit(cc->db)) {
        rc = sqlite3_exec(cc->db, "ROLLBACK", NULL, NULL, NULL);
        if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "channel create: rollback ch=%s rc=%d: %s",
            req->channel_id, rc, sqlite3_errmsg(cc->db));
        if (!sqlite3_get_autocommit(cc->db)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "channel create: transaction remains open ch=%s; requesting owner stop/DB close", req->channel_id);
            utun_instance_stop(inst);
        }
    }
    return -1;
}

void chat_core_create_channel_trampoline(void* arg) {
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "chat_core: TRAMPOLINE invoked arg=%p", arg);
    struct chat_channel_create* req = (struct chat_channel_create*)arg;
    if (!req) return;
    chat_channel_create_submit(req->inst, req, NULL, NULL);
    OPENSSL_cleanse(req, sizeof(*req));
    u_free(req);
}

void chat_core_create_channel_auto_trampoline(void* arg) {
    struct chat_create_auto_arg* a = (struct chat_create_auto_arg*)arg;
    if (!a) return;
    chat_channel_create_auto_submit(a->inst, a->name, a->group_type[0] ? a->group_type : "chat", NULL, NULL);
    u_free(arg);
}

int chat_channel_generate_keys(uint8_t x[32], uint8_t x_private[32], uint8_t ed[32], uint8_t seed[32]) {
    if (sc_identity_generate(x, x_private, ed, seed) != SC_OK) return -1;
    if (memcmp(ed + 8, (uint8_t[24]){0}, 24)) return 0;
    OPENSSL_cleanse(x, 32); OPENSSL_cleanse(x_private, 32); OPENSSL_cleanse(ed, 32); OPENSSL_cleanse(seed, 32);
    DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "generated group identity falls in reserved address range"); return -1;
}

uint64_t chat_core_create_channel_auto(struct UTUN_INSTANCE* inst, const char* name) {
    return group_core_create_auto(inst, name, "chat");
}

uint64_t group_core_create_auto(struct UTUN_INSTANCE* inst, const char* name, const char* type) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!type || (strcmp(type, "chat") && strcmp(type, "kanban")) ||
        !cc || !cc->initialized || !cc->db || !inst->topo_groups || !name || strnlen(name, 128) == 128) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "channel auto create: invalid context/name or name exceeds127 bytes"); return 0;
    }
    if (!sqlite3_get_autocommit(cc->db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "channel auto create: external transaction rejected before key generation"); return 0;
    }
    struct chat_channel_create req = {.inst = inst};
    memcpy(req.owner_node_id, inst->my_ed25519_pubkey, 32);
    strcpy(req.group_type, type);
    uint8_t description[CHAT_CHANNEL_DESCRIPTION_MAX]; uint64_t group_id = 0; int result = -1;
    if (chat_channel_generate_keys(req.x25519_pubkey, req.x25519_privkey, req.ed25519_pubkey, req.ed25519_privkey)) goto done;
    sc_public_key_hex(req.ed25519_pubkey, req.channel_id);
    memcpy(req.name, name, strlen(name) + 1);
    int bytes = typed_description(req.channel_id, req.name, type, req.owner_node_id,
        req.x25519_pubkey, req.ed25519_pubkey, description, sizeof(description));
    if (bytes < 0 || sc_ed25519_sign(req.ed25519_privkey, description, (size_t)bytes, req.signature) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "channel auto create: description signing failed ch=%s", req.channel_id); goto done;
    }
    result = chat_core_create_channel(inst, &req);
    group_id = topo_groups_channel_slot(inst->topo_groups, req.channel_id);
done:
    OPENSSL_cleanse(&req, sizeof(req));
    return result == 0 ? group_id : 0;
}

/* ─── управление подключением к каналу (выбор в GUI) ─── */

struct channel_connect_job { struct UTUN_INSTANCE* inst; char channel[65]; uint8_t peer[32]; int manual; };
static void channel_connected_prepared(void* data, const struct chat_channel_result* result) {
    struct channel_connect_job* job = data;
    if (!result->status) {
        if (job->manual) chat_core_connect_node(job->inst, job->channel, job->peer);
        else chat_core_connect_channel(job->inst, job->channel);
    } else DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "channel connect preparation failed ch=%s peer=%s rc=%d",
        job->channel, sc_public_key_hex(job->manual ? job->peer : NULL, (char[65]){0}), result->sqlite_code);
    u_free(job);
}
static int channel_connect_prepare(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t* peer) {
    if (!db_async_can_write(inst->db_worker) || topo_groups_find_channel(inst->topo_groups, channel)) return 0;
    struct channel_connect_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "channel connect continuation allocation ch=%s", channel); return 1; }
    job->inst = inst; snprintf(job->channel, sizeof(job->channel), "%s", channel);
    if (peer) { memcpy(job->peer, peer, 32); job->manual = 1; }
    chat_channel_prepare_submit(inst, channel, channel_connected_prepared, job); return 1;
}

void chat_core_connect_channel(struct UTUN_INSTANCE* inst, const char* ch_id) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    DEBUG_TRACE(DEBUG_CATEGORY_CHAT_SYNC, "%s: connect_channel called ch_id=%s initialized=%d", CC_ID, ch_id ? ch_id : "(null)", cc ? cc->initialized : 0);
    if (!cc || !cc->initialized || !ch_id || !ch_id[0]) { DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "%s: connect_channel — skip (not ready)", CC_ID); return; }
    if (!inst->topo_groups) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: connect_channel — no topo_groups", CC_ID); return; }

    if (!chat_channel_valid(ch_id)) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: connect_channel — invalid ch_id=%s", CC_ID, ch_id); return; }

    if (channel_connect_prepare(inst, ch_id, NULL) || chat_core_ensure_channel_ready(inst, ch_id)) return;

    struct TOPO_GROUP* group = topo_groups_find_channel(inst->topo_groups, ch_id);
    if (!group) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "connect_channel: group not found ch=%s", ch_id); return; }
    uint64_t gid = group->group_id;

    if (group->connect) {
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "%s: connect_channel — already in progress ch=%s active=%d",
                   CC_ID, ch_id, topo_group_connect_active_count(group));
        return;
    }

    if (!chat_setting_get_int(inst, "group_autoconnect", 1)) {
        DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "%s: connect_channel ch=%s — group_autoconnect disabled, skip auto-connect loop",
                   CC_ID, ch_id);
        return;
    }

    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "%s: connect_channel ch=%s gid=%016llx group_type=%d — starting connect_init", CC_ID, ch_id, (unsigned long long)gid, group->group_type);
    topo_group_connect_init(group);
}

void chat_core_connect_channel_trampoline(void* arg) {
    struct chat_create_auto_arg* a = (struct chat_create_auto_arg*)arg;
    DEBUG_TRACE(DEBUG_CATEGORY_CHAT_SYNC, "%s: connect_channel_trampoline ch_id=%s", CC_ID, a ? a->name : "(null)");
    if (!a) return;
    chat_core_connect_channel(a->inst, a->name);
    u_free(a);
}

/* ─── однократное подключение к конкретному узлу канала (GUI → uasync) ─── */

void chat_core_connect_node(struct UTUN_INSTANCE* inst, const char* ch_id, const uint8_t ed[32]) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized || !chat_channel_valid(ch_id) || !ed) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "%s: connect_node — skip (not ready or invalid args)", CC_ID);
        return;
    }
    if (!memcmp(ed, inst->my_ed25519_pubkey, 32)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "%s: connect_node requested self ch=%s", CC_ID, ch_id);
        return;
    }

    if (!inst->topo_groups || channel_connect_prepare(inst, ch_id, ed) ||
        chat_core_ensure_channel_ready(inst, ch_id)) return;

    struct TOPO_GROUP* group = topo_groups_find_channel(inst->topo_groups, ch_id);
    if (!group) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: connect_node — group not found ch=%s", CC_ID, ch_id);
        return;
    }

    topo_group_connect_node_once(group, ed);
}

void chat_core_connect_node_trampoline(void* arg) {
    struct chat_connect_node_req* r = (struct chat_connect_node_req*)arg;
    if (!r) return;
    chat_core_connect_node(r->inst, r->ch_id, r->ed);
    u_free(r);
}

/* ─── применение настройки group_autoconnect ко всем CHAT-группам ─── */

void chat_core_apply_group_autoconnect(struct UTUN_INSTANCE* inst) {
    int enabled = chat_setting_get_int(inst, "group_autoconnect", 1);
    if (!inst || !inst->topo_groups || !inst->topo_groups->group_list) return;

    struct ll_entry* e = inst->topo_groups->group_list->head;
    while (e) {
        struct TOPO_GROUP* g = (struct TOPO_GROUP*)e;
        if (g->group_type == TOPO_GROUP_TYPE_CHAT && !g->stopping) {
            if (enabled && !g->connect) {
                topo_group_connect_init(g);
            } else if (!enabled && g->connect) {
                DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "%s: group_autoconnect OFF — stop auto-connect ch=%s", CC_ID, g->channel_id);
                topo_group_connect_destroy(g);
            }
        }
        e = e->next;
    }
}

/* ─── локальное удаление канала ─── */

struct channel_delete_job {
    char channel[65]; uint64_t self;
    struct UTUN_INSTANCE* inst; /* Только guard/done: worker использует channel/self. */
    struct TOPO_GROUP* reservation; /* Только сравнение с вновь найденным живым group, без dereference после callbacks. */
    uint64_t generation, group_generation;
    unsigned holds;
    int temporary, committed;
};

static int channel_delete_current(struct channel_delete_job* job) {
    return !job->inst->destroying && job->inst->group_started && chat_core_is_initialized(job->inst) &&
        chat_core_generation(job->inst) == job->generation && job->inst->topo_groups;
}
static void channel_delete_release(struct channel_delete_job* job) {
    if (--job->holds) return;
    if (channel_delete_current(job)) {
        struct TOPO_GROUP* group = topo_groups_find_channel(job->inst->topo_groups, job->channel);
        if (group == job->reservation && group->generation == job->group_generation) {
            if (job->committed || job->temporary) topo_groups_remove_group(job->inst->topo_groups, group->group_id);
            else group->stopping = 0;
        }
    }
    u_free(job);
}
static int channel_delete_guard(void* data) {
    struct channel_delete_job* job = data;
    if (!channel_delete_current(job)) return 0;
    struct TOPO_GROUP* group = topo_groups_find_channel(job->inst->topo_groups, job->channel);
    return group == job->reservation && group->generation == job->group_generation && group->stopping;
}
static int channel_delete_work(sqlite3* db, void* data) {
    struct channel_delete_job* job = data;
    int rc = chat_attachment_forget_write(db, job->channel, NULL); if (rc) return rc;
    return topo_node_sqlite_channel_delete_write(db, job->channel);
}
static void channel_delete_files_done(void* data, const struct chat_attachment_forget_result* result) {
    struct channel_delete_job* job = data;
    if (result->status) DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "channel removed with deferred file cleanup ch=%s rc=%d; durable retry retained",
        job->channel, result->sqlite_code);
    else DEBUG_INFO(DEBUG_CATEGORY_GROUP, "channel deletion cleanup complete ch=%s", job->channel);
    channel_delete_release(job);
}
static void channel_delete_done(void* data, const struct db_async_result* result) {
    struct channel_delete_job* job = data;
    if (result->status) {
        DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "channel deletion rejected ch=%s status=%d rc=%d; saved group retained: %s",
            job->channel, result->status, result->sqlite_code, result->message); return;
    }
    job->committed = 1;
    if (channel_delete_current(job)) {
        struct UTUN_INSTANCE* inst = job->inst; uint64_t group_id = job->reservation->group_id;
        chat_member_forget_channel(inst, job->channel); group_policy_forget(inst, job->channel);
        /* Reservation остаётся stopping до конца I/O, чтобы rejoin не воскресил удаляемые paths. */
        media_delivery_cancel_channel(inst, group_id); member_files_cancel_group(inst, group_id);
        group_avatar_cancel_group(inst, group_id); member_sync_cancel_channel(inst, job->channel);
        struct TOPO_GROUP* group = topo_groups_find(inst->topo_groups, group_id);
        if (group == job->reservation && group->connect) topo_group_connect_destroy(group);
        struct DB_SYNC_INSTANCE* history = si_find(inst, job->channel); if (history) db_sync_instance_remove(history);
        chat_sync_on_channel_deleted(inst, job->channel, topo_group_wire_id(group));
        if (channel_delete_current(job)) {
            uint8_t event[65]; size_t size = strlen(job->channel); event[0] = (uint8_t)size;
            memcpy(event + 1, job->channel, size); chat_event_post(inst, CHAT_EVT_CHANNEL_DELETED, event, (int)size + 1);
        }
    }
    job->holds++; chat_attachment_forget_submit(job->inst, job->channel, channel_delete_files_done, job);
}
static void channel_delete_cleanup(void* data) { channel_delete_release(data); }
static void channel_delete_submit(struct UTUN_INSTANCE* inst, const char* channel) {
    if (!chat_channel_valid(channel) || !chat_core_is_initialized(inst) || !inst->topo_groups) {
        DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "channel delete invalid runtime/ID"); return;
    }
    struct TOPO_GROUP* group = topo_groups_find_channel(inst->topo_groups, channel);
    if (group && (group->group_type != TOPO_GROUP_TYPE_CHAT || group->stopping)) {
        DEBUG_WARN(DEBUG_CATEGORY_GROUP, "channel deletion already pending or wrong group type ch=%s", channel); return;
    }
    if (chat_media_defer_channel_delete(inst, channel)) return;
    struct channel_delete_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "channel deletion allocation ch=%s", channel); return; }
    if (!group) {
        group = topo_groups_create_channel(inst->topo_groups, channel); job->temporary = 1;
        if (!group) { DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "channel deletion reservation failed ch=%s", channel); u_free(job); return; }
    }
    strcpy(job->channel, channel); job->self = inst->node_id; job->inst = inst;
    job->generation = chat_core_generation(inst); job->group_generation = group->generation;
    job->reservation = group; job->holds = 1; group->stopping = 1;
    db_async_run(inst->db_worker, &(struct db_async_task){.name="channel-delete", .mode=DB_ASYNC_WRITE,
        .guard=channel_delete_guard, .work=channel_delete_work, .done=channel_delete_done, .cleanup=channel_delete_cleanup, .data=job});
}

void chat_core_delete_channel(struct UTUN_INSTANCE* inst, const char* ch_id) {
    if (inst && db_async_can_write(inst->db_worker)) { channel_delete_submit(inst, ch_id); return; }
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized || !cc->db || !inst) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: delete_channel — not initialized", CC_ID);
        return;
    }
    if (!chat_channel_valid(ch_id)) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: delete_channel — invalid channel ID", CC_ID); return; }
    if (!sqlite3_get_autocommit(cc->db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: delete_channel rejected inside external transaction ch=%s", CC_ID, ch_id); return;
    }
    if (chat_media_defer_channel_delete(inst, ch_id)) return;

    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "%s: delete_channel BEGIN ch=%s", CC_ID, ch_id);
    uint64_t gid = topo_groups_channel_slot(inst->topo_groups, ch_id);

    /* До commit не трогаем работающую группу и durable очереди: SQL-отказ сохраняет канал целиком. */
    if (topo_node_sqlite_channel_delete(cc->db, ch_id)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "channel metadata deletion failed ch=%s; group, requests and files preserved", ch_id);
        if (!sqlite3_get_autocommit(cc->db)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "channel delete left transaction open ch=%s; requesting owner stop", ch_id);
            chat_attachment_quiesce(inst); utun_instance_stop(inst);
        }
        return;
    }
    chat_member_forget_channel(inst, ch_id);
    group_policy_forget(inst, ch_id);
    struct TOPO_GROUP* group = topo_groups_find(inst->topo_groups, gid);
    if (group) group->stopping = 1; /* Cleanup callbacks уже не могут начать новую работу или читать удалённые peers. */
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "channel deletion committed ch=%s; cancelling runtime work", ch_id);

    /* Отмена медиа-загрузок/релэев/суперузлов канала (держат conn_mgr handle группы). */
    media_delivery_cancel_channel(inst, gid);
    member_files_cancel_group(inst, gid);
    group_avatar_cancel_group(inst, gid);

    /* Снять merkle-сессии этого namespace. */
    member_sync_cancel_channel(inst, ch_id);

    chat_sync_on_channel_deleted(inst, ch_id, topo_group_wire_id(group));

    struct DB_SYNC_INSTANCE* history = si_find(inst, ch_id);
    if (history) db_sync_instance_remove(history);
    if (group) topo_groups_remove_group(inst->topo_groups, gid);

    /* Удалить файлы только после commit; I/O-отказ не восстанавливает удалённый канал. */
    int media_error = chat_attachment_delete_channel(inst, ch_id);
    char media_base[MEDIA_BASE_MAX];
    if (media_index_get_base(cc->db, media_base) || media_index_delete_channel(cc->db, ch_id, media_base)) media_error = 1;
    if (media_error) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "channel=%s removed with incomplete media cleanup; retry required", ch_id);

    /* Уведомить GUI после очистки runtime. */
    {
        uint8_t evt[66]; uint8_t cl = (uint8_t)strlen(ch_id);
        evt[0] = cl; memcpy(evt + 1, ch_id, cl);
        chat_event_post(inst, CHAT_EVT_CHANNEL_DELETED, evt, 1 + cl);
    }

    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "%s: delete_channel DONE ch=%s gid=0x%016llx media_cleanup=%s",
               CC_ID, ch_id, (unsigned long long)gid, media_error ? "incomplete" : "complete");
}

void chat_core_delete_channel_trampoline(void* arg) {
    struct chat_delete_channel_req* req = (struct chat_delete_channel_req*)arg;
    if (!req) return;
    chat_core_delete_channel(req->inst, req->ch_id);
    u_free(req);
}

/* Frozen SQL-входы и staged результаты. Runtime ниже читают только guard/done, worker instance не использует. */
struct channel_job {
    struct UTUN_INSTANCE* inst;
    uint64_t generation;
    struct chat_channel_create request;
    struct member_sync_write_context writer;
    uint8_t self_x[32];
    char profile[MS_USERINFO_MAX + 1];
    struct group_protocol_prepared prepared;
    struct group_policy_update* admission;
    struct group_policy_cache* access;
    int create, content, changes;
    chat_channel_done_fn done;
    void* arg;
};
static int channel_job_current(void* arg) {
    struct channel_job* job = arg;
    if (job->inst->destroying || (db_async_can_write(job->inst->db_worker) && !job->inst->group_started) ||
        !job->generation || chat_core_generation(job->inst) != job->generation) return 0;
    struct TOPO_GROUP* group = topo_groups_find_channel(job->inst->topo_groups, job->request.channel_id);
    if (group && (group->stopping || group->group_type != TOPO_GROUP_TYPE_CHAT)) return 0;
    job->content = job->inst->chat_content_started; return 1;
}
static int channel_job_guard(void* arg) {
    struct channel_job* job = arg;
    if (!channel_job_current(job)) return 0;
    return !job->create || !topo_groups_find_channel(job->inst->topo_groups, job->request.channel_id);
}
static int channel_create_write(sqlite3* db, struct channel_job* job) {
    struct chat_channel_create* req = &job->request;
    uint8_t body[MS_ADM_TAGS_MAX + 128], join[64], tree[64], update[64]; char initial[GROUP_INFO_MAX + 1];
    const char* type = req->group_type[0] ? req->group_type : "chat";
    int bytes = typed_description(req->channel_id, req->name, type, req->owner_node_id,
        req->x25519_pubkey, req->ed25519_pubkey, body, sizeof(body));
    if (bytes < 0) return SQLITE_MISMATCH;
    int rc = channel_creation_keys(req); if (rc) return rc;
    sc_status_t checked = sc_ed25519_verify(req->ed25519_pubkey, body, (size_t)bytes, req->signature);
    if (checked != SC_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_GROUP, "group creation signature check failed ch=%s rc=%d", req->channel_id, checked);
        return checked == SC_ERR_AUTH_FAILED ? SQLITE_AUTH : SQLITE_ERROR;
    }
    sqlite3_stmt* st = NULL; rc = sqlite3_prepare_v2(db, "SELECT 1 FROM channels WHERE channel_id=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, req->channel_id, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) rc = SQLITE_CONSTRAINT;
    else if (rc == SQLITE_DONE) rc = SQLITE_OK;
    int final = sqlite3_finalize(st); if (!rc) rc = final; if (rc) return rc;
    if (group_info_initial_typed(req->name, type, initial)) return SQLITE_ERROR;
    rc = topo_node_sqlite_channel_put_description_write(db,
        req->channel_id, initial, req->owner_node_id, req->x25519_pubkey, req->x25519_privkey,
        req->ed25519_pubkey, req->ed25519_privkey, req->signature); if (rc) return rc;
    rc = group_protocol_prepare_write(db, req->channel_id, job->content, &job->prepared); if (rc) return rc;
    uint64_t timestamp = job->writer.now / 1000000;
    if (!timestamp || timestamp > INT64_MAX) return SQLITE_RANGE;
    bytes = member_sync_build_join_msg(req->x25519_pubkey, req->ed25519_pubkey, job->writer.public_key,
        job->self_x, body, sizeof(body));
    uint8_t join_id[32];
    if (bytes < 0 || member_sync_join_id(body, join_id) || sc_ed25519_sign(job->writer.private_key, body, (size_t)bytes, join) != SC_OK ||
        member_sync_sign_join(req->ed25519_privkey, req->channel_id, NULL, job->self_x, job->writer.public_key, join_id, timestamp, tree)) return SQLITE_ERROR;
    bytes = member_sync_build_update_msg(join_id, timestamp, job->profile, body, sizeof(body));
    if (bytes < 0 || sc_ed25519_sign(job->writer.private_key, body, (size_t)bytes, update) != SC_OK) return SQLITE_ERROR;
    struct ms_member_rec member = {.node_id=job->writer.self, .x25519=job->self_x, .ed25519=job->writer.public_key,
        .join_sig=join, .join_ts=timestamp, .update_sig=update, .update_ts=timestamp, .userinfo=job->profile, .signature=tree};
    job->changes = member_sync_record_write(db, &job->writer, req->channel_id, NULL, &member);
    if (job->changes < 0) return job->writer.sqlite_code ? job->writer.sqlite_code : SQLITE_AUTH;
    rc = group_policy_project(db, job->writer.self, req->channel_id, &job->admission);
    return rc < 0 ? -rc : rc;
}
static int channel_job_work(sqlite3* db, void* arg) {
    struct channel_job* job = arg; int rc = job->create ? channel_create_write(db, job) : SQLITE_OK;
    if (!rc && !job->create) rc = group_protocol_prepare_write(db, job->request.channel_id, job->content, &job->prepared);
    if (!rc && !job->create) rc = group_policy_snapshot_read(db, job->request.channel_id, &job->access);
    if (!rc && job->prepared.kind == GROUP_PROTOCOL_KANBAN)
        rc = kanban_prepare_group_write(db, job->request.channel_id, job->writer.public_key);
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "channel WRITE failed ch=%s create=%d rc=%d: %s",
        job->request.channel_id, job->create, rc, sqlite3_errstr(rc));
    return rc;
}
static void channel_job_done(void* arg, const struct db_async_result* outcome) {
    struct channel_job* job = arg; struct chat_channel_result result = {.status=-1, .sqlite_code=outcome->sqlite_code, .committed=!outcome->status};
    if (!outcome->status) {
        strcpy(result.channel, job->request.channel_id);
        if (channel_job_current(job)) {
            if (job->access) { group_policy_snapshot_publish(job->inst, job->access); job->access = NULL; }
            /* Как при legacy create: cached допуск готов до публикации новой topology. До этого у канала нет runtime операций. */
            if (job->admission) { struct group_policy_update* update = job->admission; job->admission = NULL; group_policy_publish(job->inst, update); }
            if (channel_job_current(job) && !chat_channel_activate(job->inst, job->request.channel_id, &job->prepared)) {
                result.status = 0;
                if (job->create && channel_job_current(job)) {
                    member_sync_notify_props(job->inst, job->request.channel_id, job->writer.public_key);
                    uint8_t event[65]; size_t len = strlen(job->request.channel_id); event[0] = (uint8_t)len;
                    memcpy(event + 1, job->request.channel_id, len); chat_event_post(job->inst, CHAT_EVT_CHANNEL_UPDATED, event, (int)len + 1);
                }
            } else result.sqlite_code = SQLITE_ERROR;
        } else result.sqlite_code = SQLITE_ABORT;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_GROUP, "channel complete ch=%s create=%d committed=%d status=%d rc=%d",
        job->request.channel_id, job->create, result.committed, result.status, result.sqlite_code);
    if (job->done) job->done(job->arg, &result);
}
static void channel_job_cleanup(void* arg) {
    struct channel_job* job = arg; group_policy_discard(job->admission); group_policy_snapshot_free(job->access);
    OPENSSL_cleanse(&job->request, sizeof(job->request)); OPENSSL_cleanse(&job->writer, sizeof(job->writer)); u_free(job);
}
static void channel_dispatch(struct channel_job* job) {
    if (db_async_can_write(job->inst->db_worker)) {
        db_async_run(job->inst->db_worker, &(struct db_async_task){.name="channel-write", .mode=DB_ASYNC_WRITE,
            .guard=channel_job_guard, .work=channel_job_work, .data=job, .done=channel_job_done, .cleanup=channel_job_cleanup}); return;
    }
    sqlite3* db = chat_core_get_db(job->inst); const char* file = db ? sqlite3_db_filename(db, "main") : NULL;
    int owns = 0, rc = !db || (!job->inst->db_worker && (!file || *file)) || !sqlite3_get_autocommit(db) ? SQLITE_MISUSE : SQLITE_OK;
    if (!rc) rc = channel_job_guard(job) ? sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) : SQLITE_ABORT;
    if (!rc) { owns = 1; rc = channel_job_work(db, job); }
    if (!rc) rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    if (rc && owns && !sqlite3_get_autocommit(db)) {
        int rollback = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        if (rollback) DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "channel rollback failed ch=%s rc=%d", job->request.channel_id, rollback);
        if (!sqlite3_get_autocommit(db)) utun_instance_stop(job->inst);
    }
    struct db_async_result outcome = {.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK, .sqlite_code=rc};
    channel_job_done(job, &outcome); channel_job_cleanup(job);
}
void chat_channel_create_submit(struct UTUN_INSTANCE* inst, const struct chat_channel_create* req, chat_channel_done_fn done, void* arg) {
    struct chat_channel_result failed = {.status=-1, .sqlite_code=SQLITE_MISUSE};
    if (!inst || inst->destroying || !chat_core_is_initialized(inst) || !req || !chat_channel_valid(req->channel_id) ||
        memcmp(req->owner_node_id, inst->my_ed25519_pubkey, 32) || !memchr(req->name, 0, sizeof(req->name)) ||
        !memchr(req->group_type, 0, sizeof(req->group_type))) {
        DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "channel create submit invalid request/owner"); if (done) done(arg, &failed); return;
    }
    struct channel_job* job = u_calloc(1, sizeof(*job));
    if (!job) { failed.sqlite_code = SQLITE_NOMEM; DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "channel create allocation failed"); if (done) done(arg, &failed); return; }
    job->inst = inst; job->generation = chat_core_generation(inst); job->request = *req; job->request.inst = NULL;
    job->create = 1; job->done = done; job->arg = arg;
    job->writer.self = inst->node_id; job->writer.now = ntp_time_get_us(inst);
    memcpy(job->writer.public_key, inst->my_ed25519_pubkey, 32); memcpy(job->writer.private_key, inst->my_ed25519_privkey, 32);
    memcpy(job->self_x, inst->my_keys.public_key, 32);
    if (chat_core_name_userinfo(inst->name, job->profile, sizeof(job->profile))) {
        channel_job_cleanup(job); failed.sqlite_code = SQLITE_TOOBIG; if (done) done(arg, &failed); return;
    }
    channel_dispatch(job);
}
void chat_channel_prepare_submit(struct UTUN_INSTANCE* inst, const char* channel, chat_channel_done_fn done, void* arg) {
    struct chat_channel_result failed = {.status=-1, .sqlite_code=SQLITE_MISUSE};
    if (!inst || inst->destroying || !chat_core_is_initialized(inst) || !chat_channel_valid(channel)) {
        DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "channel prepare submit invalid owner/channel"); if (done) done(arg, &failed); return;
    }
    struct channel_job* job = u_calloc(1, sizeof(*job));
    if (!job) { failed.sqlite_code = SQLITE_NOMEM; DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "channel prepare allocation failed"); if (done) done(arg, &failed); return; }
    job->inst = inst; job->generation = chat_core_generation(inst); job->done = done; job->arg = arg;
    memcpy(job->writer.public_key, inst->my_ed25519_pubkey, sizeof(job->writer.public_key));
    snprintf(job->request.channel_id, sizeof(job->request.channel_id), "%s", channel); channel_dispatch(job);
}
void chat_channel_create_auto_submit(struct UTUN_INSTANCE* inst, const char* name, const char* type, chat_channel_done_fn done, void* arg) {
    struct chat_channel_create req = {.inst=inst};
    if (inst) memcpy(req.owner_node_id, inst->my_ed25519_pubkey, 32);
    struct chat_channel_result failed = {.status=-1, .sqlite_code=SQLITE_MISUSE};
    if (!name || strnlen(name, sizeof(req.name)) == sizeof(req.name) || !type || (strcmp(type, "chat") && strcmp(type, "kanban"))) {
        DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "channel auto submit invalid name/type"); if (done) done(arg, &failed); return;
    }
    if (chat_channel_generate_keys(req.x25519_pubkey, req.x25519_privkey, req.ed25519_pubkey, req.ed25519_privkey)) {
        failed.sqlite_code = SQLITE_ERROR; goto fail;
    }
    sc_public_key_hex(req.ed25519_pubkey, req.channel_id); strcpy(req.name, name); strcpy(req.group_type, type);
    uint8_t body[CHAT_CHANNEL_DESCRIPTION_MAX];
    int bytes = typed_description(req.channel_id, req.name, type, req.owner_node_id, req.x25519_pubkey, req.ed25519_pubkey, body, sizeof(body));
    if (bytes < 0 || sc_ed25519_sign(req.ed25519_privkey, body, (size_t)bytes, req.signature) != SC_OK) { failed.sqlite_code = SQLITE_ERROR; goto fail; }
    chat_channel_create_submit(inst, &req, done, arg); OPENSSL_cleanse(&req, sizeof(req)); return;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "channel auto submit key/description generation failed rc=%d", failed.sqlite_code);
    OPENSSL_cleanse(&req, sizeof(req)); if (done) done(arg, &failed);
}
