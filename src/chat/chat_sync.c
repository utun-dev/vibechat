#include "chat_sync.h"
#include "../db_changes.h"
#include "../group/group_protocol.h"
#include "chat_core.h"
#include "chat_core_priv.h"
#include "chat_member.h"
#include "chat_setting.h"
#include "chat_state.h"
#include "chat_event.h"
#include "chat_admin.h"
#include "chat_join.h"
#include "group_policy.h"
#include "group_info.h"
#include "chat_join_port.h"
#include "chat_sync_sql.h"
#include "chat_channel.h"
#include "invite_link.h"
#include "invite_build.h"
#include <openssl/crypto.h>
#include "group_avatar.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../routing_layer/topo_group.h"
#include "../routing_layer/topo_group_connect.h"
#include "../../lib/json_flat.h"
#include "member_sync.h"
#include "merkle_sync.h"
#include "chat_feedback.h"

#include "../utun_instance.h"
#include "../transport_layer/etcp_api.h"
#include "../transport_layer/etcp.h"
#include "../transport_layer/node_conn_direct.h"
#include "../transport_layer/secure_channel.h"
#include "../ntp_time.h"
#include "../../lib/u_async.h"
#include "../../lib/ll_queue.h"
#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include "../../lib/db_async.h"
#include "../../lib/platform_compat.h"
#ifdef UTUN_HAVE_STANDBY
#include "standby.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <openssl/rand.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

static inline struct chat_sync* cs_of(struct UTUN_INSTANCE* inst) {
    return inst ? inst->chat_sync : NULL;
}

struct channel_cache {
    char      channel_id[65];
    uint8_t (*peers)[32];
    int       peer_count;
};
struct join_recovery {
    struct join_recovery* next;
    struct InviteData hints; /* Только адреса, REALITY и публичный ключ C. */
    char channel[65];
    struct chat_join_path path; /* Текущий раунд с A через закреплённый exit. */
    uint64_t carrier_handle, carrier_deadline;
    int carrier_ready;
    struct NODE_CONN_DIRECT* transport;
    uint64_t retry_at, request; /* request действует в текущем процессе, в SQLite не хранится. */
    unsigned retry_delay;
    int running;
};

struct chat_sync_catalog {
    struct channel_cache* channels;
    int count;
    char (*notifications)[65]; /* Собственные имена, остаются валидными при удалении канала из callback. */
    struct join_recovery* recovery;
};

/* Только отложенный запрос запуска; состоянием активного обмена владеет Merkle. */
struct pending_sync {
    struct pending_sync* next;
    struct NODE_CONN_DIRECT* transport; /* Удерживает UP до передачи активному раунду Merkle или отмены. */
    uint64_t channel_id;
    const char* reason;          /* статическая строка вызывающего обработчика */
    uint8_t in_batch;            /* задание присутствовало в начале текущего прохода таймера */
};

/* Короткая очередь подтверждённых транспортом кадров на время стартового ожидания часов. */
struct cs_wait_frame {
    struct cs_wait_frame* next;
    struct chat_join_route route;
    size_t size;
    uint8_t data[];
};

struct chat_sync {
    struct chat_join_port* port;
    struct UTUN_INSTANCE* inst;
    struct channel_cache* channels;
    int      channel_count;
    void*    refresh_timer;
    void*    refresh_wait;        /* standby_wait handle (Android) */
    void*    info_req_timer;
    void*    join_timer;
    void*    sync_timer;
    struct pending_sync* sync_pending;
    void* recovery_timer;
    struct join_recovery* recovery;
    uint8_t  sync_draining;       /* новые задания во время flush обслужит следующий проход */
    void*    invite_send_timer;   /* timer waiting for joiner to respond to CHANNEL_INVITE */
    uint8_t  initialized;
    uint64_t generation, refresh_revision, carrier_generation;
    int refresh_running;
    struct cs_wait_frame* time_wait_frames;
    unsigned time_wait_count; /* максимум 32 кадра, каждый не больше CHAT_JOIN_WIRE_MAX */
    uint8_t direct_bound, admin_bound, join_bound; /* Только успешно установленные service bindings снимаются в destroy. */
    struct {
        uint64_t channel, peer, key, request, attempt;
        uint8_t expected_group[32]; /* Ed25519 клиента SERVER_JOIN или подписанного push-приглашения. */
        uint8_t inviter_ed[32]; /* Полный Ed25519 A из ссылки или прямого транспорта. */
        uint8_t inviter_pubkey[32]; /* X25519 A из ссылки/приглашения; адресат E2E JOIN. */
        uint8_t inviter, phase; /* 0=idle,1=connect,2=info,3=ready,4=offer,5=prepare,6=offer READ,7=accept WRITE. */
        uint8_t carrier_ready; /* carrier подтвердил ключ; JOIN ещё может ждать стартовую синхронизацию часов */
        struct chat_join_route route;
        struct NODE_CONN_DIRECT* handle;
        void* transport_timer;
        uint8_t server_offer; /* отправляем серверу обычное приглашение после проверки add_key */
        uint8_t add_key[INVITE_ADD_KEY_SIZE]; /* только исходящий SERVER_JOIN_REQ */
        char offer_link[INVITE_LINK_MAX]; /* подтверждённое обычное приглашение, одинаковое при повторах */
        struct chat_invite_build* offer_build;
        void* operation_timer; /* общий deadline серверной операции, сохраняется при reconnect */
        uint64_t origin_operation; /* входящий запрос сервера: ответить peer после обычного JOIN */
        struct InviteData recovery_hints;
        struct cs_accept_job* accept_retry; /* WRITE проверенного INFO между подачами в SQLite. */
        char recovery_channel[65]; /* Известен до активации runtime-группы после COMMIT. */
    } join;
    struct cs_join_write* ready_replies; /* Только отложенные ответы A после COMMIT. */
    /* Pending incoming invites (для ask/deny) */
    struct cs_invitation {
        uint64_t join_key;
        uint8_t group_ed[32]; /* Подтверждённый Ed25519 группы. */
        uint8_t inviter[32], inviter_ed[32]; /* Оба публичных ключа подтверждённого транспорта. */
    } pending_inv_in[4];
    int      pending_inv_in_count;
};

#define CS_ID "chat_sync"
#define CS_INFO_REQ_TIMEOUT_MS  (5000 + NTP_STARTUP_WAIT_SEC * 1000) /* Пир может ещё ждать NTP при уже готовом транспорте. */
#define CS_JOIN_TIMEOUT_MS      8000
#define CS_INVITE_SEND_TIMEOUT_MS (3000 + NTP_STARTUP_WAIT_SEC * 1000)
#define CS_SYNC_INTERVAL_MS     1000
#define CS_CARRIER_TIMEOUT_MS  20000 /* GATT discovery и фрагментация JOIN при минимальном ATT MTU. */
#define CS_ACCEPT_WRITE_TIMEOUT_MS 30000
#define CS_MEMBERS_TIMEOUT_MS  30000
enum { CS_JOIN_WAIT_READY = 3, CS_JOIN_ACCEPT_WRITE = 7, CS_JOIN_WAIT_MEMBERS, CS_JOIN_CANCEL_PENDING };

static void cs_info_req_timeout_cb(void* arg);
static void cs_join_finish(struct chat_sync* cs, int result);
static void cs_recovery_arm(struct chat_sync* cs);
static void cs_recovery_wake(struct chat_sync* cs, struct join_recovery* item);
static void cs_join_clear_ready(struct chat_sync* cs);
static void cs_recovery_remove(struct chat_sync* cs, const char* channel);
static void cs_recovery_carrier_closed(struct chat_sync* cs, uint64_t handle);
static void cs_recovery_backoff(struct join_recovery* item);
static void cs_accept_drop_retry(struct chat_sync* cs);
static int cs_recovery_finish(struct chat_sync* cs, const char* channel, uint64_t attempt, int result);
static struct join_recovery* cs_recovery_find(struct chat_sync* cs, const char* channel);
static void cs_join_begin_members(struct chat_sync* cs, const char* reason, int ready);
static int cs_server_join_reply(struct chat_sync* cs, const uint8_t group_ed[32], const struct chat_join_route* route, uint64_t operation, int result);

static void cs_join_connect_timeout(void* arg) {
    struct chat_sync* cs = arg;
    cs->join.transport_timer = NULL;
    cs_info_req_timeout_cb(cs);
}

static void cs_server_operation_timeout(void* arg) {
    struct chat_sync* cs = arg;
    cs->join.operation_timer = NULL;
    cs_info_req_timeout_cb(cs);
}

static void cs_join_release_transport(struct chat_sync* cs) {
    if (cs->join.transport_timer) {
        uasync_cancel_timeout(cs->inst->ua, cs->join.transport_timer); cs->join.transport_timer = NULL;
    }
    struct NODE_CONN_DIRECT* handle = cs->join.handle;
    cs->join.handle = NULL;
    if (handle) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "join transport release peer=%016llx",
                    (unsigned long long)node_conn_direct_node_id(handle));
        node_conn_direct_close(handle);
    }
}

static int cs_invitation_find(const struct chat_sync* cs, const uint8_t group_ed[32], const uint8_t inviter[32]) {
    for (int i = 0; i < cs->pending_inv_in_count; i++)
        if (!memcmp(cs->pending_inv_in[i].group_ed, group_ed, 32) && !memcmp(cs->pending_inv_in[i].inviter, inviter, 32)) return i;
    return -1;
}

static void cs_invitation_remove(struct chat_sync* cs, int index) {
    cs->pending_inv_in_count--;
    memmove(cs->pending_inv_in + index, cs->pending_inv_in + index + 1,
            (cs->pending_inv_in_count - index) * sizeof(*cs->pending_inv_in));
    OPENSSL_cleanse(&cs->pending_inv_in[cs->pending_inv_in_count], sizeof(*cs->pending_inv_in));
}

static const char* cs_msg_name(uint8_t type) {
    switch (type) {
    case CS_MSG_SERVER_JOIN_REQ: return "SERVER_JOIN_REQ";
    case CS_MSG_SERVER_JOIN_RESULT: return "SERVER_JOIN_RESULT";
    case CS_MSG_ERROR:              return "ERROR";
    case CS_MSG_CHANNEL_INVITE:     return "CHANNEL_INVITE";
    case CS_MSG_JOIN_INFO_REQ:      return "JOIN_INFO_REQ";
    case CS_MSG_JOIN_INFO_RESP:     return "JOIN_INFO_RESP";
    case CS_MSG_JOIN_REQUEST:       return "JOIN_REQUEST";
    case CS_MSG_JOIN_READY:         return "JOIN_READY";
    case CS_MSG_SOUND_SETTING:      return "SOUND_SETTING";
    default:                        return "???";
    }
}

static void cs_cancel_proto_timers(struct chat_sync* cs) {
    if (cs->info_req_timer)     { uasync_cancel_timeout(cs->inst->ua, cs->info_req_timer);     cs->info_req_timer     = NULL; }
    if (cs->join_timer)         { uasync_cancel_timeout(cs->inst->ua, cs->join_timer);         cs->join_timer         = NULL; }
    if (cs->invite_send_timer)  { uasync_cancel_timeout(cs->inst->ua, cs->invite_send_timer);  cs->invite_send_timer  = NULL; }
}

void chat_sync_join_result(struct UTUN_INSTANCE* inst, uint64_t request, uint64_t channel, const uint8_t peer[32], int result) {
    uint8_t data[CHAT_CONNECT_RESULT_SIZE] = {0}; int32_t code = result;
    struct TOPO_GROUP* group = channel && inst->topo_groups ? topo_groups_find(inst->topo_groups, channel) : NULL;
    const uint8_t* group_ed = topo_group_wire_id(group);
    memcpy(data, peer, 32); memcpy(data + CHAT_CONNECT_RESULT_CODE_OFF, &code, 4);
    if (group_ed) memcpy(data + CHAT_CONNECT_RESULT_GROUP_OFF, group_ed, 32);
    memcpy(data + CHAT_CONNECT_RESULT_REQUEST_OFF, &request, 8);
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "join result request=%llu group=%s peer=%s result=%d",
        (unsigned long long)request, sc_public_key_hex(group_ed, (char[65]){0}), sc_public_key_hex(peer, (char[65]){0}), result);
    chat_event_post(inst, CHAT_EVT_CONNECT_RESULT, data, sizeof(data));
}

static void cs_join_end(struct chat_sync* cs, int result, int notify) {
    if (!cs->join.attempt) return;
    struct UTUN_INSTANCE* inst = cs->inst;
    uint64_t channel = cs->join.channel, request = cs->join.request;
    struct chat_join_route route = cs->join.route;
    struct NODE_CONN_DIRECT* handle = cs->join.handle;
    uint64_t carrier_handle = cs->join.route.handle;
    struct chat_invite_build* build = cs->join.offer_build;
    uint64_t origin = cs->join.origin_operation;
    uint8_t group_ed[32]; memcpy(group_ed, cs->join.expected_group, 32);
    cs_cancel_proto_timers(cs);
    cs_accept_drop_retry(cs);
    if (cs->join.transport_timer) uasync_cancel_timeout(inst->ua, cs->join.transport_timer);
    if (cs->join.operation_timer) uasync_cancel_timeout(inst->ua, cs->join.operation_timer);
    OPENSSL_cleanse(&cs->join, sizeof(cs->join));
    chat_join_port_complete(cs->port, request);
    if (build) chat_invite_build_cancel(build);
    if (origin && notify) cs_server_join_reply(cs, group_ed, &route, origin, result);
    if (handle) node_conn_direct_close(handle);
    if (carrier_handle && inst->join_carrier)
        inst->join_carrier->close(inst->join_carrier->ctx, carrier_handle, result == 0);
    if (notify) chat_sync_join_result(inst, request, channel, route.public_key, result);
}

static void cs_join_finish(struct chat_sync* cs, int result) { cs_join_end(cs, result, 1); }

static int cs_join_reserve(struct chat_sync* cs, uint64_t channel, const uint8_t peer_x[32], uint64_t request, int inviter) {
    if (!cs || !cs->initialized || !chat_core_generation(cs->inst) || (inviter && !channel) || channel > INT64_MAX || !peer_x ||
        !memcmp(peer_x, (uint8_t[32]){0}, 32) || !memcmp(peer_x, cs->inst->my_keys.public_key, 32) || (db_async_can_write(cs->inst->db_worker) && !cs->inst->group_started)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "invalid or stopped local join"); return CHAT_CONNECT_INVALID;
    }
    if (cs->join.attempt) { DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "local join busy"); return CHAT_CONNECT_BUSY; }
    uint64_t attempt;
    if (RAND_bytes((uint8_t*)&attempt, sizeof(attempt)) != 1 || !attempt) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "join attempt random generation failed"); return CHAT_CONNECT_ERROR;
    }
    cs->join.channel = channel; cs->join.peer = sc_derive_node_id_from_pubkey(peer_x); cs->join.request = request;
    cs->join.attempt = attempt; cs->join.inviter = inviter; cs->join.phase = 5;
    cs->join.route.peer = cs->join.peer;
    memcpy(cs->join.route.public_key, peer_x, 32);
    return 0;
}

/* ── Forward decl ── */
static struct ETCP_CONN* cs_find_conn_for_key(struct UTUN_INSTANCE* inst, const uint8_t* key);

/* ── Send ── */

static int cs_write_header(struct UTUN_INSTANCE* inst, uint8_t* p, const char* channel, uint8_t type, const uint8_t* group_ed) {
    p[0] = ETCP_ID_CHAT_SYNC; p[CHAT_SYNC_TYPE_OFF] = type;
    const uint8_t* address = group_ed;
    if (!address) address = topo_group_wire_id(topo_groups_find_channel(inst->topo_groups, channel));
    if (!address || !memcmp(address, (uint8_t[32]){0}, 32)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "missing CHAT_SYNC group key ch=%s type=%u", channel, type); return -1;
    }
    memcpy(p + CHAT_SYNC_SCOPE_OFF, address, 32); return 0;
}

static int cs_send_key(struct chat_sync* cs, const char* ch_id, uint64_t dst,
                   const uint8_t* payload, size_t len, const struct chat_join_route* route, const uint8_t* group_ed) {
    size_t header = CHAT_SYNC_GROUP_SIZE;
    if (!len || len > CHAT_JOIN_WIRE_MAX - header + 1 || !route || route->peer != dst ||
        !memcmp(route->public_key, (uint8_t[32]){0}, 32)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: invalid send length/route peer=%016llx bytes=%zu", CS_ID, (unsigned long long)dst, len);
        return -1;
    }
    size_t total = header + len - 1;
    uint8_t* buf = u_malloc(total);
    if (!buf) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: cs_send alloc(%zu) failed", CS_ID, total); return -1; }
    if (cs_write_header(cs->inst, buf, ch_id, payload[0], group_ed)) { u_free(buf); return -1; }
    memcpy(buf + header, payload + 1, len - 1);
    if (route->handle) {
        const struct chat_join_carrier* carrier = cs->inst->join_carrier;
        int rc = carrier ?
            carrier->send(carrier->ctx, route->handle, buf, total) : -1;
        u_free(buf);
        if (rc) DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN carrier send failed handle=%016llx type=%u bytes=%zu",
                          (unsigned long long)route->handle, payload[0], total);
        else DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN carrier SEND %s peer=%016llx handle=%016llx bytes=%zu",
                        cs_msg_name(payload[0]), (unsigned long long)dst, (unsigned long long)route->handle, total);
        return rc;
    }
    struct ll_entry* entry = queue_entry_new(0);
    if (!entry) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: cs_send queue_entry_new failed", CS_ID); u_free(buf); return -1; }
    entry->dgram = buf; entry->len = total;
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "%s: SEND %s to=%016llx ch=%s len=%zu",
               CS_ID, cs_msg_name(payload[0]), (unsigned long long)dst, ch_id, len);
    struct ETCP_CONN* conn = cs_find_conn_for_key(cs->inst, route->public_key);
    if (!conn) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: no conn for node %016llx", CS_ID, (unsigned long long)dst); u_free(buf); queue_entry_free(entry); return -1; }
    int rc = etcp_send(conn, entry);
    if (rc != 0) { queue_dgram_free(entry); queue_entry_free(entry); }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "%s: etcp_send rc=%d id=0x%02x to=%016llx conn=%s", CS_ID, rc, payload[0], (unsigned long long)dst, conn->log_name);
    return rc;
}

static int cs_send_attempt_key(struct chat_sync* cs, const char* channel, uint64_t peer,
                           const uint8_t* payload, size_t len, uint64_t attempt, const struct chat_join_route* route, const uint8_t* group_ed) {
    if (!attempt || !len || len > CHAT_JOIN_WIRE_MAX - CHAT_SYNC_GROUP_SIZE - CHAT_SYNC_ATTEMPT_SIZE + 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "invalid JOIN attempt or payload length bytes=%zu", len); return -1;
    }
    uint8_t* data = u_malloc(len + 8);
    if (!data) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "join wire allocation failed"); return -1; }
    data[0] = payload[0]; uint64_t wire = htobe64(attempt); memcpy(data + 1, &wire, 8);
    memcpy(data + 9, payload + 1, len - 1);
    int rc = cs_send_key(cs, channel, peer, data, len + 8, route, group_ed); u_free(data);
    return rc;
}

static int cs_send_attempt(struct chat_sync* cs, const char* channel, uint64_t peer,
                           const uint8_t* payload, size_t len, uint64_t attempt, const struct chat_join_route* route) {
    return cs_send_attempt_key(cs, channel, peer, payload, len, attempt, route, NULL);
}

/* ── Channel cache ── */

static int cs_server_join_reply(struct chat_sync* cs, const uint8_t group_ed[32], const struct chat_join_route* route, uint64_t operation, int result) {
    char group[65]; sc_public_key_hex(group_ed, group);
    uint8_t data[5] = {CS_MSG_SERVER_JOIN_RESULT};
    uint32_t code = htonl((uint32_t)result); memcpy(data + 1, &code, sizeof(code));
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "server join reply operation=%016llx group=%s peer=%016llx result=%d",
        (unsigned long long)operation, group, (unsigned long long)route->peer, result);
    int rc = cs_send_attempt_key(cs, group, route->peer, data, sizeof(data), operation, route, group_ed);
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "server join reply failed operation=%016llx rc=%d", (unsigned long long)operation, rc);
    return rc;
}

