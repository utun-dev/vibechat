/* Три настоящих ядра A/B/M: подписанный storage, wire-запросы, загрузка A,
 * скачивание B, запрет чужой записи, удалённого участника и restart сервиса. */
#include "../src/member_files/member_files.h"
#include "../src/member_files/private.h"
#include "../src/utun_instance.h"
#include "../src/chat/chat_core.h"
#include "../src/chat/chat_member.h"
#include <assert.h>
#include "../src/chat/chat_event.h"
#include "../src/chat/member_sync.h"
#include "../src/routing_layer/topo_node_sqlite.h"
#include "../src/routing_layer/topo_node.h"
#include "../src/routing_layer/etcp_router.h"
#include "../src/transport_layer/etcp.h"
#include "../src/transport_layer/secure_channel.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_utils.h"
#include "test_clock.h"
#include "test_net.h"
#include "test_membership.h"
#include <openssl/sha.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

static uint64_t group_slot;
static char group_channel[65];
#define BYTES (128 * MF_BLOCK_SIZE + 37)
enum { AUTHOR, READER, SERVER, NODE_COUNT };
struct api_case {
    struct mf_transfer* handle;
    uint64_t id, done, total;
    int finished, status, cancel_after_progress;
    char local[768];
};
struct network_test {
    struct UASYNC* ua;
    struct UTUN_INSTANCE* nodes[NODE_COUNT];
    struct SC_MYKEYS keys[NODE_COUNT];
    uint8_t seeds[NODE_COUNT][32];
    uint64_t ids[NODE_COUNT];
    uint8_t ed[NODE_COUNT][32], channel_x[32], channel_ed[32], channel_private[32];
    char dir[512], source[768], destination[768];
    struct mf_message response;
    unsigned phase, ticks, progress;
    char avatar_source[768], avatar_remote[96];
    int answered, finished, transfer_status, failed;
    int slow_queued, slow_done, receive_pauses;
    pthread_mutex_t disk_mutex;
    pthread_cond_t disk_condition;
    uint64_t disk_started_tb;
    int disk_released, disk_error;
    unsigned peak_inflight;
    int (*server_receive_ready)(const struct ETCP_ROUTER_CONN*, const uint8_t*, size_t);
    int (*saved_receive_ready)(const struct ETCP_ROUTER_CONN*, const uint8_t*, size_t);
    unsigned restart_tick;
    unsigned preservation_case;
    char preserved_temp[864];
    unsigned mutation_index, busy_responses, dropped_response;
    unsigned deleted_wire, deleted_replies; // биты STAT/QUOTA: запрос дошёл до сервера и получил отказ
    uint64_t deletion_started; // монотонное время публикации удаления для ожидания репликации
    uint64_t deadline_tb; // Общий 300-секундный дедлайн стенда, включая ожидание BUSY.
    etcp_recv_fn reader_receive;
    struct api_case api[8];
    void* timer;
};
static struct network_test* active;
static void tick(void* arg);

static void fail(struct network_test* t, const char* why) {
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files network: FAIL phase=%u reason=%s", t->phase, why);
    t->failed = 1; uasync_stop(t->ua);
}

/* Один таймер шага. Ответ операции может разбудить ожидающую фазу до общего дедлайна. */
static void schedule_tick(struct network_test* t, int delay_tb) {
    if (t->failed) return;
    if (t->timer && uasync_cancel_timeout(t->ua, t->timer)) { fail(t, "cancel scheduled test tick"); return; }
    t->timer = uasync_set_timeout(t->ua, delay_tb, t, tick, "member_files_test");
    if (!t->timer) fail(t, "test tick allocation");
}

static int bulk_phase(const struct network_test* t) { return t->phase >= 24 && t->phase <= 27; }

static void hex(const uint8_t* input, char* out) { for (unsigned i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", input[i]); }

static void slow_disk(void* arg) {
    struct network_test* t = arg;
    pthread_mutex_lock(&t->disk_mutex);
    t->disk_started_tb = get_time_tb();
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files test: disk worker blocked started_tb=%llu",
               (unsigned long long)t->disk_started_tb);
    while (!t->disk_released) {
        int rc = pthread_cond_wait(&t->disk_condition, &t->disk_mutex);
        if (rc) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files test: disk wait failed rc=%d", rc); t->disk_error = rc; break; }
    }
    pthread_mutex_unlock(&t->disk_mutex);
}
static void slow_done(void* arg, int error) {
    struct network_test* t = arg;
    if (error || t->disk_error) fail(t, "slow disk task rejected or wait failed");
    t->slow_done = 1;
}
static void capture_log(int level, const char* category, const char* message) {
    (void)level; (void)category;
    struct network_test* t = active;
    if (strstr(message, "router receive paused:")) t->receive_pauses++;
}

/* Задержка диска начинается с первого DATA; сценарий не зависит от текста и уровня логов. */
static int server_receive_ready(const struct ETCP_ROUTER_CONN* route, const uint8_t* payload, size_t bytes) {
    struct network_test* t = active;
    uint64_t group = route->group_id;
    const uint8_t* peer = route->remote_ed;
    if (bytes >= MF_WIRE_HEADER && payload[1] == MF_REQUEST && mf_get64(payload + 5) >= 3000 && mf_get64(payload + 5) <= 3001) {
        struct mf_message q;
        if (mf_decode(payload, bytes, &q) || group != group_slot || memcmp(peer, t->ed[READER], 32) || memcmp(q.owner, t->ed[AUTHOR], 32) ||
            q.command != (q.request_id == 3000 ? MF_STAT : MF_QUOTA) || mf_server_allowed(route->inst->member_files, group, peer, &q)) {
            fail(t, "deleted reader wire request identity or server ACL"); return 0;
        }
        t->deleted_wire |= 1U << (q.request_id - 3000);
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files test: deleted reader reached server cmd=%s id=%llu allowed=0 wire_mask=%u",
                   mf_command_text(q.command), (unsigned long long)q.request_id, t->deleted_wire);
    }
    if (!t->slow_queued && bytes >= MF_WIRE_HEADER && payload[1] == MF_REQUEST && payload[2] == MF_STREAM_DATA) {
        t->slow_queued = 1;
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files test: first DATA received; delaying disk worker");
        mf_worker_submit(t->nodes[SERVER]->member_files->disk, slow_disk, t, slow_done, t);
    }
    return t->server_receive_ready(route, payload, bytes);
}

