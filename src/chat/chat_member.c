/*
 * chat_member.c — единый код запроса мемберов для GUI
 *
 * Используется: chatgui (desktop), headless CLI, Android.
 * Live-состояние участников читается только в uasync-потоке.
 */

#include "chat_core_priv.h"
#include "chat_member.h"
#include "chat_member_read.h"
#include "chat_event.h"
#include "member_sync.h"
#include "../member_files/protocol.h"

#include "../utun_instance.h"
#include "../routing_layer/topo_group.h"
#include "../radio/radio.h"
#include "../call/call.h"
#include "../../lib/mem.h"
#include "../../lib/db_async.h"
#include "../../lib/ll_queue.h"
#include "../../lib/platform_compat.h"
#include "../../lib/u_async.h"

#include <stdlib.h>

/* ─── live-онлайн: тот же живой путь, которым пользуется маршрутизатор ─── */

int chat_core_member_online(struct UTUN_INSTANCE* inst, const char* ch_id, const uint8_t ed[32]) {
    if (!inst || !CC(inst) || !ed || !chat_channel_valid(ch_id)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "member online: inactive instance or invalid channel/key"); return 0;
    }
    if (!memcmp(ed, inst->my_ed25519_pubkey, 32)) return 1;
    struct TOPO_GROUP* group = inst->topo_groups ? topo_groups_find_channel(inst->topo_groups, ch_id) : NULL;
    return group && group->group_type == TOPO_GROUP_TYPE_CHAT && topo_node_best_path(topo_node_find_by_ed(group, ed));
}

/* Состояние звонка/рации берётся у владельца ключа из того же снимка маршрутов. */
static uint8_t member_radio_flag(struct UTUN_INSTANCE* inst, struct TOPO_GROUP* group, struct TOPO_GROUP_NODE* node, int self) {
    if (!group || group->group_type != TOPO_GROUP_TYPE_CHAT) return 0;
    uint8_t status = 0, flags = 0;
    if (self) {
        status = call_phone_status(inst);
        flags = group->radio_active ? CHAT_MEMBER_FLAG_RADIO : 0;
    } else if (node) {
        status = node->phone_status;
        flags = node->radio ? CHAT_MEMBER_FLAG_RADIO : 0;
    }
    if (status & TOPO_FLAG_PHONE_KNOWN) flags |= CHAT_MEMBER_FLAG_PHONE_KNOWN;
    if (status & TOPO_FLAG_PHONE_BUSY) flags |= CHAT_MEMBER_FLAG_PHONE_BUSY;
    return flags;
}

/* ─── сериализация (struct → wire) ─── */

void chat_core_serialize_member(const struct chat_member_display* src, uint8_t* dst) {
    if (!src || !dst) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member serialization: missing argument"); return; }
    dst[0]  = src->online;
    dst[1]  = src->is_self;
    dst[2] = src->flags;
    dst[3] = (uint8_t)src->rtt; dst[4] = (uint8_t)(src->rtt >> 8);
    dst[5] = src->nat_type;
    dst[6] = src->storage;
    /* Поле на проводе всегда завершается NUL и не содержит хвост памяти исходной строки. */
    strncpy((char*)dst + 7, src->name, sizeof(src->name) - 1);
    dst[70] = 0;
    memcpy(dst + 71, src->avatar_server, sizeof(src->avatar_server));
    strncpy((char*)dst + 103, src->avatar_file, 69); dst[172] = 0;
    dst[173] = src->storage_role;
    dst[174] = src->verified; dst[175] = src->node_type;
    dst[176] = src->conn_presence; dst[177] = src->conn_up;
    dst[178] = (uint8_t)src->intermediaries; dst[179] = (uint8_t)(src->intermediaries >> 8);
    memcpy(dst + 180, src->devgroup, 32);
    strncpy((char*)dst + 212, src->devgroup_name, 63); dst[275] = 0;
    dst[276] = src->protocol_state;
    dst[277] = (uint8_t)src->protocol_local; dst[278] = (uint8_t)(src->protocol_local >> 8);
    dst[279] = (uint8_t)src->protocol_remote; dst[280] = (uint8_t)(src->protocol_remote >> 8);
    memcpy(dst + CHAT_MEMBER_ED_OFF, src->ed25519, sizeof(src->ed25519));
    DEBUG_TRACE(DEBUG_CATEGORY_CHAT_SYNC, "member snapshot: serialized peer=%s name_bytes=%zu zero_padded=1",
                sc_public_key_hex(src->ed25519, (char[65]){0}), strlen((const char*)dst + 7));
}