static void cs_handle_server_join(struct chat_sync* cs, const struct chat_join_route* route, const char* channel,
                                   const uint8_t group_ed[32], const uint8_t* data, size_t len, uint64_t operation) {
    uint64_t peer = route->peer;
    const struct global_config* cfg = &cs->inst->config->global;
    int result = CHAT_CONNECT_INVALID;
    if (len < INVITE_ADD_KEY_SIZE + 2) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "server join request truncated channel=%s peer=%016llx bytes=%zu",
            channel, (unsigned long long)peer, len);
        cs_server_join_reply(cs, group_ed, route, operation, CHAT_CONNECT_INVALID); return;
    }
    if (!cfg->chatserver_enabled || !cfg->chatserver_has_add_key) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "server join unavailable channel=%s peer=%016llx enabled=%d key_configured=%d",
            channel, (unsigned long long)peer, cfg->chatserver_enabled, cfg->chatserver_has_add_key);
        cs_server_join_reply(cs, group_ed, route, operation, CHAT_CONNECT_AUTH); return;
    }
    if (CRYPTO_memcmp(data, cfg->chatserver_add_key, INVITE_ADD_KEY_SIZE)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "server join access key mismatch channel=%s peer=%016llx operation=%016llx",
            channel, (unsigned long long)peer, (unsigned long long)operation);
        cs_server_join_reply(cs, group_ed, route, operation, CHAT_CONNECT_SERVER_KEY); return;
    }
    size_t size = ((size_t)data[32] << 8) | data[33];
    struct InviteData invite;
    if (size != len - 34 || invite_link_decode((const char*)data + 34, size, &invite, NULL, 0) ||
        invite.kind != INVITE_KIND_CHANNEL || memcmp(invite.pubkey, route->public_key, 32) ||
        sc_identity_check("server JOIN inviter", route->public_key, invite.inviter_pubkey) != SC_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "server join invitation rejected channel=%s peer=%016llx bytes=%zu",
            channel, (unsigned long long)peer, size);
    } else if (cs->join.attempt) {
        if (cs->join.origin_operation == operation && !memcmp(cs->join.route.public_key, route->public_key, 32) &&
            !memcmp(cs->join.expected_group, group_ed, 32) && cs->join.key == invite.join_key) {
            DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "server join duplicate active operation=%016llx", (unsigned long long)operation); return;
        }
        result = CHAT_CONNECT_BUSY;
    } else {
        struct ETCP_CONN* conn = cs_find_conn_for_key(cs->inst, route->public_key);
        if (conn && topo_groups_channel_slot(cs->inst->topo_groups, channel) && group_policy_member_key_allowed(cs->inst, channel, cs->inst->my_keys.public_key, cs->inst->my_ed25519_pubkey, 0) &&
            group_policy_member_key_allowed(cs->inst, channel, route->public_key, conn->peer_ed25519_pubkey, 0)) {
            cs_server_join_reply(cs, group_ed, route, operation, 0); return;
        }
        result = chat_sync_join_link(cs->inst, 0, (const char*)data + 34, size);
        if (!result) {
            cs->join.origin_operation = operation;
            cs->join.channel = topo_groups_channel_slot(cs->inst->topo_groups, channel);
            memcpy(cs->join.expected_group, group_ed, 32);
            DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "server join authorized; ordinary JOIN started operation=%016llx channel=%s peer=%016llx",
                (unsigned long long)operation, channel, (unsigned long long)peer); return;
        }
    }
    cs_server_join_reply(cs, group_ed, route, operation, result);
}

static void cs_handle_server_join_result(struct chat_sync* cs, const struct chat_join_route* route, const char* channel,
                                          const uint8_t* data, size_t len, uint64_t operation) {
    if (!cs->join.server_offer || cs->join.phase != 4 || memcmp(cs->join.route.public_key, route->public_key, 32) ||
        cs->join.channel != topo_groups_channel_slot(cs->inst->topo_groups, channel) || cs->join.attempt != operation) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "ignore stale server join result operation=%016llx channel=%s peer=%016llx",
            (unsigned long long)operation, channel, (unsigned long long)route->peer); return;
    }
    if (len != 4) { DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "invalid server join result bytes=%zu", len); return; }
    uint32_t code; memcpy(&code, data, sizeof(code));
    int result = (int32_t)ntohl(code);
    if (result != 0 && result != CHAT_CONNECT_ERROR && result != CHAT_CONNECT_TIMEOUT && result != CHAT_CONNECT_INVALID &&
        result != CHAT_CONNECT_BUSY && result != CHAT_CONNECT_CANCELLED && result != CHAT_CONNECT_AUTH &&
        result != CHAT_CONNECT_INVITE_DENIED && result != CHAT_CONNECT_SERVER_KEY) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "unknown server join result=%d", result); return;
    }
    cs_join_finish(cs, result);
}

/* ── Forward declarations for new message handlers ── */

static void cs_handle_channel_invite(struct chat_sync* cs, uint64_t peer,
    const uint8_t group_ed[32], const uint8_t* pl, size_t len, const struct chat_join_route* route);
static int cs_send_channel_invite(struct chat_sync* cs, struct UTUN_INSTANCE* inst,
                                   const char* ch_id, uint64_t target_node_id,
                                   const char* ch_name, const uint8_t owner[32],
                                   const uint8_t* ch_x25519, const uint8_t* ch_ed,
                                   const uint8_t* ch_sig);
/* ── Recv dispatcher ── */

/* Общий автомат JOIN получает уже подтверждённую личность и явный ответный маршрут. */
static void cs_receive(struct chat_sync* cs, const struct chat_join_route* route, const uint8_t* d, size_t dlen) {
    if (!cs || !cs->initialized) return;
    if (!d || dlen < CHAT_SYNC_BOOTSTRAP_SIZE || dlen > CHAT_JOIN_WIRE_MAX || d[0] != ETCP_ID_CHAT_SYNC) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "chat_sync: invalid frame bytes=%zu", dlen); return;
    }
    uint64_t peer = route->peer;
    uint8_t type = d[CHAT_SYNC_TYPE_OFF];
    if (cs->inst->ntp.startup_wait) {
        if (cs->time_wait_count >= 32) {
            DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "chat time wait queue full peer=%llx count=%u bytes=%zu",
                (unsigned long long)peer, cs->time_wait_count, dlen); return;
        }
        struct cs_wait_frame* frame = u_malloc(sizeof(*frame) + dlen);
        if (!frame) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "chat time wait frame allocation failed"); return; }
        *frame = (struct cs_wait_frame){.route = *route, .size = dlen};
        memcpy(frame->data, d, dlen);
        struct cs_wait_frame** tail = &cs->time_wait_frames;
        while (*tail) tail = &(*tail)->next;
        *tail = frame; cs->time_wait_count++;
        DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "chat frame waits for initial time peer=%llx type=%u queued=%u",
            (unsigned long long)peer, type, cs->time_wait_count); return;
    }
    if (type == CS_MSG_JOIN_RELAY) {
        chat_join_receive_direct(cs->inst, route, d + 2, dlen - 2);
        return;
    }
    if (dlen < CHAT_SYNC_GROUP_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "truncated CHAT_SYNC type=%u bytes=%zu", type, dlen);
        return;
    }
    const uint8_t* address = d + CHAT_SYNC_SCOPE_OFF;
    const uint8_t* pl = d + CHAT_SYNC_GROUP_SIZE;
    size_t plen = dlen - CHAT_SYNC_GROUP_SIZE;
    uint64_t attempt = 0;
    if (type == CS_MSG_SERVER_JOIN_REQ || type == CS_MSG_SERVER_JOIN_RESULT) {
        if (plen < 8) { DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "server JOIN missing operation type=%u", type); return; }
        memcpy(&attempt, pl, 8); attempt = be64toh(attempt); pl += 8; plen -= 8;
        if (!attempt) { DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "server JOIN zero operation type=%u", type); return; }
    }
    struct TOPO_GROUP* group = topo_groups_find_wire(cs->inst->topo_groups, address);
    uint64_t group_id = group && group->group_type == TOPO_GROUP_TYPE_CHAT ? group->group_id : 0;
    if (!group_id && type != CS_MSG_CHANNEL_INVITE && type != CS_MSG_SERVER_JOIN_REQ) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "unknown CHAT_SYNC group key=%s type=%u",
            sc_public_key_hex(address, (char[65]){0}), type);
        return;
    }
    char ch_id[65]; sc_public_key_hex(address, ch_id);
    DEBUG_TRACE(DEBUG_CATEGORY_MEMBER_SYNC, "%s: recv %s(%02x) from=%016llx ch=%s len=%zu",
               CS_ID, cs_msg_name(type), type, (unsigned long long)peer, ch_id, dlen);
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "%s: RECV %s from=%016llx ch=%s len=%zu",
               CS_ID, cs_msg_name(type), (unsigned long long)peer, ch_id, plen);

    switch (type) {
    case CS_MSG_CHANNEL_INVITE:   cs_handle_channel_invite(cs, peer, address, pl, plen, route); break;
    case CS_MSG_SERVER_JOIN_REQ: cs_handle_server_join(cs, route, ch_id, address, pl, plen, attempt); break;
    case CS_MSG_SERVER_JOIN_RESULT: cs_handle_server_join_result(cs, route, ch_id, pl, plen, attempt); break;
    case CS_MSG_SOUND_SETTING:
        if (plen == 1 && pl[0] <= 1) {
            struct ETCP_CONN* conn = instance_find_conn_by_key(cs->inst, route->public_key);
            if (conn) db_sync_set_peer_sound(cs->inst, group_id, conn->peer_ed25519_pubkey, pl[0]);
        }
        else DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "invalid SOUND_SETTING size=%zu", plen);
        break;
    default:                      DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "%s: UNKNOWN msg type=%02x from=%016llx", CS_ID, type, (unsigned long long)peer); break;
    }
}

/* ETCP передаёт владение entry; обработчик JOIN заимствует только подтверждённые key и bytes. */
static void chat_sync_recv_cb(struct ETCP_CONN* conn, struct ll_entry* entry) {
    if (conn && entry && entry->dgram) {
        struct chat_join_route route = {.peer = conn->peer_node_id};
        memcpy(route.public_key, conn->crypto_ctx.peer_public_key, 32);
        memcpy(route.peer_ed, conn->peer_ed25519_pubkey, 32);
        cs_receive(cs_of(conn->instance), &route, entry->dgram, entry->len);
    } else DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "chat_sync: missing ETCP connection/frame");
    if (entry) { queue_dgram_free(entry); queue_entry_free(entry); }
}

/* Carrier не имеет доступа к остальным командам CHAT_SYNC даже при корректной криптографии. */
void chat_sync_join_carrier_receive(struct UTUN_INSTANCE* inst, uint64_t handle, const uint8_t public_key[32],
                                  const uint8_t* data, size_t len) {
    if (!inst || !inst->join_carrier || !handle || !public_key || !data || len < CHAT_SYNC_BOOTSTRAP_SIZE + CHAT_SYNC_ATTEMPT_SIZE || len > CHAT_JOIN_WIRE_MAX ||
        data[CHAT_SYNC_TYPE_OFF] != CS_MSG_JOIN_RELAY) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN carrier rejected frame handle=%016llx bytes=%zu", (unsigned long long)handle, len);
        return;
    }
    struct chat_join_route route = {.peer = sc_derive_node_id_from_pubkey(public_key), .handle = handle};
    memcpy(route.public_key, public_key, 32); cs_receive(cs_of(inst), &route, data, len);
}

/* ── Connection callbacks ── */

static void cs_info_req_timeout_cb(void* arg) {
    struct chat_sync* cs = arg;
    if (!cs || !cs->initialized) return;
    DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "join timeout phase=%u channel=%llu peer=%016llx",
        cs->join.phase, (unsigned long long)cs->join.channel, (unsigned long long)cs->join.peer);
    cs_join_finish(cs, CHAT_CONNECT_TIMEOUT);
}
static void cs_join_timeout_cb(void* arg) {
    struct chat_sync* cs = arg;
    cs->join_timer = NULL;
    if (cs->join.phase == CS_JOIN_WAIT_READY) cs_join_begin_members(cs, "ready_timeout", 0);
    else cs_info_req_timeout_cb(cs);
}
static void cs_invite_send_timeout_cb(void* arg) { cs_info_req_timeout_cb(arg); }

/* Поиск активного ETCP по полному ключу транспорта. */

static struct ETCP_CONN* cs_find_conn_for_key(struct UTUN_INSTANCE* inst, const uint8_t* key) {
    struct ETCP_CONN* conn = key ? instance_find_conn_by_key(inst, key) : NULL;
    if (conn && conn->initialized && conn->links_up && !conn->close_requested) return conn;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN transport unavailable x=%s state=%d links=%d closing=%d",
        sc_public_key_hex(key, (char[65]){0}), conn ? conn->initialized : 0, conn ? conn->links_up : 0, conn ? conn->close_requested : 0);
    return NULL;
}

/* SQLite сортирует BLOB по байтам; тот же порядок используется для поиска в снимке. */
static int cs_compare_ed(const void* a, const void* b) { return memcmp(a, b, 32); }

static int cs_channel_has_peer(const struct channel_cache* channel, const uint8_t ed[32]) {
    return channel->peer_count && bsearch(ed, channel->peers, channel->peer_count, sizeof(*channel->peers), cs_compare_ed) != NULL;
}

static int cs_online(const struct ETCP_CONN* conn) {
    return conn && conn->initialized && conn->links_up && !conn->close_requested;
}

static void cs_post_channel_online(struct chat_sync* cs, const char* ch_id) {
    char channel[65]; snprintf(channel, sizeof(channel), "%s", ch_id); ch_id = channel;
    int online = 0;
    for (int i = 0; i < cs->channel_count; i++) {
        if (strcmp(cs->channels[i].channel_id, ch_id)) continue;
        for (struct ll_entry* e = cs->inst->connections ? cs->inst->connections->head : NULL; e; e = e->next) {
            struct ETCP_CONN* conn = ((struct conn_queue_entry*)e->data)->conn;
            if (cs_online(conn) && cs_channel_has_peer(&cs->channels[i], conn->peer_ed25519_pubkey)) online++;
        }
        break;
    }
    size_t cl = strlen(ch_id);
    if (cl > 63) cl = 63;
    uint8_t evt[66]; evt[0] = (uint8_t)cl;
    memcpy(evt + 1, ch_id, cl);
    uint16_t oc = online > UINT16_MAX ? UINT16_MAX : (uint16_t)online; memcpy(evt + 1 + cl, &oc, 2);
    chat_event_post(cs->inst, CHAT_EVT_CHANNEL_PEERS_ONLINE, evt, 1 + (int)cl + 2);
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "%s: PEERS_ONLINE ch=%.*s total_online=%d", CS_ID, (int)cl, ch_id, online);
}

/* ── Адресные запросы синхронизации; один таймер на накопленные пары ── */

static void cs_sync_timer_cb(void* arg);

/* Committed transport admission актуальнее GUI-каталога и не требует SQL в uasync. */
static int cs_sync_member(struct chat_sync* cs, uint64_t channel_id, const struct ETCP_CONN* conn) {
    struct TOPO_GROUP* group = topo_groups_find(cs->inst->topo_groups, channel_id);
    if (!group || group->group_type != TOPO_GROUP_TYPE_CHAT) return 0;
    return group_policy_member_key_allowed(cs->inst, group->channel_id, conn->crypto_ctx.peer_public_key, conn->peer_ed25519_pubkey, 1);
}

static int cs_sync_peer_available(struct chat_sync* cs, const struct ETCP_CONN* conn) {
    return conn && conn->instance == cs->inst && conn->initialized && conn->links_up && conn->state != 2
        && memcmp(conn->peer_ed25519_pubkey, cs->inst->my_ed25519_pubkey, 32)
        && !conn->close_requested && !conn->reinit_pending && !conn->session_required && !conn->peer_sleep_phase;
}

/* Нулевой peer/channel выбирает все задания соответствующего измерения. Сессии Merkle здесь не трогаем. */
static void cs_cancel_sync(struct chat_sync* cs, struct ETCP_CONN* conn, uint64_t channel_id, const char* reason) {
    struct pending_sync** p = &cs->sync_pending;
    struct pending_sync* cancelled = NULL;
    while (*p) {
        struct pending_sync* job = *p;
        if ((conn && node_conn_direct_get_conn(job->transport) != conn) || (channel_id && job->channel_id != channel_id)) { p = &job->next; continue; }
        *p = job->next;
        job->next = cancelled; cancelled = job;
    }
    if (!cs->sync_pending && cs->sync_timer) {
        uasync_cancel_timeout(cs->inst->ua, cs->sync_timer); cs->sync_timer = NULL;
    }
    while (cancelled) {
        struct pending_sync* job = cancelled; cancelled = job->next;
        DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "sync request cancelled ch=%llu peer=%s reason=%s",
            (unsigned long long)job->channel_id, sc_public_key_hex(node_conn_direct_get_key(job->transport), (char[65]){0}), reason);
        struct NODE_CONN_DIRECT* transport = job->transport;
        u_free(job);
        node_conn_direct_close(transport);
    }
}

/* Срок первого задания не переносится последующими событиями других пиров. */
static int cs_arm_sync(struct chat_sync* cs) {
    if (!cs->sync_pending || cs->sync_timer || cs->sync_draining || cs->inst->ntp.startup_wait) return 0;
    cs->sync_timer = uasync_set_timeout(cs->inst->ua, CS_SYNC_INTERVAL_MS * 10, cs, cs_sync_timer_cb, "cs_sync");
    if (cs->sync_timer) return 0;
    DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "sync request timer allocation failed; cancelling pending requests");
    cs_cancel_sync(cs, 0, 0, "timer_failed");
    return -1;
}

/* Извлекаем задание до callbacks; отмена видит все оставшиеся, новые идут в конец очереди. */
static void cs_sync_timer_cb(void* arg) {
    struct chat_sync* cs = arg;
    cs->sync_timer = NULL;
    if (!cs->initialized || cs->inst->ntp.startup_wait) return;
    for (struct pending_sync* job = cs->sync_pending; job; job = job->next) job->in_batch = 1;
    cs->sync_draining = 1;
    while (cs->sync_pending && cs->sync_pending->in_batch) {
        struct pending_sync* job = cs->sync_pending;
        cs->sync_pending = job->next;
        struct ETCP_CONN* conn = node_conn_direct_get_conn(job->transport);
        uint64_t channel_id = job->channel_id;
        const char* reason = job->reason;
        if (!cs_sync_peer_available(cs, conn) || !cs_sync_member(cs, channel_id, conn)) {
            DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "sync request discarded ch=%llu peer=%s reason=%s unavailable",
                (unsigned long long)channel_id, sc_public_key_hex(conn ? conn->peer_ed25519_pubkey : NULL, (char[65]){0}), reason);
        } else {
            char channel[65]; strcpy(channel, topo_groups_channel(cs->inst->topo_groups, channel_id));
            DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "sync request started ch=%s peer=%s reason=%s", channel,
                sc_public_key_hex(conn ? conn->peer_ed25519_pubkey : NULL, (char[65]){0}), reason);
            if (member_sync_start(cs->inst, conn, channel, NULL, NULL) < 0)
                DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "sync request rejected ch=%s peer=%s reason=%s", channel,
                    sc_public_key_hex(conn ? conn->peer_ed25519_pubkey : NULL, (char[65]){0}), reason);
        }
        node_conn_direct_close(job->transport);
        u_free(job);
    }
    cs->sync_draining = 0;
    cs_arm_sync(cs);
}

/* Запрос одной пары: объединяем дубликаты и удерживаем готовый транспорт независимо от BGP-владельцев. */
static int cs_schedule_sync(struct chat_sync* cs, uint64_t channel_id, struct ETCP_CONN* conn, const char* reason) {
    if (!cs->initialized || !channel_id || !cs_sync_peer_available(cs, conn)
        || !cs_sync_member(cs, channel_id, conn) || cs_recovery_find(cs, topo_groups_channel(cs->inst->topo_groups, channel_id))) return 0;
    struct pending_sync** tail = &cs->sync_pending;
    while (*tail) {
        if ((*tail)->channel_id == channel_id && node_conn_direct_get_conn((*tail)->transport) == conn) {
            DEBUG_TRACE(DEBUG_CATEGORY_MEMBER_SYNC, "sync request coalesced ch=%llu peer=%s reason=%s",
                (unsigned long long)channel_id, sc_public_key_hex(conn ? conn->peer_ed25519_pubkey : NULL, (char[65]){0}), reason);
            return 0;
        }
        tail = &(*tail)->next;
    }
    struct pending_sync* job = u_calloc(1, sizeof(*job));
    if (!job) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "sync request allocation failed ch=%llu peer=%s reason=%s",
            (unsigned long long)channel_id, sc_public_key_hex(conn ? conn->peer_ed25519_pubkey : NULL, (char[65]){0}), reason); return -1;
    }
    job->channel_id = channel_id; job->reason = reason;
    if (node_conn_direct_hold(conn, &job->transport) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "sync request transport hold failed ch=%llu peer=%s",
            (unsigned long long)channel_id, sc_public_key_hex(conn ? conn->peer_ed25519_pubkey : NULL, (char[65]){0})); u_free(job); return -1;
    }
    *tail = job;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "sync request queued ch=%llu peer=%s reason=%s",
        (unsigned long long)channel_id, sc_public_key_hex(conn ? conn->peer_ed25519_pubkey : NULL, (char[65]){0}), reason);
    return cs_arm_sync(cs);
}