static void api_event(void* arg, const struct mf_transfer_progress* p) {
    struct api_case* c = arg;
    if (p->id != c->id || p->done < c->done || p->done > p->total || c->finished) { fail(active, "API progress identity/order"); return; }
    c->done = p->done; c->total = p->total;
    if (p->finished) { c->handle = NULL; c->finished++; c->status = p->status; }
    else if (c->cancel_after_progress && p->done) {
        c->cancel_after_progress = 0; member_files_transfer_cancel(c->handle);
    }
}

static void api_start(struct network_test* t, unsigned index, int node, int upload, const char* remote, const char* source) {
    struct api_case* c = &t->api[index]; memset(c, 0, sizeof(*c)); c->id = 100 + index;
    snprintf(c->local, sizeof(c->local), "%s/api%u.bin", t->dir, index);
    struct mf_transfer_request q = { .inst = t->nodes[node], .id = c->id, .group = group_slot,
         .upload = upload };
        memcpy(q.server, t->ed[SERVER], 32); memcpy(q.owner, t->ed[upload ? node : AUTHOR], 32);
    snprintf(q.remote, sizeof(q.remote), "%s", remote);
    snprintf(q.local, sizeof(q.local), "%s", upload ? source : c->local);
    if (!upload && source) memcpy(q.version, source, 16);
    c->handle = member_files_transfer_start(&q, api_event, c);
    if (!c->handle) fail(t, "parallel API start");
}

static int check_file(const char* path, unsigned size) {
    FILE* file = fopen(path, "rb"); if (!file) return 0;
    int ok = 1;
    for (unsigned i = 0; i < size; i++) if (fgetc(file) != (int)((i * 73 + 19) & 255)) { ok = 0; break; }
    if (fgetc(file) != EOF) ok = 0;
    fclose(file); return ok;
}

/* Чужой temp и прежнее назначение должны пережить отказ до создания своего файла. */
static void preservation_start(struct network_test* t) {
    struct api_case* c = &t->api[7]; memset(c, 0, sizeof(*c)); c->id = 207 + t->preservation_case;
    snprintf(c->local, sizeof(c->local), "%s/preserved.bin", t->dir);
    snprintf(t->preserved_temp, sizeof(t->preserved_temp), "%s.utun-%016llx.part", c->local, (unsigned long long)c->id);
    const char* paths[] = { c->local, t->preserved_temp };
    for (unsigned n = 0; n < 2; n++) {
        FILE* file = fopen(paths[n], "wb");
        if (!file) { fail(t, "preservation fixture open"); return; }
        int error = 0;
        for (unsigned i = 0; i < 37; i++) if (fputc((i * 73 + 19) & 255, file) == EOF) error = 1;
        if (fclose(file) || error) { fail(t, "preservation fixture write/close"); return; }
    }
    struct mf_transfer_request q = { .inst = t->nodes[READER], .id = c->id, .group = group_slot,
         .max_bytes = t->preservation_case == 1 ? BYTES - 1 : 0 };
        memcpy(q.server, t->ed[SERVER], 32); memcpy(q.owner, t->ed[AUTHOR], 32);
    snprintf(q.remote, sizeof(q.remote), "file.bin"); snprintf(q.local, sizeof(q.local), "%s", c->local);
    c->handle = member_files_transfer_start(&q, api_event, c);
    if (!c->handle) { fail(t, "preservation transfer start"); return; }
    if (t->preservation_case == 2) member_files_transfer_cancel(c->handle);
}

static int hold_receive(const struct ETCP_ROUTER_CONN* route, const uint8_t* payload, size_t bytes) {
    (void)route; (void)payload; (void)bytes; return 0;
}

static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    (void)inst;
    struct network_test* t = active;
    if (type != CHAT_EVT_MEMBER_FILES_TRANSFER || len != MF_EVENT_TRANSFER_SIZE) return;
    t->progress++;
    if (data[MF_EVENT_TRANSFER_STATE] == 2) { t->finished = 1; t->transfer_status = data[MF_EVENT_TRANSFER_STATUS] | ((int)data[MF_EVENT_TRANSFER_STATUS + 1] << 8); }
}

static void response(void* arg, const struct mf_message* r) {
    struct network_test* t = arg; t->response = *r; t->answered = 1;
    if (bulk_phase(t) && t->timer) schedule_tick(t, 100);
}