int chat_core_deserialize_member(const uint8_t* src, struct chat_member_display* dst) {
    if (!src || !dst) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member deserialization: missing argument"); return -1; }
    memcpy(dst->ed25519, src + CHAT_MEMBER_ED_OFF, sizeof(dst->ed25519));
    if (mf_zero(dst->ed25519, 32)) { DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "member deserialization: missing Ed25519"); return -1; }
    memcpy(dst->devgroup, src + 180, 32);
    memcpy(dst->devgroup_name, src + 212, 64); dst->devgroup_name[63] = 0;
    dst->protocol_state = src[276];
    dst->protocol_local = (uint16_t)src[277] | ((uint16_t)src[278] << 8);
    dst->protocol_remote = (uint16_t)src[279] | ((uint16_t)src[280] << 8);
    dst->online   = src[0];
    dst->is_self  = src[1];
    dst->flags    = src[2];
    dst->rtt = (uint16_t)src[3] | ((uint16_t)src[4] << 8);
    dst->nat_type = src[5];
    dst->storage  = src[6];
    memcpy(dst->name,      src + 7, 64);
    dst->name[63] = '\0';
    memcpy(dst->avatar_server, src + 71, sizeof(dst->avatar_server));
    memcpy(dst->avatar_file, src + 103, 70); dst->avatar_file[69] = 0;
    dst->storage_role = src[173];
    dst->verified = src[174]; dst->node_type = src[175];
    dst->conn_presence = src[176]; dst->conn_up = src[177];
    dst->intermediaries = (uint16_t)src[178] | ((uint16_t)src[179] << 8);
    return 0;
}

/* Live-часть заполняется после SQL; повторный вызов заменяет только текущие сетевые поля. */
int chat_member_apply_live(struct UTUN_INSTANCE* inst, const char* ch_id, struct chat_member_display* m) {
    if (!inst || !chat_channel_valid(ch_id) || !m) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member live snapshot: invalid instance/channel/member"); return -1;
    }
    m->flags &= ~(CHAT_MEMBER_FLAG_RADIO | CHAT_MEMBER_FLAG_PHONE_KNOWN | CHAT_MEMBER_FLAG_PHONE_BUSY);
    m->online = m->conn_presence = m->conn_up = m->protocol_state = 0;
    m->rtt = UINT16_MAX; m->intermediaries = m->protocol_remote = 0;
    struct TOPO_GROUP* group = inst->topo_groups ? topo_groups_find_channel(inst->topo_groups, ch_id) : NULL;
    struct TOPO_GROUP_NODE* node = topo_node_find_by_ed(group, m->ed25519);
    m->flags |= member_radio_flag(inst, group, node, m->is_self);
    const struct topo_peer_protocol* protocol = topo_group_peer_protocol(group, m->ed25519);
    m->protocol_local = TOPO_GROUP_PROTOCOL_VERSION;
    if (protocol) { m->protocol_remote = protocol->version; m->protocol_state = protocol->state; }
    if (node) {
        m->conn_presence = node->conn_presence; m->conn_up = node->conn_up;
        struct TOPO_NODEPATH* path = topo_node_best_path(node);
        if (path) {
            m->rtt = topo_get_chain_rtt_avg(node);
            m->intermediaries = path->hop_count > 0 ? path->hop_count - 1 : 0;
            m->online = 1;
        }
    }
    if (m->is_self) m->online = 1;
    if (m->flags & CHAT_MEMBER_FLAG_DELETED) m->online = 0;
    if (!m->online) m->flags &= ~(CHAT_MEMBER_FLAG_PHONE_KNOWN | CHAT_MEMBER_FLAG_PHONE_BUSY);
    return 0;
}

/* Онлайн выше offline, затем полный ключ в лексикографическом порядке. */
static int compare_members(const void* left, const void* right) {
    const struct chat_member_display *a = left, *b = right;
    if (a->online != b->online) return a->online > b->online ? -1 : 1;
    return memcmp(a->ed25519, b->ed25519, sizeof(a->ed25519));
}

enum member_read_events { MEMBER_READ_LIST = 1, MEMBER_READ_UPDATES = 2 };

/* Только эта часть запроса доступна worker; runtime-поля внешнего job принадлежат uasync. */
struct member_sql_query {
    char channel[65];
    uint8_t self[32];
    uint8_t only_ed[32];
    struct chat_member_display* rows;
    size_t count;
};