static void cs_start_admin_sync(struct chat_sync* cs, const char* channel, struct ETCP_CONN* conn) {
    struct TOPO_GROUP* group = topo_groups_find_channel(cs->inst->topo_groups, channel);
    /* Если обе стороны у нас допущены, эти же записи разнесёт обычный MEMBERS. */
    if (cs_sync_peer_available(cs, conn) && !topo_group_chat_connection_allowed(group, conn) &&
        member_sync_admin_allowed(cs->inst, channel, conn) &&
        merkle_sync_start(cs->inst, MT_DATA_MEMBER_ADMIN, conn, channel, NULL, NULL) < 0)
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBER_ADMIN start failed ch=%s peer=%s", channel,
            sc_public_key_hex(conn->peer_ed25519_pubkey, (char[65]){0}));
}

void chat_sync_admission_changed(struct UTUN_INSTANCE* inst, const char* channel, int admin_changed) {
    struct chat_sync* cs = cs_of(inst);
    if (!cs || !cs->initialized || inst->ntp.startup_wait) return;
    if (admin_changed) merkle_sync_changed(inst, MT_DATA_MEMBER_ADMIN, channel);
    /* merkle_sync_start удерживает UP до того, как публикация закроет групповые сессии. */
    for (struct ll_entry* entry = inst->connections ? inst->connections->head : NULL; entry; entry = entry->next)
        cs_start_admin_sync(cs, channel, ((struct conn_queue_entry*)entry->data)->conn);
}

/* Событие транспорта относится только к общим каналам этого пира; группы актуальнее кеша. */
static void cs_schedule_peer_sync(struct chat_sync* cs, struct ETCP_CONN* conn, const char* reason) {
    if (!cs_sync_peer_available(cs, conn)) return;
    struct TOPO_GROUPS* groups = cs->inst->topo_groups;
    for (struct ll_entry* e = groups && groups->group_list ? groups->group_list->head : NULL; e; e = e->next) {
        struct TOPO_GROUP* group = (struct TOPO_GROUP*)e;
        if (group->group_type == TOPO_GROUP_TYPE_CHAT) {
            cs_schedule_sync(cs, group->group_id, conn, reason);
            cs_start_admin_sync(cs, group->channel_id, conn);
        }
    }
}

/* ── Peer sleep (keepalive-протокол): спячка → отменить сессии; пробуждение → пересинхронизация ── */

static void cs_on_peer_sleep(struct ETCP_CONN* conn, int sleeping, void* arg) {
    struct chat_sync* cs = (struct chat_sync*)arg;
    if (!cs || !cs->initialized) return;
    if (sleeping) {
        cs_cancel_sync(cs, conn, 0, "peer_sleep");
        DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "%s: peer sleep peer=%s — cancel member_sync sessions", CS_ID, sc_public_key_hex(conn->peer_ed25519_pubkey, (char[65]){0}));
        merkle_sync_recheck_peer(conn);
    } else {
        DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "%s: peer awake peer=%s — schedule sync", CS_ID, sc_public_key_hex(conn->peer_ed25519_pubkey, (char[65]){0}));
        cs_schedule_peer_sync(cs, conn, "peer_awake");
    }
    db_sync_peer_policy_changed(conn);
}

/* ── Per-chat «play sound on message» (троттлинг db_sync на сервере) ── */

static void cs_send_sound_setting(struct ETCP_CONN* conn, const char* channel, int on) {
    struct ll_entry* entry = ll_alloc_lldgram(CHAT_SYNC_GROUP_SIZE + 1);
    if (!entry) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "sound setting frame allocation failed channel=%s", channel); return; }
    if (cs_write_header(conn->instance, entry->dgram, channel, CS_MSG_SOUND_SETTING, NULL)) {
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    entry->dgram[CHAT_SYNC_GROUP_SIZE] = !!on; entry->len = CHAT_SYNC_GROUP_SIZE + 1;
    int rc = etcp_send(conn, entry);
    if (rc) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "sound setting send failed channel=%s peer=%s rc=%d",
            channel, sc_public_key_hex(conn->peer_ed25519_pubkey, (char[65]){0}), rc);
        queue_dgram_free(entry); queue_entry_free(entry);
    } else DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "sound setting sent channel=%s peer=%s on=%d",
        channel, sc_public_key_hex(conn->peer_ed25519_pubkey, (char[65]){0}), !!on);
}

void chat_sync_broadcast_sound_setting(struct UTUN_INSTANCE* inst, const char* ch_id, int on) {
    struct chat_sync* cs = cs_of(inst);
    if (!cs || !cs->initialized || !ch_id || !ch_id[0]) return;
    for (int i = 0; i < cs->channel_count; i++) {
        if (strcmp(cs->channels[i].channel_id, ch_id)) continue;
        for (struct ll_entry* e = inst->connections ? inst->connections->head : NULL; e; e = e->next) {
            struct ETCP_CONN* conn = ((struct conn_queue_entry*)e->data)->conn;
            if (cs_online(conn) && cs_channel_has_peer(&cs->channels[i], conn->peer_ed25519_pubkey)) cs_send_sound_setting(conn, ch_id, on);
        }
        break;
    }
}

struct sound_peer_request {
    struct UTUN_INSTANCE* inst;
    uint8_t peer[32];
    char channel[65];
};

/* За время SQL peer мог сменить соединение или выйти из канала. */
static void cs_sound_loaded(void* data, const struct db_async_result* result, int on) {
    struct sound_peer_request* request = data;
    struct chat_sync* cs = cs_of(request->inst);
    if (!result->status && cs && cs->initialized) {
        for (int i = 0; i < cs->channel_count; i++) {
            if (strcmp(cs->channels[i].channel_id, request->channel)) continue;
            if (cs_channel_has_peer(&cs->channels[i], request->peer)) {
                struct ETCP_CONN* conn = instance_find_conn_by_ed(request->inst, request->peer);
                if (cs_online(conn)) cs_send_sound_setting(conn, request->channel, on);
            }
            break;
        }
    } else if (result->status && result->status != DB_ASYNC_CANCELLED)
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "sound setting read failed channel=%s peer=%s status=%d",
            request->channel, sc_public_key_hex(request->peer, (char[65]){0}), result->status);
    u_free(request);
}

static void cs_send_all_sound_settings(struct chat_sync* cs, const uint8_t peer[32]) {
    for (int i = 0; i < cs->channel_count; i++) {
        if (!cs_channel_has_peer(&cs->channels[i], peer)) continue;
        struct sound_peer_request* request = u_calloc(1, sizeof(*request));
        if (!request) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "sound setting allocation failed channel=%s peer=%s",
                cs->channels[i].channel_id, sc_public_key_hex(peer, (char[65]){0})); continue;
        }
        request->inst = cs->inst; memcpy(request->peer, peer, 32);
        snprintf(request->channel, sizeof(request->channel), "%s", cs->channels[i].channel_id);
        chat_sound_read(cs->inst, request->channel, cs_sound_loaded, request);
    }
}

/* ── Peer status change (только GUI + online-бар; онлайн в БД не хранится) ── */

static void cs_on_peer_status_changed(struct chat_sync* cs, const uint8_t peer[32], int online) {
    int count = 0;
    char (*channels)[65] = cs->channel_count ? u_malloc((size_t)cs->channel_count * sizeof(*channels)) : NULL;
    if (cs->channel_count && !channels) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "peer status snapshot allocation failed"); return;
    }
    for (int i = 0; i < cs->channel_count; i++) {
        if (cs_channel_has_peer(&cs->channels[i], peer)) strcpy(channels[count++], cs->channels[i].channel_id);
    }
    for (int i = 0; i < count; i++) {
        const char* channel = channels[i];
        struct TOPO_GROUP* group = topo_groups_find_channel(cs->inst->topo_groups, channel);
        if (!group) continue;
        chat_member_state_changed(group, peer);
        cs_post_channel_online(cs, channel);
    }
    u_free(channels);
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "peer status peer=%s online=%d channels=%d",
        sc_public_key_hex(peer, (char[65]){0}), online, count);
}

/* ── send JOIN_INFO_REQ to start join handshake ── */

static struct chat_join_path cs_guest_path(struct chat_sync* cs, uint64_t attempt, const uint8_t inviter[32],
    const uint8_t exit_x[32], uint64_t handle) {
    struct chat_join_path path = {.attempt = attempt, .handle = handle};
    memcpy(path.inviter, inviter, 32);
    memcpy(path.guest, cs->inst->my_ed25519_pubkey, 32);
    memcpy(path.exit_x, exit_x, 32);
    memcpy(path.hop_x, exit_x, 32);
    return path;
}

static int cs_guest_send(struct chat_sync* cs, uint8_t type, const uint8_t* data, size_t size) {
    struct chat_join_path path = cs_guest_path(cs, cs->join.attempt, cs->join.inviter_ed,
        cs->join.route.public_key, cs->join.route.handle);
    return chat_join_send(cs->inst, &path, type, data, size);
}

static int cs_start_channel_join(struct chat_sync* cs, uint64_t peer) {
    if (!cs || cs->info_req_timer || cs->join_timer) return 0;
    if (cs->inst->ntp.startup_wait) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN waits for initial time peer=%llx", (unsigned long long)peer); return 0;
    }
    char channel[65]; strcpy(channel, topo_groups_channel(cs->inst->topo_groups, cs->join.channel));
    uint64_t key = htobe64(cs->join.key);
    unsigned timeout = cs->join.route.handle ? CS_CARRIER_TIMEOUT_MS : CS_INFO_REQ_TIMEOUT_MS;
    cs->info_req_timer = uasync_set_timeout(cs->inst->ua, timeout * 10, cs,
                                           cs_info_req_timeout_cb, "cs_info_req");
    if (!cs->info_req_timer) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "join info deadline allocation failed"); return -1; }
    cs->join.phase = 2;
    if (cs_guest_send(cs, CS_MSG_JOIN_INFO_REQ, (const uint8_t*)&key, sizeof(key))) return -1;
    if (cs->join.transport_timer) { uasync_cancel_timeout(cs->inst->ua, cs->join.transport_timer); cs->join.transport_timer = NULL; }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "join info requested channel=%s attempt=%016llx", channel,
        (unsigned long long)cs->join.attempt);
    return 0;
}

/* Событие относится к сохранённому handle, а не к любому соединению с тем же node_id. */
void chat_sync_join_carrier_ready(struct UTUN_INSTANCE* inst, uint64_t handle, const uint8_t public_key[32]) {
    struct chat_sync* cs = cs_of(inst);
    if (cs && cs->initialized && handle) {
        for (struct join_recovery* item = cs->recovery; item; item = item->next) {
            if (item->carrier_handle != handle) continue;
            if (!public_key || memcmp(public_key, item->hints.pubkey, 32)) {
                DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN recovery carrier identity mismatch ch=%s", item->channel);
                cs_recovery_carrier_closed(cs, handle);
                inst->join_carrier->close(inst->join_carrier->ctx, handle, 0);
            } else {
                item->carrier_ready = 1;
                if (item->retry_at != UINT64_MAX) cs_recovery_wake(cs, item);
            }
            return;
        }
    }
    if (!cs || !cs->initialized || !handle || cs->join.route.handle != handle || cs->join.phase != 1) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "ignore inactive JOIN carrier ready handle=%016llx", (unsigned long long)handle); return;
    }
    if (!public_key || memcmp(public_key, cs->join.route.public_key, 32) ||
        sc_derive_node_id_from_pubkey(public_key) != cs->join.peer) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN carrier identity differs from invitation"); cs_join_finish(cs, CHAT_CONNECT_AUTH); return;
    }
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN carrier authenticated peer=%016llx handle=%016llx",
               (unsigned long long)cs->join.peer, (unsigned long long)handle);
    cs->join.carrier_ready = 1;
    if (cs_start_channel_join(cs, cs->join.peer) < 0) cs_join_finish(cs, CHAT_CONNECT_ERROR);
}

/* Закрытие затрагивает только попытки этого пути; сохранённое членство не откатывается. */
void chat_sync_join_carrier_closed(struct UTUN_INSTANCE* inst, uint64_t handle) {
    struct chat_sync* cs = cs_of(inst);
    if (!cs || !handle) return;
    struct cs_wait_frame** frame = &cs->time_wait_frames;
    while (*frame) {
        struct cs_wait_frame* item = *frame;
        if (item->route.handle != handle) { frame = &item->next; continue; }
        *frame = item->next; cs->time_wait_count--; u_free(item);
    }
    chat_join_relay_closed(inst, NULL, handle);
    cs_recovery_carrier_closed(cs, handle);
    if (cs->join.route.handle == handle) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN carrier closed handle=%016llx phase=%u", (unsigned long long)handle, cs->join.phase);
        if (cs->join.phase == CS_JOIN_WAIT_READY) {
            cs->join.route.handle = 0;
            cs_join_begin_members(cs, "carrier_closed", 0);
        } else if (cs->join.phase >= CS_JOIN_ACCEPT_WRITE) cs->join.route.handle = 0;
        else cs_join_finish(cs, CHAT_CONNECT_ERROR);
    }
}

/* Provider переживает restart чата; при замене освобождаем пути JOIN старого provider. */
int chat_sync_set_join_carrier(struct UTUN_INSTANCE* inst, const struct chat_join_carrier* carrier) {
    if (!inst || (carrier && (!carrier->ctx || !carrier->open || !carrier->send || !carrier->close))) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "invalid JOIN carrier registration"); return -1;
    }
    if (inst->join_carrier == carrier) return 0;
    struct chat_sync* cs = cs_of(inst);
    if (cs) {
        if (cs->carrier_generation == UINT64_MAX) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN carrier registration generation exhausted"); return -1;
        }
        cs->carrier_generation++;
        chat_join_relay_closed(inst, NULL, 0);
        cs_recovery_carrier_closed(cs, 0);
        if (cs->join.route.handle) {
            if (cs->join.phase == CS_JOIN_WAIT_READY || cs->join.phase >= CS_JOIN_ACCEPT_WRITE) {
                uint64_t handle = cs->join.route.handle; cs->join.route.handle = 0;
                inst->join_carrier->close(inst->join_carrier->ctx, handle, 0);
                if (cs->join.phase == CS_JOIN_WAIT_READY) cs_join_begin_members(cs, "carrier_replaced", 0);
            } else cs_join_finish(cs, CHAT_CONNECT_CANCELLED);
        }
        struct cs_wait_frame** frame = &cs->time_wait_frames;
        while (*frame) {
            struct cs_wait_frame* item = *frame;
            if (!item->route.handle) { frame = &item->next; continue; }
            DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "discard queued JOIN on carrier replacement handle=%llx",
                (unsigned long long)item->route.handle);
            *frame = item->next; cs->time_wait_count--; u_free(item);
        }
    }
    inst->join_carrier = carrier;
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN carrier registered=%d", carrier != NULL);
    return 0;
}

static int cs_send_server_offer(struct chat_sync* cs) {
    char channel[65]; strcpy(channel, topo_groups_channel(cs->inst->topo_groups, cs->join.channel));
    size_t len = strlen(cs->join.offer_link);
    uint8_t data[35 + INVITE_LINK_MAX] = {CS_MSG_SERVER_JOIN_REQ};
    memcpy(data + 1, cs->join.add_key, INVITE_ADD_KEY_SIZE);
    data[33] = (uint8_t)(len >> 8); data[34] = (uint8_t)len; memcpy(data + 35, cs->join.offer_link, len);
    cs->join.phase = 4;
    int rc = cs_send_attempt(cs, channel, cs->join.peer, data, len + 35, cs->join.attempt, &cs->join.route);
    OPENSSL_cleanse(data, sizeof(data));
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "server join request send failed channel=%s rc=%d", channel, rc);
    return rc;
}

static void cs_server_invite_ready(void* arg, int result, const char* link) {
    struct chat_sync* cs = arg;
    if (!cs->join.server_offer) return; /* Отмена уже освободила состояние операции. */
    cs->join.offer_build = NULL;
    if (result != CHAT_JOIN_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "server invitation build failed result=%d", result);
        cs_join_finish(cs, result == CHAT_JOIN_TIMEOUT ? CHAT_CONNECT_TIMEOUT : CHAT_CONNECT_ERROR); return;
    }
    struct InviteData invitation;
    if (!link || invite_link_decode(link, strlen(link), &invitation, NULL, 0) ||
        invitation.kind != INVITE_KIND_CHANNEL || chat_join_key_group(cs->inst, invitation.join_key) != cs->join.channel ||
        memcmp(invitation.pubkey, cs->inst->my_keys.public_key, 32)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "server invitation builder returned unexpected target");
        cs_join_finish(cs, CHAT_CONNECT_ERROR); return;
    }
    strcpy(cs->join.offer_link, link); cs->join.key = invitation.join_key;
    struct ETCP_CONN* conn = node_conn_direct_get_conn(cs->join.handle);
    if (conn && conn->initialized && conn->links_up) {
        if (cs_send_server_offer(cs)) cs_join_finish(cs, CHAT_CONNECT_ERROR);
    } else cs->join.phase = 1;
}

enum cs_group_read_kind { CS_GROUP_OFFER, CS_GROUP_INFO };
struct cs_group_read_job {
    struct chat_join_path path; /* Ответ INFO от пригласившего. */
    struct chat_profile_identity identity;
    struct chat_sync_group_snapshot snapshot;
    char channel[65];
    uint64_t core_generation, generation, group_generation, carrier_generation, peer, attempt, join_key;
    enum cs_group_read_kind kind;
    struct chat_join_route route; /* Только scalar handle/подтверждённый публичный ключ, без borrowed connection. */
    struct UTUN_INSTANCE* inst; /* Только guard/done, SQL использует собственные поля выше. */
};
static struct chat_sync* cs_group_read_current(struct cs_group_read_job* job) {
    if (!job->core_generation || job->inst->destroying || !job->inst->group_started ||
        chat_core_generation(job->inst) != job->core_generation) return NULL;
    struct chat_sync* cs = cs_of(job->inst);
    struct TOPO_GROUP* group = job->inst->topo_groups ? topo_groups_find_channel(job->inst->topo_groups, job->channel) : NULL;
    if (!cs || !cs->initialized || cs->generation != job->generation || !group || group->stopping ||
        group->generation != job->group_generation) return NULL;
    if (job->kind == CS_GROUP_OFFER && (cs->join.attempt != job->attempt || cs->join.phase != 6 ||
        memcmp(cs->join.route.public_key, job->route.public_key, 32) || cs->join.channel != group->group_id)) return NULL;
    if (job->kind == CS_GROUP_INFO && job->path.group) return cs;
    if (job->route.handle) {
        if (!job->inst->join_carrier || cs->carrier_generation != job->carrier_generation) return NULL;
    } else {
        if (!cs_find_conn_for_key(job->inst, job->route.public_key)) return NULL;
    }
    return cs;
}
static int cs_group_read_authorized(struct cs_group_read_job* job) {
    if (job->kind == CS_GROUP_OFFER) return group_policy_can_invite(job->inst, job->channel, job->identity.ed) == 1;
    return chat_join_key_matches(job->inst, topo_groups_channel_slot(job->inst->topo_groups, job->channel), job->join_key, job->identity.ed);
}
static int cs_group_read_guard(void* data) {
    struct cs_group_read_job* job = data; return cs_group_read_current(job) && cs_group_read_authorized(job);
}
static int cs_group_read_work(sqlite3* db, void* data) {
    struct cs_group_read_job* job = data; return chat_sync_group_read(db, &job->identity, job->channel, &job->snapshot);
}
/* Снимок содержит запись A для INFO и описание для push-приглашения. */
static int cs_group_read_send(struct chat_sync* cs, struct cs_group_read_job* job) {
    const struct chat_sync_group_snapshot* record = &job->snapshot;
    uint64_t key = 0;
    if (job->kind == CS_GROUP_OFFER) {
        if (RAND_bytes((uint8_t*)&key, sizeof(key)) != 1 || !key ||
            chat_join_store_local_key(job->inst, topo_groups_channel_slot(job->inst->topo_groups, job->channel), key)) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN offer key generation/registration failed ch=%s", job->channel); return -1;
        }
        cs->join.key = key;
        cs->invite_send_timer = uasync_set_timeout(cs->inst->ua, CS_INVITE_SEND_TIMEOUT_MS * 10, cs,
            cs_invite_send_timeout_cb, "cs_invite_send");
        if (!cs->invite_send_timer) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN offer deadline allocation failed"); return -1; }
        cs->join.phase = 4;
    }
    uint8_t buf[MS_USERINFO_MAX + GROUP_INFO_MAX + 768]; size_t off = 0;
    size_t description = strlen(record->description.json), profile = strlen(record->profile.json);
    if (job->kind == CS_GROUP_INFO) {
        memcpy(buf + off, record->x, 32); off += 32;
        memcpy(buf + off, record->ed, 32); off += 32;
    } else buf[off++] = CS_MSG_CHANNEL_INVITE;
    buf[off++] = job->kind == CS_GROUP_INFO ? 1 : 2; buf[off++] = (uint8_t)(description >> 8); buf[off++] = (uint8_t)description;
    memcpy(buf + off, record->description.json, description); off += description;
    memcpy(buf + off, record->owner, 32); off += 32;
    if (job->kind == CS_GROUP_OFFER) { memcpy(buf + off, record->x, 32); off += 32; }
    memcpy(buf + off, record->description.signature, 64); off += 64;
    if (job->kind == CS_GROUP_INFO) {
        memcpy(buf + off, job->identity.x, 32); off += 32;
        memcpy(buf + off, job->identity.ed, 32); off += 32;
    } else buf[off++] = PEERS_FLAG_HAS_JOIN;
    memcpy(buf + off, record->join_signature, 64); off += 64; memcpy(buf + off, &record->join_ts, 8); off += 8;
    memcpy(buf + off, record->profile.signature, 64); off += 64; memcpy(buf + off, &record->profile.version, 8); off += 8;
    buf[off++] = (uint8_t)(profile >> 8); buf[off++] = (uint8_t)profile; memcpy(buf + off, record->profile.json, profile); off += profile;
    if (job->kind == CS_GROUP_INFO) {
        memcpy(buf + off, record->signed_by, 32); off += 32; memcpy(buf + off, record->invitation_signature, 64); off += 64;
    } else { memcpy(buf + off, &key, 8); off += 8; }
    int rc = job->kind == CS_GROUP_INFO ? chat_join_send(cs->inst, &job->path, CS_MSG_JOIN_INFO_RESP, buf, off)
        : cs_send_key(cs, job->channel, job->peer, buf, off, &job->route, record->ed);
    if (!rc && job->kind == CS_GROUP_OFFER && cs->join.transport_timer) {
        uasync_cancel_timeout(cs->inst->ua, cs->join.transport_timer); cs->join.transport_timer = NULL;
    }
    return rc;
}
static void cs_join_reply_error(struct UTUN_INSTANCE* inst, const struct chat_join_path* path, int status) {
    uint32_t wire = htonl((uint32_t)(status == CHAT_CONNECT_AUTH ? CHAT_CONNECT_AUTH : CHAT_CONNECT_ERROR));
    DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN denied attempt=%016llx guest=%s status=%d",
        (unsigned long long)path->attempt, sc_public_key_hex(path->guest, (char[65]){0}), status);
    chat_join_send(inst, path, CS_MSG_ERROR, (const uint8_t*)&wire, sizeof(wire));
}