/* Ответы двух независимых запросов могут прийти в любом порядке; каждый обязан быть пустым отказом. */
static void deleted_response(void* arg, const struct mf_message* r) {
    struct network_test* t = arg;
    if (r->request_id < 3000 || r->request_id > 3001 || memcmp(r->owner, t->ed[AUTHOR], 32) ||
        r->command != (r->request_id == 3000 ? MF_STAT : MF_QUOTA) || r->status != MF_ACCESS_DENIED || r->data_len) {
        fail(t, "deleted reader response identity/status or leaked data"); return;
    }
    unsigned bit = 1U << (r->request_id - 3000);
    if (t->deleted_replies & bit) { fail(t, "duplicate deleted reader response"); return; }
    t->deleted_replies |= bit;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files test: deleted reader denied cmd=%s id=%llu replies_mask=%u",
               mf_command_text(r->command), (unsigned long long)r->request_id, t->deleted_replies);
}

/* Потеря первого результата проверяет повтор с тем же ID без повторного mkdir. */
static void mutation_receive(struct ETCP_CONN* conn, struct ll_entry* e) {
    struct network_test* t = active;
    struct mf_message m;
    if (e && e->dgram && e->len > ROUTER_SVC_PAYLOAD_OFF &&
        !mf_decode(e->dgram + ROUTER_SVC_PAYLOAD_OFF, e->len - ROUTER_SVC_PAYLOAD_OFF, &m) &&
        m.kind == MF_RESPONSE && m.command == MF_MKDIR && m.request_id >= 1000 && m.request_id < 1076) {
        if (m.status == MF_BUSY) t->busy_responses++;
        if (!m.status && m.request_id == 1000 && !t->dropped_response) {
            t->dropped_response = 1;
            DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files test: dropped successful mutation response id=1000; waiting for replay");
            queue_dgram_free(e); queue_entry_free(e); return;
        }
    }
    t->reader_receive(conn, e);
}

static void mutation_start(struct network_test* t) {
    struct mf_message q = { .command = t->phase == 26 ? MF_STAT : MF_MKDIR,
        .request_id = t->phase == 26 ? 2000 + 2 * t->mutation_index : 1000 + t->mutation_index };
        memcpy(q.owner, t->ed[READER], 32);
    snprintf(q.path, sizeof(q.path), "bulk_%02u", t->mutation_index); t->answered = 0;
    if (!member_files_request(t->nodes[READER], group_slot, t->ed[SERVER], &q, response, t)) fail(t, "bulk mutation start");
}

static int put_member(struct network_test* t, int destination, int member, int deleted) {
    uint8_t msg[512], join[64], tags_sig[64], tree[64], update[64];
    uint64_t ts = 100000;
    int n = member_sync_build_join_msg(t->channel_x, t->channel_ed, t->nodes[member]->my_ed25519_pubkey, t->keys[member].public_key, msg, sizeof(msg));
    if (n < 0 || sc_ed25519_sign(t->nodes[member]->my_ed25519_privkey, msg, n, join) != SC_OK) return -1;
    const char* tags = deleted ? "{\"deleted\":\"yes\",\"ver\":\"2\"}" :
                       member == SERVER ? "{\"storage\":\"yes\",\"ver\":\"1\"}" : "{\"ver\":\"1\"}";
    n = member_sync_build_owner_msg(group_channel, t->ed[member], tags, msg, sizeof(msg));
    if (n < 0 || sc_ed25519_sign(t->channel_private, msg, n, tags_sig) != SC_OK ||
        member_sync_sign_join(t->channel_private, group_channel, NULL, t->keys[member].public_key, t->ed[member],
            TEST_JOIN_ID(t->channel_x, t->channel_ed, t->nodes[member]->my_ed25519_pubkey, t->keys[member].public_key), ts, tree)) return -1;
    char name[64]; snprintf(name, sizeof(name), "{\"name\":\"node%d\"}", member);
    if (test_member_profile(t->nodes[member]->my_ed25519_privkey,
        TEST_JOIN_ID(t->channel_x, t->channel_ed, t->nodes[member]->my_ed25519_pubkey, t->keys[member].public_key), ts, name, update)) return -1;
    return member_sync_put(t->nodes[destination], group_channel, t->keys[member].public_key, t->ed[member],
                           join, ts, update, ts, name, tags, tags_sig, member == SERVER, 0, tree) < 0 ? -1 : 0;
}

static int create(struct network_test* t, int index) {
    char pub[65], priv[65], server[65], path[768], db[768];
    hex(t->keys[index].public_key, pub); hex(t->seeds[index], priv); hex(t->keys[SERVER].public_key, server);
    snprintf(path, sizeof(path), "%s/node%d.conf", t->dir, index); snprintf(db, sizeof(db), "%s/db%d", t->dir, index);
    FILE* f = fopen(path, "w"); if (!f) return -1;
    fprintf(f, "[global]\nmy_private_key=%s\nmy_public_key=%s\ndb_path=%s\n"
               "[server:s1]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=1\n"
               "[chatserver]\nstorage_autoload=0\nmember_files_user_quota_mb=8\n", priv, pub, db);
    if (index != SERVER) fprintf(f, "[client:to_m]\npeer_public_key=%s\nlink=s1:127.0.0.1:0\n", server);
    fclose(f);
    t->nodes[index] = utun_instance_create(t->ua, path);
    if (!t->nodes[index]) return -1;
    memcpy(t->ed[index], t->nodes[index]->my_ed25519_pubkey, 32); t->ids[index] = t->nodes[index]->node_id;
    return 0;
}