struct member_read_job {
    struct member_sql_query sql;
    struct member_read_job* next;
    struct UTUN_INSTANCE* owner;
    sqlite3* expected_db;
    unsigned events;
    int again, suppress;
};

static void member_read_request(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t ed[32], unsigned events, int refresh);

static int member_read_work(sqlite3* db, void* arg) {
    struct member_sql_query* query = arg;
    return chat_member_rows_read(db, query->channel, query->self, mf_zero(query->only_ed, 32) ? NULL : query->only_ed, &query->rows, &query->count);
}

/* Job остаётся в списке во время UI callbacks: удаление канала/stop отсоединяют и текущую доставку. */
static void member_read_done(void* arg, const struct db_async_result* result) {
    struct member_read_job* job = arg;
    struct UTUN_INSTANCE* inst = job->owner;
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    int success = !result->status && cc && cc->initialized && cc->db == job->expected_db;
    if (result->status && inst) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member async READ ch=%s peer=%s status=%d rc=%d: %s",
        job->sql.channel, sc_public_key_hex(job->sql.only_ed, (char[65]){0}), result->status, result->sqlite_code, result->message);
    else if (!inst) DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "member async result detached ch=%s peer=%s status=%d",
        job->sql.channel, sc_public_key_hex(job->sql.only_ed, (char[65]){0}), result->status);
    if (success && !job->suppress) {
        for (size_t i = 0; i < job->sql.count; i++) if (chat_member_apply_live(inst, job->sql.channel, &job->sql.rows[i])) {
            success = 0; break;
        }
        if (success && job->sql.count > 1) qsort(job->sql.rows, job->sql.count, sizeof(*job->sql.rows), compare_members);
        size_t cl = strlen(job->sql.channel);
        unsigned events = job->events;
        if (success && (events & MEMBER_READ_LIST)) {
            size_t size = cl + 3 + job->sql.count * CHAT_MEMBER_DISPLAY_SIZE;
            uint8_t* event = u_malloc(size);
            if (!event) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member list event allocation ch=%s count=%zu bytes=%zu",
                job->sql.channel, job->sql.count, size);
            else {
                uint16_t count = (uint16_t)job->sql.count;
                event[0] = (uint8_t)cl; memcpy(event + 1, job->sql.channel, cl); memcpy(event + 1 + cl, &count, 2);
                for (size_t i = 0; i < job->sql.count; i++)
                    chat_core_serialize_member(&job->sql.rows[i], event + cl + 3 + i * CHAT_MEMBER_DISPLAY_SIZE);
                DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "member list ready ch=%s count=%zu", job->sql.channel, job->sql.count);
                chat_event_post(inst, CHAT_EVT_MEMBER_LIST, event, (int)size); u_free(event);
            }
        }
        if (success && (events & MEMBER_READ_UPDATES)) {
            uint8_t event[1 + 64 + 1 + CHAT_MEMBER_DISPLAY_SIZE];
            event[0] = (uint8_t)cl; memcpy(event + 1, job->sql.channel, cl); event[cl + 1] = 1;
            for (size_t i = 0; job->owner && !job->suppress && i < job->sql.count; i++) {
                /* Предыдущий callback мог поменять live-состояние следующего участника. */
                if (chat_member_apply_live(inst, job->sql.channel, &job->sql.rows[i])) break;
                chat_core_serialize_member(&job->sql.rows[i], event + cl + 2);
                chat_event_post(inst, CHAT_EVT_MEMBER_UPDATED, event, (int)cl + 2 + CHAT_MEMBER_DISPLAY_SIZE);
            }
        }
    }
    inst = job->owner;
    int again = success && inst && job->again;
    unsigned events = job->events;
    uint8_t ed[32]; memcpy(ed, job->sql.only_ed, sizeof(ed));
    char channel[65]; memcpy(channel, job->sql.channel, sizeof(channel));
    if (inst) {
        cc = CC(inst);
        struct member_read_job** pending = &cc->member_reads;
        while (*pending && *pending != job) pending = &(*pending)->next;
        if (*pending) *pending = job->next;
    }
    u_free(job->sql.rows); u_free(job);
    if (again) member_read_request(inst, channel, ed, events, 0);
}