static void cs_group_read_done(void* data, const struct db_async_result* result) {
    struct cs_group_read_job* job = data; struct chat_sync* cs = cs_group_read_current(job);
    if (result->status) DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN group READ failed ch=%s kind=%d status=%d rc=%d: %s",
        job->channel, job->kind, result->status, result->sqlite_code, result->message);
    if (!cs) return;
    int authorized = cs_group_read_authorized(job);
    if (!result->status && authorized && !cs_group_read_send(cs, job)) return;
    if (job->kind == CS_GROUP_OFFER) cs_join_finish(cs, CHAT_CONNECT_ERROR);
    else cs_join_reply_error(job->inst, &job->path, authorized ? CHAT_CONNECT_ERROR : CHAT_CONNECT_AUTH);
}
static void cs_group_read_free(void* data) { u_free(data); }
static int cs_group_read_submit(struct chat_sync* cs, const char* channel, uint64_t peer, enum cs_group_read_kind kind,
    uint64_t key, uint64_t attempt, const struct chat_join_route* route, const struct chat_join_path* path) {
    struct TOPO_GROUP* group = topo_groups_find_channel(cs->inst->topo_groups, channel);
    if (!group || group->stopping) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN group READ missing runtime ch=%s", channel); return -1; }
    struct cs_group_read_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN group READ allocation failed ch=%s", channel); return -1; }
    job->inst = cs->inst; job->core_generation = chat_core_generation(cs->inst); job->generation = cs->generation;
    job->group_generation = group->generation;
    job->carrier_generation = cs->carrier_generation;
    job->identity.node = cs->inst->node_id; memcpy(job->identity.x, cs->inst->my_keys.public_key, 32);
    memcpy(job->identity.ed, cs->inst->my_ed25519_pubkey, 32); strcpy(job->channel, channel);
    job->peer = peer; job->kind = kind; job->join_key = key; job->attempt = attempt;
    job->route = *route;
    if (path) {
        job->path = *path;
        if (!db_async_can_write(cs->inst->db_worker)) {
            int rc = cs_group_read_guard(job) ? cs_group_read_work(cs->inst->topo_sqlite_db, job) : SQLITE_AUTH;
            if (rc == SQLITE_NOTFOUND) {
                chat_core_update_my_member(cs->inst);
                rc = cs_group_read_guard(job) ? cs_group_read_work(cs->inst->topo_sqlite_db, job) : SQLITE_AUTH;
            }
            cs_group_read_done(job, &(struct db_async_result){.status=rc,.sqlite_code=rc});
            cs_group_read_free(job);
            return 0;
        }
    }
    db_async_run(cs->inst->db_worker, &(struct db_async_task){.name="JOIN-group-read",.mode=DB_ASYNC_READ,.data=job,
        .guard=cs_group_read_guard,.work=cs_group_read_work,.done=cs_group_read_done,.cleanup=cs_group_read_free});
    return 0;
}

static int cs_start_channel_offer(struct chat_sync* cs, uint64_t peer) {
    if (cs->inst->ntp.startup_wait) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN offer waits for initial time peer=%llx", (unsigned long long)peer); return 0;
    }
    if (cs->join.server_offer) {
        if (cs->join.offer_link[0]) return cs_send_server_offer(cs);
        if (cs->join.offer_build) return 0;
        cs->join.phase = 6;
        cs->join.offer_build = chat_invite_build_link(cs->inst, cs->join.channel, cs->inst->my_ed25519_pubkey, cs_server_invite_ready, cs);
        if (!cs->join.offer_build) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "cannot build ordinary invitation for server"); return -1; }
        return 0;
    }
    if (db_async_can_write(cs->inst->db_worker)) {
        char channel[65]; strcpy(channel, topo_groups_channel(cs->inst->topo_groups, cs->join.channel));
        cs->join.phase = 6; return cs_group_read_submit(cs, channel, peer, CS_GROUP_OFFER, 0, cs->join.attempt, &cs->join.route, NULL);
    }
    char channel[65]; uint8_t owner[32]; struct group_info_record info;
    uint8_t x[32], ed[32];
    strcpy(channel, topo_groups_channel(cs->inst->topo_groups, cs->join.channel));
    if (topo_node_sqlite_channel_get(cs->inst->topo_sqlite_db, channel, NULL, 0, owner, x, ed, NULL) ||
        group_info_get(cs->inst->topo_sqlite_db, channel, &info)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "offer channel missing ch=%s", channel); return -1;
    }
    cs->invite_send_timer = uasync_set_timeout(cs->inst->ua, CS_INVITE_SEND_TIMEOUT_MS * 10, cs,
                                               cs_invite_send_timeout_cb, "cs_invite_send");
    if (!cs->invite_send_timer) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "offer deadline allocation failed"); return -1; }
    cs->join.phase = 4;
    if (cs_send_channel_invite(cs, cs->inst, channel, peer, info.json, owner, x, ed, info.signature)) return -1;
    if (cs->join.transport_timer) { uasync_cancel_timeout(cs->inst->ua, cs->join.transport_timer); cs->join.transport_timer = NULL; }
    return 0;
}

/* ── Connection callbacks ── */

/* NCD может привязать handle к уже поднятому ETCP после чтения SQLite, без нового ETCP UP. */
static int cs_join_start_if_ready(struct chat_sync* cs, const char* source) {
    if (cs->join.phase != 1) return 0;
    struct ETCP_CONN* conn = node_conn_direct_get_conn(cs->join.handle);
    if (!conn || !conn->initialized || !conn->links_up || conn->close_requested || cs->inst->ntp.startup_wait) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN waiting peer=%016llx source=%s bound=%d initialized=%d links=%d closing=%d time_wait=%d",
            (unsigned long long)cs->join.peer, source, conn != NULL, conn ? conn->initialized : 0, conn ? conn->links_up : 0,
            conn ? conn->close_requested : 0, cs->inst->ntp.startup_wait);
        return 0;
    }
    if (memcmp(cs->join.route.public_key, conn->crypto_ctx.peer_public_key, 32)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN transport key differs from invitation peer=%016llx", (unsigned long long)cs->join.peer);
        return -1;
    }
    if (cs->join.transport_timer) {
        uasync_cancel_timeout(cs->inst->ua, cs->join.transport_timer); cs->join.transport_timer = NULL;
    }
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN transport ready peer=%016llx channel=%llu source=%s role=%s",
        (unsigned long long)cs->join.peer, (unsigned long long)cs->join.channel, source, cs->join.inviter ? "inviter" : "joiner");
    return cs->join.inviter ? cs_start_channel_offer(cs, cs->join.peer) : cs_start_channel_join(cs, cs->join.peer);
}

static void cs_join_transport_event(struct NODE_CONN_DIRECT* handle, enum ncd_event event, void* arg) {
    struct chat_sync* cs = arg;
    if (cs->join.handle != handle) return;
    if (event == NCD_EVENT_UP) {
        if (cs_join_start_if_ready(cs, "ncd_up") < 0) cs_join_finish(cs, CHAT_CONNECT_ERROR);
    } else if (event == NCD_EVENT_CLOSED) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN transport closed peer=%016llx phase=%u", (unsigned long long)cs->join.peer, cs->join.phase);
        if (cs->join.phase == CS_JOIN_WAIT_READY) {
            cs_join_release_transport(cs);
            cs_join_begin_members(cs, "transport_closed", 0);
        } else if (cs->join.phase >= CS_JOIN_ACCEPT_WRITE) cs_join_release_transport(cs);
        else cs_join_finish(cs, CHAT_CONNECT_ERROR);
    }
}

static int cs_join_open_transport(struct chat_sync* cs, uint64_t node_id, struct TOPO_NODE* ni) {
    if (cs->join.handle) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "join already owns transport"); return -1; }
    void* timer = uasync_set_timeout(cs->inst->ua, CS_INFO_REQ_TIMEOUT_MS * 10, cs,
                                     cs_join_connect_timeout, "join_transport_timeout");
    if (!timer) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "cannot reserve join deadline"); return -1; }
    struct NODE_CONN_DIRECT* handle = NULL;
    const uint8_t* key = cs->join.route.public_key;
    int rc = ni ? node_conn_direct_open_node(cs->inst, node_id, cs_join_transport_event, cs, &handle, ni, NULL)
                : node_conn_direct_open_key(cs->inst, node_id, key, cs_join_transport_event, cs, &handle, NULL);
    if (rc == NCD_ERR) {
        uasync_cancel_timeout(cs->inst->ua, timer);
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "cannot acquire join transport peer=%016llx", (unsigned long long)node_id);
        return -1;
    }
    cs->join.handle = handle; cs->join.transport_timer = timer; cs->join.phase = 1;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "join transport acquired peer=%016llx", (unsigned long long)node_id);
    return cs_join_start_if_ready(cs, "open");
}

static void cs_on_conn_up(struct ETCP_CONN* conn, int event, void* arg) {
    struct chat_sync* cs = (struct chat_sync*)arg;
    if (!conn || !cs) return;
    uint8_t peer[32]; memcpy(peer, conn->peer_ed25519_pubkey, sizeof(peer));
    if (cs->inst->ntp.startup_wait || !conn->initialized || !memcmp(peer, (uint8_t[32]){0}, 32) ||
        !memcmp(peer, cs->inst->my_ed25519_pubkey, 32)) return;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "%s: conn_up peer=%s pending_invite=%llu links_up=%d", CS_ID,
        sc_public_key_hex(peer, (char[65]){0}), (unsigned long long)cs->join.channel, conn->links_up);
    cs_schedule_peer_sync(cs, conn, event == ETCP_CBK_EVENT_REINIT ? "reinit" : "transport_up");
    cs_recovery_arm(cs);

    if (cs->join.attempt && !cs->join.route.handle && cs->join.phase == 1 &&
        node_conn_direct_get_conn(cs->join.handle) == conn) {
        if (cs_join_start_if_ready(cs, "etcp_up") < 0) cs_join_finish(cs, CHAT_CONNECT_ERROR);
        cs_on_peer_status_changed(cs, peer, 1);
        return;
    }

    cs_on_peer_status_changed(cs, peer, 1);
    cs_send_all_sound_settings(cs, peer);
}

/* После NTP/4 секунд обработать уже живые соединения и удержанные запросы, без нового транспортного UP. */
void chat_sync_time_ready(struct UTUN_INSTANCE* inst) {
    if (!inst || inst->ntp.startup_wait) return;
    merkle_sync_time_ready(inst);
    struct chat_sync* cs = cs_of(inst);
    if (!cs || !cs->initialized) return;
    if (cs->join.carrier_ready && cs->join.phase == 1 && cs_start_channel_join(cs, cs->join.peer) < 0)
        cs_join_finish(cs, CHAT_CONNECT_ERROR);
    for (struct ll_entry* e = inst->connections ? inst->connections->head : NULL; e; e = e->next) {
        struct ETCP_CONN* conn = ((struct conn_queue_entry*)e->data)->conn;
        if (conn && conn->initialized && conn->links_up && !conn->close_requested) cs_on_conn_up(conn, ETCP_CBK_EVENT_UP, cs);
    }
    cs_arm_sync(cs);
    cs_recovery_arm(cs);
    while (cs->time_wait_frames) {
        struct cs_wait_frame* frame = cs->time_wait_frames;
        cs->time_wait_frames = frame->next; cs->time_wait_count--;
        cs_receive(cs, &frame->route, frame->data, frame->size);
        u_free(frame);
    }
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "chat synchronization released after initial time wait");
}

static void cs_on_conn_down(struct ETCP_CONN* conn, int event, void* arg) { (void)event;
    struct chat_sync* cs = (struct chat_sync*)arg;
    if (!conn || !cs) return;
    uint64_t peer = conn->peer_node_id;
    DEBUG_TRACE(DEBUG_CATEGORY_MEMBER_SYNC, "%s: conn_down peer=%016llx", CS_ID, (unsigned long long)peer);
    chat_join_relay_closed(cs->inst, conn->crypto_ctx.peer_public_key, 0);
    cs_cancel_sync(cs, conn, 0, "transport_down");

    cs_on_peer_status_changed(cs, conn->peer_ed25519_pubkey, 0);

    if (cs->join.peer == peer && !cs->join.route.handle &&
        !memcmp(cs->join.route.public_key, conn->crypto_ctx.peer_public_key, 32)) {
        if (cs->join.phase == CS_JOIN_WAIT_READY) {
            cs_join_begin_members(cs, "transport_down", 0);
            return;
        }
        if (cs->join.phase == 5 || cs->join.phase >= CS_JOIN_ACCEPT_WRITE) return;
        if (!cs->join.inviter && cs->join.phase != 1) {
            uint64_t attempt;
            if (RAND_bytes((uint8_t*)&attempt, sizeof(attempt)) != 1 || !attempt || attempt == cs->join.attempt) {
                DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN reconnect attempt generation failed peer=%016llx", (unsigned long long)peer);
                cs_join_finish(cs, CHAT_CONNECT_ERROR);
                return;
            }
            DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "JOIN reconnect peer=%016llx old_attempt=%016llx attempt=%016llx",
                (unsigned long long)peer, (unsigned long long)cs->join.attempt, (unsigned long long)attempt);
            cs->join.attempt = attempt;
        }
        cs_cancel_proto_timers(cs);
        cs->join.phase = 1;
        if (cs->join.transport_timer) uasync_cancel_timeout(cs->inst->ua, cs->join.transport_timer);
        cs->join.transport_timer = uasync_set_timeout(cs->inst->ua, CS_INFO_REQ_TIMEOUT_MS * 10,
                                                     cs, cs_join_connect_timeout, "join_reconnect_timeout");
        if (!cs->join.transport_timer) { cs_join_finish(cs, CHAT_CONNECT_ERROR); return; }
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "%s: conn down while waiting invite resp peer=%016llx, timers cancelled, state kept for retry on reconnect",
                   CS_ID, (unsigned long long)peer);
        /* invite state survives connection flaps: only timeout or explicit protocol end clears it */
    }
}

static void cs_on_conn_status(struct ETCP_CONN* conn, int status, void* arg) {
    switch (status) {
    case ETCP_CONN_STATUS_UP:   cs_on_conn_up(conn, ETCP_CBK_EVENT_UP, arg); break;
    case ETCP_CONN_STATUS_REINIT:
        cs_on_conn_down(conn, ETCP_CBK_EVENT_DOWN, arg);
        cs_on_conn_up(conn, ETCP_CBK_EVENT_REINIT, arg);
        break;
    case ETCP_CONN_STATUS_DOWN:
    case ETCP_CONN_STATUS_DELETE: cs_on_conn_down(conn, ETCP_CBK_EVENT_DOWN, arg); break;
    default: break;
    }
}

/* ── Periodic refresh from DB ── */

static void cs_cache_free(struct channel_cache* channels, int count) {
    for (int i = 0; i < count; i++) u_free(channels[i].peers);
    u_free(channels);
}