static int setup(struct network_test* t) {
    uint8_t x_private[32];
    if (chat_channel_generate_keys(t->channel_x, x_private, t->channel_ed, t->channel_private)) return -1;
    sc_public_key_hex(t->channel_ed, group_channel);
    uint8_t signature[64];
    if (test_group_signature(group_channel, "member-files", t->nodes[AUTHOR]->my_ed25519_pubkey, t->channel_x, t->channel_ed, t->channel_private, signature)) return -1;
    for (unsigned i = 0; i < NODE_COUNT; i++) {
        if (topo_node_sqlite_channel_put(t->nodes[i]->topo_sqlite_db, group_channel, "member-files", t->nodes[AUTHOR]->my_ed25519_pubkey,
            t->channel_x, NULL, t->channel_ed, i == AUTHOR ? t->channel_private : NULL, signature)) return -1;
        chat_core_ensure_channel_ready(t->nodes[i], group_channel);
        uint64_t slot = topo_groups_channel_slot(t->nodes[i]->topo_groups, group_channel);
        if (!i) group_slot = slot;
        if (!slot || slot != group_slot) return -1;
        for (unsigned j = 0; j < NODE_COUNT; j++) if (put_member(t, i, j, 0)) return -1;
    }
    snprintf(t->source, sizeof(t->source), "%s/source.bin", t->dir);
    snprintf(t->destination, sizeof(t->destination), "%s/download.bin", t->dir);
    FILE* file = fopen(t->source, "wb"); if (!file) return -1;
    for (unsigned i = 0; i < BYTES; i++) if (fputc((i * 73 + 19) & 255, file) == EOF) { fclose(file); return -1; }
    return fclose(file);
}

static void transfer(struct network_test* t, int upload, uint64_t id) {
    struct mf_transfer_request* q = u_calloc(1, sizeof(*q));
    if (!q) { fail(t, "transfer allocation"); return; }
    q->inst = t->nodes[upload ? AUTHOR : READER]; memcpy(q->group_key, t->channel_ed, 32); memcpy(q->server, t->ed[SERVER], 32); memcpy(q->owner, t->ed[AUTHOR], 32);
    q->id = id; q->upload = upload;
    snprintf(q->remote, sizeof(q->remote), "file.bin"); snprintf(q->local, sizeof(q->local), "%s", upload ? t->source : t->destination);
    t->finished = 0; member_files_gui_transfer(q);
}

static void request(struct network_test* t, uint8_t command, const uint8_t owner[32]) {
    struct mf_message q = { .command = command };
    if (owner) memcpy(q.owner, owner, 32);
    if (owner && command != MF_QUOTA) snprintf(q.path, sizeof(q.path), "file.bin");
    t->answered = 0;
    if (!member_files_request(t->nodes[READER], group_slot, t->ed[SERVER], &q, response, t)) fail(t, "request start");
}

static int downloaded(struct network_test* t) {
    return check_file(t->destination, BYTES);
}