/* Сериализованный worker публикует предыдущий снимок до начала следующего READ; live refresh не обесценивает SQL. */
static void member_read_request(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t ed[32], unsigned events, int refresh) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized || (!cc->db && !inst->db_worker) || !chat_channel_valid(channel)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "member READ request: stopped group service or invalid channel"); return;
    }
    for (struct member_read_job* job = cc->member_reads; job; job = job->next) {
        if (memcmp(job->sql.only_ed, ed ? ed : (uint8_t[32]){0}, 32) || strcmp(job->sql.channel, channel)) continue;
        if (refresh || (events & ~job->events)) job->again = 1;
        job->events |= events; return;
    }
    struct member_read_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member READ request allocation ch=%s", channel); return; }
    memcpy(job->sql.channel, channel, strlen(channel) + 1); memcpy(job->sql.self, inst->my_ed25519_pubkey, 32);
    if (ed) memcpy(job->sql.only_ed, ed, 32);
    job->owner = inst; job->expected_db = cc->db; job->events = events;
    job->next = cc->member_reads; cc->member_reads = job;
    if (inst->db_worker) db_async_submit(inst->db_worker, "member-display", DB_ASYNC_READ, member_read_work, job, member_read_done);
    else {
        const char* file = sqlite3_db_filename(cc->db, "main");
        struct db_async_result result = {.status=DB_ASYNC_CLOSED};
        if (!file || !*file) {
            result.sqlite_code = member_read_work(cc->db, job);
            result.status = result.sqlite_code == SQLITE_OK ? DB_ASYNC_OK : DB_ASYNC_SQL;
            snprintf(result.message, sizeof(result.message), "%s", sqlite3_errstr(result.sqlite_code));
        } else snprintf(result.message, sizeof(result.message), "file database has no asynchronous reader");
        member_read_done(job, &result);
    }
}

/* Немедленный committed mute уже опубликован отдельно; старый SQL не должен вернуть прежний флаг. */
static void member_read_invalidate(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t ed[32]) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    for (struct member_read_job* job = cc ? cc->member_reads : NULL; job; job = job->next)
        if (!strcmp(job->sql.channel, channel) && (mf_zero(job->sql.only_ed, 32) || !memcmp(job->sql.only_ed, ed, 32))) job->suppress = job->again = 1;
}

static int member_query(struct UTUN_INSTANCE* inst, const char* ch_id, const uint8_t ed[32],
                        uint8_t** out, int* count) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    *out = NULL; *count = 0;
    if (!cc || !cc->initialized || !chat_channel_valid(ch_id) || !cc->db || !cc->inst) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member snapshot: invalid instance/channel"); return -1;
    }
    struct chat_member_display* members = NULL;
    size_t size = 0;
    if (chat_member_rows_read(cc->db, ch_id, inst->my_ed25519_pubkey, ed, &members, &size) != SQLITE_OK) return -1;
    int cnt = (int)size;
    for (int i = 0; i < cnt; i++) if (chat_member_apply_live(inst, ch_id, &members[i])) {
        u_free(members); return -1;
    }
    if (cnt > 1) qsort(members, (size_t)cnt, sizeof(*members), compare_members);
    if (cnt) {
        *out = u_malloc((size_t)cnt * CHAT_MEMBER_DISPLAY_SIZE);
        if (!*out) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member snapshot: wire allocation ch=%s count=%d", ch_id, cnt);
            u_free(members); return -1;
        }
        for (int i = 0; i < cnt; i++) chat_core_serialize_member(&members[i], *out + i * CHAT_MEMBER_DISPLAY_SIZE);
    }
    u_free(members); *count = cnt;
    return 0;
}

int chat_core_get_member_list(struct UTUN_INSTANCE* inst, const char* ch_id, uint8_t** out, int* count) {
    if (out) *out = NULL;
    if (count) *count = 0;
    if (!out || !count) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member snapshot: null output"); return -1; }
    return member_query(inst, ch_id, NULL, out, count);
}

int chat_core_get_single_member(struct UTUN_INSTANCE* inst, const char* ch_id, const uint8_t ed[32], uint8_t* out) {
    if (!out || !ed || mf_zero(ed, 32)) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member snapshot: missing output or member"); return -1; }
    uint8_t* wire = NULL; int count = 0;
    if (member_query(inst, ch_id, ed, &wire, &count) < 0) return -1;
    if (count != 1) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "member snapshot: absent or ambiguous ch=%s peer=%s rows=%d", ch_id, sc_public_key_hex(ed, (char[65]){0}), count);
        u_free(wire); return -1;
    }
    memcpy(out, wire, CHAT_MEMBER_DISPLAY_SIZE); u_free(wire);
    return 0;
}