/* Новый SQL-снимок строится целиком без runtime/callbacks; ошибка сохраняет прежний кеш. */
int chat_sync_catalog_read(sqlite3* db, struct chat_sync_catalog** out) {
    if (out) *out = NULL;
    if (!db || !out) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "channel catalog read requires database/output"); return SQLITE_MISUSE; }
    struct channel_cache* channels = NULL; int count = 0, capacity = 0;
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT channel_id FROM channels ORDER BY created_at,channel_id", -1, &st, NULL);
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char* id = (const char*)sqlite3_column_text(st, 0);
        int bytes = sqlite3_column_bytes(st, 0);
        if (!id || bytes != 64 || strlen(id) != (size_t)bytes || !chat_channel_valid(id)) {
            rc = SQLITE_CORRUPT; break;
        }
        if (count == capacity) {
            int next = capacity ? capacity * 2 : 8;
            if (capacity >= 65536) { rc = SQLITE_TOOBIG; break; }
            void* grown = u_realloc(channels, (size_t)next * sizeof(*channels));
            if (!grown) { rc = SQLITE_NOMEM; break; }
            channels = grown; capacity = next;
        }
        struct channel_cache* channel = &channels[count++]; memset(channel, 0, sizeof(*channel)); strcpy(channel->channel_id, id);
        char sql[192]; snprintf(sql, sizeof(sql), "SELECT ed25519_pubkey FROM peers_%s WHERE record_state=0 AND deleted=0 ORDER BY ed25519_pubkey", id);
        sqlite3_stmt* peers = NULL; int peer_capacity = 0;
        int prc = sqlite3_prepare_v2(db, sql, -1, &peers, NULL);
        if (prc == SQLITE_OK) while ((prc = sqlite3_step(peers)) == SQLITE_ROW) {
            if (sqlite3_column_type(peers, 0) != SQLITE_BLOB || sqlite3_column_bytes(peers, 0) != 32) { prc = SQLITE_CORRUPT; break; }
            if (channel->peer_count == peer_capacity) {
                int next = peer_capacity ? peer_capacity * 2 : 16;
                if (peer_capacity >= 65536) { prc = SQLITE_TOOBIG; break; }
                void* grown = u_realloc(channel->peers, (size_t)next * sizeof(*channel->peers));
                if (!grown) { prc = SQLITE_NOMEM; break; }
                channel->peers = grown; peer_capacity = next;
            }
            memcpy(channel->peers[channel->peer_count++], sqlite3_column_blob(peers, 0), 32);
        }
        int final = sqlite3_finalize(peers);
        if (prc != SQLITE_DONE || final != SQLITE_OK) { rc = prc == SQLITE_DONE ? final : prc; break; }
    }
    int final = sqlite3_finalize(st);
    char (*notifications)[65] = count ? u_malloc((size_t)count * sizeof(*notifications)) : NULL;
    if (rc != SQLITE_DONE || final != SQLITE_OK || (count && !notifications)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "channel cache refresh failed rc=%d finalize=%d channels=%d: %s",
            rc, final, count, sqlite3_errmsg(db));
        u_free(notifications); cs_cache_free(channels, count);
        return rc != SQLITE_DONE ? rc : final != SQLITE_OK ? final : SQLITE_NOMEM;
    }
    struct chat_sync_catalog* catalog = u_calloc(1, sizeof(*catalog));
    if (!catalog) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "channel catalog allocation failed");
        u_free(notifications); cs_cache_free(channels, count); return SQLITE_NOMEM;
    }
    for (int i = 0; i < count; i++) strcpy(notifications[i], channels[i].channel_id);
    catalog->channels = channels; catalog->count = count; catalog->notifications = notifications;
    struct join_recovery** tail = &catalog->recovery;
    st = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT channel,peer,inviter,hints FROM chat_join_recovery ORDER BY channel", -1, &st, NULL);
    if (!rc) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char* channel = (const char*)sqlite3_column_text(st, 0);
        const uint8_t* peer = sqlite3_column_blob(st, 1);
        const uint8_t* inviter = sqlite3_column_blob(st, 2);
        uint8_t inviter_x[32];
        if (!chat_channel_valid(channel) || !peer || sqlite3_column_bytes(st, 1) != 32 || !inviter ||
            sqlite3_column_bytes(st, 2) != 32 || sc_identity_public_x("JOIN recovery inviter", inviter, inviter_x) != SC_OK) { rc = SQLITE_CORRUPT; break; }
        struct join_recovery* item = u_calloc(1, sizeof(*item));
        if (!item) { rc = SQLITE_NOMEM; break; }
        if (invite_deserialize_addrs(sqlite3_column_blob(st, 3), (size_t)sqlite3_column_bytes(st, 3), &item->hints)) {
            u_free(item); rc = SQLITE_CORRUPT; break;
        }
        strcpy(item->channel, channel); memcpy(item->hints.pubkey, peer, 32);
        memcpy(item->hints.inviter_pubkey, inviter, 32);
        *tail = item; tail = &item->next;
    }
    final = sqlite3_finalize(st);
    if (rc == SQLITE_DONE) rc = final;
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN recovery catalog read rc=%d (%s)", rc, sqlite3_errstr(rc));
        chat_sync_catalog_free(catalog); return rc;
    }
    *out = catalog;
    return SQLITE_OK;
}
void chat_sync_catalog_free(struct chat_sync_catalog* catalog) {
    if (!catalog) return;
    while (catalog->recovery) {
        struct join_recovery* next = catalog->recovery->next;
        u_free(catalog->recovery); catalog->recovery = next;
    }
    cs_cache_free(catalog->channels, catalog->count); u_free(catalog->notifications); u_free(catalog);
}
static void cs_catalog_install(struct chat_sync* cs, struct chat_sync_catalog* catalog) {
    struct UTUN_INSTANCE* inst = cs->inst; uint64_t generation = cs->generation; int count = catalog->count;
    cs_cache_free(cs->channels, cs->channel_count);
    cs->channels = catalog->channels; cs->channel_count = count; catalog->channels = NULL; catalog->count = 0;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "channel cache refreshed channels=%d", count);
    for (int i = 0; i < count; i++) {
        struct chat_sync* current = cs_of(inst);
        if (!current || !current->initialized || current->generation != generation) break;
        cs_post_channel_online(current, catalog->notifications[i]);
    }
}
struct cs_refresh_job {
    struct UTUN_INSTANCE* inst; /* Только guard/done. */
    uint64_t core_generation, generation, revision;
    struct chat_sync_catalog* catalog;
};
static struct chat_sync* cs_refresh_current(struct cs_refresh_job* job) {
    if (!job->core_generation || job->inst->destroying || chat_core_generation(job->inst) != job->core_generation ||
        (db_async_can_write(job->inst->db_worker) && !job->inst->group_started)) return NULL;
    struct chat_sync* cs = cs_of(job->inst);
    return cs && cs->initialized && cs->generation == job->generation ? cs : NULL;
}
static int cs_refresh_guard(void* data) {
    struct cs_refresh_job* job = data; struct chat_sync* cs = cs_refresh_current(job);
    if (!cs) return 0; job->revision = cs->refresh_revision; return 1;
}
static int cs_refresh_work(sqlite3* db, void* data) {
    struct cs_refresh_job* job = data; return chat_sync_catalog_read(db, &job->catalog);
}
static int cs_refresh_submit(struct chat_sync* cs);
static void cs_refresh_done(void* data, const struct db_async_result* result) {
    struct cs_refresh_job* job = data; struct chat_sync* cs = cs_refresh_current(job);
    if (!cs) return;
    cs->refresh_running = 0;
    if (result->status) DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "channel catalog READ failed status=%d rc=%d: %s",
        result->status, result->sqlite_code, result->message);
    else if (cs->refresh_revision == job->revision) cs_catalog_install(cs, job->catalog);
    cs = cs_refresh_current(job);
    if (cs && cs->refresh_revision != job->revision && !cs->refresh_running) cs_refresh_submit(cs);
}
static void cs_refresh_cleanup(void* data) {
    struct cs_refresh_job* job = data; chat_sync_catalog_free(job->catalog); u_free(job);
}
static int cs_refresh_submit(struct chat_sync* cs) {
    struct cs_refresh_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "channel catalog READ request allocation failed"); return -1; }
    job->inst = cs->inst; job->core_generation = chat_core_generation(cs->inst); job->generation = cs->generation;
    cs->refresh_running = 1;
    struct db_async_task task = {.name="chat-sync-catalog", .mode=DB_ASYNC_READ, .data=job, .guard=cs_refresh_guard,
        .work=cs_refresh_work, .done=cs_refresh_done, .cleanup=cs_refresh_cleanup};
    db_async_run(cs->inst->db_worker, &task); return 0;
}
static int cs_refresh_channels(struct chat_sync* cs) {
    if (!cs || !cs->initialized) return -1;
    cs->refresh_revision++;
    if (db_async_can_write(cs->inst->db_worker)) return cs->refresh_running ? 0 : cs_refresh_submit(cs);
    struct chat_sync_catalog* catalog = NULL;
    int rc = chat_sync_catalog_read(cs->inst->topo_sqlite_db, &catalog);
    if (rc == SQLITE_OK) cs_catalog_install(cs, catalog);
    chat_sync_catalog_free(catalog); return rc == SQLITE_OK ? 0 : -1;
}

static void refresh_timer_cb(void* arg) {
    struct chat_sync* cs = (struct chat_sync*)arg;
    if (!cs || !cs->initialized) return;
    cs->refresh_timer = NULL;
#ifdef UTUN_HAVE_STANDBY
    if (standby_get_sleep_tb() > 0) {
        cs->refresh_wait = standby_wait(cs, refresh_timer_cb);
        return;
    }
    cs->refresh_wait = NULL;
#endif
    cs_refresh_channels(cs);
    cs->refresh_timer = uasync_set_timeout(cs->inst->ua,
        30u * 10000u, cs, refresh_timer_cb, "cs_refresh");
    if (!cs->refresh_timer) DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "channel cache refresh timer allocation failed");
}

/* ── TTL cleanup ── */


/* ── Public API ── */

int chat_sync_activate(struct UTUN_INSTANCE* inst, struct chat_sync_catalog* catalog) {
    if (!inst || !inst->ua || !chat_core_is_initialized(inst) || !inst->msync || !CC(inst)->group_avatar || !catalog) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "chat_sync activation requires prepared core/MEMBERS/avatar/catalog"); return -1;
    }
    if (inst->chat_sync) return 0;
    if (CC(inst)->sync_generation == UINT64_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "chat_sync generation exhausted"); return -1;
    }
    DEBUG_TRACE(DEBUG_CATEGORY_MEMBER_SYNC, "%s: init", CS_ID);
    struct chat_sync* cs = u_calloc(1, sizeof(*cs));
    if (!cs) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "chat_sync context allocation failed"); return -1; }
    cs->inst = inst; cs->generation = ++CC(inst)->sync_generation;
    cs->initialized = 1;
    inst->chat_sync = cs;
    cs->port = chat_join_port_create(inst);
    if (!cs->port) { chat_join_port_close(cs->port); chat_join_port_release(cs->port); inst->chat_sync = NULL; u_free(cs); return -1; }

    if (etcp_bind(inst, ETCP_ID_CHAT_SYNC, chat_sync_recv_cb)) goto fail;
    cs->direct_bound = 1;
    etcp_add_conn_status_cbk(inst, cs_on_conn_status, cs);
    struct etcp_status_cbk_entry* status = inst->conn_status_cbks;
    while (status && (status->fn != cs_on_conn_status || status->arg != cs)) status = status->next;
    if (!status) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "chat_sync status subscription failed"); goto fail; }

    cs->recovery = catalog->recovery; catalog->recovery = NULL;
    cs_catalog_install(cs, catalog);
    if (cs_of(inst) != cs) { DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "chat_sync activation cancelled by catalog callback"); return -1; }

    cs->refresh_timer = uasync_set_timeout(inst->ua, 50000u, cs,
        refresh_timer_cb, "cs_refresh");
    if (!cs->refresh_timer) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "chat_sync refresh timer allocation failed"); goto fail; }
    cs->info_req_timer = NULL;
    cs->join_timer = NULL;

    utun_add_peer_sleep_cbk(inst, cs_on_peer_sleep, cs);
    struct peer_sleep_cbk_entry* sleep = inst->peer_sleep_cbks;
    while (sleep && (sleep->removed || sleep->fn != cs_on_peer_sleep || sleep->arg != cs)) sleep = sleep->next;
    if (!sleep) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "chat_sync sleep subscription failed"); goto fail; }
    if (chat_admin_init(inst)) goto fail;
    cs->admin_bound = 1;
    if (chat_join_init(inst)) goto fail;
    cs->join_bound = 1;

    /* После restart транспорт может уже быть UP: нового conn_up не будет. */
    for (struct ll_entry* e = inst->connections ? inst->connections->head : NULL; e; e = e->next) {
        struct conn_queue_entry* ce = (struct conn_queue_entry*)e->data;
        cs_schedule_peer_sync(cs, ce->conn, "startup");
    }
    cs_recovery_arm(cs);

    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "%s: initialized", CS_ID);
    return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "chat_sync initialization failed; releasing partial runtime");
    chat_sync_destroy(inst); return -1;
}
int chat_sync_init(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->ua || !chat_core_is_initialized(inst)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "chat_sync legacy init requires chat_core"); return -1;
    }
    if (inst->chat_sync) return 0;
    if (!inst->topo_sqlite_db || db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "chat_sync legacy init requires synchronous database owner"); return -1;
    }
    if (member_sync_init(inst) < 0) return -1;
    if (group_avatar_init(inst) < 0) { member_sync_destroy(inst); return -1; }
    struct chat_sync_catalog* catalog = NULL;
    int result = chat_sync_catalog_read(inst->topo_sqlite_db, &catalog);
    if (result == SQLITE_OK) result = chat_sync_activate(inst, catalog);
    else { group_avatar_destroy(inst); member_sync_destroy(inst); }
    chat_sync_catalog_free(catalog); return result == 0 ? 0 : -1;
}

int chat_sync_is_initialized(struct UTUN_INSTANCE* inst) {
    struct chat_sync* cs = cs_of(inst); return cs && cs->initialized;
}
void chat_sync_quiesce(struct UTUN_INSTANCE* inst) {
    struct chat_sync* cs = cs_of(inst);
    if (!cs) return;
    cs->initialized = 0; chat_join_port_close(cs->port); cs_join_finish(cs, CHAT_CONNECT_CANCELLED);
}
void chat_sync_cancel_join(struct UTUN_INSTANCE* inst, uint64_t request) {
    struct chat_sync* cs = cs_of(inst);
    if (cs && cs->join.attempt && cs->join.request == request) {
        if (cs->join.phase == CS_JOIN_CANCEL_PENDING) return;
        if (cs->join.recovery_channel[0]) {
            if (cs_recovery_finish(cs, cs->join.recovery_channel, cs->join.attempt, CHAT_CONNECT_CANCELLED))
                cs_join_finish(cs, CHAT_CONNECT_ERROR);
            return;
        }
        cs_join_finish(cs, CHAT_CONNECT_CANCELLED);
        return;
    }
    if (!cs || !cs->initialized || !request) return;
    for (struct join_recovery* item = cs->recovery; item; item = item->next) {
        if (item->request != request) continue;
        if (!strcmp(cs->join.recovery_channel, item->channel)) return; /* Уже обрабатывается новая попытка этой группы. */
        cs_recovery_finish(cs, item->channel, 0, CHAT_CONNECT_CANCELLED);
        return;
    }
}

void chat_sync_destroy(struct UTUN_INSTANCE* inst) {
    struct chat_sync* cs = cs_of(inst);
    if (!cs || !inst) return;
    DEBUG_TRACE(DEBUG_CATEGORY_MEMBER_SYNC, "%s: destroy", CS_ID);
    cs->initialized = 0;
    chat_join_port_close(cs->port);
    cs_join_finish(cs, CHAT_CONNECT_CANCELLED);
    if (cs->recovery_timer) uasync_cancel_timeout(inst->ua, cs->recovery_timer);
    while (cs->recovery) cs_recovery_remove(cs, cs->recovery->channel);
    cs_cancel_sync(cs, 0, 0, "service_stop");
    cs_join_clear_ready(cs);
    while (cs->time_wait_frames) {
        struct cs_wait_frame* frame = cs->time_wait_frames;
        cs->time_wait_frames = frame->next; u_free(frame);
    }
    cs->time_wait_count = 0;
    if (cs->invite_send_timer) { uasync_cancel_timeout(inst->ua, cs->invite_send_timer); cs->invite_send_timer = NULL; }
    utun_remove_peer_sleep_cbk(inst, cs_on_peer_sleep, cs);
    group_avatar_destroy(inst);
    member_sync_destroy(inst);
    if (cs->admin_bound) chat_admin_destroy(inst);
    if (cs->join_bound) chat_join_destroy(inst);
    cs->initialized = 0; inst->chat_sync = NULL;

    if (cs->direct_bound) etcp_unbind(inst, ETCP_ID_CHAT_SYNC);
    etcp_remove_conn_status_cbk(inst, cs_on_conn_status, cs);
    if (cs->refresh_timer) { uasync_cancel_timeout(inst->ua, cs->refresh_timer); cs->refresh_timer = NULL; }
#ifdef UTUN_HAVE_STANDBY
    if (cs->refresh_wait) { standby_wait_cancel(cs->refresh_wait); cs->refresh_wait = NULL; }
#endif
    cs_cancel_proto_timers(cs);
    cs_join_release_transport(cs);

    cs_cache_free(cs->channels, cs->channel_count);
    chat_join_port_release(cs->port);
    u_free(cs);
}

struct chat_join_port* chat_sync_join_port(struct UTUN_INSTANCE* inst) {
    struct chat_sync* cs = cs_of(inst);
    if (!cs || !cs->initialized) return NULL;
    chat_join_port_retain(cs->port); return cs->port;
}

void chat_sync_retry_channels_on_socket_change(struct UTUN_INSTANCE* inst) {
    struct chat_sync* cs = cs_of(inst);
    if (!cs || !cs->initialized || !inst->topo_groups) return;
    for (int i = 0; i < cs->channel_count; i++) {
        uint64_t gid = topo_groups_channel_slot(cs->inst->topo_groups, cs->channels[i].channel_id);
        struct TOPO_GROUP* group = topo_groups_find(inst->topo_groups, gid);
        if (!group || !group->connect) continue;
        topo_group_connect_restart(group);
    }
}

void chat_sync_on_channel_deleted(struct UTUN_INSTANCE* inst, const char* ch_id, const uint8_t group_ed[32]) {
    struct chat_sync* cs = cs_of(inst);
    if (!cs) return;
    cs_recovery_remove(cs, ch_id);
    uint64_t channel = topo_groups_channel_slot(inst->topo_groups, ch_id);
    if (!channel) {
        if (!strcmp(cs->join.recovery_channel, ch_id)) cs_join_finish(cs, CHAT_CONNECT_CANCELLED);
        return;
    }
    cs_cancel_sync(cs, 0, channel, "channel_deleted");
    for (int i = 0; i < cs->pending_inv_in_count; ) {
        if (group_ed && !memcmp(cs->pending_inv_in[i].group_ed, group_ed, 32)) cs_invitation_remove(cs, i); else i++;
    }
    for (int i = 0; i < cs->channel_count; i++) {
        if (strcmp(cs->channels[i].channel_id, ch_id) != 0) continue;
        if (cs->channels[i].peers) u_free(cs->channels[i].peers);
        memmove(&cs->channels[i], &cs->channels[i + 1],
                (size_t)(cs->channel_count - i - 1) * sizeof(struct channel_cache));
        cs->channel_count--;
        DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "%s: channel removed from cache ch=%s", CS_ID, ch_id);
        break;
    }
    if (cs->join.channel == channel || !strcmp(cs->join.recovery_channel, ch_id)) cs_join_finish(cs, CHAT_CONNECT_CANCELLED);
}

static void cs_node_clear(struct TOPO_NODE* node) {
    while (node->v4_addrs) { struct TOPO_ADDR4* next = node->v4_addrs->next; u_free(node->v4_addrs); node->v4_addrs = next; }
    while (node->v6_addrs) { struct TOPO_ADDR6* next = node->v6_addrs->next; u_free(node->v6_addrs); node->v6_addrs = next; }
    while (node->reality_socks) {
        struct TOPO_REALITY_SOCK* next = node->reality_socks->next; u_free(node->reality_socks); node->reality_socks = next;
    }
}

/* Подготовить временную адресную подсказку для NCD из проверенных полей. */
static int cs_node_from_addresses(struct TOPO_NODE* node, const uint8_t peer_x[32], const struct InviteAddr* addresses,
    unsigned count, const struct InviteReality* reality) {
    node->node_id = sc_derive_node_id_from_pubkey(peer_x);
    memcpy(node->public_key, peer_x, 32);
    for (unsigned i = 0; i < count; i++) {
        const struct InviteAddr* source = &addresses[i];
        if (source->family == 4) {
            struct TOPO_ADDR4* a = u_calloc(1, sizeof(*a));
            if (!a) goto failure;
            memcpy(a->addr, source->address, 4); a->port = source->port;
            a->socket_id = source->socketId; a->protocol = source->proto; a->type = TOPO_ADDR_NAT;
            a->next = node->v4_addrs; node->v4_addrs = a;
        } else {
            struct TOPO_ADDR6* a = u_calloc(1, sizeof(*a));
            if (!a) goto failure;
            memcpy(a->addr, source->address, 16); a->port = source->port;
            a->socket_id = source->socketId; a->protocol = source->proto; a->type = TOPO_ADDR_NAT;
            a->next = node->v6_addrs; node->v6_addrs = a;
        }
        if (source->proto & INVITE_PROTO_REALITY) {
            if (!reality) goto failure;
            struct TOPO_REALITY_SOCK* r = u_calloc(1, sizeof(*r));
            if (!r) goto failure;
            r->socket_id = source->socketId; memcpy(r->short_id, reality->short_id, 8);
            memcpy(r->server_pubkey, reality->server_pubkey, 32); memcpy(r->version, reality->version, 3);
            snprintf(r->server_name, sizeof(r->server_name), "%s", reality->server_name);
            r->next = node->reality_socks; node->reality_socks = r;
        }
    }
    if (node->node_id) return 0;
failure:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN could not prepare transport addresses peer=%s count=%u",
        sc_public_key_hex(peer_x, (char[65]){0}), count);
    return -1;
}

static int cs_connect_invite(struct UTUN_INSTANCE* inst, uint64_t request, const struct InviteData* data, uint64_t offer_channel) {
    struct chat_sync* cs = cs_of(inst);
    const struct InviteData invite = *data;
    int rc = cs_join_reserve(cs, offer_channel, invite.pubkey, request, offer_channel != 0);
    if (rc) return rc;
    cs->join.server_offer = offer_channel && invite.kind == INVITE_KIND_SERVER;
    if (cs->join.server_offer) {
        memcpy(cs->join.add_key, invite.add_key, sizeof(cs->join.add_key));
        cs->join.operation_timer = uasync_set_timeout(inst->ua, 300000 + CS_MEMBERS_TIMEOUT_MS * 10, cs, cs_server_operation_timeout, "server_join_deadline");
        if (!cs->join.operation_timer) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "server join deadline allocation failed");
            cs_join_end(cs, CHAT_CONNECT_ERROR, 0); return CHAT_CONNECT_ERROR;
        }
    }
    struct TOPO_NODE node = {0};
    if (cs_node_from_addresses(&node, invite.pubkey, invite.addrs, invite.addrCount, &invite.reality)) goto failure;
    if (!offer_channel) {
        cs->join.recovery_hints.addrCount = invite.addrCount;
        cs->join.recovery_hints.reality = invite.reality;
        memcpy(cs->join.recovery_hints.addrs, invite.addrs, sizeof(invite.addrs));
        cs->join.key = invite.join_key;
        memcpy(cs->join.inviter_ed, invite.inviter_pubkey, 32);
        if (sc_identity_public_x("JOIN inviter", cs->join.inviter_ed, cs->join.inviter_pubkey) != SC_OK) goto failure;
    }
    if (!offer_channel && inst->join_carrier && !cs_find_conn_for_key(inst, invite.pubkey)) {
        uint64_t handle = 0;
        int selected = inst->join_carrier->open(inst->join_carrier->ctx, invite.pubkey, &handle);
        if (selected < 0 || (!selected && !handle)) goto failure;
        if (!selected) {
            cs->join.route.handle = handle; cs->join.phase = 1;
            cs->join.transport_timer = uasync_set_timeout(inst->ua, CS_CARRIER_TIMEOUT_MS * 10, cs,
                                                         cs_join_connect_timeout, "join_carrier_open");
            if (!cs->join.transport_timer) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN carrier deadline allocation failed"); goto failure; }
            DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN carrier selected peer=%016llx handle=%016llx attempt=%016llx",
                       (unsigned long long)node.node_id, (unsigned long long)handle, (unsigned long long)cs->join.attempt);
            cs_node_clear(&node); return 0;
        }
    }
    rc = cs_join_open_transport(cs, node.node_id, &node);
    if (rc < 0) goto failure;
    cs_node_clear(&node);
    return 0;