static void tick(void* arg) {
    struct network_test* t = arg; t->timer = NULL;
    uint64_t now = get_time_tb();
    if (now >= t->deadline_tb) { fail(t, "300s deadline including receipt expiry"); return; }
    t->ticks++;
    // Отсчитываем полные 6с от фактической блокировки worker, не от постановки задания.
    pthread_mutex_lock(&t->disk_mutex);
    if (t->disk_started_tb && !t->disk_released && get_time_tb() - t->disk_started_tb >= 60000) {
        t->disk_released = 1;
        pthread_cond_signal(&t->disk_condition);
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files test: disk released elapsed_tb=%llu receive_pauses=%d peak_inflight=%u",
                   (unsigned long long)(get_time_tb() - t->disk_started_tb), t->receive_pauses, t->peak_inflight);
    }
    pthread_mutex_unlock(&t->disk_mutex);
    struct ETCP_ROUTER_CONN* route = etcp_router_conn_get(t->nodes[AUTHOR], group_slot, t->ids[SERVER], ETCP_RT_ID_MEMBER_FILES);
    if (route->inflight_q->count > t->peak_inflight) t->peak_inflight = route->inflight_q->count;
    if (t->phase == 100 && t->nodes[AUTHOR]->member_files->storage_status == MF_OK &&
        chat_core_member_online(t->nodes[READER], group_channel, t->nodes[AUTHOR]->my_ed25519_pubkey)) {
        assert(!member_files_member(t->nodes[AUTHOR], group_slot, t->ed[AUTHOR], 1));
        struct mf_message q = {.command=MF_MKDIR};
        memcpy(q.owner, t->ed[AUTHOR], 32); strcpy(q.path, "avatars");
        t->answered = 0; t->phase = 101;
        if (!member_files_request(t->nodes[AUTHOR], group_slot, t->ed[AUTHOR], &q, response, t)) fail(t, "own avatar mkdir");
    } else if (t->phase == 101 && t->answered) {
        if (t->response.status != MF_OK) { fail(t, "own avatar mkdir denied"); return; }
        struct api_case* c = &t->api[0]; memset(c, 0, sizeof(*c)); c->id = 9100;
        struct mf_transfer_request q = {.inst=t->nodes[AUTHOR], .id=c->id, .group=group_slot,
             .upload=1};
        memcpy(q.server, t->ed[AUTHOR], 32); memcpy(q.owner, t->ed[AUTHOR], 32);
        strcpy(q.local, t->avatar_source); strcpy(q.remote, t->avatar_remote); t->phase = 102;
        c->handle = member_files_transfer_start(&q, api_event, c); if (!c->handle) fail(t, "own avatar upload start");
    } else if (t->phase == 102 && t->api[0].finished) {
        if (t->api[0].status) { fail(t, "own avatar upload"); return; }
        struct chat_avatar_req* q = u_calloc(1, sizeof(*q)); assert(q);
        q->inst = t->nodes[AUTHOR]; q->id = 9102; memcpy(q->group, t->channel_ed, 32); memcpy(q->server, t->ed[AUTHOR], 32);
        strcpy(q->file, t->avatar_remote + 8); chat_core_set_my_avatar_trampoline(q); t->phase = 104;
    } else if (t->phase == 104) {
        uint8_t wire[CHAT_MEMBER_DISPLAY_SIZE]; struct chat_member_display member;
        if (chat_core_get_single_member(t->nodes[READER], group_channel, t->nodes[AUTHOR]->my_ed25519_pubkey, wire)) goto again;
        chat_core_deserialize_member(wire, &member);
        if (memcmp(member.avatar_server, t->ed[AUTHOR], 32) || strcmp(member.avatar_file, t->avatar_remote + 8)) goto again;
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS own avatar signed reference reached reader server=%s file=%s",
            sc_public_key_hex(member.avatar_server, (char[65]){0}), member.avatar_file);
        struct api_case* c = &t->api[0]; memset(c, 0, sizeof(*c)); c->id = 9101;
        snprintf(c->local, sizeof(c->local), "%s/own-avatar-received.png", t->dir);
        struct mf_transfer_request q = {.inst=t->nodes[READER], .id=c->id, .group=group_slot,
            };
        memcpy(q.server, t->ed[AUTHOR], 32); memcpy(q.owner, t->ed[AUTHOR], 32);
        strcpy(q.local, c->local); strcpy(q.remote, t->avatar_remote); t->phase = 103;
        c->handle = member_files_transfer_start(&q, api_event, c); if (!c->handle) fail(t, "own avatar download start");
    } else if (t->phase == 103 && t->api[0].finished) {
        if (t->api[0].status || !check_file(t->api[0].local, 37)) { fail(t, "own avatar remote read"); return; }
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS own avatar: author without storage uploaded locally; reader received exact bytes");
        t->phase = 0;
    } else     if (t->phase == 0) {
        /* CM нужен известный адрес цели; готовность storage не означает, что NODEINFO уже дошёл до клиентов.
         * Не открываем CM заранее: запрос маршрута по-прежнему принадлежит самой передаче. */
        struct TOPO_NODE* author_node = topo_node_registry_find(t->nodes[AUTHOR]->topo_groups, t->ids[SERVER]);
        struct TOPO_NODE* reader_node = topo_node_registry_find(t->nodes[READER]->topo_groups, t->ids[SERVER]);
        int author_ready = author_node && author_node->timestamp && (author_node->v4_addrs || author_node->v6_addrs);
        int reader_ready = reader_node && reader_node->timestamp && (reader_node->v4_addrs || reader_node->v6_addrs);
        if (t->nodes[SERVER]->member_files->storage_status == MF_OK && author_ready && reader_ready) {
            DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files test: storage and target snapshots ready; upload opens its own CM route request");
            t->phase = 1; transfer(t, 1, 1);
        } else if (t->ticks == 1 || t->ticks % 100 == 0)
            DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files test: waiting for setup storage=%s author_snapshot=%d reader_snapshot=%d",
                        mf_status_text(t->nodes[SERVER]->member_files->storage_status), author_ready, reader_ready);
    } else if (t->phase == 1 && t->finished) {
        if (t->transfer_status) { fail(t, mf_status_text(t->transfer_status)); return; }
        t->phase = 2; request(t, MF_STAT, t->ed[AUTHOR]);
    } else if (t->phase == 2 && t->answered) {
        if (t->response.status || t->response.size != BYTES) { fail(t, "STAT after upload"); return; }
        t->phase = 20; request(t, MF_QUOTA, t->ed[AUTHOR]);
    } else if (t->phase == 20 && t->answered) {
        if (t->response.status || t->response.data_len != MF_QUOTA_BYTES || mf_get64(t->response.data) != BYTES ||
            mf_get64(t->response.data + 8) != 8 * 1024 * 1024 || mf_get64(t->response.data + 16) ||
            mf_get64(t->response.data + 24) != 1 || mf_get64(t->response.data + 32)) {
            fail(t, "signed/encrypted quota response"); return;
        }
        t->phase = 3; request(t, MF_BEGIN_WRITE, t->ed[AUTHOR]);
    } else if (t->phase == 3 && t->answered) {
        if (t->response.status != MF_ACCESS_DENIED) { fail(t, "foreign write accepted"); return; }
        t->phase = 4; transfer(t, 0, 2);
    } else if (t->phase == 4 && t->finished) {
        if (t->transfer_status || !downloaded(t)) { fail(t, "download bytes"); return; }
        t->phase = 5; request(t, MF_LIST, NULL);
    } else if (t->phase == 5 && t->answered) {
        if (t->response.status || !t->response.data_len) { fail(t, "virtual group root"); return; }
        t->phase = 6; t->answered = 0;
        struct mf_message q = { .command = MF_STAT };
        memcpy(q.owner, t->ed[AUTHOR], 32);
        snprintf(q.path, sizeof(q.path), "file.bin");
        if (!member_files_request(t->nodes[SERVER], group_slot, t->ed[SERVER], &q, response, t)) { fail(t, "local server request"); return; }
    } else if (t->phase == 6 && t->answered) {
        if (t->response.status || t->response.size != BYTES) { fail(t, "local server STAT"); return; }
        if (!t->slow_done || !t->receive_pauses || t->peak_inflight < 2) { fail(t, "stream did not use receive backpressure/pipelining"); return; }
        // Переполненная очередь запросов не должна блокировать ответы в обратном направлении.
        struct UTUN_INSTANCE* inst = t->nodes[AUTHOR]; struct member_files* files = inst->member_files;
        unsigned jobs = files->job_count; files->job_count = 32;
        struct mf_message q = { .kind = MF_RESPONSE, .command = MF_STAT, .request_id = 1 };
        struct ETCP_ROUTER_CONN route = { .inst = inst, .group_id = group_slot, .remote_node_id = t->ids[SERVER] };
        memcpy(route.remote_ed, t->ed[SERVER], 32);
        uint8_t wire[MF_WIRE_MAX]; int bytes = mf_encode(&q, wire, sizeof(wire));
        int response_ready = inst->router_bindings.receive_ready[ETCP_RT_ID_MEMBER_FILES](&route, wire, bytes);
        q.kind = MF_REQUEST; bytes = mf_encode(&q, wire, sizeof(wire));
        int request_ready = inst->router_bindings.receive_ready[ETCP_RT_ID_MEMBER_FILES](&route, wire, bytes);
        files->job_count = jobs;
        if (!response_ready || request_ready) { fail(t, "full request queue blocked response or admitted request"); return; }
        t->phase = 8;
        api_start(t, 0, AUTHOR, 1, "parallel-a.bin", t->source);
        api_start(t, 1, AUTHOR, 1, "parallel-b.bin", t->source);
        api_start(t, 2, AUTHOR, 0, "file.bin", NULL);
        api_start(t, 3, AUTHOR, 1, "cancelled.bin", t->source); t->api[3].cancel_after_progress = 1;
        api_start(t, 4, SERVER, 0, "file.bin", NULL);
        api_start(t, 5, SERVER, 1, "local.bin", t->source);
        char empty[768]; snprintf(empty, sizeof(empty), "%s/empty.bin", t->dir);
        FILE* file = fopen(empty, "wb"); if (!file) { fail(t, "empty source"); return; } fclose(file);
        api_start(t, 6, AUTHOR, 1, "empty.bin", empty);
    } else if (t->phase == 8) {
        for (unsigned i = 0; i < 7; i++) if (!t->api[i].finished) goto again;
        for (unsigned i = 0; i < 7; i++) if (t->api[i].status != (i == 3 ? MF_CANCELLED : MF_OK)) { fail(t, "parallel completion status"); return; }
        if (!check_file(t->api[2].local, BYTES) || !check_file(t->api[4].local, BYTES)) { fail(t, "duplex/local download bytes"); return; }
        t->phase = 9;
        api_start(t, 0, AUTHOR, 0, "parallel-a.bin", NULL);
        api_start(t, 1, AUTHOR, 0, "parallel-b.bin", NULL);
        api_start(t, 7, AUTHOR, 0, "empty.bin", NULL);
    } else if (t->phase == 9) {
        if (!t->api[0].finished || !t->api[1].finished || !t->api[7].finished) goto again;
        if (t->api[0].status || t->api[1].status || t->api[7].status || !check_file(t->api[0].local, BYTES) ||
            !check_file(t->api[1].local, BYTES) || !check_file(t->api[7].local, 0)) { fail(t, "parallel/empty download bytes"); return; }
        t->phase = 22;
        uint8_t stale[16]; memset(stale, 0xa5, sizeof(stale));
        api_start(t, 7, READER, 0, "file.bin", (const char*)stale);
    } else if (t->phase == 22 && t->api[7].finished) {
        if (t->api[7].status != MF_STALE || !check_file(t->api[7].local, 0)) {
            fail(t, "stale download changed existing destination"); return;
        }
        t->phase = 23; preservation_start(t);
    } else if (t->phase == 23 && t->api[7].finished) {
        const int expected[] = { MF_EXISTS, MF_INVALID_RANGE, MF_CANCELLED };
        int target_ok = check_file(t->api[7].local, 37), temp_ok = check_file(t->preserved_temp, 37);
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files: preservation case=%u status=%s expected=%s target=%d foreign_temp=%d",
            t->preservation_case, mf_status_text(t->api[7].status), mf_status_text(expected[t->preservation_case]), target_ok, temp_ok);
        if (t->api[7].status != expected[t->preservation_case] || !target_ok || !temp_ok) {
            fail(t, "download refusal/cancel removed unowned temp or changed target"); return;
        }
        if (++t->preservation_case < 3) { preservation_start(t); goto again; }
        t->phase = 24;
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files test: bulk mutations and removals advance on response; deadline_tb=%llu",
                   (unsigned long long)t->deadline_tb);
        t->reader_receive = t->nodes[READER]->router_bindings.callbacks[ETCP_RT_ID_MEMBER_FILES];
        t->nodes[READER]->router_bindings.callbacks[ETCP_RT_ID_MEMBER_FILES] = mutation_receive;
        mutation_start(t);
    } else if (t->phase == 24 && t->answered) {
        if (t->response.status) { fail(t, "bulk mutation did not survive busy/replay"); return; }
        if (++t->mutation_index < 76) { mutation_start(t); goto again; }
        t->nodes[READER]->router_bindings.callbacks[ETCP_RT_ID_MEMBER_FILES] = t->reader_receive;
        if (!t->dropped_response || !t->busy_responses) { fail(t, "bulk mutation did not exercise lost result and busy"); return; }
        t->phase = 25; request(t, MF_QUOTA, t->ed[READER]);
    } else if (t->phase == 25 && t->answered) {
        if (t->response.status || t->response.data_len != MF_QUOTA_BYTES || mf_get64(t->response.data) ||
            mf_get64(t->response.data + 16) || mf_get64(t->response.data + 24) || mf_get64(t->response.data + 32) != 76) {
            fail(t, "bulk mutation duplicated or lost directories"); return;
        }
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files test: PASS76 mutations, lost response replayed once, busy_responses=%u directories=76",
            t->busy_responses);
        t->phase = 26; t->mutation_index = 0; mutation_start(t);
    } else if (t->phase == 26 && t->answered) {
        if (t->response.status) { fail(t, "bulk removal did not survive busy"); return; }
        if (t->response.command == MF_STAT) {
            struct mf_message q = t->response; q.command = MF_REMOVE_PATH; q.request_id++; t->answered = 0;
            if (!member_files_request(t->nodes[READER], group_slot, t->ed[SERVER], &q, response, t)) fail(t, "bulk remove start");
            goto again;
        }
        if (++t->mutation_index < 76) { mutation_start(t); goto again; }
        t->phase = 27; request(t, MF_QUOTA, t->ed[READER]);
    } else if (t->phase == 27 && t->answered) {
        if (t->response.status || t->response.data_len != MF_QUOTA_BYTES || mf_get64(t->response.data) ||
            mf_get64(t->response.data + 16) || mf_get64(t->response.data + 24) || mf_get64(t->response.data + 32)) {
            fail(t, "bulk removal left directories or quota"); return;
        }
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files test: PASS76 removals, directories=0, used=0, reserved=0");
        t->phase = 10; t->answered = 0;
        struct mf_message q = { .command = MF_STAT };
        memcpy(q.owner, t->ed[AUTHOR], 32); snprintf(q.path, sizeof(q.path), "cancelled.bin");
        if (!member_files_request(t->nodes[READER], group_slot, t->ed[SERVER], &q, response, t)) { fail(t, "cancelled STAT"); return; }
    } else if (t->phase == 10 && t->answered) {
        if (t->response.status != MF_NOT_FOUND) { fail(t, "cancelled upload published"); return; }
        t->saved_receive_ready = t->nodes[AUTHOR]->router_bindings.receive_ready[ETCP_RT_ID_MEMBER_FILES];
        etcp_router_set_receive_ready(t->nodes[AUTHOR], ETCP_RT_ID_MEMBER_FILES, hold_receive);
        api_start(t, 0, AUTHOR, 1, "restart.bin", t->source);
        api_start(t, 1, AUTHOR, 0, "file.bin", NULL);
        t->restart_tick = t->ticks; t->phase = 11;
    } else if (t->phase == 11 && t->ticks > t->restart_tick + 2) {
        etcp_router_conn_restart(t->nodes[AUTHOR], group_slot, t->ids[SERVER], ETCP_RT_ID_MEMBER_FILES);
        etcp_router_set_receive_ready(t->nodes[AUTHOR], ETCP_RT_ID_MEMBER_FILES, t->saved_receive_ready);
        t->phase = 12;
    } else if (t->phase == 12) {
        if (!t->api[0].finished || !t->api[1].finished || t->nodes[SERVER]->member_files->server_transfer_count) goto again;
        if (t->api[0].status != MF_IO_ERROR || t->api[1].status != MF_IO_ERROR) { fail(t, "restart completion status"); return; }
        if (put_member(t, SERVER, READER, 1)) { fail(t, "signed deletion"); return; }
        int reader_allowed = member_files_member(t->nodes[READER], group_slot, t->ed[READER], 0);
        int server_allowed = member_files_member(t->nodes[SERVER], group_slot, t->ed[READER], 0);
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files test: deletion published reader_local_allowed=%d server_reader_allowed=%d",
                   reader_allowed, server_allowed);
        if (!reader_allowed || server_allowed) { fail(t, "deleted reader ACL fixture"); return; }
        t->phase = 21; t->deletion_started = get_time_tb();
        struct TOPO_GROUP* group = topo_groups_find(t->nodes[SERVER]->topo_groups, group_slot);
        if (topo_group_peer_ready(group, t->ids[READER])) { fail(t, "deleted reader retained group session"); return; }
        /* Исключение закрывает маршрут: серверный ACL проверяем без ожидания ответа по уже снятой сессии. */
        for (unsigned i = 0; i < 2; i++) {
            struct mf_message q = { .command = i ? MF_QUOTA : MF_STAT, .request_id = 3000 + i };
        memcpy(q.owner, t->ed[AUTHOR], 32);
            if (!i) snprintf(q.path, sizeof(q.path), "file.bin");
            if (mf_server_allowed(t->nodes[SERVER]->member_files, group_slot, t->ed[READER], &q)) {
                fail(t, "deleted reader server ACL accepted"); return;
            }
        }
    } else if (t->phase == 7 || t->phase == 21) {
        if (get_time_tb() - t->deletion_started >= 100000) { fail(t, "deleted reader replication timeout"); return; }
        t->phase = 21;
        /* Состав распространяется оставшимся участникам; исключённому сервер уже не выдаёт MEMBERS. */
        if (member_files_member(t->nodes[AUTHOR], group_slot, t->ed[READER], 0)) goto again;
        if (put_member(t, READER, READER, 1)) { fail(t, "local signed revocation"); return; }
        if (!member_files_member(t->nodes[READER], group_slot, t->ed[SERVER], 1)) { fail(t, "storage role lost during deletion"); return; }
        unsigned pending = t->nodes[READER]->member_files->pending_count;
        for (unsigned i = 0; i < 2; i++) {
            struct mf_message q = { .command = i ? MF_QUOTA : MF_STAT, .request_id = 4000 + i };
        memcpy(q.owner, t->ed[AUTHOR], 32);
            if (!i) snprintf(q.path, sizeof(q.path), "file.bin");
            if (member_files_request(t->nodes[READER], group_slot, t->ed[SERVER], &q, deleted_response, t)) {
                fail(t, "deleted reader local request accepted"); return;
            }
        }
        if (t->nodes[READER]->member_files->pending_count != pending) { fail(t, "local ACL refusal created pending requests"); return; }
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files test: deleted reader local_allowed=0 storage_allowed=1 wire=%u replies=%u pending=%u",
                   t->deleted_wire, t->deleted_replies, pending);
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files network: PASS progress=%u ticks=%u peak_inflight=%u pauses=%d",
            t->progress, t->ticks, t->peak_inflight, t->receive_pauses);
        uasync_stop(t->ua); return;
    }