int chat_core_toggle_member_radio_mute(struct UTUN_INSTANCE* inst, const char* ch_id, const uint8_t ed[32]) {
    uint8_t* wire = NULL; int count = 0;
    if (!ed || mf_zero(ed, 32) || member_query(inst, ch_id, ed, &wire, &count) < 0 || count != 1) {
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio: mute toggle member unavailable member=%s rows=%d", sc_public_key_hex(ed, (char[65]){0}), count);
        u_free(wire); return -1;
    }
    uint8_t member[CHAT_MEMBER_DISPLAY_SIZE]; memcpy(member, wire, sizeof(member)); u_free(wire);
    int muted = !(member[CHAT_MEMBER_FLAGS_OFF] & CHAT_MEMBER_FLAG_RADIO_MUTED);
    if (radio_set_member_muted(inst, topo_groups_channel_slot(inst->topo_groups, ch_id), ed, muted) < 0) return -1;
    member_read_invalidate(inst, ch_id, ed);
    /* Снимок уже готов: после committed mute нет нового чтения, способного превратить успех в ошибку. */
    member[CHAT_MEMBER_FLAGS_OFF] = (uint8_t)((member[CHAT_MEMBER_FLAGS_OFF] & ~CHAT_MEMBER_FLAG_RADIO_MUTED) | (muted ? CHAT_MEMBER_FLAG_RADIO_MUTED : 0));
    if (!inst->topo_groups || !topo_groups_find_channel(inst->topo_groups, ch_id)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "radio: mute committed, group removed by audio callback ch=%s", ch_id); return 0;
    }
    size_t cl = strlen(ch_id);
    uint8_t evt[1 + 64 + 1 + CHAT_MEMBER_DISPLAY_SIZE];
    evt[0] = (uint8_t)cl; memcpy(evt + 1, ch_id, cl); evt[1 + cl] = 1;
    memcpy(evt + 2 + cl, member, sizeof(member));
    chat_event_post(inst, CHAT_EVT_MEMBER_UPDATED, evt, (int)(cl + 2 + sizeof(member)));
    return 0;
}

/* Результат WRITE не зависит от следующего UI READ; старый ожидающий снимок перечитывается. */
static void member_mute_saved(void* arg, int rc, int muted) {
    struct chat_member_radio_mute_req* req = arg;
    (void)muted;
    if (!rc) {
        member_read_invalidate(req->inst, req->ch_id, req->ed);
        member_read_request(req->inst, req->ch_id, req->ed, MEMBER_READ_UPDATES, 0);
    }
    uint8_t result[CHAT_RADIO_MUTE_RESULT_SIZE];
    sc_hex_to_binary(req->ch_id, result, 32); memcpy(result + 32, req->ed, 32); result[64] = rc == 0;
    chat_event_post(req->inst, CHAT_EVT_RADIO_MUTE_RESULT, result, sizeof(result));
    u_free(req);
}

void chat_core_toggle_member_radio_mute_trampoline(void* arg) {
    struct chat_member_radio_mute_req* req = arg;
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio: null member mute request"); return; }
    if (!memchr(req->ch_id, 0, sizeof(req->ch_id))) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio: unterminated mute channel"); req->ch_id[0] = 0; member_mute_saved(req, -1, 0); return;
    }
    radio_member_mute_save(req->inst, topo_groups_channel_slot(req->inst->topo_groups, req->ch_id), req->ed, 2, member_mute_saved, req);
}

/* ─── трамплины для gui_bridge ─── */

void chat_core_request_member_list_trampoline(void* arg) {
    struct chat_member_list_req* req = (struct chat_member_list_req*)arg;
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member list: null request"); return; }
    member_read_request(req->inst, req->ch_id, NULL, MEMBER_READ_LIST, 1); u_free(req);
}

void chat_core_request_member_rtt_trampoline(void* arg) {
    struct chat_member_list_req* req = (struct chat_member_list_req*)arg;
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member RTT: null request"); return; }
    member_read_request(req->inst, req->ch_id, NULL, MEMBER_READ_UPDATES, 0); u_free(req);
}

/* ─── node_props_changed callback (BGP-события узла → MEMBER_UPDATED в GUI) ─── */