failure:
    DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "join preparation failed request=%llu", (unsigned long long)request);
    cs_node_clear(&node);
    cs_join_end(cs, CHAT_CONNECT_ERROR, 0);
    return CHAT_CONNECT_ERROR;
}

int chat_sync_join_link(struct UTUN_INSTANCE* inst, uint64_t request, const char* link, size_t len) {
    struct InviteData invite;
    if (invite_link_decode(link, len, &invite, NULL, 0) || invite.kind != INVITE_KIND_CHANNEL) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "ordinary JOIN requires a channel invitation"); return CHAT_CONNECT_INVALID;
    }
    return cs_connect_invite(inst, request, &invite, 0);
}
/* Проверка права приглашать общая для обоих способов выбора адресата. */
static int cs_offer_permission(struct UTUN_INSTANCE* inst, uint64_t request, uint64_t channel, const uint8_t peer_x[32]) {
    if (!inst || !channel) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "offer requires an active instance and a local channel request=%llu", (unsigned long long)request);
        return CHAT_CONNECT_INVALID;
    }
    const char* ch = topo_groups_channel(inst->topo_groups, channel);
    int allowed = group_policy_can_invite(inst, ch, inst->my_ed25519_pubkey);
    if (allowed < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "offer permission check failed request=%llu channel=%s",
            (unsigned long long)request, ch); return CHAT_CONNECT_ERROR;
    }
    if (!allowed) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "offer denied: no invitation permission request=%llu channel=%s self=%016llx target=%s",
            (unsigned long long)request, ch, (unsigned long long)inst->node_id, sc_public_key_hex(peer_x, (char[65]){0}));
        return CHAT_CONNECT_INVITE_DENIED;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "offer permission granted request=%llu channel=%s target=%s",
        (unsigned long long)request, ch, sc_public_key_hex(peer_x, (char[65]){0}));
    return 0;
}

int chat_sync_offer_link(struct UTUN_INSTANCE* inst, uint64_t request, uint64_t channel, const char* link, size_t len) {
    struct InviteData invite;
    if (invite_link_decode(link, len, &invite, NULL, 0)) return CHAT_CONNECT_INVALID;
    int rc = cs_offer_permission(inst, request, channel, invite.pubkey);
    return rc ? rc : cs_connect_invite(inst, request, &invite, channel);
}

int chat_sync_invite_to_channel(struct UTUN_INSTANCE* inst, uint64_t channel, const uint8_t peer_ed[32], const struct InviteAddr* address) {
    uint8_t peer_x[32];
    if (sc_identity_public_x("address invitation", peer_ed, peer_x) != SC_OK) return CHAT_CONNECT_INVALID;
    if (address && ((address->family != 4 && address->family != 6) || !address->port ||
        (address->proto != INVITE_PROTO_UDP && address->proto != INVITE_PROTO_TCP))) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "address invitation invalid endpoint peer=%s family=%d proto=%u port=%u",
            sc_public_key_hex(peer_ed, (char[65]){0}), address->family, address->proto, address->port);
        return CHAT_CONNECT_INVALID;
    }
    int rc = cs_offer_permission(inst, 0, channel, peer_x);
    if (rc) return rc;
    struct chat_sync* cs = cs_of(inst);
    rc = cs_join_reserve(cs, channel, peer_x, 0, 1);
    if (rc) return rc;
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "address invitation reserved channel=%llu peer=%s attempt=%016llx family=%d proto=%u port=%u",
        (unsigned long long)channel, sc_public_key_hex(peer_ed, (char[65]){0}), (unsigned long long)cs->join.attempt,
        address ? address->family : 0, address ? address->proto : 0u, address ? address->port : 0u);
    struct TOPO_NODE node = {0};
    if (!address || !cs_node_from_addresses(&node, peer_x, address, 1, NULL))
        rc = cs_join_open_transport(cs, cs->join.peer, address ? &node : NULL);
    else rc = -1;
    cs_node_clear(&node);
    if (rc >= 0) return 0;
    cs_join_end(cs, CHAT_CONNECT_ERROR, 0);
    return CHAT_CONNECT_ERROR;
}

/* ─── chat_sync_join_channel: join channel via already-connected peer ─── */

static int cs_join_existing(struct UTUN_INSTANCE* inst, uint64_t key,
                            const uint8_t inviter_pubkey[32], const uint8_t inviter_ed[32], const uint8_t group_ed[32]) {
    struct chat_sync* cs = cs_of(inst);
    struct TOPO_GROUP* group = topo_groups_find_wire(inst->topo_groups, group_ed);
    uint64_t peer = sc_derive_node_id_from_pubkey(inviter_pubkey);
    if (!inviter_ed || sc_identity_check("JOIN push inviter", inviter_pubkey, inviter_ed) != SC_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN push inviter identity unavailable peer=%016llx", (unsigned long long)peer);
        return CHAT_CONNECT_AUTH;
    }
    int rc = cs_join_reserve(cs, group ? group->group_id : 0, inviter_pubkey, 0, 0);
    if (rc) return rc;
    cs->join.key = key;
    memcpy(cs->join.expected_group, group_ed, 32);
    memcpy(cs->join.inviter_pubkey, inviter_pubkey, 32);
    memcpy(cs->join.inviter_ed, inviter_ed, 32);
    if (cs_join_open_transport(cs, peer, NULL) < 0) {
        cs_join_finish(cs, CHAT_CONNECT_ERROR); return CHAT_CONNECT_ERROR;
    }
    return 0;
}
static struct TOPO_GROUP* cs_join_target_group(struct TOPO_GROUPS* groups, const uint8_t ed[32]) {
    return topo_groups_find_channel(groups, sc_public_key_hex(ed, (char[65]){0}));
}

static int cs_recovery_store(sqlite3* db, const struct join_recovery* item);

struct cs_accept_job {
    struct chat_sync_join_input input;
    struct chat_sync_join_prepared prepared;
    struct join_recovery* recovery;
    struct UTUN_INSTANCE* inst; /* Только uasync guard/done; worker использует собственные данные. */
    uint64_t core_generation, generation, group_generation, attempt, expires;
    void* retry_timer;
    unsigned retry_delay;
    char channel[65], name[128];
    uint8_t bytes[]; /* Собственная копия payload, pointers input не переживают job. */
};
static struct chat_sync* cs_accept_active(const struct cs_accept_job* job) {
    struct UTUN_INSTANCE* inst = job->inst;
    if (!job->core_generation || inst->destroying || !inst->group_started || !inst->topo_groups ||
        chat_core_generation(inst) != job->core_generation) return NULL;
    struct chat_sync* cs = cs_of(inst);
    if (!cs || !cs->initialized || cs->generation != job->generation || cs->join.phase != CS_JOIN_ACCEPT_WRITE ||
        cs->join.attempt != job->attempt) return NULL;
    return cs;
}

static int cs_accept_group_current(const struct cs_accept_job* job) {
    struct TOPO_GROUP* group = topo_groups_find_channel(job->inst->topo_groups, job->channel);
    return (group ? group->generation : 0) == job->group_generation && (!group || !group->stopping);
}

static int cs_accept_guard(void* data) { return cs_accept_active(data) && cs_accept_group_current(data); }

static int cs_accept_work(sqlite3* db, void* data) {
    struct cs_accept_job* job = data;
    int rc = chat_sync_join_accept_write(db, &job->input, &job->prepared);
    if (!rc) rc = cs_recovery_store(db, job->recovery);
    return rc;
}

static int cs_accept_send(struct chat_sync* cs, const struct cs_accept_job* job) {
    const struct chat_sync_join_prepared* prepared = &job->prepared;
    struct join_member_data member = {.x25519=job->input.x,.ed25519=job->input.identity.public_key,
        .join_sig=prepared->join_signature,.join_ts=prepared->join_ts,.update_sig=prepared->update_signature,.update_ts=prepared->update_ts};
    snprintf(member.userinfo, sizeof(member.userinfo), "%s", prepared->userinfo);
    uint8_t request[JOIN_REQUEST_MAX_SIZE + 40]; size_t size = 0;
    memcpy(request, job->input.group_keys + 32, 32);
    uint64_t wire_key = htobe64(cs->join.key); memcpy(request + 32, &wire_key, 8);
    if (chat_join_serialize_member(&member, request + 40, sizeof(request) - 40, &size)) return -1;
    unsigned timeout = cs->join.route.handle ? CS_CARRIER_TIMEOUT_MS : CS_JOIN_TIMEOUT_MS;
    cs->join_timer = uasync_set_timeout(job->inst->ua, timeout * 10, cs, cs_join_timeout_cb, "cs_join");
    if (!cs->join_timer) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN request deadline allocation failed ch=%s", job->channel); return -1; }
    cs->join.phase = CS_JOIN_WAIT_READY;
    int rc = cs_guest_send(cs, CS_MSG_JOIN_REQUEST, request, size + 40);
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN recovery committed; request send ch=%s attempt=%016llx peer=%016llx rc=%d",
        job->channel, (unsigned long long)job->attempt, (unsigned long long)job->input.peer, rc);
    return rc;
}

static void cs_accept_free(void* data) {
    struct cs_accept_job* job = data;
    if (job->retry_timer) return; /* До повторной подачи задание принадлежит таймеру. */
    group_policy_discard(job->prepared.admission);
    u_free(job->recovery);
    OPENSSL_cleanse(job->input.identity.private_key, sizeof(job->input.identity.private_key));
    u_free(job);
}

static void cs_accept_drop_retry(struct chat_sync* cs) {
    struct cs_accept_job* job = cs->join.accept_retry;
    if (!job) return;
    cs->join.accept_retry = NULL;
    uasync_cancel_timeout(cs->inst->ua, job->retry_timer); job->retry_timer = NULL;
    cs_accept_free(job);
}

static void cs_accept_run(struct cs_accept_job* job);

static void cs_accept_retry(void* data) {
    struct cs_accept_job* job = data;
    struct chat_sync* cs = cs_of(job->inst);
    job->retry_timer = NULL;
    if (cs && cs->join.accept_retry == job) {
        cs->join.accept_retry = NULL;
        int current = cs_accept_guard(job);
        if (current && get_time_tb() < job->expires) {
            cs_accept_run(job);
            return;
        }
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN accept retry ended ch=%s attempt=%016llx", job->channel,
            (unsigned long long)job->attempt);
        cs_join_finish(cs, current ? CHAT_CONNECT_TIMEOUT : CHAT_CONNECT_CANCELLED);
    }
    cs_accept_free(job);
}

static int cs_accept_defer(struct chat_sync* cs, struct cs_accept_job* job) {
    unsigned delay = job->retry_delay ? job->retry_delay : 1000;
    if (get_time_tb() + delay >= job->expires) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN accept WRITE deadline ch=%s attempt=%016llx",
            job->channel, (unsigned long long)job->attempt);
        return CHAT_CONNECT_TIMEOUT;
    }
    group_policy_discard(job->prepared.admission);
    job->prepared = (struct chat_sync_join_prepared){0};
    job->retry_timer = uasync_set_timeout(cs->inst->ua, delay, job, cs_accept_retry, "join_accept_retry");
    if (!job->retry_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN accept retry timer allocation failed ch=%s", job->channel);
        return CHAT_CONNECT_ERROR;
    }
    cs->join.accept_retry = job;
    job->retry_delay = delay < 5000 ? delay * 2 : 10000;
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "JOIN accept deferred ch=%s attempt=%016llx delay_tb=%u",
        job->channel, (unsigned long long)job->attempt, delay);
    return 0;
}

static void cs_accept_done(void* data, const struct db_async_result* result) {
    struct cs_accept_job* job = data;
    struct chat_sync* cs = cs_accept_active(job);
    if (result->status) DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN accept ch=%s attempt=%016llx status=%d rc=%d: %s",
        job->channel, (unsigned long long)job->attempt, result->status, result->sqlite_code,
        result->message[0] ? result->message : sqlite3_errstr(result->sqlite_code));
    if (!cs) return;
    if (!cs_accept_group_current(job)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN accept group replaced ch=%s attempt=%016llx", job->channel,
            (unsigned long long)job->attempt);
        cs_join_finish(cs, CHAT_CONNECT_CANCELLED);
        return;
    }
    if (result->status) {
        int code = result->sqlite_code & 255;
        int status = code == SQLITE_AUTH ? CHAT_CONNECT_AUTH : CHAT_CONNECT_ERROR;
        if (result->status == DB_ASYNC_FULL || code == SQLITE_BUSY || code == SQLITE_LOCKED)
            status = cs_accept_defer(cs, job);
        if (status) cs_join_finish(cs, status);
        return;
    }
    if (chat_channel_activate(job->inst, job->channel, &job->prepared.protocol)) goto fail;
    struct TOPO_GROUP* group = topo_groups_find_channel(job->inst->topo_groups, job->channel);
    if (!group) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN accept activation removed group ch=%s", job->channel); goto fail; }
    cs->join.channel = group->group_id;
    job->group_generation = group->generation;
    struct group_policy_update* admission = job->prepared.admission; job->prepared.admission = NULL;
    group_policy_publish(job->inst, admission);
    if (!(cs = cs_accept_active(job))) return;
    member_sync_record_committed(job->inst, job->channel, job->prepared.inviter_ed, job->prepared.changes);
    if (!(cs = cs_accept_active(job))) return;
    cs_recovery_remove(cs, job->channel);
    if (!(cs = cs_accept_active(job))) return;
    job->recovery->next = cs->recovery;
    cs->recovery = job->recovery;
    job->recovery = NULL;
    if (!cs_accept_send(cs, job)) return;
    if (cs->join.attempt != job->attempt) return;
fail:
    if (cs_recovery_finish(cs, job->channel, job->attempt, CHAT_CONNECT_ERROR)) cs_join_finish(cs, CHAT_CONNECT_ERROR);
}

static void cs_accept_run(struct cs_accept_job* job) {
    struct UTUN_INSTANCE* inst = job->inst;
    if (db_async_can_write(inst->db_worker)) {
        db_async_run(inst->db_worker, &(struct db_async_task){.name="JOIN-accept",.mode=DB_ASYNC_WRITE,.data=job,
            .guard=cs_accept_guard,.work=cs_accept_work,.done=cs_accept_done,.cleanup=cs_accept_free});
        return;
    }
    sqlite3* db = inst->topo_sqlite_db;
    int own = sqlite3_get_autocommit(db);
    int rc = own ? sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) : SQLITE_MISUSE;
    if (!rc) rc = cs_accept_work(db, job);
    if (!rc) rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    if (rc && own && !sqlite3_get_autocommit(db) && sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL) != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN accept rollback failed ch=%s: %s", job->channel, sqlite3_errmsg(db));
        utun_instance_stop(inst);
    }
    cs_accept_done(job, &(struct db_async_result){.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK,.sqlite_code=rc});
    cs_accept_free(job);
}

static void cs_accept_submit(struct chat_sync* cs, uint64_t peer, const uint8_t* bytes, size_t size, const uint8_t keys[64]) {
    if (size > GROUP_INFO_MAX + MS_USERINFO_MAX + 768) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN accept payload too large bytes=%zu", size); cs_join_finish(cs, CHAT_CONNECT_ERROR); return;
    }
    struct cs_accept_job* job = u_calloc(1, sizeof(*job) + size);
    if (job) job->recovery = u_calloc(1, sizeof(*job->recovery));
    if (!job || !job->recovery) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN accept allocation failed bytes=%zu", size);
        u_free(job); cs_join_finish(cs, CHAT_CONNECT_ERROR); return;
    }
    job->inst = cs->inst; job->core_generation = chat_core_generation(cs->inst); job->generation = cs->generation;
    job->attempt = cs->join.attempt; job->expires = get_time_tb() + CS_ACCEPT_WRITE_TIMEOUT_MS * 10;
    struct TOPO_GROUP* group = cs_join_target_group(cs->inst->topo_groups, keys + 32);
    job->group_generation = group ? group->generation : 0; snprintf(job->name, sizeof(job->name), "%s", cs->inst->name);
    memcpy(job->input.group_keys, keys, 64);
    sc_public_key_hex(keys + 32, job->channel);
    strcpy(job->recovery->channel, job->channel);
    job->recovery->hints = cs->join.recovery_hints;
    memcpy(job->recovery->hints.pubkey, cs->join.route.public_key, 32);
    memcpy(job->recovery->hints.inviter_pubkey, cs->join.inviter_ed, 32);
    job->recovery->retry_at = UINT64_MAX; /* Ждём READY или локального таймаута, при рестарте строка готова сразу. */
    job->recovery->request = cs->join.request;
    memcpy(job->bytes, bytes, size); job->input.data = job->bytes; job->input.size = size;
    job->input.channel = job->channel; job->input.name = job->name; job->input.peer = peer;
    job->input.chat_content = cs->inst->chat_content_started;
    job->input.identity.self = cs->inst->node_id; job->input.identity.now = ntp_time_get_us(cs->inst);
    memcpy(job->input.x, cs->inst->my_keys.public_key, 32);
    memcpy(job->input.peer_x, cs->join.inviter_pubkey, 32);
    memcpy(job->input.peer_ed, cs->join.inviter_ed, 32);
    memcpy(job->input.identity.public_key, cs->inst->my_ed25519_pubkey, 32);
    memcpy(job->input.identity.private_key, cs->inst->my_ed25519_privkey, 32);
    cs_cancel_proto_timers(cs);
    strcpy(cs->join.recovery_channel, job->channel);
    cs->join.phase = CS_JOIN_ACCEPT_WRITE;
    cs_accept_run(job);
}

static void cs_handle_join_info_resp(struct chat_sync* cs, uint64_t peer, const uint8_t* pl, size_t len) {
    if (len < 67 || len > CHAT_JOIN_BODY_MAX || pl[64] != 1 || sc_identity_check("JOIN INFO group", pl, pl + 32) != SC_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN INFO invalid response bytes=%zu", len); cs_join_finish(cs, CHAT_CONNECT_AUTH); return;
    }
    const uint8_t* keys = pl;
    if (memcmp(cs->join.expected_group, (uint8_t[32]){0}, 32) && memcmp(cs->join.expected_group, keys + 32, 32)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN INFO group mismatch expected=%s got=%s",
            sc_public_key_hex(cs->join.expected_group, (char[65]){0}), sc_public_key_hex(keys + 32, (char[65]){0}));
        cs_join_finish(cs, CHAT_CONNECT_AUTH); return;
    }
    struct TOPO_GROUP* registered = cs_join_target_group(cs->inst->topo_groups, keys + 32);
    if (registered && registered->stopping) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN target group is being removed ch=%s", registered->channel_id);
        cs_join_finish(cs, CHAT_CONNECT_BUSY); return;
    }
    char channel[65]; sc_public_key_hex(keys + 32, channel);
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "JOIN INFO authenticated group keys ch=%s inviter_x=%s attempt=%016llx",
        channel, sc_public_key_hex(cs->join.inviter_pubkey, (char[65]){0}), (unsigned long long)cs->join.attempt);
    cs_accept_submit(cs, peer, pl + 64, len - 64, keys);
}

static struct join_recovery* cs_recovery_find(struct chat_sync* cs, const char* channel) {
    for (struct join_recovery* item = cs->recovery; item; item = item->next)
        if (!strcmp(item->channel, channel)) return item;
    return NULL;
}

/* A проверяет свой допуск и допуск J; J сверяет выбранный источник с INFO и маршрутом recovery. */
int chat_sync_join_members_allowed(struct UTUN_INSTANCE* inst, const struct chat_join_path* path, const char* channel) {
    struct chat_sync* cs = cs_of(inst);
    struct TOPO_GROUP* group = topo_groups_find_channel(inst->topo_groups, channel);
    if (!cs || !cs->initialized || !group || group->stopping || !path->attempt) return 0;
    if (!memcmp(inst->my_ed25519_pubkey, path->inviter, 32)) {
        if (path->group) {
            struct TOPO_GROUP* route = topo_groups_find(inst->topo_groups, path->group);
            if (!route || route->stopping || route->generation != path->generation ||
                !topo_group_chat_key_allowed(inst, path->group, path->hop_ed)) return 0;
        } else if (path->handle ? !inst->join_carrier : !cs_find_conn_for_key(inst, path->hop_x)) return 0;
        return topo_group_chat_key_allowed(inst, group->group_id, path->guest);
    }
    struct join_recovery* item = cs_recovery_find(cs, channel);
    if (!item || !item->running || memcmp(path, &item->path, sizeof(*path))) return 0;
    uint8_t x[32];
    if (sc_identity_public_x("JOIN MEMBERS source", path->inviter, x) != SC_OK) return 0;
    if (db_async_can_write(inst->db_worker)) return group_policy_recovery_source(inst, channel, path->inviter, x);
    int verified = 0;
    return !member_sync_recovery_source_read(inst->topo_sqlite_db, channel, path->inviter, x, &verified) && verified;
}