again:
    /* Пока ждём итог запроса, сеть и штатные таймеры продолжают обслуживаться обычным poll. */
    schedule_tick(t, bulk_phase(t) && !t->answered ? (int)(t->deadline_tb - now) : 100);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); debug_set_category_level(DEBUG_CATEGORY_MEDIA, DEBUG_LEVEL_DEBUG);
    debug_set_category_level(DEBUG_CATEGORY_GENERAL, getenv("UTUN_TEST_DEBUG") ? DEBUG_LEVEL_DEBUG : DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_BGP, getenv("UTUN_TEST_DEBUG") ? DEBUG_LEVEL_DEBUG : DEBUG_LEVEL_WARN);
    test_clock_init(1000);
    debug_set_category_level(DEBUG_CATEGORY_ETCPROUTE, DEBUG_LEVEL_DEBUG);
    utun_instance_set_tun_init_enabled(0);
    struct network_test t = { .disk_mutex = PTHREAD_MUTEX_INITIALIZER, .disk_condition = PTHREAD_COND_INITIALIZER }; active = &t;
    debug_set_log_hook(capture_log);
    snprintf(t.dir, sizeof(t.dir), "/tmp/utun_member_files_net_XXXXXX");
    if (test_mkdtemp(t.dir)) return 1;
    t.ua = uasync_create();
    for (unsigned i = 0; i < NODE_COUNT; i++) if (sc_identity_generate_config(t.keys[i].public_key, t.seeds[i])) return 1;
    for (unsigned i = 0; i < NODE_COUNT; i++) if (create(&t, i)) { t.failed = 1; goto stop; }
    for (unsigned i = 0; i < NODE_COUNT; i++) {
        if (i != SERVER) test_etcp_set_peer(t.nodes[i], "to_m", t.nodes[SERVER], "s1");
        if (utun_instance_init(t.nodes[i])) { t.failed = 1; goto stop; }
        chat_event_set_handler(t.nodes[i], event);
    }
    if (setup(&t)) { t.failed = 1; goto stop; }
    t.server_receive_ready = t.nodes[SERVER]->router_bindings.receive_ready[ETCP_RT_ID_MEMBER_FILES];
    etcp_router_set_receive_ready(t.nodes[SERVER], ETCP_RT_ID_MEMBER_FILES, server_receive_ready);
    t.phase = 100;
    snprintf(t.avatar_source, sizeof(t.avatar_source), "%s/own-avatar.png", t.dir);
    uint8_t data[37], hash[32]; for (unsigned i=0; i<sizeof(data); i++) data[i] = (i * 73 + 19) & 255;
    FILE* avatar = fopen(t.avatar_source, "wb"); assert(avatar);
    assert(fwrite(data, 1, sizeof(data), avatar) == sizeof(data) && !fclose(avatar));
    SHA256(data, sizeof(data), hash); char hash_hex[65]; hex(hash, hash_hex);
    snprintf(t.avatar_remote, sizeof(t.avatar_remote), "avatars/%s.json", hash_hex);
    t.deadline_tb = get_time_tb() + 3000000;
    schedule_tick(&t, 100);
    test_clock_mainloop(t.ua);
    if (!t.failed) {
        // Stop/destroy вне callbacks: server cleanup ещё использует storage после текущего I/O.
        api_start(&t, 0, AUTHOR, 1, "stop.bin", t.source);
        api_start(&t, 1, AUTHOR, 0, "file.bin", NULL);
        uint64_t deadline = get_time_tb() + 50000;
        while (!t.nodes[SERVER]->member_files->server_transfer_count && get_time_tb() < deadline) {
            test_clock_poll(t.ua, 10);
        }
        if (!t.nodes[SERVER]->member_files->server_transfer_count) fail(&t, "server stream did not start before teardown");
    }
stop:
    if (t.timer) uasync_cancel_timeout(t.ua, t.timer);
    // При ошибке сценария освобождаем worker до destroy, иначе drain будет ждать блокировку.
    pthread_mutex_lock(&t.disk_mutex);
    t.disk_released = 1; pthread_cond_signal(&t.disk_condition);
    pthread_mutex_unlock(&t.disk_mutex);
    for (unsigned i = 0; i < NODE_COUNT; i++) if (t.nodes[i]) utun_instance_destroy(t.nodes[i]);
    if (!t.failed && (!t.api[0].finished || !t.api[1].finished || t.api[0].status != MF_CANCELLED || t.api[1].status != MF_CANCELLED))
        t.failed = 1;
    uasync_destroy(t.ua, 0);
    pthread_cond_destroy(&t.disk_condition); pthread_mutex_destroy(&t.disk_mutex);
    debug_set_log_hook(NULL);
    test_clock_finish();
    printf("member_files network: %s phase=%u\n", t.failed ? "FAIL" : "PASS", t.phase); return t.failed;
}