static void on_member_props_changed(const uint8_t ed[32], const char* channel_id, void* arg) {
    struct UTUN_INSTANCE* inst = (struct UTUN_INSTANCE*)arg;
    if (mf_zero(ed, 32)) { DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "member update: missing Ed25519 ch=%s", channel_id); return; }
    member_read_request(inst, channel_id, ed, MEMBER_READ_UPDATES, 1);
}

struct member_route_event {
    struct member_route_event* next;
    struct UTUN_INSTANCE* inst;
    struct posted_task* task;
    uint8_t ed[32];
    char channel[65];
};

/* Удаляем из списка до UI callback: он вправе удалить канал или снять подписку. */
static void member_route_ready(void* arg) {
    struct member_route_event* event = arg;
    struct chat_core_ctx* cc = CC(event->inst);
    struct member_route_event** pending = &cc->member_route_events;
    while (*pending && *pending != event) pending = &(*pending)->next;
    if (*pending) *pending = event->next;
    member_read_request(event->inst, event->channel, event->ed, MEMBER_READ_UPDATES, 0);
    u_free(event);
}

/* Нельзя публиковать UI из topo dispatch: caller/следующие observers ещё используют group/node. */
void chat_member_state_changed(struct TOPO_GROUP* group, const uint8_t ed[32]) {
    struct UTUN_INSTANCE* inst = group ? group->instance : NULL; struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized || !group || group->stopping || !ed || !memcmp(ed, (uint8_t[32]){0}, 32) || group->group_type != TOPO_GROUP_TYPE_CHAT) return;
    if (!chat_channel_valid(group->channel_id)) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "member route: invalid channel"); return; }
    for (struct member_route_event* event = cc->member_route_events; event; event = event->next)
        if (!memcmp(event->ed, ed, 32) && !strcmp(event->channel, group->channel_id)) return;
    struct member_route_event* event = u_calloc(1, sizeof(*event));
    struct posted_task* task = event ? u_calloc(1, sizeof(*task)) : NULL;
    if (!task) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "member route: event allocation failed"); u_free(event); return; }
    event->inst = inst; memcpy(event->ed, ed, 32); event->task = task;
    memcpy(event->channel, group->channel_id, strlen(group->channel_id) + 1);
    event->next = cc->member_route_events; cc->member_route_events = event;
    task->callback = member_route_ready; task->arg = event;
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "member route: deferred ch=%s peer=%s", event->channel, sc_public_key_hex(ed, (char[65]){0}));
    uasync_post_reserved(inst->ua, task);
}

static void on_member_route_changed(struct TOPO_GROUP* group, struct TOPO_GROUP_NODE* node, void* arg) {
    (void)arg;
    if (node) chat_member_state_changed(group, node->ed25519);
}

int chat_member_init(struct UTUN_INSTANCE* inst) {
    if (member_sync_add_props_cbk(inst, on_member_props_changed, inst) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat_member: properties subscription failed"); return -1;
    }
    if (utun_add_nodeinfo_cbk(inst, on_member_route_changed, inst) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat_member: routes subscription failed");
        member_sync_remove_props_cbk(inst, on_member_props_changed, inst); return -1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "chat_member: initialized (group properties/routes -> complete MEMBER_UPDATED)");
    return 0;
}

void chat_member_forget_channel(struct UTUN_INSTANCE* inst, const char* channel) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc) return;
    unsigned reads = 0, routes = 0;
    struct member_read_job** read = &cc->member_reads;
    while (*read) {
        struct member_read_job* job = *read;
        if (channel && strcmp(job->sql.channel, channel)) { read = &job->next; continue; }
        *read = job->next; job->next = NULL; job->owner = NULL; reads++;
    }
    struct member_route_event** route = &cc->member_route_events;
    while (*route) {
        struct member_route_event* event = *route;
        if (channel && strcmp(event->channel, channel)) { route = &event->next; continue; }
        *route = event->next;
        if (uasync_cancel_post(inst->ua, event->task) != ERR_OK)
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "member route: pending event missing on stop ch=%s", event->channel);
        u_free(event); routes++;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "member pending detached ch=%s reads=%u routes=%u", channel ? channel : "all", reads, routes);
}

void chat_member_destroy(struct UTUN_INSTANCE* inst) {
    utun_remove_nodeinfo_cbk(inst, on_member_route_changed, inst);
    member_sync_remove_props_cbk(inst, on_member_props_changed, inst);
    chat_member_forget_channel(inst, NULL);
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "chat_member: properties/routes subscriptions removed");
}