static void cs_recovery_stop_round(struct chat_sync* cs, struct join_recovery* item) {
    if (item->path.attempt) merkle_sync_join_cancel(cs->inst, &item->path, item->channel);
    item->path.attempt = 0;
    item->running = 0;
}

static void cs_recovery_carrier_closed(struct chat_sync* cs, uint64_t handle) {
    for (struct join_recovery* item = cs->recovery; item; item = item->next) {
        uint64_t old = item->carrier_handle;
        if (!old || (handle && old != handle)) continue;
        item->carrier_handle = 0;
        item->carrier_ready = 0;
        if (item->path.handle == old) cs_recovery_stop_round(cs, item);
        cs_recovery_backoff(item);
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "JOIN recovery carrier removed ch=%s handle=%llu", item->channel, (unsigned long long)old);
        if (!handle && cs->inst->join_carrier) cs->inst->join_carrier->close(cs->inst->join_carrier->ctx, old, 0);
    }
    cs_recovery_arm(cs);
}

static void cs_recovery_remove(struct chat_sync* cs, const char* channel) {
    struct join_recovery** link = &cs->recovery;
    while (*link && strcmp((*link)->channel, channel)) link = &(*link)->next;
    if (!*link) return;
    struct join_recovery* item = *link; *link = item->next;
    cs_recovery_stop_round(cs, item);
    if (item->carrier_handle && cs->inst->join_carrier) {
        struct UTUN_INSTANCE* inst = cs->inst;
        uint64_t group = topo_groups_channel_slot(inst->topo_groups, channel);
        int admitted = topo_group_chat_key_allowed(inst, group, inst->my_ed25519_pubkey);
        inst->join_carrier->close(inst->join_carrier->ctx, item->carrier_handle, admitted);
    }
    node_conn_direct_close(item->transport); u_free(item);
}

static void cs_recovery_backoff(struct join_recovery* item) {
    if (item->retry_at == UINT64_MAX) return;
    unsigned delay = item->retry_delay ? item->retry_delay : 10000;
    item->retry_at = get_time_tb() + delay;
    item->retry_delay = delay < 150000 ? delay * 2 : 300000;
}

static int cs_recovery_complete(struct chat_sync* cs, const char* channel) {
    struct join_recovery* item = cs_recovery_find(cs, channel);
    struct UTUN_INSTANCE* inst = cs->inst;
    uint64_t group = topo_groups_channel_slot(inst->topo_groups, channel);
    if (!item || !topo_group_chat_key_allowed(inst, group, inst->my_ed25519_pubkey)) return 0;
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "JOIN local admission published ch=%s", channel);
    cs_recovery_remove(cs, channel);
    if (cs->join.channel == group && (cs->join.phase == CS_JOIN_WAIT_READY || cs->join.phase == CS_JOIN_WAIT_MEMBERS))
        cs_join_finish(cs, 0);
    return 1;
}

void chat_sync_members_published(struct UTUN_INSTANCE* inst, const char* channel) {
    struct chat_sync* cs = cs_of(inst);
    if (cs && cs->initialized) cs_recovery_complete(cs, channel);
}

static void cs_recovery_done(const uint8_t peer[32], const char* channel, int result, void* arg) {
    struct chat_sync* cs = arg;
    struct join_recovery* item = cs_recovery_find(cs, channel);
    if (!cs->initialized || !item || !item->running || memcmp(peer, item->hints.inviter_pubkey, 32)) return;
    cs_recovery_stop_round(cs, item);
    if (result == MT_OK && cs_recovery_complete(cs, channel)) return;
    DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN recovery deferred ch=%s peer=%s MEMBERS result=%d local_admitted=0",
        channel, sc_public_key_hex(peer, (char[65]){0}), result);
    cs_recovery_backoff(item);
    cs_recovery_arm(cs);
}

static void cs_recovery_transport(struct NODE_CONN_DIRECT* handle, enum ncd_event event, void* arg) {
    struct chat_sync* cs = arg;
    for (struct join_recovery* item = cs->recovery; item; item = item->next) {
        if (item->transport != handle) continue;
        if (event == NCD_EVENT_CLOSED || event == NCD_EVENT_TIMEOUT) {
            item->transport = NULL;
            if (!item->path.handle) cs_recovery_stop_round(cs, item);
            node_conn_direct_close(handle);
            cs_recovery_backoff(item);
            DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN recovery transport lost ch=%s peer=%s event=%d retry_at_tb=%llu",
                item->channel, sc_public_key_hex(item->hints.pubkey, (char[65]){0}), event, (unsigned long long)item->retry_at);
        } else if (event == NCD_EVENT_UP && item->retry_at != UINT64_MAX) cs_recovery_wake(cs, item);
        break;
    }
    cs_recovery_arm(cs);
}

static void cs_recovery_run(void* arg) {
    struct chat_sync* cs = arg; cs->recovery_timer = NULL;
    if (!cs->initialized) return;
    for (struct join_recovery* item = cs->recovery; item; item = item->next) {
        if (item->running || item->retry_at > get_time_tb()) continue;
        struct UTUN_INSTANCE* inst = cs->inst;
        struct TOPO_GROUP* group = topo_groups_find_channel(inst->topo_groups, item->channel);
        if (!group || group->stopping || inst->ntp.startup_wait) { cs_recovery_backoff(item); continue; }
        struct ETCP_CONN* conn = cs_find_conn_for_key(inst, item->hints.pubkey);
        int etcp_ready = cs_sync_peer_available(cs, conn);
        if (item->carrier_ready || etcp_ready) {
            uint64_t attempt = 0;
            if (RAND_bytes((uint8_t*)&attempt, sizeof(attempt)) != 1 || !attempt) {
                DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN recovery attempt RNG failed ch=%s", item->channel);
            } else {
                item->path = cs_guest_path(cs, attempt, item->hints.inviter_pubkey, item->hints.pubkey,
                    item->carrier_ready ? item->carrier_handle : 0);
                item->running = 1;
                if (!merkle_sync_join_pull(inst, &item->path, item->channel, cs_recovery_done, cs)) {
                    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "JOIN recovery pull ch=%s inviter=%s exit=%s attempt=%016llx carrier=%llu",
                        item->channel, sc_public_key_hex(item->path.inviter, (char[65]){0}),
                        sc_public_key_hex(item->path.exit_x, (char[65]){0}), (unsigned long long)attempt,
                        (unsigned long long)item->path.handle);
                    continue;
                }
                item->running = 0;
            }
        } else {
            if (item->carrier_handle && item->carrier_deadline <= get_time_tb()) {
                uint64_t handle = item->carrier_handle;
                item->carrier_handle = 0;
                DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN recovery carrier open timed out ch=%s", item->channel);
                if (inst->join_carrier) inst->join_carrier->close(inst->join_carrier->ctx, handle, 0);
            } else if (!item->carrier_handle && inst->join_carrier) {
                uint64_t handle = 0;
                int rc = inst->join_carrier->open(inst->join_carrier->ctx, item->hints.pubkey, &handle);
                if (!rc && handle) {
                    item->carrier_handle = handle;
                    item->carrier_deadline = get_time_tb() + CS_CARRIER_TIMEOUT_MS * 10;
                } else if (rc < 0 || !rc) {
                    DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN recovery carrier open failed ch=%s rc=%d", item->channel, rc);
                }
            }
            if (!item->carrier_handle && !item->transport) {
                struct TOPO_NODE node = {0};
                const struct InviteData* hints = &item->hints;
                if (!cs_node_from_addresses(&node, hints->pubkey, hints->addrs, hints->addrCount, &hints->reality) &&
                    node_conn_direct_open_node(inst, node.node_id, cs_recovery_transport, cs, &item->transport, &node, NULL) == NCD_ERR)
                    DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN recovery ETCP unavailable ch=%s exit=%s", item->channel,
                        sc_public_key_hex(item->hints.pubkey, (char[65]){0}));
                cs_node_clear(&node);
            }
        }
        cs_recovery_backoff(item);
    }
    cs_recovery_arm(cs);
}

static void cs_recovery_arm(struct chat_sync* cs) {
    if (!cs->initialized || !cs->recovery || cs->recovery_timer || cs->inst->ntp.startup_wait) return;
    uint64_t next = UINT64_MAX, now = get_time_tb();
    for (struct join_recovery* item = cs->recovery; item; item = item->next) {
        if (!item->running && item->retry_at < next) next = item->retry_at;
    }
    if (next == UINT64_MAX) return;
    cs->recovery_timer = uasync_set_timeout(cs->inst->ua, next > now ? (int)(next - now) : 1, cs, cs_recovery_run, "join_recovery");
    if (!cs->recovery_timer) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN recovery timer allocation failed");
}

static void cs_recovery_wake(struct chat_sync* cs, struct join_recovery* item) {
    item->retry_at = 0;
    if (cs->recovery_timer) { uasync_cancel_timeout(cs->inst->ua, cs->recovery_timer); cs->recovery_timer = NULL; }
    cs_recovery_arm(cs);
}

static int cs_recovery_store(sqlite3* db, const struct join_recovery* item) {
    uint8_t hints[1 + 44 + INVITE_SNAME_MAX + INVITE_ADDR_MAX * 21];
    int bytes = invite_serialize_addrs(&item->hints, hints, sizeof(hints));
    sqlite3_stmt* st = NULL;
    int rc = bytes < 0 ? SQLITE_MISMATCH : sqlite3_prepare_v2(db,
        "INSERT INTO chat_join_recovery(channel,peer,inviter,hints) VALUES(?,?,?,?)"
        " ON CONFLICT(channel) DO UPDATE SET peer=excluded.peer,inviter=excluded.inviter,hints=excluded.hints", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, item->channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, item->hints.pubkey, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 3, item->hints.inviter_pubkey, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 4, hints, bytes, SQLITE_TRANSIENT);
    if (!rc) rc = sqlite3_step(st);
    int final = sqlite3_finalize(st);
    return rc == SQLITE_DONE ? final : rc;
}

struct cs_recovery_delete {
    struct UTUN_INSTANCE* inst;
    uint64_t generation, registration, attempt;
    int result;
    char channel[65];
};

static int cs_recovery_delete_sql(sqlite3* db, void* data) {
    struct cs_recovery_delete* job = data;
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "DELETE FROM chat_join_recovery WHERE channel=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, job->channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int final = sqlite3_finalize(st);
    return rc == SQLITE_DONE ? final : rc;
}

static void cs_recovery_delete_free(void* data) { u_free(data); }

static void cs_recovery_delete_done(void* data, const struct db_async_result* result) {
    struct cs_recovery_delete* job = data;
    if (result->status) DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN recovery delete failed ch=%s attempt=%016llx status=%d rc=%d: %s",
        job->channel, (unsigned long long)job->attempt, result->status, result->sqlite_code,
        result->message[0] ? result->message : sqlite3_errstr(result->sqlite_code));
    else DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "JOIN recovery deleted ch=%s attempt=%016llx result=%d",
        job->channel, (unsigned long long)job->attempt, job->result);
    struct chat_sync* cs = cs_of(job->inst);
    if (!cs || !cs->initialized || cs->generation != job->generation) return;
    struct TOPO_GROUP* group = topo_groups_find_channel(job->inst->topo_groups, job->channel);
    if ((group ? group->generation : 0) == job->registration) {
        if (!result->status) cs_recovery_remove(cs, job->channel);
        else {
            struct join_recovery* item = cs_recovery_find(cs, job->channel);
            if (item) cs_recovery_wake(cs, item);
        }
    }
    if (job->attempt && cs->join.attempt == job->attempt && cs->join.phase == CS_JOIN_CANCEL_PENDING)
        cs_join_finish(cs, result->status ? CHAT_CONNECT_ERROR : job->result);
}

/* DELETE идёт за принятым INFO WRITE в очереди SQLite, в том числе при последующем stop. */
static int cs_recovery_finish(struct chat_sync* cs, const char* channel, uint64_t attempt, int result) {
    struct join_recovery* item = cs_recovery_find(cs, channel);
    struct cs_recovery_delete* job = u_calloc(1, sizeof(*job));
    if (!job) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN recovery delete allocation failed ch=%s", channel);
        if (item) cs_recovery_wake(cs, item);
        return -1;
    }
    struct TOPO_GROUP* group = topo_groups_find_channel(cs->inst->topo_groups, channel);
    *job = (struct cs_recovery_delete){.inst=cs->inst,.generation=cs->generation,.registration=group ? group->generation : 0,
        .attempt=attempt,.result=result};
    strcpy(job->channel, channel);
    if (attempt) {
        cs->join.phase = CS_JOIN_CANCEL_PENDING;
        cs_accept_drop_retry(cs);
        cs_cancel_proto_timers(cs);
        if (cs->join.operation_timer) uasync_cancel_timeout(cs->inst->ua, cs->join.operation_timer);
        cs->join.operation_timer = NULL;
    }
    if (item) { cs_recovery_stop_round(cs, item); item->retry_at = UINT64_MAX; }
    if (db_async_can_write(cs->inst->db_worker)) {
        db_async_run(cs->inst->db_worker, &(struct db_async_task){.name="join-recovery-delete",.mode=DB_ASYNC_WRITE,
            .work=cs_recovery_delete_sql,.done=cs_recovery_delete_done,.cleanup=cs_recovery_delete_free,.data=job});
    } else {
        int rc = cs_recovery_delete_sql(cs->inst->topo_sqlite_db, job);
        cs_recovery_delete_done(job, &(struct db_async_result){.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK,.sqlite_code=rc});
        u_free(job);
    }
    return 0;
}

static void cs_join_begin_members(struct chat_sync* cs, const char* reason, int ready) {
    const char* channel = cs->join.recovery_channel;
    struct join_recovery* item = cs_recovery_find(cs, channel);
    if (!item) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN recovery missing ch=%s attempt=%016llx source=%s", channel,
            (unsigned long long)cs->join.attempt, reason);
        cs_join_finish(cs, CHAT_CONNECT_ERROR);
        return;
    }
    if (ready && topo_group_chat_key_allowed(cs->inst, cs->join.channel, cs->inst->my_ed25519_pubkey)) {
        if (cs_recovery_finish(cs, channel, cs->join.attempt, CHAT_JOIN_OK)) cs_join_finish(cs, CHAT_CONNECT_ERROR);
        return;
    }
    cs_cancel_proto_timers(cs);
    cs->join.phase = CS_JOIN_WAIT_MEMBERS;
    cs->join_timer = uasync_set_timeout(cs->inst->ua, CS_MEMBERS_TIMEOUT_MS * 10, cs, cs_join_timeout_cb, "join_members");
    item->carrier_handle = cs->join.route.handle;
    item->carrier_ready = !!item->carrier_handle;
    cs->join.route.handle = 0;
    if (cs->join.handle) {
        item->transport = cs->join.handle; cs->join.handle = NULL;
        node_conn_direct_set_callback(item->transport, cs_recovery_transport, cs);
    }
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "JOIN start MEMBERS ch=%s attempt=%016llx source=%s",
        channel, (unsigned long long)cs->join.attempt, reason);
    cs_recovery_wake(cs, item);
    if (!cs->join_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN members deadline allocation failed ch=%s", channel);
        cs_join_finish(cs, CHAT_CONNECT_ERROR);
    }
}

/* Ответить только после публикации допуска на пригласившем узле. */
struct cs_join_write {
    struct cs_join_write* next;
    struct UTUN_INSTANCE* inst;
    void* timer;
    uint64_t deadline;
    unsigned delay;
    struct chat_join_path path;
    uint64_t group, key, group_generation, generation, carrier_generation;
};

static struct TOPO_GROUP* cs_join_current_group(const struct cs_join_write* job, struct chat_sync* cs) {
    if (!cs || !cs->initialized || cs->generation != job->generation ||
        (job->path.handle && cs->carrier_generation != job->carrier_generation)) return NULL;
    struct TOPO_GROUP* group = topo_groups_find(job->inst->topo_groups, job->group);
    return group && !group->stopping && group->generation == job->group_generation ? group : NULL;
}

static void cs_join_ready_retry(void* arg);

static void cs_join_reply_ready(struct chat_sync* cs, struct cs_join_write* job, struct TOPO_GROUP* group) {
    if (!topo_group_chat_key_allowed(job->inst, job->group, job->path.guest)) {
        cs_join_reply_error(job->inst, &job->path, CHAT_CONNECT_AUTH);
    } else if (!chat_join_send(job->inst, &job->path, CS_MSG_JOIN_READY, topo_group_wire_id(group), 32)) {
        DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "JOIN inviter admitted guest ch=%s attempt=%016llx",
            group->channel_id, (unsigned long long)job->path.attempt);
        if (cs->join.inviter && !cs->join.server_offer && cs->join.phase == 4 && cs->join.channel == job->group &&
            cs->join.key == job->key && !job->path.group && !job->path.handle &&
            !memcmp(cs->join.route.public_key, job->path.hop_x, 32)) {
            u_free(job);
            cs_join_finish(cs, CHAT_JOIN_OK);
            return;
        }
    } else if (get_time_tb() + job->delay < job->deadline) {
        job->timer = uasync_set_timeout(job->inst->ua, job->delay, job, cs_join_ready_retry, "join_ready_retry");
        if (job->timer) {
            job->delay = job->delay < 5000 ? job->delay * 2 : 10000;
            job->next = cs->ready_replies;
            cs->ready_replies = job;
            return;
        }
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN READY retry allocation failed attempt=%016llx", (unsigned long long)job->path.attempt);
    } else {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN READY delivery expired attempt=%016llx", (unsigned long long)job->path.attempt);
    }
    u_free(job);
}

static void cs_join_ready_retry(void* arg) {
    struct cs_join_write* job = arg;
    struct chat_sync* cs = cs_of(job->inst);
    struct cs_join_write** link = &cs->ready_replies;
    while (*link != job) link = &(*link)->next;
    *link = job->next;
    struct TOPO_GROUP* group = cs_join_current_group(job, cs);
    if (group) cs_join_reply_ready(cs, job, group);
    else u_free(job);
}

static void cs_join_clear_ready(struct chat_sync* cs) {
    while (cs->ready_replies) {
        struct cs_join_write* job = cs->ready_replies;
        cs->ready_replies = job->next;
        uasync_cancel_timeout(cs->inst->ua, job->timer);
        u_free(job);
    }
}

static void cs_join_write_done(void* arg, const struct chat_join_request_result* result) {
    struct cs_join_write* job = arg;
    struct chat_sync* cs = cs_of(job->inst);
    struct TOPO_GROUP* group = cs_join_current_group(job, cs);
    if (group && !result->status && result->committed) {
        cs_join_reply_ready(cs, job, group);
        return;
    }
    if (group) cs_join_reply_error(job->inst, &job->path, result->status ? result->status : CHAT_CONNECT_ERROR);
    u_free(job);
}

static void cs_join_inviter_receive(struct chat_sync* cs, const struct chat_join_path* path, uint8_t type,
    const uint8_t* data, size_t size) {
    struct UTUN_INSTANCE* inst = cs->inst;
    size_t key_off = type == CS_MSG_JOIN_REQUEST ? 32 : 0;
    if ((type == CS_MSG_JOIN_INFO_REQ && size != 8) || (type == CS_MSG_JOIN_REQUEST && size < 40 + JOIN_MEMBER_FIXED_SIZE) ||
        (type != CS_MSG_JOIN_INFO_REQ && type != CS_MSG_JOIN_REQUEST)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN inviter invalid message type=%u bytes=%zu", type, size);
        cs_join_reply_error(inst, path, CHAT_CONNECT_ERROR);
        return;
    }
    uint64_t key; memcpy(&key, data + key_off, 8); key = be64toh(key);
    uint64_t id = chat_join_key_group(inst, key);
    struct TOPO_GROUP* group = id ? topo_groups_find(inst->topo_groups, id) : NULL;
    const uint8_t* group_ed = group && !group->stopping ? topo_group_wire_id(group) : NULL;
    if (!group_ed || !chat_join_key_matches(inst, id, key, inst->my_ed25519_pubkey) ||
        (type == CS_MSG_JOIN_REQUEST && memcmp(data, group_ed, 32))) {
        cs_join_reply_error(inst, path, CHAT_CONNECT_AUTH);
        return;
    }
    if (type == CS_MSG_JOIN_INFO_REQ) {
        struct chat_join_route route = {.handle = path->handle, .peer = sc_derive_node_id_from_pubkey(path->hop_x)};
        memcpy(route.public_key, path->hop_x, 32);
        if (cs_group_read_submit(cs, group->channel_id, route.peer, CS_GROUP_INFO, key, path->attempt, &route, path))
            cs_join_reply_error(inst, path, CHAT_CONNECT_ERROR);
        return;
    }
    struct cs_join_write* job = u_malloc(sizeof(*job));
    if (!job) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN write reply allocation failed ch=%s", group->channel_id);
        cs_join_reply_error(inst, path, CHAT_CONNECT_ERROR);
        return;
    }
    *job = (struct cs_join_write){.inst = inst, .path = *path, .group = id, .key = key, .group_generation = group->generation,
        .generation = cs->generation, .carrier_generation = cs->carrier_generation,
        .deadline = get_time_tb() + (path->handle ? CS_CARRIER_TIMEOUT_MS : CS_JOIN_TIMEOUT_MS) * 10, .delay = 1000};
    chat_join_process_submit(inst, id, path->guest, key, path->attempt, data + 40, size - 40, cs_join_write_done, job);
}

void chat_sync_join_receive(struct UTUN_INSTANCE* inst, const struct chat_join_path* path, uint8_t type,
    const uint8_t* data, size_t size) {
    struct chat_sync* cs = cs_of(inst);
    if (!cs || !cs->initialized) return;
    if (type == CS_MSG_JOIN_MEMBERS) {
        merkle_sync_join_receive(inst, path, data, size);
        return;
    }
    if (!memcmp(path->inviter, inst->my_ed25519_pubkey, 32)) {
        cs_join_inviter_receive(cs, path, type, data, size);
        return;
    }
    if (cs->join.attempt != path->attempt || cs->join.route.handle != path->handle ||
        memcmp(cs->join.route.public_key, path->exit_x, 32) || memcmp(cs->join.inviter_ed, path->inviter, 32)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "JOIN stale response attempt=%016llx type=%u phase=%u",
            (unsigned long long)path->attempt, type, cs->join.phase);
        return;
    }
    if (type == CS_MSG_JOIN_INFO_RESP && cs->join.phase == 2) {
        cs_handle_join_info_resp(cs, sc_derive_node_id_from_pubkey(cs->join.inviter_pubkey), data, size);
        return;
    }
    if (type == CS_MSG_JOIN_READY && cs->join.phase == CS_JOIN_WAIT_READY && size == 32) {
        struct TOPO_GROUP* group = topo_groups_find_wire(inst->topo_groups, data);
        if (group && !group->stopping && group->group_id == cs->join.channel) {
            cs_join_begin_members(cs, "ready", 1);
            return;
        }
    }
    if (type == CS_MSG_ERROR && (cs->join.phase == 2 || cs->join.phase == CS_JOIN_WAIT_READY || cs->join.phase == CS_JOIN_WAIT_MEMBERS) && size == 4) {
        uint32_t wire; memcpy(&wire, data, 4);
        int32_t status = (int32_t)ntohl(wire);
        if (status == CHAT_CONNECT_AUTH || status == CHAT_CONNECT_ERROR) {
            DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN remote error attempt=%016llx phase=%u status=%d",
                (unsigned long long)path->attempt, cs->join.phase, status);
            if (cs->join.recovery_channel[0]) {
                if (status == CHAT_CONNECT_AUTH) {
                    if (!cs_recovery_finish(cs, cs->join.recovery_channel, cs->join.attempt, status)) return;
                    status = CHAT_CONNECT_ERROR;
                } else if (cs->join.phase == CS_JOIN_WAIT_READY) {
                    uint64_t attempt = cs->join.attempt;
                    cs_join_begin_members(cs, "remote_error", 0);
                    if (cs->join.attempt != attempt) return;
                }
            }
            cs_join_finish(cs, status);
            return;
        }
    }
    DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN unexpected response attempt=%016llx type=%u bytes=%zu phase=%u",
        (unsigned long long)path->attempt, type, size, cs->join.phase);
}

/* ─── CHANNEL_INVITE (0x0F): inviter → joiner (push-приглашение в канал) ─── */

struct cs_invite_snapshot { struct group_info_profile profile; char inviter[128]; };
static void cs_process_channel_invite(struct chat_sync* cs, uint64_t peer, const uint8_t ed_pub[32],
    const uint8_t* pl, size_t len, const uint8_t inviter_pubkey[32], const uint8_t inviter_ed[32], const struct cs_invite_snapshot* snapshot) {
    if (cs->inst->config->global.chatserver_enabled) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "server rejects unauthenticated CHANNEL_INVITE group=%s peer=%016llx, use SERVER_JOIN_REQ",
            sc_public_key_hex(ed_pub, (char[65]){0}), (unsigned long long)peer); return;
    }
    DEBUG_TRACE(DEBUG_CATEGORY_MEMBER_SYNC, "%s: CHANNEL_INVITE group=%s from=%016llx len=%zu",
               CS_ID, sc_public_key_hex(ed_pub, (char[65]){0}), (unsigned long long)peer, len);
    if (len < 3 + 32 + 32 + 64 + 1 + 64 + 8 + 2 + 8 || pl[0] != 2) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "%s: CHANNEL_INVITE invalid version/length len=%zu peer=%016llx", CS_ID, len, (unsigned long long)peer);
        return;
    }
    /* Same payload format as CHANNEL_INFO_RESP */
    const uint8_t* p = pl;
    p++; size_t nl = (size_t)p[0] * 256 + p[1]; p += 2;
    if (!nl || nl > GROUP_INFO_MAX || 3 + nl + 32 + 32 + 64 + 1 + 64 + 8 + 2 + 8 > len || memchr(p, 0, nl)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "%s: CHANNEL_INVITE truncated or invalid JSON length", CS_ID); return;
    }
    char json[GROUP_INFO_MAX + 1]; memcpy(json, p, nl); json[nl] = '\0'; p += nl;
    struct group_info_profile profile;
    if (snapshot) profile = snapshot->profile;
    else if (group_info_parse(cs->inst->topo_sqlite_db, json, &profile)) return;
    const char* name = profile.name;
    const uint8_t* owner = p; p += 32;
    const uint8_t* x25519 = p; p += 32;
    char ch_id[65]; sc_public_key_hex(ed_pub, ch_id);
    const uint8_t* ch_sig = p; p += 64;
    uint8_t inv_flags = *p++;
    if (inv_flags & ~PEERS_FLAG_HAS_JOIN) { DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "CHANNEL_INVITE: unknown flags=%u", inv_flags); return; }
    const uint8_t* inviter_join_sig = NULL; uint64_t inviter_join_ts = 0;
    if (inv_flags & PEERS_FLAG_HAS_JOIN) {
        if (p + 72 > pl + len) { DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "%s: CHANNEL_INVITE truncated at join_sig/ts from=%016llx", CS_ID, (unsigned long long)peer); return; }
        inviter_join_sig = p; p += 64; memcpy(&inviter_join_ts, p, 8); p += 8; }
    if ((size_t)(pl + len - p) < 64 + 8 + 2 + 8) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "%s: CHANNEL_INVITE truncated profile header", CS_ID); return;
    }
    /* skip inviter_update_sig/ts/userinfo for now — verify channel sig only */
    p += 64; uint64_t inviter_update_ts; memcpy(&inviter_update_ts, p, 8); p += 8;
    size_t userinfo_len = ((size_t)p[0] << 8) | p[1]; p += 2;
    if (userinfo_len > MS_USERINFO_MAX || (size_t)(pl + len - p) != userinfo_len + 8 || memchr(p, 0, userinfo_len)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "%s: CHANNEL_INVITE invalid profile bytes=%zu", CS_ID, userinfo_len); return;
    }
    p += userinfo_len;
    /* Локальный ключ пригласившего занимает последние 8 байт. */
    uint64_t join_key = 0;
    if (p + 8 > pl + len) { DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "%s: CHANNEL_INVITE truncated (no join_key) from=%016llx", CS_ID, (unsigned long long)peer); return; }
    memcpy(&join_key, p, 8); p += 8;
    if (!join_key) { DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "CHANNEL_INVITE: missing join key"); return; }

    /* Verify channel signature */
    if (chat_channel_check_public_keys(ch_id, x25519, ed_pub)) return;
    uint8_t vmsg[CHAT_CHANNEL_DESCRIPTION_MAX];
    int vlen = group_info_signature_body(json, owner, x25519, ed_pub, vmsg, sizeof(vmsg));
    if (vlen < 0 || sc_ed25519_verify(ed_pub, vmsg, (size_t)vlen, ch_sig) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: CHANNEL_INVITE invalid ch_sig ch=%s from=%016llx", CS_ID, ch_id, (unsigned long long)peer);
        return;
    }

    /* Обычные узлы используют настройку GUI; сервер принимает только авторизованный SERVER_JOIN_REQ. */
    int policy;
    {
        policy = chat_setting_get_int(cs->inst, "join_policy", 1);
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "%s: CHANNEL_INVITE policy=%d (chatserver=%d) ch=%s from=%016llx",
               CS_ID, policy, cs->inst->config->global.chatserver_enabled, ch_id, (unsigned long long)peer);

    if (policy == 2) { /* deny */
        DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "%s: CHANNEL_INVITE denied (policy=deny) ch=%s from=%016llx",
                   CS_ID, ch_id, (unsigned long long)peer);
        return;
    }

    if (policy == 0) { /* autojoin */
        DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "%s: CHANNEL_INVITE autojoin ch=%s from=%016llx owner=%s name=%s",
                   CS_ID, ch_id, (unsigned long long)peer, sc_public_key_hex(owner, (char[65]){0}), name);
        /* Описание и участника сохраняет проверенный JOIN_INFO_RESP. */
        cs_join_existing(cs->inst, join_key, inviter_pubkey, inviter_ed, ed_pub);
        return;
    }

    /* policy == ask — post event to GUI */
    {
        /* сохранить join-ключ, чтобы принять инвайт позже через chat_sync_accept_invite */
        int slot = cs_invitation_find(cs, ed_pub, inviter_pubkey);
        if (slot < 0 && cs->pending_inv_in_count < 4) slot = cs->pending_inv_in_count++;
        if (slot < 0) { DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "CHANNEL_INVITE: pending invitation limit reached"); return; }
        cs->pending_inv_in[slot] = (struct cs_invitation){.join_key=join_key};
        memcpy(cs->pending_inv_in[slot].group_ed, ed_pub, 32);
        memcpy(cs->pending_inv_in[slot].inviter, inviter_pubkey, 32);
        memcpy(cs->pending_inv_in[slot].inviter_ed, inviter_ed, 32);

        size_t inv_name_len = 0; char inv_name[128] = "";
        if (snapshot) snprintf(inv_name, sizeof(inv_name), "%s", snapshot->inviter);
        else chat_core_get_node_name(cs->inst, inviter_pubkey, inv_name, sizeof(inv_name));
        inv_name_len = strlen(inv_name); size_t name_len = strlen(name);
        uint8_t evt[512]; int epos = 0;
        uint8_t cl = (uint8_t)strlen(ch_id); evt[epos++] = cl; memcpy(evt + epos, ch_id, cl); epos += cl;
        evt[epos++] = (uint8_t)name_len; memcpy(evt + epos, name, name_len); epos += name_len;
        memcpy(evt + epos, inviter_pubkey, 32); epos += 32;
        evt[epos++] = (uint8_t)inv_name_len;
        if (inv_name_len) { memcpy(evt + epos, inv_name, inv_name_len); epos += inv_name_len; }
        chat_event_post(cs->inst, CHAT_EVT_INVITE_RECEIVED, evt, epos);
        DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "%s: CHANNEL_INVITE ask posted to GUI ch=%s name=%s from=%016llx (%s)",
                   CS_ID, ch_id, name, (unsigned long long)peer, inv_name);
    }
}

struct cs_invite_job {
    struct UTUN_INSTANCE* inst; /* Только guard/done; SQL читает frozen channel/peer/payload. */
    uint64_t core_generation, generation, peer;
    uint8_t key[32], inviter_ed[32], group_ed[32];
    size_t size;
    struct cs_invite_snapshot snapshot;
    uint8_t bytes[];
};
static struct chat_sync* cs_invite_current(struct cs_invite_job* job) {
    struct UTUN_INSTANCE* inst = job->inst;
    if (!job->core_generation || inst->destroying || !inst->group_started || chat_core_generation(inst) != job->core_generation) return NULL;
    struct chat_sync* cs = cs_of(inst);
    return cs && cs->initialized && cs->generation == job->generation && cs_find_conn_for_key(inst, job->key) ? cs : NULL;
}
static int cs_invite_guard(void* data) { return cs_invite_current(data) != NULL; }
static int cs_invite_work(sqlite3* db, void* data) {
    struct cs_invite_job* job = data;
    if (job->size < 3 || job->bytes[0] != 2) return SQLITE_MISMATCH;
    size_t size = (size_t)job->bytes[1] * 256 + job->bytes[2];
    if (!size || size > GROUP_INFO_MAX || job->size < size + 3 || memchr(job->bytes + 3, 0, size)) return SQLITE_MISMATCH;
    char json[GROUP_INFO_MAX + 1]; memcpy(json, job->bytes + 3, size); json[size] = 0;
    int rc = group_info_parse(db, json, &job->snapshot.profile); if (rc) return -rc;
    sqlite3_stmt* st = NULL; rc = sqlite3_prepare_v2(db, "SELECT name FROM nodes WHERE x25519_pubkey=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, job->key, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        int type = sqlite3_column_type(st, 0); const char* name = (const char*)sqlite3_column_text(st, 0);
        rc = type == SQLITE_NULL || (type == SQLITE_TEXT && name) ? SQLITE_OK : name ? SQLITE_CORRUPT : SQLITE_NOMEM;
        if (!rc && name) snprintf(job->snapshot.inviter, sizeof(job->snapshot.inviter), "%s", name);
    } else if (rc == SQLITE_DONE) rc = SQLITE_OK;
    int final = sqlite3_finalize(st); return rc ? rc : final;
}
static void cs_invite_done(void* data, const struct db_async_result* result) {
    struct cs_invite_job* job = data; struct chat_sync* cs = cs_invite_current(job);
    if (result->status) DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "CHANNEL_INVITE READ group=%s peer=%016llx status=%d rc=%d: %s",
        sc_public_key_hex(job->group_ed, (char[65]){0}), (unsigned long long)job->peer, result->status, result->sqlite_code, result->message);
    if (!result->status && cs) cs_process_channel_invite(cs, job->peer, job->group_ed, job->bytes, job->size, job->key, job->inviter_ed, &job->snapshot);
}
static void cs_invite_free(void* data) { u_free(data); }
static void cs_handle_channel_invite(struct chat_sync* cs, uint64_t peer,
    const uint8_t group_ed[32], const uint8_t* bytes, size_t size, const struct chat_join_route* route) {
    if (sc_identity_check("channel invite sender", route->public_key, route->peer_ed) != SC_OK) return;
    if (!db_async_can_write(cs->inst->db_worker)) { cs_process_channel_invite(cs, peer, group_ed, bytes, size, route->public_key, route->peer_ed, NULL); return; }
    if (cs->inst->config->global.chatserver_enabled) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "CHANNEL_INVITE refused server group=%s peer=%016llx",
            sc_public_key_hex(group_ed, (char[65]){0}), (unsigned long long)peer); return;
    }
    if (size > GROUP_INFO_MAX + MS_USERINFO_MAX + 768) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "CHANNEL_INVITE refused oversize group=%s peer=%016llx bytes=%zu",
            sc_public_key_hex(group_ed, (char[65]){0}), (unsigned long long)peer, size); return;
    }
    struct cs_invite_job* job = u_calloc(1, sizeof(*job) + size);
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "CHANNEL_INVITE READ allocation failed group=%s", sc_public_key_hex(group_ed, (char[65]){0})); return; }
    job->inst = cs->inst; job->core_generation = chat_core_generation(cs->inst); job->generation = cs->generation; job->peer = peer;
    memcpy(job->group_ed, group_ed, 32); memcpy(job->key, route->public_key, 32);
    memcpy(job->inviter_ed, route->peer_ed, 32);
    memcpy(job->bytes, bytes, size); job->size = size;
    db_async_run(cs->inst->db_worker, &(struct db_async_task){.name="JOIN-invite",.mode=DB_ASYNC_READ,.data=job,
        .guard=cs_invite_guard,.work=cs_invite_work,.done=cs_invite_done,.cleanup=cs_invite_free});
}

/* ─── helper: build and send CS_MSG_CHANNEL_INVITE to peer ─── */

static int cs_send_channel_invite(struct chat_sync* cs, struct UTUN_INSTANCE* inst,
                                   const char* ch_id, uint64_t target_node_id,
                                   const char* ch_name, const uint8_t owner[32],
                                   const uint8_t* ch_x25519, const uint8_t* ch_ed,
                                   const uint8_t* ch_sig) {
    uint8_t join_sig[64] = {0}; uint64_t join_ts = 0;
    int code = topo_node_sqlite_member_get_join(inst->topo_sqlite_db, ch_id, inst->my_ed25519_pubkey, join_sig, &join_ts);
    if (code) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "CHANNEL_INVITE: own JOIN unavailable ch=%s rc=%d", ch_id, -code); return -1; }
    uint8_t upd_sig[64] = {0}; uint64_t upd_ts;
    char juser[MS_USERINFO_MAX + 1];
    if (chat_core_get_my_profile(inst, ch_id, juser, sizeof(juser), upd_sig, &upd_ts) < 0) {
        chat_core_update_my_member(inst);
        if (chat_core_get_my_profile(inst, ch_id, juser, sizeof(juser), upd_sig, &upd_ts) < 0) return -1;
    }
    /* join-ключ: генерируем и регистрируем локально (инвайтер == connection-узел),
       чтобы джойнер смог пройти JOIN_INFO_REQ с валидным ключом. */
    uint64_t join_key = 0;
    if (RAND_bytes((uint8_t*)&join_key, sizeof(join_key)) != 1 || join_key == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: join_key generation failed ch=%s", CS_ID, ch_id);
        return -1;
    }
    if (chat_join_store_local_key(inst, topo_groups_channel_slot(inst->topo_groups, ch_id), join_key) != 0) return -1;
    if (cs->join.inviter && cs->join.peer == target_node_id) cs->join.key = join_key;

    uint8_t buf[MS_USERINFO_MAX + GROUP_INFO_MAX + 768]; size_t boff = 0;
    buf[boff++] = CS_MSG_CHANNEL_INVITE;
    size_t cnl = strlen(ch_name); buf[boff++] = 2;
    buf[boff++] = (uint8_t)(cnl >> 8); buf[boff++] = (uint8_t)cnl; memcpy(buf + boff, ch_name, cnl); boff += cnl;
    memcpy(buf + boff, owner, 32); boff += 32;
    memcpy(buf + boff, ch_x25519, 32); boff += 32;
    memcpy(buf + boff, ch_sig, 64); boff += 64;
    buf[boff++] = PEERS_FLAG_HAS_JOIN;
    memcpy(buf + boff, join_sig, 64); boff += 64;
    memcpy(buf + boff, &join_ts, 8); boff += 8;
    memcpy(buf + boff, upd_sig, 64); boff += 64;
    memcpy(buf + boff, &upd_ts, 8); boff += 8;
    size_t il = strlen(juser);
    buf[boff++] = (uint8_t)(il >> 8); buf[boff++] = (uint8_t)il; memcpy(buf + boff, juser, il); boff += il;
    memcpy(buf + boff, &join_key, 8); boff += 8;
    int rc = cs_send_key(cs, ch_id, target_node_id, buf, boff, &cs->join.route, ch_ed);
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "%s: sent CHANNEL_INVITE to 0x%016llx ch=%s rc=%d",
               CS_ID, (unsigned long long)target_node_id, ch_id, rc);
    return rc;
}

/* ─── accept/deny incoming invite ─── */

void chat_sync_accept_invite(struct UTUN_INSTANCE* inst, const uint8_t group_ed[32], const uint8_t inviter[32]) {
    struct chat_sync* cs = cs_of(inst);
    if (!cs || !group_ed || !memcmp(group_ed, (uint8_t[32]){0}, 32) || !inviter) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: accept_invite invalid parameters", CS_ID);
        return;
    }
    int slot = cs_invitation_find(cs, group_ed, inviter);
    if (slot < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "%s: pending invitation unavailable group=%s inviter=%s",
                   CS_ID, sc_public_key_hex(group_ed, (char[65]){0}), sc_public_key_hex(inviter, (char[65]){0}));
        return;
    }
    struct cs_invitation invitation = cs->pending_inv_in[slot];
    if (!cs_join_existing(inst, invitation.join_key, invitation.inviter, invitation.inviter_ed, invitation.group_ed)) {
        struct chat_sync* current = cs_of(inst);
        if (current == cs) {
            slot = cs_invitation_find(cs, group_ed, invitation.inviter);
            if (slot >= 0 && cs->pending_inv_in[slot].join_key == invitation.join_key) cs_invitation_remove(cs, slot);
        }
    }
    OPENSSL_cleanse(&invitation, sizeof(invitation));
}

void chat_sync_deny_invite(struct UTUN_INSTANCE* inst, const uint8_t group_ed[32], const uint8_t inviter[32]) {
    struct chat_sync* cs = cs_of(inst);
    if (!cs || !group_ed || !inviter) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "%s: deny_invite invalid parameters", CS_ID);
        return;
    }
    for (int slot = cs_invitation_find(cs, group_ed, inviter); slot >= 0;
         slot = cs_invitation_find(cs, group_ed, inviter)) cs_invitation_remove(cs, slot);
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "%s: deny_invite group=%s inviter=%s",
               CS_ID, sc_public_key_hex(group_ed, (char[65]){0}), sc_public_key_hex(inviter, (char[65]){0}));
}
