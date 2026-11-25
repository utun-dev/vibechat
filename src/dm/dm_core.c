/*
 * dm_core.c — прямой p2p чат между двумя пользователями (DM)
 *
 * Отдельная подсистема (не канал): беседы хранятся в dm_conversations,
 * сообщения в dm_messages, синхронизация идёт напрямую через etcp_router
 * (сервис 0x34), при недоступности пира — через dm_mailbox (0x35).
 *
 * Два независимых направленных потока (я→пир, пир→я) с монотонным seq.
 * Каноническое тело сообщения — см. dm_core.h (общее с dm_mailbox).
 */

#include "dm_core.h"
#include "dm_store.h"
#include "dm_crypto.h"
#include "dm_history.h"
#include "dm_schema.h"
#include "dm_mailbox.h"
#include "../chat/chat_core.h"
#include "../chat/chat_event.h"
#include "../chat/chat_member.h"
#include "../chat/chat_member_read.h"
#include "../chat/group_policy.h"
#include "../member_files/protocol.h"
#include "../chat/invite_build.h"
#include "../chat/chat_join.h"
#include "../utun_instance.h"
#include "../ntp_time.h"
#include "../routing_layer/etcp_router.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../routing_layer/topo_group.h"
#include "../transport_layer/etcp_api.h"
#include "../transport_layer/etcp.h"
#include "../transport_layer/secure_channel.h"
#include "../../lib/mem.h"
#include "../../lib/ll_queue.h"
#include "../../lib/debug_config.h"
#include "../../lib/platform_compat.h"
#include "../../lib/db_async.h"

#include <string.h>
#include <stdlib.h>
#include <sqlite3.h>
#include <openssl/sha.h>
#include <openssl/crypto.h>

#define DM_ID "dm_core"

/* Подкоманды протокола DM (первый байт payload после ROUTER_SVC-заголовка). */
#define DM_SUBCMD_MSG    0x01   /* [msg] — сообщение */
#define DM_SUBCMD_ACK    0x02   /* Подписанная квитанция получателя. */

/* Per-instance состояние модуля. */
struct dm_state {
    struct UTUN_INSTANCE* inst;
    sqlite3*              db;
    int                   initialized;
    void*                 timer;
    struct dm_invite_req*  invite;
    struct dm_invite_job* invite_job; /* RW pipeline; request остаётся жив до текущего SQL/build/send callback. */
    struct dm_json_job* reads; /* Незавершённые READ; destroy отсоединяет их до освобождения DM. */
    struct dm_members_job* members; /* Бинарные snapshots устройств; тот же weak lifetime, что у JSON. */
    struct dm_receipt_job* receipts; /* Незавершённые WRITE квитанций; та же слабая связь с владельцем. */
    struct dm_start_job* starts; /* Создание бесед: stop отсоединяет ожидающие/активные WRITE. */
    struct dm_send_job* sends; /* Исходящие сообщения: запросы живут до completion, owner отсоединяется при stop. */
    struct dm_receive_job* receives; /* Входящие WRITE: слабый owner обнуляется при stop. */
    struct dm_delete_job* deletes; /* Удаление истории: SQL может закончиться после stop, публикация отменяется. */
    struct dm_delete_event* delete_events; /* Ответы UI также отсоединяются при stop, без raw-pointer ABA. */
    struct dm_ui_event* ui_events; /* START/SEND replies не переходят в новый запуск DM. */
    struct dm_route_job* routes; /* READ pinned сторон для звонка; stop отсоединяет, delete инвалидирует по conv. */
    struct dm_live_job* live; /* Ответы live-отправителям; живут до callback операции приёма. */
    struct dm_outbox_job* delivery; /* Одна пачка повторной доставки, включая ожидание в DB-очереди. */
};

struct dm_outbox_job {
    struct dm_outbox_batch batch; /* Только frozen SQL-входы и собственные тела сообщений. */
    struct dm_state* owner;      /* Слабая ссылка в uasync; destroy обнуляет до освобождения DM. */
    struct UTUN_INSTANCE* inst;  /* Ядро живёт до DB drain. */
};

struct dm_start_job {
    struct dm_start_write sql;   /* Worker получает только собственные входы/результаты SQL. */
    struct dm_state* owner;     /* Слабая ссылка, только uasync. */
    struct dm_start_job* next;
    struct UTUN_INSTANCE* inst; /* Ядро живёт до DB drain. */
    dm_start_done_fn done;
    int (*valid)(void*);
    void* arg;
};

struct dm_send_job {
    struct dm_send_write sql;  /* Worker видит только входные копии и SQL-результаты. */
    struct dm_state* owner;   /* Слабый owner; используется/отсоединяется только uasync. */
    struct dm_send_job* next;
    struct UTUN_INSTANCE* inst; /* Ядро живёт до DB drain. */
    dm_send_done_fn done;
    int (*valid)(void*);
    void* arg;
};

struct dm_receive_job {
    struct dm_receive_write sql; /* Worker получает только собственные входы/результаты. */
    struct dm_state* owner;      /* Слабый owner, только uasync. */
    struct dm_receive_job* next;
    struct UTUN_INSTANCE* inst; /* Ядро живёт до DB drain. */
    dm_receive_done_fn done;
    int (*valid)(void*);
    void* arg;
};
struct dm_delete_job {
    char conv[64], root[DM_MEDIA_ROOT_SIZE]; /* Frozen данные worker. */
    uint64_t generation;
    struct dm_state* owner;      /* Слабый владелец, только uasync. */
    struct dm_delete_job* next;
    struct UTUN_INSTANCE* inst; /* Живёт до DB drain. */
    void* media;                 /* Только сравнение с текущим владельцем, не разыменовывать. */
    dm_delete_done_fn done;
    int (*valid)(void*);
    void* arg;
};
struct dm_live_job {
    struct dm_state* owner;     /* Destroy отсоединяет до освобождения DM. */
    struct dm_live_job* next;
    struct UTUN_INSTANCE* inst; /* Живёт до callback dm_receive_submit. */
    uint64_t group;             /* Маршрут входного пакета; допуск перепроверяется перед ответом. */
    uint8_t source_ed[32];
};

struct dm_json_sql {
    char* json;                   /* Собственный буфер job, ни одного runtime-указателя в work. */
    size_t capacity, length;
    char conv[64];                /* Пустая строка — список бесед, иначе история. */
    struct dm_store_identity identity; /* Секрет копируется до enqueue и очищается cleanup. */
    uint64_t generation;
    int count, offset, include_hidden;
};
struct dm_json_job {
    struct dm_json_sql sql;
    struct dm_state* owner;       /* Слабая ссылка; destroy обязательно обнуляет. Только uasync. */
    struct dm_json_job* next;
    struct UTUN_INSTANCE* inst;   /* Ядро живёт до DB drain. */
    int invalidated;              /* Commit удаления запрещает публикацию старого списка/этой истории. */
    int (*valid)(void*);
    dm_json_done_fn done;
    void* arg;
    char buffer[];                /* JSON и job выделяются одним блоком. */
};

struct dm_members_job {
    struct dm_store_identity identity; /* Frozen ключи только для SQL-снимка закреплённой беседы. */
    char conv[64];
    uint64_t generation;
    char preferred[65];
    struct dm_target targets[DM_TARGET_MAX];
    struct { char group[65]; struct chat_member_display display; } rows[DM_TARGET_MAX];
    size_t count;
    int phase, invalidated; /* 0=состав устройств,1=статические display-поля; uasync меняет только между READ. */
    struct dm_state* owner;
    struct dm_members_job* next;
    struct UTUN_INSTANCE* inst;
    dm_members_done_fn done;
    void* arg;
};

struct dm_delete_event {
    struct dm_delete_req* req;
    struct dm_state* owner;       /* Linked weak owner обнуляется до освобождения DM. */
    struct dm_delete_event* next;
    uint64_t group_generation;    /* Ответ старого контекста групп не публикуется в новый runtime. */
};
struct dm_ui_event {
    struct dm_ui_event* next;
    struct dm_state* owner;
    struct UTUN_INSTANCE* inst;
    uint64_t group_generation;
    void* request;
};

struct dm_receipt_sql {
    uint8_t self_ed[32];         /* Автор durable outbox, зафиксирован до enqueue. */
    uint8_t receipt[DM_RECEIPT_SIZE]; /* Собственная неизменяемая копия входящего ACK. */
    struct dm_receipt_update update; /* SQL-результат, публикуется только после commit. */
};
struct dm_receipt_job {
    struct dm_receipt_sql sql;   /* Worker видит только эту часть, без runtime. */
    struct dm_state* owner;     /* Слабая ссылка; destroy обнуляет до освобождения DM. */
    struct dm_receipt_job* next;
    struct UTUN_INSTANCE* inst; /* Ядро живёт до DB drain. */
    dm_receipt_done_fn done;
    void* arg;
};
struct dm_route_job {
    struct dm_store_identity identity;
    char conv[64];
    uint64_t generation;
    char preferred[65];
    struct dm_target targets[DM_TARGET_MAX];
    size_t count;
    struct dm_call_route_result result;
    struct dm_state* owner; /* Только uasync weak owner, NULL после quiesce. */
    struct dm_route_job* next;
    struct UTUN_INSTANCE* inst;
    int invalidated;
    dm_call_route_done_fn done;
    void* arg;
};
static void dm_invite_async_cancel(struct dm_state* dm, int detach);

/* ── вспомогательные ── */

static struct dm_state* dm_of(struct UTUN_INSTANCE* inst) {
    return inst ? inst->dm : NULL;
}

/* Выполнить SQL без результата. Возвращает 0/ошибку. */
static int dm_exec(struct dm_state* dm, const char* sql) {
    char* err = NULL;
    if (sqlite3_exec(dm->db, sql, NULL, NULL, &err) != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "%s: sql failed: %s (%s)", DM_ID, sql, err ? err : "?");
        if (err) sqlite3_free(err);
        return -1;
    }
    return 0;
}

/* Общий CHAT-канал с пиром. Возвращает 1 и кладёт group_id (== channel_id) в *out_group_id. */
static int dm_shared_group_id(struct dm_state* dm, uint64_t peer_id, const uint8_t peer_ed[32], uint64_t* out_group_id) {
    if (!dm || !dm->inst || (!peer_ed && !peer_id) || !out_group_id) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM shared group: invalid owner/peer/output"); return 0;
    }
    struct TOPO_GROUPS* groups = dm->inst->topo_groups;
    for (struct ll_entry* e = groups && groups->group_list ? groups->group_list->head : NULL; e; e = e->next) {
        struct TOPO_GROUP* group = (struct TOPO_GROUP*)e;
        if (peer_ed ? !topo_group_chat_key_allowed(dm->inst, group->group_id, peer_ed) :
            !topo_group_chat_allowed(dm->inst, group->group_id, peer_id)) continue;
        *out_group_id = group->group_id;
        DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM shared group selected peer=%016llx group=%llu",
            (unsigned long long)peer_id, (unsigned long long)group->group_id); return 1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM shared group unavailable peer=%016llx", (unsigned long long)peer_id); return 0;
}

/* Выбрать общую область с действительно живым маршрутом, включая BGP-транзит. */
static int dm_find_route_group(struct UTUN_INSTANCE* inst, uint64_t peer, const uint8_t* ed, uint64_t preferred,
                               uint64_t* group_id, int bgp_only, uint8_t service) {
    if (!inst || (!ed && !peer) || !group_id) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "%s: invalid route lookup", DM_ID);
        return 0;
    }
    struct TOPO_GROUPS* groups = inst->topo_groups;
    struct TOPO_GROUP* first = preferred && groups ? topo_groups_find(groups, preferred) : NULL;
    struct ll_entry* entry = groups && groups->group_list ? groups->group_list->head : NULL;
    while (first || entry) {
        struct TOPO_GROUP* group = first;
        if (first) first = NULL;
        else { group = (struct TOPO_GROUP*)entry; entry = entry->next; }
        uint64_t gid = group->group_id;
        if (group->group_type != TOPO_GROUP_TYPE_CHAT || group->stopping) continue;
        if (ed) {
            if (!topo_group_chat_key_allowed(inst, gid, ed)) continue;
            if (bgp_only ? !topo_node_best_path(topo_node_find_by_ed(group, ed)) :
                !etcp_router_peer_reachable(inst, gid, ed, service)) continue;
        } else if (!topo_group_chat_allowed(inst, gid, peer)) continue;
        else if (bgp_only) {
            if (!topo_group_find_conn_for_node(group, peer)) continue;
        } else {
            struct ETCP_ROUTER_PATH path = etcp_router_get_path(inst, gid, peer, service);
            if (!path.next_hop_node_id) continue;
        }
        *group_id = gid;
        DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM route selected peer=%016llx group=%llu preferred=%llu bgp_only=%d",
            (unsigned long long)peer, (unsigned long long)gid, (unsigned long long)preferred, bgp_only); return 1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM route unavailable peer=%016llx preferred=%llu bgp_only=%d",
        (unsigned long long)peer, (unsigned long long)preferred, bgp_only); return 0;
}

int dm_route_group(struct UTUN_INSTANCE* inst, uint64_t peer, uint64_t preferred, uint64_t* group_id) {
    return dm_find_route_group(inst, peer, NULL, preferred, group_id, 0, ETCP_RT_ID_DM);
}

int dm_route_group_key(struct UTUN_INSTANCE* inst, const uint8_t ed[32], uint64_t preferred, uint64_t* group_id, uint8_t service) {
    return dm_find_route_group(inst, 0, ed, preferred, group_id, 0, service);
}

/* Роль из любой общей группы не привязывает DM к маршруту её объявления. */
static int dm_find_peer_role(struct UTUN_INSTANCE* inst, uint64_t peer, const uint8_t* ed, const char* role) {
    enum group_member_role required = role && !strcmp(role, "supernode") ? GROUP_MEMBER_SUPERNODE :
        role && !strcmp(role, "storage") ? GROUP_MEMBER_STORAGE : 0;
    if (!inst || (!peer && !ed) || !required) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM role: invalid instance/peer/role peer=%llu role=%s",
            (unsigned long long)peer, role ? role : "NULL"); return 0;
    }
    struct TOPO_GROUPS* groups = inst->topo_groups;
    for (struct ll_entry* e = groups && groups->group_list ? groups->group_list->head : NULL; e; e = e->next) {
        struct TOPO_GROUP* group = (struct TOPO_GROUP*)e;
        if (!(ed ? topo_group_chat_key_allowed(inst, group->group_id, ed) : topo_group_chat_allowed(inst, group->group_id, peer))) continue;
        int allowed = ed ? group_policy_member_role_key(inst, group->channel_id, ed, required)
                         : group_policy_member_role(inst, group->channel_id, peer, required);
        if (allowed < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM role snapshot failed peer=%016llx key=%s group=%llu role=%s",
                (unsigned long long)peer, ed ? sc_public_key_hex(ed, (char[65]){0}) : "", (unsigned long long)group->group_id, role); return 0;
        }
        if (!allowed) continue;
        DEBUG_TRACE(DEBUG_CATEGORY_DM, "DM role admitted peer=%016llx key=%s group=%llu role=%s",
            (unsigned long long)peer, ed ? sc_public_key_hex(ed, (char[65]){0}) : "", (unsigned long long)group->group_id, role); return 1;
    }
    return 0;
}

int dm_peer_role(struct UTUN_INSTANCE* inst, uint64_t peer, const char* role) {
    return dm_find_peer_role(inst, peer, NULL, role);
}

int dm_peer_role_key(struct UTUN_INSTANCE* inst, const uint8_t ed[32], const char* role) {
    return dm_find_peer_role(inst, 0, ed, role);
}

/* Router владеет entry после вызова, в том числе при ошибке. */
static int dm_route_send(struct dm_state* dm, uint64_t group_id, const uint8_t ed[32], uint8_t subcmd,
                         const uint8_t* body, size_t body_len) {
    if (!topo_group_chat_key_allowed(dm->inst, group_id, ed)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM send denied group=%llu peer=%s cmd=%u",
            (unsigned long long)group_id, sc_public_key_hex(ed, (char[65]){0}), subcmd); return -1;
    }
    struct ETCP_ROUTER_CONN* route = etcp_router_conn_get_key(dm->inst, group_id, ed, ETCP_RT_ID_DM);
    if (!route) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "%s: router channel allocation failed peer=%s", DM_ID, sc_public_key_hex(ed, (char[65]){0}));
        return -1;
    }
    if (!etcp_router_conn_send_q_has_room(route)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_DM, "%s: send deferred by backpressure peer=%s", DM_ID, sc_public_key_hex(ed, (char[65]){0}));
        return -1;
    }
    struct ll_entry* entry = queue_entry_new(0);
    if (!entry) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "%s: packet allocation failed", DM_ID); return -1; }
    entry->dgram = u_malloc(2 + body_len);
    if (!entry->dgram) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "%s: payload allocation failed", DM_ID);
        queue_entry_free(entry);
        return -1;
    }
    entry->dgram[0] = ETCP_RT_ID_DM;
    entry->dgram[1] = subcmd;
    if (body_len) memcpy(entry->dgram + 2, body, body_len);
    entry->len = (uint16_t)(2 + body_len);
    int rc = etcp_route_send_key(dm->inst, group_id, ed, entry, 1, ROUTER_FLAG_SIGNED);
    if (rc) DEBUG_WARN(DEBUG_CATEGORY_DM, "%s: route send failed peer=%s cmd=%u rc=%d",
                       DM_ID, sc_public_key_hex(ed, (char[65]){0}), subcmd, rc);
    return rc;
}

/* ── работа с беседой ── */

enum dm_conv_result { DM_CONV_ERROR = -1, DM_CONV_FOUND = 0, DM_CONV_NOT_FOUND = 1 };

/* Архив читаем, но новые операции требуют текущей принадлежности закреплённой стороне. */
static int dm_conv_active(struct dm_state* dm, const struct dm_conv* c) {
    if (!c->closed && (c->mode == DM_MODE_DEVICE || dm_party_member(dm->inst, c->parties, dm->inst->node_id) == 1)) return 1;
    DEBUG_WARN(DEBUG_CATEGORY_DM, "DM conversation is archived/inactive conv=%s mode=%d", c->conv_id, c->mode);
    return 0;
}

/* FOUND выдаёт полную беседу; NOT_FOUND — только SQLITE_DONE; ERROR не меняет *c и БД. */
static enum dm_conv_result dm_conv_load(struct dm_state* dm, const char* conv_id, struct dm_conv* c) {
    if (!dm || !dm->inst || !dm->db) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM conversation: load without active database/identity"); return DM_CONV_ERROR;
    }
    struct dm_store_identity identity = {.node=dm->inst->node_id};
    memcpy(identity.x, dm->inst->my_keys.public_key, 32); memcpy(identity.ed, dm->inst->my_ed25519_pubkey, 32);
    memcpy(identity.private_key, dm->inst->my_keys.private_key, 32); int found = 0;
    int rc = dm_store_conv_read(dm->db, &identity, conv_id, c, &found); OPENSSL_cleanse(&identity, sizeof(identity));
    if (rc != SQLITE_OK) return DM_CONV_ERROR;
    if (!found) return DM_CONV_NOT_FOUND;
    /* Если область ещё не выбрана, используем известную общую группу. */
    if (!c->group_id[0]) {
        uint64_t gid = 0;
        if (dm_shared_group_id(dm, 0, c->peer_ed25519, &gid)) strcpy(c->group_id, topo_groups_channel(dm->inst->topo_groups, gid));
    }
    DEBUG_TRACE(DEBUG_CATEGORY_DM, "%s: conversation loaded conv=%s out=%llu in=%llu", DM_ID, conv_id,
                (unsigned long long)c->last_out_seq, (unsigned long long)c->last_in_seq);
    return DM_CONV_FOUND;
}

int dm_conversation_access(struct UTUN_INSTANCE* inst, const char* conv, struct dm_party parties[2], uint8_t key[32]) {
    struct dm_state* dm = dm_of(inst);
    struct dm_conv c;
    if (!dm || !conv) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM access: invalid context"); return -1; }
    if (dm_conv_load(dm, conv, &c) != DM_CONV_FOUND) return -1;
    int active = dm_conv_active(dm, &c);
    if (active && parties) memcpy(parties, c.parties, sizeof(c.parties));
    if (active && key) memcpy(key, c.content_key, 32);
    OPENSSL_cleanse(c.content_key, sizeof(c.content_key));
    return active ? 0 : -1;
}

int dm_group_leave_write(sqlite3* db, const uint8_t group_ed[32]) {
    if (!db || !group_ed || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM leave: database/transaction/group required"); return SQLITE_MISUSE;
    }
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "UPDATE dm_conversations SET closed=1 WHERE mode=1"
        " AND substr(local_party,1,1)=x'01' AND substr(local_party,2,32)=?", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, group_ed, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int changed = sqlite3_changes(db), final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK) {
        if (rc == SQLITE_DONE) rc = final;
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM leave: close failed rc=%d final=%d: %s", rc, final, sqlite3_errmsg(db)); return rc;
    }
    const char* queues[] = {
        "DELETE FROM dm_outbox WHERE conv_id IN (SELECT conv_id FROM dm_conversations WHERE closed=1)",
        "DELETE FROM dm_delivery WHERE conv_id IN (SELECT conv_id FROM dm_conversations WHERE closed=1)"
    };
    for (unsigned i = 0; i < 2; i++) {
        rc = sqlite3_exec(db, queues[i], NULL, NULL, NULL);
        if (rc) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM leave: queue=%u cleanup rc=%d: %s", i, rc, sqlite3_errmsg(db)); return rc; }
    }
    DEBUG_INFO(DEBUG_CATEGORY_DM, "DM leave: archived conversations=%d, queued sends stopped", changed);
    return SQLITE_OK;
}
int dm_group_leave_prepare(struct UTUN_INSTANCE* inst, const uint8_t group_ed[32]) {
    struct dm_state* dm = dm_of(inst);
    return dm ? (dm_group_leave_write(dm->db, group_ed) ? -1 : 0) : 0;
}

/* ── сборка/разбор канонического тела ── */

/* Проверить тело перед любой записью: подпись связывает conv, автора и адресата. */
int dm_verify_message(struct UTUN_INSTANCE* inst, uint64_t recipient, const uint8_t* msg, size_t len) {
    return dm_store_verify_message(inst ? inst->topo_sqlite_db : NULL, recipient, msg, len) ? -1 : 0;
}

/* Слабый owner и guard запрещают старые события/ACK после stop либо отзыва сетевого допуска. */
static int dm_receive_current(void* arg) {
    struct dm_receive_job* job = arg;
    return job->owner && !job->inst->destroying && job->inst->dm == job->owner && job->owner->initialized &&
        (!job->valid || job->valid(job->arg));
}
static void dm_receive_cleanup(void* arg) {
    struct dm_receive_job* job = arg;
    if (job->owner) {
        struct dm_receive_job** slot = &job->owner->receives;
        while (*slot && *slot != job) slot = &(*slot)->next;
        if (*slot) *slot = job->next;
        else DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM receive cleanup lost owner=%p", (void*)job->owner);
    }
    OPENSSL_cleanse(&job->sql, sizeof(job->sql)); u_free(job);
}
/* Worker-результат становится видимым только после commit; observer может остановить сервис. */
static void dm_receive_done(void* arg, const struct db_async_result* outcome) {
    struct dm_receive_job* job = arg; struct dm_receive_update* update = &job->sql.update;
    struct dm_receive_result result = {.status=-1, .sqlite_code=outcome->sqlite_code}; int current = dm_receive_current(job);
    if (!outcome->status && current) {
        if (update->inserted) {
            char conv[64]; snprintf(conv, sizeof(conv), "%llu", (unsigned long long)update->conversation);
            size_t length = strlen(conv); uint8_t event[1 + 64 + 32]; event[0] = (uint8_t)length;
            memcpy(event + 1, conv, length);
            if (update->created) {
                memcpy(event + 1 + length, &update->generation, 8);
                chat_event_post(job->inst, CHAT_EVT_DM_CONV_UPDATED, event, (int)(length + 9));
            }
            if (dm_receive_current(job)) {
                memcpy(event + 1 + length, job->sql.body + DM_MSG_AUTHOR_OFF, 32);
                chat_event_post(job->inst, CHAT_EVT_DM_MSG_RECEIVED, event, (int)(length + 33));
            }
        }
        current = dm_receive_current(job);
        if (current) {
            result.status = 0; result.conversation = update->conversation; result.author = update->author;
            result.seq = update->seq; result.generation = update->generation; result.inserted = update->inserted;
            result.deleted = update->deleted; result.receipt = update->receipt; result.media_receipt = update->media_receipt;
            if (result.inserted) DEBUG_INFO(DEBUG_CATEGORY_DM, "DM received conv=%llu seq=%llu author=%016llx generation=%llu",
                (unsigned long long)result.conversation, (unsigned long long)result.seq, (unsigned long long)result.author,
                (unsigned long long)result.generation);
            DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM receive committed conv=%llu seq=%llu inserted=%d deleted=%d",
                (unsigned long long)result.conversation, (unsigned long long)result.seq, result.inserted, result.deleted);
        }
    }
    if (result.status) {
        if (!result.sqlite_code) result.sqlite_code = SQLITE_ABORT;
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM receive complete status=%d sqlite=%d current=%d: %s", outcome->status,
            result.sqlite_code, current, outcome->message[0] ? outcome->message : "owner/request stopped");
    }
    job->done(job->arg, &result);
}
static int dm_receive_work(sqlite3* db, void* arg) { return dm_store_receive(db, arg); }
/* Переходный writer исполняет ту же операцию и не затрагивает чужую транзакцию. */
static void dm_receive_legacy(struct dm_receive_job* job) {
    sqlite3* db = job->owner->db; const char* file = db ? sqlite3_db_filename(db, "main") : NULL;
    int owns = db && sqlite3_get_autocommit(db) && (job->inst->db_worker || (file && !*file));
    const char* stage = "begin"; int rc = owns ? sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) : SQLITE_MISUSE;
    if (!rc) { stage = "write"; rc = dm_receive_work(db, &job->sql); }
    if (!rc && !dm_receive_current(job)) { stage = "cancel"; rc = SQLITE_ABORT; }
    if (!rc) { stage = "commit"; rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL); }
    struct db_async_result result = {.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK, .sqlite_code=rc};
    if (rc) {
        snprintf(result.message, sizeof(result.message), "legacy receive %s: %s", stage,
            db && (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM receive legacy rc=%d: %s", rc, result.message);
        if (owns && !sqlite3_get_autocommit(db)) {
            int rollback = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            if (rollback) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM receive rollback rc=%d: %s", rollback, sqlite3_errmsg(db));
            if (!sqlite3_get_autocommit(db)) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM receive left transaction open"); utun_instance_stop(job->inst); }
        }
    }
    dm_receive_done(job, &result); dm_receive_cleanup(job);
}
void dm_receive_submit(struct UTUN_INSTANCE* inst, const struct dm_receive_query* query, dm_receive_done_fn done, void* arg) {
    struct dm_state* dm = dm_of(inst); struct dm_receive_result failed = {.status=-1, .sqlite_code=SQLITE_MISUSE};
    if (!done) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM receive: missing completion"); return; }
    if (!dm || !dm->initialized || inst->destroying || !query || !query->body || !query->size || query->size > DM_BODY_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM receive: invalid owner/body/size"); done(arg, &failed); return;
    }
    struct dm_receive_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM receive allocation failed"); failed.sqlite_code = SQLITE_NOMEM; done(arg, &failed); return; }
    job->owner = dm; job->inst = inst; job->done = done; job->valid = query->valid; job->arg = arg;
    job->sql.identity.node = inst->node_id; memcpy(job->sql.identity.x, inst->my_keys.public_key, 32);
    memcpy(job->sql.identity.ed, inst->my_ed25519_pubkey, 32); memcpy(job->sql.identity.private_key, inst->my_keys.private_key, 32);
    memcpy(job->sql.signing_key, inst->my_ed25519_privkey, 32); memcpy(job->sql.body, query->body, query->size);
    job->sql.size = query->size; job->sql.now = (uint64_t)ntp_time_get_seconds(inst);
    if (query->file_offer) job->sql.offer = *query->file_offer;
    const uint8_t* media = dm_message_media(job->sql.body, job->sql.size);
    if (media && inst->dm_media && dm_media_path_snapshot(inst, media, job->sql.file_path)) {
        job->sql.file_path[0] = 0;
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM receive: no valid media path; only an exact deleted replay can be accepted");
    }
    job->next = dm->receives; dm->receives = job;
    if (db_async_can_write(inst->db_worker)) {
        db_async_run(inst->db_worker, &(struct db_async_task){.name="dm-receive", .mode=DB_ASYNC_WRITE, .work=dm_receive_work,
            .data=&job->sql, .guard=dm_receive_current, .done=dm_receive_done, .cleanup=dm_receive_cleanup}); return;
    }
    if (!dm_receive_current(job)) {
        struct db_async_result cancelled = {.status=DB_ASYNC_CANCELLED, .sqlite_code=SQLITE_ABORT};
        dm_receive_done(job, &cancelled); dm_receive_cleanup(job); return;
    }
    dm_receive_legacy(job);
}
struct dm_receive_sync { int status; uint8_t* receipt; };
static void dm_receive_sync_done(void* arg, const struct dm_receive_result* result) {
    struct dm_receive_sync* sync = arg; sync->status = result->status;
    if (!result->status) memcpy(sync->receipt, result->receipt, DM_RECEIPT_SIZE);
}
int dm_accept_message(struct UTUN_INSTANCE* inst, const uint8_t* msg, size_t len, uint8_t receipt[DM_RECEIPT_SIZE]) {
    if (!receipt || (inst && db_async_can_write(inst->db_worker))) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM synchronous receive requires output and RAM/legacy backend"); return -1;
    }
    struct dm_receive_sync result = {.status=-1, .receipt=receipt};
    dm_receive_submit(inst, &(struct dm_receive_query){.body=msg, .size=len}, dm_receive_sync_done, &result); return result.status;
}

/* Квитанцию может пересылать суперузел, но подписывает её только получатель. */
int dm_verify_receipt(struct UTUN_INSTANCE* inst, const uint8_t receipt[DM_RECEIPT_SIZE], const uint8_t* msg, size_t len) {
    return dm_store_verify_receipt(inst ? inst->topo_sqlite_db : NULL, receipt, msg, len) ? -1 : 0;
}

/* Удалить исходящую задачу по точной подписанной квитанции; историю беседы оставить. */
int dm_accept_receipt(struct UTUN_INSTANCE* inst, const uint8_t receipt[DM_RECEIPT_SIZE]) {
    struct dm_state* dm = dm_of(inst);
    if (!dm || !dm->initialized || !dm->db || db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM receipt: missing legacy writer or RW worker requires submit"); return -1;
    }
    struct dm_receipt_update update; const char* stage = "begin"; int started = 0;
    int rc = sqlite3_exec(dm->db, "BEGIN IMMEDIATE", NULL, NULL, NULL); if (rc != SQLITE_OK) goto error;
    started = 1; stage = "apply"; rc = dm_store_receipt_apply(dm->db, inst->my_ed25519_pubkey, receipt, &update); if (rc != SQLITE_OK) goto error;
    stage = "commit"; rc = sqlite3_exec(dm->db, "COMMIT", NULL, NULL, NULL); if (rc != SQLITE_OK) goto error;
    if (update.present) {
        DEBUG_INFO(DEBUG_CATEGORY_DM, "DM delivery confirmed conv=%llu seq=%llu recipient=%016llx complete=%d",
            (unsigned long long)update.conv, (unsigned long long)update.seq, (unsigned long long)update.recipient, update.complete);
        dm_media_notify(inst, update.conv);
    }
    return 0;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM legacy receipt stage=%s rc=%d: %s", stage, rc,
        (sqlite3_errcode(dm->db) & 255) == (rc & 255) ? sqlite3_errmsg(dm->db) : sqlite3_errstr(rc));
    if (started && !sqlite3_get_autocommit(dm->db)) {
        int rollback = sqlite3_exec(dm->db, "ROLLBACK", NULL, NULL, NULL);
        if (rollback != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM receipt rollback rc=%d: %s", rollback, sqlite3_errmsg(dm->db));
        if (!sqlite3_get_autocommit(dm->db)) {
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM receipt transaction remains open; requesting owner stop/DB close"); utun_instance_stop(inst);
        }
    }
    return -1;
}

/* WRITE получает только immutable ACK/self и самостоятельный результат. */
static int dm_receipt_work(sqlite3* db, void* arg) {
    struct dm_receipt_sql* sql = arg; return dm_store_receipt_apply(db, sql->self_ed, sql->receipt, &sql->update);
}
static int dm_receipt_current(void* arg) {
    struct dm_receipt_job* job = arg;
    return !job->inst->destroying && job->owner && job->owner->initialized && dm_of(job->inst) == job->owner;
}
static void dm_receipt_cleanup(void* arg) {
    struct dm_receipt_job* job = arg;
    if (job->owner) {
        struct dm_receipt_job** slot = &job->owner->receipts;
        while (*slot && *slot != job) slot = &(*slot)->next;
        if (*slot) *slot = job->next;
        else DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM receipt cleanup lost owner=%p", (void*)job->owner);
    }
    u_free(job);
}
/* Notify может остановить DM; повторная проверка запрещает fanout старого результата после этого события. */
static void dm_receipt_done(void* arg, const struct db_async_result* result) {
    struct dm_receipt_job* job = arg; int failed = result->status || !dm_receipt_current(job);
    if (!failed && job->sql.update.present) {
        const struct dm_receipt_update* update = &job->sql.update;
        DEBUG_INFO(DEBUG_CATEGORY_DM, "DM delivery committed conv=%llu seq=%llu recipient=%016llx complete=%d",
            (unsigned long long)update->conv, (unsigned long long)update->seq, (unsigned long long)update->recipient, update->complete);
        dm_media_notify(job->inst, update->conv); failed = !dm_receipt_current(job);
    }
    if (failed) DEBUG_WARN(DEBUG_CATEGORY_DM, "DM receipt complete status=%d rc=%d current=%d: %s",
        result->status, result->sqlite_code, dm_receipt_current(job),
        result->message[0] ? result->message : "DM stopped/replaced or core is being destroyed");
    if (job->done) job->done(job->arg, failed ? -1 : 0, failed ? NULL : job->sql.receipt);
}

/* Скопировать вход до освобождения router entry; один completion/cleanup на любом исходе очереди. */
void dm_receipt_submit(struct UTUN_INSTANCE* inst, const uint8_t receipt[DM_RECEIPT_SIZE], dm_receipt_done_fn done, void* arg) {
    struct dm_state* dm = dm_of(inst);
    if (!dm || !dm->initialized || inst->destroying || !receipt) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM receipt submit: invalid/stopped owner or missing receipt");
        if (done) done(arg, -1, NULL);
        return;
    }
    struct dm_receipt_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM receipt request allocation failed"); if (done) done(arg, -1, NULL); return; }
    memcpy(job->sql.self_ed, inst->my_ed25519_pubkey, 32); memcpy(job->sql.receipt, receipt, DM_RECEIPT_SIZE); job->owner = dm; job->inst = inst;
    job->done = done; job->arg = arg; job->next = dm->receipts; dm->receipts = job;
    if (db_async_can_write(inst->db_worker)) {
        db_async_run(inst->db_worker, &(struct db_async_task){.name="dm-receipt", .mode=DB_ASYNC_WRITE, .work=dm_receipt_work,
            .data=&job->sql, .done=dm_receipt_done, .cleanup=dm_receipt_cleanup, .guard=dm_receipt_current}); return;
    }
    const char* file = dm->db ? sqlite3_db_filename(dm->db, "main") : NULL;
    struct db_async_result result = {.status=DB_ASYNC_CLOSED, .sqlite_code=SQLITE_ABORT};
    snprintf(result.message, sizeof(result.message), "DM receipt has no asynchronous writer");
    if (dm->db && (inst->db_worker || (file && !*file))) {
        int rc = dm_accept_receipt(inst, job->sql.receipt); result.status = rc ? DB_ASYNC_SQL : DB_ASYNC_OK;
        result.sqlite_code = rc ? SQLITE_ERROR : SQLITE_OK;
        snprintf(result.message, sizeof(result.message), "legacy DM receipt: %s", sqlite3_errstr(result.sqlite_code));
    }
    dm_receipt_done(job, &result); dm_receipt_cleanup(job);
}

static int dm_outbox_work(sqlite3* db, void* arg) { return dm_outbox_batch_claim(db, arg); }
static int dm_outbox_current(void* arg) {
    struct dm_outbox_job* job = arg;
    return job->owner && !job->inst->destroying && job->inst->dm == job->owner && job->owner->initialized &&
        job->owner->delivery == job;
}
static void dm_outbox_cleanup(void* arg) {
    struct dm_outbox_job* job = arg;
    if (job->owner) {
        if (job->owner->delivery == job) job->owner->delivery = NULL;
        else DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM outbox cleanup lost owner=%p", (void*)job->owner);
    }
    dm_outbox_batch_clear(&job->batch); u_free(job);
}
/* Сеть видит только committed сроки и собственные копии; stop внутри транспорта прекращает обход. */
static void dm_outbox_done(void* arg, const struct db_async_result* result) {
    struct dm_outbox_job* job = arg; int current = dm_outbox_current(job);
    if (result->status || !current) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM outbox complete status=%d sqlite=%d current=%d: %s",
            result->status, result->sqlite_code, current, result->message[0] ? result->message : "DM stopped/replaced"); return;
    }
    unsigned direct_count = 0, mailbox_count = 0;
    for (unsigned i = 0; i < job->batch.count && dm_outbox_current(job); i++) {
        const struct dm_outbox_delivery* item = &job->batch.items[i]; uint64_t group = topo_groups_channel_slot(job->inst->topo_groups, item->group);
        int direct = dm_route_group_key(job->inst, item->recipient_ed, group, &group, ETCP_RT_ID_DM);
        if (direct && !dm_route_send(job->owner, group, item->recipient_ed, DM_SUBCMD_MSG, item->body, item->size)) direct_count++;
        if (dm_outbox_current(job) && (!direct || job->batch.now >= item->direct_until)) {
            DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM outbox fallback conv=%s seq=%llu recipient=%s reason=%s", item->conv,
                (unsigned long long)item->seq, sc_public_key_hex(item->recipient_ed, (char[65]){0}), direct ? "ACK deadline" : "no route");
            if (!dm_mailbox_put(job->inst, item->recipient_ed, item->body, item->size)) mailbox_count++;
        }
    }
    if (job->batch.count) DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM outbox committed count=%u direct=%u mailbox=%u retry_at=%lld current=%d",
        job->batch.count, direct_count, mailbox_count, (long long)(job->batch.now + 5), dm_outbox_current(job));
}
/* Переходный единственный writer: тот же SQL, без вторых алгоритмов и вмешательства в чужую транзакцию. */
static void dm_outbox_legacy(struct dm_outbox_job* job) {
    sqlite3* db = job->owner->db; const char* file = db ? sqlite3_db_filename(db, "main") : NULL;
    int allowed = db && sqlite3_get_autocommit(db) && (job->inst->db_worker || (file && !*file));
    int rc = allowed ? sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) : SQLITE_MISUSE;
    if (!rc) rc = dm_outbox_work(db, &job->batch);
    if (!rc) rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM outbox legacy WRITE rc=%d: %s", rc, db ? sqlite3_errmsg(db) : "database unavailable");
        if (allowed && !sqlite3_get_autocommit(db)) {
            int rollback = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            if (rollback) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM outbox rollback rc=%d: %s", rollback, sqlite3_errmsg(db));
            if (!sqlite3_get_autocommit(db)) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM outbox left transaction open"); utun_instance_stop(job->inst); }
        }
    }
    struct db_async_result result = {.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK, .sqlite_code=rc};
    snprintf(result.message, sizeof(result.message), "legacy outbox: %s", sqlite3_errstr(rc));
    dm_outbox_done(job, &result); dm_outbox_cleanup(job);
}
/* Только один ожидающий WRITE. FULL/OOM/ошибки освобождают слот; следующий таймер повторит попытку. */
static void dm_pump(struct dm_state* dm) {
    if (!dm->initialized || dm->delivery) return;
    struct dm_outbox_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM outbox job allocation failed"); return; }
    job->inst = dm->inst; job->owner = dm; dm->delivery = job;
    memcpy(job->batch.self_ed, dm->inst->my_ed25519_pubkey, 32); job->batch.now = (int64_t)ntp_time_get_seconds(dm->inst);
    if (db_async_can_write(dm->inst->db_worker)) {
        db_async_run(dm->inst->db_worker, &(struct db_async_task){.name="dm-outbox", .mode=DB_ASYNC_WRITE,
            .work=dm_outbox_work, .data=&job->batch, .guard=dm_outbox_current, .done=dm_outbox_done, .cleanup=dm_outbox_cleanup}); return;
    }
    dm_outbox_legacy(job);
}

/* Durable-задачи переживают stop; таймер является только локальным триггером. */
static void dm_tick(void* arg) {
    struct dm_state* dm = arg;
    dm->timer = NULL;
    dm->timer = uasync_set_timeout(dm->inst->ua, 10000, dm, dm_tick, "dm_outbox");
    if (!dm->timer) DEBUG_ERROR(DEBUG_CATEGORY_DM, "%s: retry timer allocation failed", DM_ID);
    dm_pump(dm);
}

/* Копии S удаляются только после durable применения ACK и проверки жизни исходного DM. */
static void dm_receipt_forward(void* arg, int status, const uint8_t* receipt) {
    if (!status) dm_mailbox_receipt(arg, receipt);
}

/* Маршрут входа остаётся разрешённым до SQL и после commit; stop обнуляет слабого owner. */
static int dm_live_current(void* arg) {
    struct dm_live_job* job = arg;
    return job->owner && !job->inst->destroying && job->inst->dm == job->owner && job->owner->initialized &&
        topo_group_chat_key_allowed(job->inst, job->group, job->source_ed);
}
static void dm_live_received(void* arg, const struct dm_receive_result* result) {
    struct dm_live_job* job = arg;
    if (!result->status && dm_live_current(job)) {
        dm_route_send(job->owner, job->group, job->source_ed, DM_SUBCMD_ACK, result->receipt, DM_RECEIPT_SIZE);
        if (dm_live_current(job)) dm_mailbox_receipt(job->inst, result->receipt);
    }
    if (job->owner) {
        struct dm_live_job** slot = &job->owner->live;
        while (*slot && *slot != job) slot = &(*slot)->next;
        if (*slot) *slot = job->next;
        else DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM live receive cleanup lost owner=%p", (void*)job->owner);
    }
    u_free(job);
}

/* Live-доставка использует тот же commit/receipt, что и доставка с суперузла. */
static void dm_recv_cb(struct ETCP_CONN* conn, struct ll_entry* entry) {
    if (!entry || !conn || !entry->dgram || entry->len <= ROUTER_SVC_PAYLOAD_OFF) {
        if (entry) { queue_dgram_free(entry); queue_entry_free(entry); }
        return; /* Уведомление router о закрытии канала. */
    }
    struct dm_state* dm = dm_of(conn->instance);
    if (!dm || !dm->initialized) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "%s: receive while stopped", DM_ID);
        goto done;
    }
    const uint8_t* p = entry->dgram + ROUTER_SVC_PAYLOAD_OFF;
    size_t len = entry->len - ROUTER_SVC_PAYLOAD_OFF - 1;
    uint64_t gid;
    memcpy(&gid, entry->dgram + ROUTER_SVC_GROUP_OFF, 8);
    const uint8_t* source_ed = entry->dgram + ROUTER_SVC_SRC_ED_OFF;
    if (!topo_group_chat_key_allowed(dm->inst, gid, source_ed)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM receive denied group=%llu peer=%s cmd=%u",
            (unsigned long long)gid, sc_public_key_hex(source_ed, (char[65]){0}), p[0]); goto done;
    }
    if (!(entry->dgram[ROUTER_SVC_FLAGS_OFF] & ROUTER_FLAG_SIGNED)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "%s: unsigned route packet source=%s", DM_ID, sc_public_key_hex(source_ed, (char[65]){0}));
        goto done;
    }
    if (p[0] == DM_SUBCMD_MSG) {
        if (len < DM_MSG_FIXED_HDR || memcmp(source_ed, p + 1 + DM_MSG_AUTHOR_OFF, 32)) {
            DEBUG_WARN(DEBUG_CATEGORY_DM, "DM live message rejected: signed author differs from router sender group=%llu peer=%s bytes=%zu",
                       (unsigned long long)gid, sc_public_key_hex(source_ed, (char[65]){0}), len); goto done;
        }
        struct dm_live_job* job = u_calloc(1, sizeof(*job));
        if (!job) {
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM live receive allocation failed source=%s", sc_public_key_hex(source_ed, (char[65]){0}));
            goto done;
        }
        job->owner = dm; job->inst = dm->inst; job->group = gid; memcpy(job->source_ed, source_ed, 32);
        job->next = dm->live; dm->live = job;
        dm_receive_submit(dm->inst, &(struct dm_receive_query){.body=p + 1, .size=len, .valid=dm_live_current}, dm_live_received, job);
    } else if (p[0] == DM_SUBCMD_ACK && len == DM_RECEIPT_SIZE) {
        if (memcmp(source_ed, p + 1 + DM_FACT_RECIPIENT_OFF, 32)) {
            DEBUG_WARN(DEBUG_CATEGORY_DM, "DM live ACK rejected: recipient differs from router sender group=%llu peer=%s",
                       (unsigned long long)gid, sc_public_key_hex(source_ed, (char[65]){0})); goto done;
        }
        dm_receipt_submit(dm->inst, p + 1, dm_receipt_forward, dm->inst);
    } else {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "%s: invalid command=%u bytes=%zu", DM_ID, p[0], len);
    }
done:
    queue_dgram_free(entry);
    queue_entry_free(entry);
}

/* ── публичный API ── */

/* Открыть существующую беседу либо создать при подтверждённом отсутствии записи. */
static int dm_start_current(void* arg) {
    struct dm_start_job* job = arg;
    return job->owner && !job->inst->destroying && job->inst->dm == job->owner && job->owner->initialized &&
        (!job->valid || job->valid(job->arg));
}
static void dm_start_cleanup(void* arg) {
    struct dm_start_job* job = arg;
    if (job->owner) {
        struct dm_start_job** slot = &job->owner->starts;
        while (*slot && *slot != job) slot = &(*slot)->next;
        if (*slot) *slot = job->next;
        else DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM start cleanup lost owner=%p", (void*)job->owner);
    }
    OPENSSL_cleanse(&job->sql, sizeof(job->sql)); u_free(job);
}
/* Событие может остановить DM; callback получает успех только при ещё действующем owner/valid. */
static void dm_start_done(void* arg, const struct db_async_result* outcome) {
    struct dm_start_job* job = arg; int current = dm_start_current(job);
    struct dm_start_result result = {.status=-1, .sqlite_code=outcome->sqlite_code};
    if (!outcome->status && current) {
        char conv[21]; snprintf(conv, sizeof(conv), "%llu", (unsigned long long)job->sql.conversation);
        size_t length = strlen(conv); uint8_t event[1 + 64 + 8]; event[0] = (uint8_t)length;
        memcpy(event + 1, conv, length); memcpy(event + 1 + length, &job->sql.generation, 8);
        DEBUG_INFO(DEBUG_CATEGORY_DM, "DM start committed conv=%s mode=%d peer=%016llx generation=%llu",
            conv, job->sql.mode, (unsigned long long)job->sql.peer, (unsigned long long)job->sql.generation);
        chat_event_post(job->inst, CHAT_EVT_DM_CONV_UPDATED, event, 1 + (int)length + 8);
        current = dm_start_current(job);
        if (current) { result.status = 0; result.conversation = job->sql.conversation; result.generation = job->sql.generation; }
    }
    if (result.status) {
        if (!result.sqlite_code) result.sqlite_code = SQLITE_ABORT;
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM start complete peer=%016llx status=%d sqlite=%d current=%d: %s",
            (unsigned long long)job->sql.peer, outcome->status, result.sqlite_code, current,
            outcome->message[0] ? outcome->message : "conversation owner/request stopped");
    }
    job->done(job->arg, &result);
}
static int dm_start_work(sqlite3* db, void* arg) { return dm_store_start(db, arg); }
/* Переходный writer использует тот же атомарный алгоритм, не вмешиваясь в чужую транзакцию. */
static void dm_start_legacy(struct dm_start_job* job) {
    sqlite3* db = job->owner->db; int owns = db && sqlite3_get_autocommit(db); const char* stage = "begin";
    int rc = owns ? sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) : SQLITE_MISUSE;
    if (!rc) { stage = "write"; rc = dm_start_work(db, &job->sql); }
    if (!rc && !dm_start_current(job)) { stage = "cancel"; rc = SQLITE_ABORT; }
    if (!rc) { stage = "commit"; rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL); }
    struct db_async_result result = {.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK, .sqlite_code=rc};
    if (rc) {
        snprintf(result.message, sizeof(result.message), "legacy start %s: %s", stage,
            db && (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM start legacy rc=%d: %s", rc, result.message);
        if (owns && !sqlite3_get_autocommit(db)) {
            int rollback = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            if (rollback) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM start rollback rc=%d: %s", rollback, sqlite3_errmsg(db));
            if (!sqlite3_get_autocommit(db)) {
                DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM start left open transaction; stopping core"); utun_instance_stop(job->inst);
            }
        }
    }
    dm_start_done(job, &result); dm_start_cleanup(job);
}
void dm_start_submit(struct UTUN_INSTANCE* inst, const struct dm_start_query* query, dm_start_done_fn done, void* arg) {
    struct dm_state* dm = dm_of(inst); struct dm_start_result failure = {.status=-1, .sqlite_code=SQLITE_MISUSE};
    if (!done) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM start: missing completion"); return; }
    uint8_t derived_x[32];
    struct dm_start_query resolved;
    if (query && query->peer_ed && !query->peer_x) {
        if (sc_identity_public_x("DM start", query->peer_ed, derived_x) != SC_OK) { done(arg, &failure); return; }
        resolved = *query;
        resolved.peer = sc_derive_node_id_from_pubkey(derived_x);
        resolved.peer_x = derived_x;
        query = &resolved;
    }
    if (!dm || !dm->initialized || inst->destroying || !query || !query->peer ||
        (query->peer_ed ? !memcmp(query->peer_ed, inst->my_ed25519_pubkey, 32) : query->peer == inst->node_id) ||
        (query->mode != DM_MODE_DEVICE && query->mode != DM_MODE_GROUPS) || (!!query->peer_x != !!query->peer_ed) ||
        (query->source && *query->source && !chat_channel_valid(query->source))) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM start: invalid owner/peer/mode/keys/source"); done(arg, &failure); return;
    }
    struct dm_start_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM start allocation failed"); failure.sqlite_code = SQLITE_NOMEM; done(arg, &failure); return; }
    job->owner = dm; job->inst = inst; job->done = done; job->arg = arg; job->valid = query->valid;
    job->sql.peer = query->peer; job->sql.mode = query->mode; job->sql.identity.node = inst->node_id;
    memcpy(job->sql.identity.x, inst->my_keys.public_key, 32); memcpy(job->sql.identity.ed, inst->my_ed25519_pubkey, 32);
    memcpy(job->sql.identity.private_key, inst->my_keys.private_key, 32);
    job->sql.now = (uint64_t)ntp_time_get_seconds(inst); if (query->source) strcpy(job->sql.group, query->source);
    uint64_t fallback = 0;
    if (dm_shared_group_id(dm, query->peer, query->peer_ed, &fallback))
        strcpy(job->sql.fallback_group, topo_groups_channel(inst->topo_groups, fallback));
    job->sql.supplied_name = query->name != NULL;
    dm_store_copy_name(job->sql.name, query->name, query->name ? strlen(query->name) : 0, "start");
    if (query->peer_x) {
        job->sql.supplied_keys = 1; memcpy(job->sql.peer_x, query->peer_x, 32); memcpy(job->sql.peer_ed, query->peer_ed, 32);
    }
    job->next = dm->starts; dm->starts = job;
    if (db_async_can_write(inst->db_worker)) {
        db_async_run(inst->db_worker, &(struct db_async_task){.name="dm-start", .mode=DB_ASYNC_WRITE,
            .work=dm_start_work, .data=&job->sql, .guard=dm_start_current, .done=dm_start_done, .cleanup=dm_start_cleanup}); return;
    }
    if (!dm_start_current(job)) {
        struct db_async_result cancelled = {.status=DB_ASYNC_CANCELLED, .sqlite_code=SQLITE_ABORT};
        dm_start_done(job, &cancelled); dm_start_cleanup(job); return;
    }
    dm_start_legacy(job);
}
static void dm_start_sync_done(void* arg, const struct dm_start_result* result) { *(struct dm_start_result*)arg = *result; }
int dm_start(struct UTUN_INSTANCE* inst, uint64_t peer_node_id,
    const uint8_t peer_x25519[32], const uint8_t peer_ed25519[32], const char* peer_name,
    const char* source_ch_id, enum dm_mode mode, uint64_t* result_conv) {
    if (!peer_x25519 || !peer_ed25519 || (inst && db_async_can_write(inst->db_worker))) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM synchronous start: missing keys or RW worker requires submit"); return -1;
    }
    struct dm_start_result result = {.status=-1};
    dm_start_submit(inst, &(struct dm_start_query){.peer=peer_node_id, .mode=mode, .name=peer_name ? peer_name : "",
        .source=source_ch_id, .peer_x=peer_x25519, .peer_ed=peer_ed25519}, dm_start_sync_done, &result);
    if (!result.status && result_conv) *result_conv = result.conversation;
    return result.status;
}

/* Проверить живого владельца и запрос в uasync перед SQL либо публикацией результата. */
static int dm_send_current(void* arg) {
    struct dm_send_job* job = arg;
    return job->owner && !job->inst->destroying && job->inst->dm == job->owner && job->owner->initialized &&
        (!job->valid || job->valid(job->arg));
}
/* Удалить запрос из списка живого владельца и стереть копии ключей/открытого текста на любом исходе. */
static void dm_send_cleanup(void* arg) {
    struct dm_send_job* job = arg;
    if (job->owner) {
        struct dm_send_job** slot = &job->owner->sends;
        while (*slot && *slot != job) slot = &(*slot)->next;
        if (*slot) *slot = job->next;
        else DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM send cleanup lost owner=%p", (void*)job->owner);
    }
    OPENSSL_cleanse(&job->sql, sizeof(job->sql)); u_free(job);
}
/* Уведомление и доставка используют только committed результат; callback может остановить DM. */
static void dm_send_done(void* arg, const struct db_async_result* outcome) {
    struct dm_send_job* job = arg; int current = dm_send_current(job);
    struct dm_send_result result = {.status=-1, .sqlite_code=outcome->sqlite_code, .committed=!outcome->status};
    if (!outcome->status && current) {
        size_t length = strlen(job->sql.conv); uint8_t event[1 + 64 + 32]; event[0] = (uint8_t)length;
        memcpy(event + 1, job->sql.conv, length); memcpy(event + 1 + length, job->sql.identity.ed, 32);
        DEBUG_INFO(DEBUG_CATEGORY_DM, "DM send committed conv=%s seq=%llu generation=%llu bytes=%u", job->sql.conv,
            (unsigned long long)job->sql.seq, (unsigned long long)job->sql.current_generation, job->sql.size);
        chat_event_post(job->inst, CHAT_EVT_DM_MSG_RECEIVED, event, (int)(1 + length + 32));
        if (dm_send_current(job)) dm_pump(job->owner);
        current = dm_send_current(job);
        if (current) { result.status = 0; result.seq = job->sql.seq; result.generation = job->sql.current_generation; }
    }
    if (result.status) {
        if (!result.sqlite_code) result.sqlite_code = SQLITE_ABORT;
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM send complete conv=%s status=%d sqlite=%d current=%d: %s", job->sql.conv,
            outcome->status, result.sqlite_code, current, outcome->message[0] ? outcome->message : "owner/request stopped");
    }
    job->done(job->arg, &result);
}
static int dm_send_work(sqlite3* db, void* arg) { return dm_store_send(db, arg); }
/* Единственный переходный writer выполняет тот же SQL и не вмешивается в чужую транзакцию. */
static void dm_send_legacy(struct dm_send_job* job) {
    sqlite3* db = job->owner->db; const char* file = db ? sqlite3_db_filename(db, "main") : NULL;
    int owns = db && sqlite3_get_autocommit(db) && (job->inst->db_worker || (file && !*file));
    const char* stage = "begin"; int rc = owns ? sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) : SQLITE_MISUSE;
    if (!rc) { stage = "write"; rc = dm_send_work(db, &job->sql); }
    if (!rc && !dm_send_current(job)) { stage = "cancel"; rc = SQLITE_ABORT; }
    if (!rc) { stage = "commit"; rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL); }
    struct db_async_result result = {.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK, .sqlite_code=rc};
    if (rc) {
        snprintf(result.message, sizeof(result.message), "legacy send %s: %s", stage,
            db && (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM send legacy rc=%d: %s", rc, result.message);
        if (owns && !sqlite3_get_autocommit(db)) {
            int rollback = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            if (rollback) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM send rollback rc=%d: %s", rollback, sqlite3_errmsg(db));
            if (!sqlite3_get_autocommit(db)) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM send left transaction open"); utun_instance_stop(job->inst); }
        }
    }
    dm_send_done(job, &result); dm_send_cleanup(job);
}

void dm_send_submit(struct UTUN_INSTANCE* inst, const struct dm_send_query* query, dm_send_done_fn done, void* arg) {
    struct dm_state* dm = dm_of(inst); struct dm_send_result failed = {.status=-1, .sqlite_code=SQLITE_MISUSE};
    if (!done) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM send: missing completion"); return; }
    if (!dm || !dm->initialized || inst->destroying || !query || !query->conv || !query->type ||
        strlen(query->conv) >= 64 || strlen(query->type) > 255 || query->size > DM_DATA_MAX ||
        (!query->data && query->size) || query->generation > INT64_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM send: invalid owner/conversation/type/data/generation"); done(arg, &failed); return;
    }
    struct dm_send_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM send allocation failed"); failed.sqlite_code = SQLITE_NOMEM; done(arg, &failed); return; }
    job->owner = dm; job->inst = inst; job->done = done; job->arg = arg; job->valid = query->valid;
    snprintf(job->sql.conv, sizeof(job->sql.conv), "%s", query->conv); snprintf(job->sql.type, sizeof(job->sql.type), "%s", query->type);
    job->sql.size = query->size; if (query->size) memcpy(job->sql.data, query->data, query->size);
    job->sql.generation = query->generation; job->sql.now_ms = (uint64_t)(ntp_time_get_us(inst) / 1000);
    job->sql.identity.node = inst->node_id; memcpy(job->sql.identity.x, inst->my_keys.public_key, 32);
    memcpy(job->sql.identity.ed, inst->my_ed25519_pubkey, 32); memcpy(job->sql.identity.private_key, inst->my_keys.private_key, 32);
    memcpy(job->sql.signing_key, inst->my_ed25519_privkey, 32);
    if (!strcmp(query->type, "file") && (query->size <= DM_MEDIA_DESC_SIZE || dm_media_path_snapshot(inst, query->data, job->sql.file_path))) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM send: missing prepared file owner/descriptor conv=%s", query->conv);
        OPENSSL_cleanse(&job->sql, sizeof(job->sql)); u_free(job); failed.sqlite_code = SQLITE_AUTH; done(arg, &failed); return;
    }
    job->next = dm->sends; dm->sends = job;
    if (db_async_can_write(inst->db_worker)) {
        db_async_run(inst->db_worker, &(struct db_async_task){.name="dm-send", .mode=DB_ASYNC_WRITE, .work=dm_send_work,
            .data=&job->sql, .guard=dm_send_current, .done=dm_send_done, .cleanup=dm_send_cleanup}); return;
    }
    if (!dm_send_current(job)) {
        struct db_async_result cancelled = {.status=DB_ASYNC_CANCELLED, .sqlite_code=SQLITE_ABORT};
        dm_send_done(job, &cancelled); dm_send_cleanup(job); return;
    }
    dm_send_legacy(job);
}
static void dm_send_sync_done(void* arg, const struct dm_send_result* result) { *(struct dm_send_result*)arg = *result; }
int dm_send(struct UTUN_INSTANCE* inst, const char* conv, const char* type, const uint8_t* data, uint32_t size) {
    if (inst && db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM synchronous send: RW worker requires dm_send_submit"); return -1;
    }
    struct dm_send_result result = {.status=-1};
    dm_send_submit(inst, &(struct dm_send_query){.conv=conv, .type=type, .data=data, .size=size}, dm_send_sync_done, &result);
    return result.status;
}

/* Таблицы принадлежат DM, соединение с SQLite — общему ядру. */
int dm_core_init(struct UTUN_INSTANCE* inst) {
    if (!inst) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "%s: init without instance", DM_ID); return -1; }
    if (inst->dm) {
        if (inst->dm->initialized) return 0;
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM init: quiesced owner still awaiting destroy"); return -1;
    }
    struct dm_state* dm = u_calloc(1, sizeof(*dm));
    if (!dm) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "%s: state allocation failed", DM_ID); return -1; }
    inst->dm = dm;
    dm->inst = inst;
    dm->db = inst->topo_sqlite_db;
    if (!db_async_can_write(inst->db_worker) && (!dm->db || dm_schema_init(inst) != 0 || dm_history_init(inst))) goto fail;
    if (etcp_router_bind(inst, ETCP_RT_ID_DM, dm_recv_cb) != 0) goto fail;
    if (etcp_router_require_crypto(inst, ETCP_RT_ID_DM, ROUTER_FLAG_SIGNED) ||
        etcp_router_set_authorize(inst, ETCP_RT_ID_DM, topo_group_chat_key_allowed)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM router policy setup failed");
        etcp_router_unbind(inst, ETCP_RT_ID_DM); goto fail;
    }
    dm->initialized = 1;
    dm->timer = uasync_set_timeout(inst->ua, 10000, dm, dm_tick, "dm_outbox");
    if (!dm->timer) {
        etcp_router_unbind(inst, ETCP_RT_ID_DM);
        goto fail;
    }
    DEBUG_INFO(DEBUG_CATEGORY_DM, "%s: initialized persistent outbox", DM_ID);
    return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "%s: initialization failed db=%p", DM_ID, (void*)dm->db);
    u_free(dm);
    inst->dm = NULL;
    return -1;
}

/* Снять producers до media drain; принятые SQL завершаются с detached owner без событий/сети. */
void dm_core_quiesce(struct UTUN_INSTANCE* inst) {
    struct dm_state* dm = dm_of(inst);
    if (!dm || !dm->initialized) return;
    dm->initialized = 0;
    for (struct dm_delete_job* job = dm->deletes; job; job = job->next) job->owner = NULL;
    dm->deletes = NULL;
    for (struct dm_delete_event* event = dm->delete_events; event; event = event->next) event->owner = NULL;
    dm->delete_events = NULL;
    for (struct dm_ui_event* event = dm->ui_events; event; event = event->next) event->owner = NULL;
    dm->ui_events = NULL;
    for (struct dm_route_job* job = dm->routes; job; job = job->next) job->owner = NULL;
    dm->routes = NULL;
    for (struct dm_live_job* job = dm->live; job; job = job->next) job->owner = NULL;
    dm->live = NULL;
    for (struct dm_receive_job* job = dm->receives; job; job = job->next) job->owner = NULL;
    dm->receives = NULL;
    for (struct dm_send_job* job = dm->sends; job; job = job->next) job->owner = NULL;
    dm->sends = NULL;
    if (dm->delivery) { dm->delivery->owner = NULL; dm->delivery = NULL; }
    for (struct dm_json_job* job = dm->reads; job; job = job->next) job->owner = NULL;
    dm->reads = NULL;
    for (struct dm_members_job* job = dm->members; job; job = job->next) job->owner = NULL;
    dm->members = NULL;
    for (struct dm_receipt_job* job = dm->receipts; job; job = job->next) job->owner = NULL;
    dm->receipts = NULL;
    for (struct dm_start_job* job = dm->starts; job; job = job->next) job->owner = NULL;
    dm->starts = NULL;
    if (dm->invite_job) dm_invite_async_cancel(dm, 1);
    else if (dm->invite) chat_invite_build_cancel(dm->invite->pending);
    if (dm->timer) uasync_cancel_timeout(inst->ua, dm->timer);
    dm->timer = NULL;
    etcp_router_unbind(inst, ETCP_RT_ID_DM);
    DEBUG_INFO(DEBUG_CATEGORY_DM, "%s: quiesced before media drain, durable outbox preserved", DM_ID);
}
void dm_core_destroy(struct UTUN_INSTANCE* inst) {
    struct dm_state* dm = dm_of(inst); if (!dm) return;
    dm_core_quiesce(inst); inst->dm = NULL; u_free(dm);
    DEBUG_INFO(DEBUG_CATEGORY_DM, "%s: destroyed", DM_ID);
}

/* ── JSON для GUI/headless ── */

static int dm_members_display_read(sqlite3* db, const uint8_t self[32], const struct dm_target* target, const char* channel,
    struct chat_member_display* member);

int dm_members_snapshot(struct UTUN_INSTANCE* inst, const char* conv, uint64_t generation, uint8_t** out, size_t* size) {
    struct dm_state* dm = dm_of(inst); struct dm_conv c;
    if (out) *out = NULL;
    if (size) *size = 0;
    if (!dm || !conv || !conv[0] || strlen(conv) > 63 || !out || !size || !dm_history_current(inst, conv, generation)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM members: invalid/stale request conv=%s generation=%llu", conv ? conv : "",
                   (unsigned long long)generation); return -1;
    }
    if (dm_conv_load(dm, conv, &c) != DM_CONV_FOUND) return -1;
    OPENSSL_cleanse(c.content_key, sizeof(c.content_key));
    struct dm_target members[DM_TARGET_MAX]; size_t count = 0;
    if (!c.closed && c.mode == DM_MODE_GROUPS && dm_identity_members(inst, c.parties, members, DM_TARGET_MAX, &count)) return -1;
    size_t cl = strlen(conv), bytes = 11 + cl + count * DM_MEMBER_WIRE_SIZE;
    uint8_t* snapshot = u_calloc(1, bytes);
    if (!snapshot) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM members: allocation failed count=%zu", count); return -1; }
    snapshot[0] = (uint8_t)cl; memcpy(snapshot + 1, conv, cl);
    mf_put64(snapshot + 1 + cl, generation); snapshot[9 + cl] = (uint8_t)count; snapshot[10 + cl] = (uint8_t)(count >> 8);
    size_t pos = 11 + cl;
    for (unsigned side = 0; side < 2; side++) for (size_t i = 0; i < count; i++) {
        if (members[i].side != side) continue;
        struct chat_member_display member = {0}; uint64_t group = 0;
        if (!dm_find_route_group(inst, members[i].node, members[i].ed, topo_groups_channel_slot(inst->topo_groups, c.group_id), &group, 1, ETCP_RT_ID_DM))
            dm_shared_group_id(dm, members[i].node, members[i].ed, &group);
        if (dm_members_display_read(dm->db, inst->my_ed25519_pubkey, members + i, topo_groups_channel(inst->topo_groups, group), &member)) goto fail;
        if (group) {
            const char* channel = topo_groups_channel(inst->topo_groups, group);
            if (chat_member_apply_live(inst, channel, &member)) goto fail;
        }
        snapshot[pos++] = (uint8_t)side; chat_core_serialize_member(&member, snapshot + pos); pos += CHAT_MEMBER_DISPLAY_SIZE;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM members: snapshot conv=%s generation=%llu count=%zu closed=%d", conv,
                (unsigned long long)generation, count, c.closed);
    *out = snapshot; *size = bytes; return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM members: snapshot failed conv=%s", conv); u_free(snapshot); return -1;
}

static int dm_members_current(void* arg) {
    struct dm_members_job* job = arg;
    return job->owner && !job->invalidated && !job->inst->destroying && job->owner->initialized && dm_of(job->inst) == job->owner;
}
static void dm_members_free(struct dm_members_job* job) {
    if (job->owner) {
        struct dm_members_job** slot = &job->owner->members;
        while (*slot && *slot != job) slot = &(*slot)->next;
        if (*slot) *slot = job->next;
        else DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM members: lost snapshot owner conv=%s", job->conv);
    }
    OPENSSL_cleanse(&job->identity, sizeof(job->identity)); u_free(job);
}
static int dm_members_display_read(sqlite3* db, const uint8_t self[32], const struct dm_target* target, const char* channel,
    struct chat_member_display* member) {
    if (channel[0]) {
        struct chat_member_display* rows = NULL; size_t count = 0;
        int rc = chat_member_rows_read(db, channel, self, target->ed, &rows, &count);
        if (!rc && count != 1) {
            DEBUG_WARN(DEBUG_CATEGORY_DM, "DM members: expected one row channel=%s Ed25519=%s count=%zu",
                channel, sc_public_key_hex(target->ed, (char[65]){0}), count);
            rc = SQLITE_NOTFOUND;
        }
        if (!rc) *member = rows[0];
        u_free(rows); return rc;
    }
    sqlite3_stmt* st = NULL; int rc = sqlite3_prepare_v2(db, "SELECT name FROM nodes WHERE ed25519_pubkey=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, target->ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    char key[65]; sc_public_key_hex(target->ed, key);
    snprintf(member->name, sizeof(member->name), "%.24s", key);
    if (rc == SQLITE_ROW) {
        int type = sqlite3_column_type(st, 0);
        if (type != SQLITE_NULL && type != SQLITE_TEXT) rc = SQLITE_CORRUPT;
        else if (type == SQLITE_TEXT) {
            const char* name = (const char*)sqlite3_column_text(st, 0); int bytes = sqlite3_column_bytes(st, 0);
            if (!name) rc = SQLITE_NOMEM;
            else if (bytes < 0 || memchr(name, 0, (size_t)bytes)) rc = SQLITE_CORRUPT;
            else { if (bytes) snprintf(member->name, sizeof(member->name), "%s", name); rc = SQLITE_OK; }
        } else rc = SQLITE_OK;
    } else if (rc == SQLITE_DONE) rc = SQLITE_OK;
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM members: fallback name Ed25519=%s rc=%d final=%d: %s", key, rc, final, sqlite3_errstr(rc));
    memcpy(member->ed25519, target->ed, 32);
    member->online = member->is_self = !memcmp(target->ed, self, 32);
    member->rtt = UINT16_MAX; member->nat_type = 255; return rc;
}
static int dm_members_work(sqlite3* db, void* arg) {
    struct dm_members_job* job = arg; uint64_t generation = 0; int hidden = 0;
    int rc = dm_history_state_read(db, job->conv, &generation, &hidden); if (rc) return rc;
    if (hidden || generation != job->generation) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM members: stale snapshot conv=%s generation=%llu/%llu hidden=%d phase=%d", job->conv,
            (unsigned long long)job->generation, (unsigned long long)generation, hidden, job->phase); return SQLITE_ABORT;
    }
    if (!job->phase) {
        struct dm_conv conv = {0}; int found = 0;
        rc = dm_store_conv_read(db, &job->identity, job->conv, &conv, &found);
        if (!rc && !found) rc = SQLITE_NOTFOUND;
        if (!rc) {
            strcpy(job->preferred, conv.group_id);
            if (!conv.closed && conv.mode == DM_MODE_GROUPS)
                rc = dm_identity_members_read(db, &job->identity, conv.parties, job->targets, DM_TARGET_MAX, &job->count);
        }
        OPENSSL_cleanse(&conv, sizeof(conv)); return rc;
    }
    for (size_t i = 0; i < job->count; i++) {
        struct chat_member_display* member = &job->rows[i].display;
        rc = dm_members_display_read(db, job->identity.ed, job->targets + i, job->rows[i].group, member);
        if (rc) {
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM members: display READ conv=%s node=%016llx group=%s rc=%d", job->conv,
                (unsigned long long)job->targets[i].node, job->rows[i].group, rc); return rc;
        }
    }
    return SQLITE_OK;
}
static void dm_members_dispatch(struct dm_members_job* job);
static void dm_members_complete(void* arg, const struct db_async_result* result) {
    struct dm_members_job* job = arg; int code = result->sqlite_code;
    if (!result->status && dm_members_current(job) && !job->phase && job->count) {
        for (size_t i = 0; i < job->count; i++) {
            uint64_t group = 0;
            if (!dm_find_route_group(job->inst, job->targets[i].node, job->targets[i].ed, topo_groups_channel_slot(job->inst->topo_groups, job->preferred), &group, 1, ETCP_RT_ID_DM))
                dm_shared_group_id(job->owner, job->targets[i].node, job->targets[i].ed, &group);
            strcpy(job->rows[i].group, topo_groups_channel(job->inst->topo_groups, group));
        }
        job->phase = 1; dm_members_dispatch(job); return;
    }
    uint8_t* data = NULL; size_t size = 0;
    if (!result->status && dm_members_current(job)) {
        size_t length = strlen(job->conv); size = 11 + length + job->count * DM_MEMBER_WIRE_SIZE;
        data = u_calloc(1, size);
        if (!data) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM members: wire allocation conv=%s bytes=%zu", job->conv, size); code = SQLITE_NOMEM; }
        else {
            data[0] = (uint8_t)length; memcpy(data + 1, job->conv, length); mf_put64(data + 1 + length, job->generation);
            data[9 + length] = (uint8_t)job->count; data[10 + length] = (uint8_t)(job->count >> 8);
            size_t pos = 11 + length;
            for (unsigned side = 0; side < 2; side++) for (size_t i = 0; i < job->count; i++) {
                if (job->targets[i].side != side) continue;
                struct chat_member_display* member = &job->rows[i].display;
                if (job->rows[i].group[0]) {
                    const char* channel = job->rows[i].group;
                    if (chat_member_apply_live(job->inst, channel, member)) { code = SQLITE_ABORT; break; }
                }
                data[pos++] = (uint8_t)side; chat_core_serialize_member(member, data + pos); pos += CHAT_MEMBER_DISPLAY_SIZE;
            }
        }
    } else if (!code) code = SQLITE_ABORT;
    if (code || !data) DEBUG_WARN(DEBUG_CATEGORY_DM, "DM members result: conv=%s phase=%d status=%d sqlite=%d current=%d: %s",
        job->conv, job->phase, result->status, code, dm_members_current(job), result->message);
    else DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM members ready: conv=%s generation=%llu count=%zu bytes=%zu", job->conv,
        (unsigned long long)job->generation, job->count, size);
    /* До внешнего callback снять weak owner: callback может остановить DM либо создать новый запрос. */
    dm_members_done_fn done = job->done; void* callback_arg = job->arg;
    dm_members_free(job);
    done(callback_arg, &(struct dm_members_result){.status=code || !data ? -1 : 0, .sqlite_code=code,
        .data=code ? NULL : data, .size=code ? 0 : size}); u_free(data);
}
static void dm_members_dispatch(struct dm_members_job* job) {
    if (job->inst->db_worker) {
        db_async_submit_guarded(job->inst->db_worker, "dm-members", DB_ASYNC_READ,
            dm_members_current, dm_members_work, job, dm_members_complete); return;
    }
    sqlite3* db = job->owner ? job->owner->db : NULL; const char* file = db ? sqlite3_db_filename(db, "main") : NULL;
    int rc = dm_members_current(job) && file && !*file ? dm_members_work(db, job) : SQLITE_ABORT;
    struct db_async_result result = {.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK, .sqlite_code=rc}; dm_members_complete(job, &result);
}
void dm_members_submit(struct UTUN_INSTANCE* inst, const struct dm_members_query* query, dm_members_done_fn done, void* arg) {
    struct dm_state* dm = dm_of(inst);
    int rc = SQLITE_MISUSE;
    if (!done || !dm || !dm->initialized || inst->destroying || !inst->node_id || !query ||
        !chat_numeric_id(query->conv) || !query->generation || query->generation >= INT64_MAX) goto fail;
    struct dm_members_job* job = u_calloc(1, sizeof(*job)); rc = SQLITE_NOMEM; if (!job) goto fail;
    job->owner = dm; job->inst = inst; job->next = dm->members; dm->members = job;
    job->done = done; job->arg = arg; job->generation = query->generation; snprintf(job->conv, sizeof(job->conv), "%s", query->conv);
    job->identity.node = inst->node_id; memcpy(job->identity.x, inst->my_keys.public_key, 32);
    memcpy(job->identity.ed, inst->my_ed25519_pubkey, 32); memcpy(job->identity.private_key, inst->my_keys.private_key, 32);
    dm_members_dispatch(job); return;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM members submit: invalid/stopped request or OOM owner=%p callback=%d rc=%d", (void*)dm, !!done, rc);
    if (done) done(arg, &(struct dm_members_result){.status=-1, .sqlite_code=rc});
}
static void dm_members_event(void* arg, const struct dm_members_result* result) {
    struct dm_messages_req* req = arg;
    if (!result->status) chat_event_post(req->inst, CHAT_EVT_DM_MEMBERS, result->data, (int)result->size);
    u_free(req);
}
void dm_members_trampoline(void* arg) {
    struct dm_messages_req* req = arg;
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM members: missing request"); return; }
    dm_members_submit(req->inst, &(struct dm_members_query){.conv=req->conv_id, .generation=req->generation}, dm_members_event, req);
}

int dm_list_conversations_json(struct UTUN_INSTANCE* inst, char* buf, size_t capacity, size_t* out_len) {
    struct dm_state* dm = dm_of(inst);
    return dm_store_conversations_read(dm ? dm->db : NULL, buf, capacity, out_len) ? -1 : 0;
}

static int dm_json_work(sqlite3* db, void* arg) {
    struct dm_json_sql* sql = arg;
    if (!sql->conv[0]) return dm_store_conversations_read(db, sql->json, sql->capacity, &sql->length);
    return dm_store_messages_read(db, &sql->identity, sql->conv, sql->count, sql->offset, sql->include_hidden,
        &sql->generation, sql->json, sql->capacity, &sql->length);
}
static int dm_json_guard(void* arg) {
    struct dm_json_job* job = arg;
    return !job->invalidated && !job->inst->destroying && job->owner && job->owner->initialized && dm_of(job->inst) == job->owner ?
        (job->valid ? job->valid(job->arg) : 1) : 0;
}
static void dm_json_cleanup(void* arg) {
    struct dm_json_job* job = arg;
    if (job->owner) {
        struct dm_json_job** slot = &job->owner->reads;
        while (*slot && *slot != job) slot = &(*slot)->next;
        if (*slot) *slot = job->next;
        else DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM JSON: cleanup lost request owner=%p", (void*)job->owner);
    }
    OPENSSL_cleanse(&job->sql.identity, sizeof(job->sql.identity)); u_free(job);
}
static void dm_json_done(void* arg, const struct db_async_result* result) {
    struct dm_json_job* job = arg;
    int current = !job->invalidated && !job->inst->destroying && job->owner && job->owner->initialized && dm_of(job->inst) == job->owner;
    int failed = result->status || !current;
    if (failed) DEBUG_WARN(DEBUG_CATEGORY_DM, "DM JSON complete: conv=%s status=%d rc=%d current=%d: %s",
        job->sql.conv, result->status, result->sqlite_code, current,
        result->message[0] ? result->message : "DM snapshot invalidated/stopped/replaced or core is being destroyed");
    else DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM JSON ready conv=%s generation=%llu bytes=%zu capacity=%zu", job->sql.conv,
        (unsigned long long)job->sql.generation, job->sql.length, job->sql.capacity);
    job->done(job->arg, &(struct dm_json_result){.status=failed ? -1 : 0, .json=failed ? NULL : job->sql.json,
        .length=failed ? 0 : job->sql.length, .generation=failed ? 0 : job->sql.generation});
}

/* Собственные query/ключи/выход живут до READ completion; runtime нужен только guard/done. */
void dm_json_read(struct UTUN_INSTANCE* inst, const struct dm_json_query* query, dm_json_done_fn done, void* arg) {
    struct dm_state* dm = dm_of(inst); size_t capacity = query ? query->capacity : 0;
    if (!done || !dm || !dm->initialized || inst->destroying || capacity < 3 || capacity > 1024u * 1024u ||
        (query->conv && (!chat_numeric_id(query->conv) || query->offset < 0 || !inst->node_id))) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM JSON: invalid request owner=%p capacity=%zu callback=%d destroying=%d",
            (void*)dm, capacity, !!done, inst ? inst->destroying : 0);
        if (done) done(arg, &(struct dm_json_result){.status=-1});
        return;
    }
    struct dm_json_job* job = u_calloc(1, sizeof(*job) + capacity);
    if (!job) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM JSON: request allocation failed capacity=%zu", capacity);
        done(arg, &(struct dm_json_result){.status=-1}); return;
    }
    job->sql.json = job->buffer; job->sql.capacity = capacity; job->owner = dm; job->inst = inst;
    job->valid = query->valid; job->done = done; job->arg = arg; job->next = dm->reads; dm->reads = job;
    if (query->conv) {
        snprintf(job->sql.conv, sizeof(job->sql.conv), "%s", query->conv);
        job->sql.generation = query->generation; job->sql.count = query->count; job->sql.offset = query->offset;
        job->sql.include_hidden = query->include_hidden; job->sql.identity.node = inst->node_id;
        memcpy(job->sql.identity.x, inst->my_keys.public_key, 32); memcpy(job->sql.identity.ed, inst->my_ed25519_pubkey, 32);
        memcpy(job->sql.identity.private_key, inst->my_keys.private_key, 32);
    }
    if (inst->db_worker) {
        db_async_run(inst->db_worker, &(struct db_async_task){.name="dm-json", .mode=DB_ASYNC_READ, .work=dm_json_work,
            .data=&job->sql, .done=dm_json_done, .cleanup=dm_json_cleanup, .guard=dm_json_guard}); return;
    }
    const char* file = dm->db ? sqlite3_db_filename(dm->db, "main") : NULL;
    struct db_async_result result = {.status=DB_ASYNC_CLOSED, .sqlite_code=SQLITE_ABORT};
    snprintf(result.message, sizeof(result.message), "DM JSON has no asynchronous reader");
    if (file && !*file && dm_json_guard(job)) {
        result.sqlite_code = dm_json_work(dm->db, &job->sql); result.status = result.sqlite_code ? DB_ASYNC_SQL : DB_ASYNC_OK;
        snprintf(result.message, sizeof(result.message), "RAM DM JSON: %s", sqlite3_errstr(result.sqlite_code));
    }
    dm_json_done(job, &result); dm_json_cleanup(job);
}

/* Сообщения беседы (расшифрованные) в JSON. */
int dm_list_messages_json(struct UTUN_INSTANCE* inst, const char* conv_id, int count, int offset,
                          char* buf, size_t buf_size, size_t* out_len) {
    struct dm_state* dm = dm_of(inst); struct dm_store_identity identity = {0}; uint64_t generation = 0;
    if (inst) {
        identity.node = inst->node_id; memcpy(identity.x, inst->my_keys.public_key, 32);
        memcpy(identity.ed, inst->my_ed25519_pubkey, 32); memcpy(identity.private_key, inst->my_keys.private_key, 32);
    }
    int rc = dm_store_messages_read(dm ? dm->db : NULL, &identity, conv_id, count, offset, 1,
        &generation, buf, buf_size, out_len);
    OPENSSL_cleanse(&identity, sizeof(identity)); return rc ? -1 : 0;
}

/* ── трамплины для gui_bridge (GUI → uasync) ── */

int dm_start_node(struct UTUN_INSTANCE* inst, uint64_t peer, const char* name, const char* source,
    enum dm_mode mode, uint64_t* result_conv) {
    if (inst && db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM synchronous start_node: RW worker requires submit"); return -1;
    }
    struct dm_start_result result = {.status=-1};
    dm_start_submit(inst, &(struct dm_start_query){.peer=peer, .mode=mode, .name=name ? name : "", .source=source},
        dm_start_sync_done, &result);
    if (!result.status && result_conv) *result_conv = result.conversation;
    return result.status;
}

/* Ответ связан с запросом UI, фактические ID/поколение уже получены из committed SQL. */
static int dm_ui_current(void* arg) {
    struct dm_ui_event* event = arg;
    return event->owner && event->inst->dm == event->owner && event->owner->initialized && !event->inst->destroying &&
        event->inst->group_generation == event->group_generation;
}
static struct dm_ui_event* dm_ui_new(struct UTUN_INSTANCE* inst, void* request) {
    struct dm_state* dm = dm_of(inst); if (!dm || !dm->initialized || inst->destroying) return NULL;
    struct dm_ui_event* event = u_calloc(1, sizeof(*event));
    if (!event) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM frontend reply OOM"); return NULL; }
    event->owner = dm; event->inst = inst; event->request = request; event->group_generation = inst->group_generation;
    event->next = dm->ui_events; dm->ui_events = event; return event;
}
static void dm_ui_free(struct dm_ui_event* event) {
    if (event->owner) {
        struct dm_ui_event** slot = &event->owner->ui_events;
        while (*slot && *slot != event) slot = &(*slot)->next;
        if (*slot) *slot = event->next;
        else DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM frontend reply lost owner");
    }
    u_free(event);
}
static void dm_start_publish(void* arg, const struct dm_start_result* outcome) {
    struct dm_start_req* req = arg;
    if (req->inst && req->request_id && dm_of(req->inst) && dm_of(req->inst)->initialized && !req->inst->destroying) {
        uint8_t result[25]; memcpy(result, &req->request_id, 8); memcpy(result + 8, &outcome->conversation, 8);
        memcpy(result + 16, &outcome->generation, 8); result[24] = !outcome->status;
        chat_event_post(req->inst, CHAT_EVT_DM_START_RESULT, result, sizeof(result));
    }
    u_free(req);
}
static void dm_start_event(void* arg, const struct dm_start_result* outcome) {
    struct dm_ui_event* event = arg; struct dm_start_req* req = event->request; int current = dm_ui_current(event);
    dm_ui_free(event); if (current) dm_start_publish(req, outcome); else u_free(req);
}
void dm_start_trampoline(void* arg) {
    struct dm_start_req* req = arg; if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM start: missing UI request"); return; }
    struct dm_ui_event* event = dm_ui_new(req->inst, req);
    if (!event) { dm_start_publish(req, &(struct dm_start_result){.status=-1, .sqlite_code=SQLITE_NOMEM}); return; }
    dm_start_submit(req->inst, &(struct dm_start_query){.peer_ed=req->peer_ed, .mode=req->mode,
        .name=req->peer_name, .source=req->source_ch_id, .valid=dm_ui_current}, dm_start_event, event);
}

/* Отправить текстовое сообщение в беседу. */
static void dm_send_publish(void* arg, const struct dm_send_result* result) {
    struct dm_send_req* req = arg;
    if (req->inst && dm_of(req->inst) && dm_of(req->inst)->initialized && !req->inst->destroying)
        chat_event_submitted(req->inst, req->request_id, !result->status);
    u_free(req);
}
static void dm_send_event(void* arg, const struct dm_send_result* result) {
    struct dm_ui_event* event = arg; struct dm_send_req* req = event->request; int current = dm_ui_current(event);
    dm_ui_free(event); if (current) dm_send_publish(req, result); else u_free(req);
}
void dm_send_trampoline(void* arg) {
    struct dm_send_req* req = arg;
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM send trampoline: missing request"); return; }
    struct dm_ui_event* event = dm_ui_new(req->inst, req);
    if (!event) { dm_send_publish(req, &(struct dm_send_result){.status=-1, .sqlite_code=SQLITE_NOMEM}); return; }
    dm_send_submit(req->inst, &(struct dm_send_query){.conv=req->conv_id, .type="text", .data=req->data,
        .size=req->data_len, .generation=req->generation, .valid=dm_ui_current}, dm_send_event, event);
}

/* Публикация привязана к исходным request_id и ключу получателя. */
static void dm_invite_post(const struct dm_invite_req* req, int result) {
    uint8_t event[60]; int32_t code = result;
    memcpy(event, &req->request_id, 8); memcpy(event + 8, &code, 4); memcpy(event + 12, req->peer_ed, 32);
    memcpy(event + 44, &req->conv_id, 8); memcpy(event + 52, &req->generation, 8);
    chat_event_post(req->inst, CHAT_EVT_DM_INVITE_RESULT, event, sizeof(event));
}

/* Завершить операцию и вернуть результат с неизменяемым адресатом. */
static void dm_invite_done(struct dm_invite_req* req, int result) {
    struct dm_state* dm = dm_of(req->inst);
    int cancelled_owner = dm && dm->invite == req && result == DM_INVITE_CANCELLED;
    if (dm && dm->invite == req) dm->invite = NULL;
    DEBUG_INFO(DEBUG_CATEGORY_DM, "invite PM: finished request=%llu peer=%s ch=%s result=%d",
               (unsigned long long)req->request_id, sc_public_key_hex(req->peer_ed, (char[65]){0}),
               req->channel_id, result);
    if (dm && (dm->initialized || cancelled_owner) && !req->inst->destroying)
        dm_invite_post(req, result);
    u_free(req);
}

/* Замороженная SQL-часть живёт между фазами; worker не читает req/owner/сеть. */
struct dm_invite_sql {
    struct dm_start_write start;
    char channel[65], conv[64];
    uint64_t generation;
    int selected;
    char* link;
    uint8_t payload[DM_DATA_MAX];
    uint32_t payload_size;
};
struct dm_invite_job {
    struct dm_invite_sql sql;
    struct dm_invite_req* request;
    struct dm_state* owner; /* Слабая ссылка, quiesce отделяет до освобождения DM. */
    uint64_t group_generation;
    int cancelled;
};
static int dm_invite_async_current(void* data) {
    struct dm_invite_job* job = data; struct dm_invite_req* req = job->request;
    if (!job->owner || !job->owner->initialized || !req->inst->group_started || req->inst->destroying || req->inst->dm != job->owner ||
        job->owner->invite_job != job || job->cancelled) return 0;
    struct TOPO_GROUP* group = topo_groups_find_channel(req->inst->topo_groups, req->channel_id);
    return group && !group->stopping && group->generation == job->group_generation &&
        group_policy_can_invite(req->inst, job->sql.channel, req->inst->my_ed25519_pubkey) == 1;
}
static int dm_invite_async_guard(void* data) {
    struct dm_invite_job* job = data;
    if (!dm_invite_async_current(job)) return 0;
    job->sql.start.now = ntp_time_get_seconds(job->request->inst);
    uint64_t fallback = 0;
    dm_shared_group_id(job->owner, job->sql.start.peer, job->sql.start.peer_ed, &fallback);
    strcpy(job->sql.start.fallback_group, topo_groups_channel(job->request->inst->topo_groups, fallback)); return 1;
}
static void dm_invite_async_finish(struct dm_invite_job* job, int result) {
    struct dm_invite_req* req = job->request; struct dm_state* dm = job->owner;
    int publish = dm && dm->initialized && req->inst->group_started && req->inst->dm == dm &&
        dm->invite_job == job && !req->inst->destroying;
    if (dm && dm->invite_job == job) { dm->invite_job = NULL; dm->invite = NULL; }
    job->owner = NULL;
    DEBUG_INFO(DEBUG_CATEGORY_DM, "invite PM worker finished request=%llu peer=%s channel=%s result=%d publish=%d",
        (unsigned long long)req->request_id, sc_public_key_hex(req->peer_ed, (char[65]){0}), job->sql.channel, result, publish);
    if (publish) dm_invite_post(req, result);
    if (job->sql.link) { OPENSSL_cleanse(job->sql.link, strlen(job->sql.link)); u_free(job->sql.link); }
    OPENSSL_cleanse(&job->sql, sizeof(job->sql)); u_free(req); u_free(job);
}
static void dm_invite_async_cancel(struct dm_state* dm, int detach) {
    struct dm_invite_job* job = dm->invite_job;
    if (!job) return;
    job->cancelled = 1;
    if (detach) { dm->invite_job = NULL; dm->invite = NULL; job->owner = NULL; }
    /* Без pending запрос принадлежит SQL/send callback либо выполняющемуся callback.
     * Здесь нельзя освобождать job: даже callback CONV_UPDATED может вызвать stop. */
    if (job->request->pending) chat_invite_build_cancel(job->request->pending);
}
static int dm_invite_access_read(sqlite3* db, struct dm_invite_sql* sql, struct dm_conv* conv, int* found) {
    int allowed = group_policy_can_invite_read(db, sql->channel, sql->start.identity.ed);
    if (allowed != 1) return allowed < 0 ? -allowed : SQLITE_AUTH;
    int rc = dm_store_conv_read(db, &sql->start.identity, sql->conv, conv, found);
    if (rc || !*found) return rc;
    if (conv->closed || memcmp(conv->peer_ed25519, sql->start.peer_ed, 32)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "invite PM: closed conversation or recipient does not match conv=%s peer=%s",
                   sql->conv, sc_public_key_hex(sql->start.peer_ed, (char[65]){0}));
        return SQLITE_AUTH;
    }
    int member = 0; rc = dm_party_member_read(db, &sql->start.identity, conv->parties, sql->start.identity.node, &member);
    return rc ? rc : member ? SQLITE_OK : SQLITE_AUTH;
}
static int dm_invite_prepare_work(sqlite3* db, void* data) {
    struct dm_invite_sql* sql = &((struct dm_invite_job*)data)->sql; struct dm_conv conv = {0}; int found = 0;
    if (!sql->selected) {
        uint64_t id = dm_derive_conv_id(sql->start.identity.ed, sql->start.peer_ed);
        if (!id) return SQLITE_AUTH;
        snprintf(sql->conv, sizeof(sql->conv), "%llu", (unsigned long long)id);
    }
    int rc = dm_invite_access_read(db, sql, &conv, &found);
    if (!rc && !found) {
        if (sql->selected) rc = SQLITE_NOTFOUND;
        else {
            rc = dm_store_start(db, &sql->start);
            if (!rc && sql->start.conversation != chat_numeric_id(sql->conv)) rc = SQLITE_CONSTRAINT;
        }
    }
    if (!rc) rc = dm_history_show_write(db, sql->conv);
    int hidden = 0;
    if (!rc) rc = dm_history_state_read(db, sql->conv, &sql->generation, &hidden);
    if (!rc && hidden) rc = SQLITE_ABORT;
    OPENSSL_cleanse(&conv, sizeof(conv)); return rc;
}
static int dm_invite_metadata_work(sqlite3* db, void* data) {
    struct dm_invite_sql* sql = &((struct dm_invite_job*)data)->sql; struct dm_conv conv = {0}; int found = 0;
    int rc = dm_invite_access_read(db, sql, &conv, &found); OPENSSL_cleanse(&conv, sizeof(conv));
    if (rc) return rc;
    if (!found) return SQLITE_NOTFOUND;
    uint64_t generation; int hidden;
    rc = dm_history_state_read(db, sql->conv, &generation, &hidden);
    if (rc) return rc;
    if (hidden || generation != sql->generation) return SQLITE_ABORT;
    sqlite3_stmt* st = NULL; rc = sqlite3_prepare_v2(db, "SELECT name FROM channels WHERE channel_id=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, sql->channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const char* name = (const char*)sqlite3_column_text(st, 0); int bytes = sqlite3_column_bytes(st, 0);
        size_t link_bytes = strlen(sql->link);
        if (!name) rc = sqlite3_errcode(db) == SQLITE_NOMEM ? SQLITE_NOMEM : SQLITE_CORRUPT;
        else if (sqlite3_column_type(st, 0) != SQLITE_TEXT || bytes < 0 || memchr(name, 0, (size_t)bytes)) rc = SQLITE_CORRUPT;
        else if ((size_t)bytes + 1 + link_bytes > sizeof(sql->payload)) rc = SQLITE_TOOBIG;
        else {
            memcpy(sql->payload, name, (size_t)bytes); sql->payload[bytes] = 0;
            memcpy(sql->payload + bytes + 1, sql->link, link_bytes); sql->payload_size = (uint32_t)((size_t)bytes + 1 + link_bytes);
            rc = SQLITE_OK;
        }
    } else if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND;
    int final = sqlite3_finalize(st); if (!rc) rc = final; return rc;
}
static void dm_invite_sent(void* data, const struct dm_send_result* result) {
    struct dm_invite_job* job = data;
    dm_invite_async_finish(job, result->committed ? DM_INVITE_OK :
        job->cancelled || !dm_invite_async_current(job) ? DM_INVITE_CANCELLED : DM_INVITE_SEND_ERROR);
}
static void dm_invite_metadata_done(void* data, const struct db_async_result* result) {
    struct dm_invite_job* job = data;
    if (result->status || !dm_invite_async_current(job)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "invite PM metadata failed channel=%s status=%d rc=%d: %s", job->sql.channel,
            result->status, result->sqlite_code, result->message);
        dm_invite_async_finish(job, dm_invite_async_current(job) ? DM_INVITE_SEND_ERROR : DM_INVITE_CANCELLED); return;
    }
    dm_send_submit(job->request->inst, &(struct dm_send_query){.conv=job->sql.conv, .type="invite", .data=job->sql.payload,
        .size=job->sql.payload_size, .generation=job->sql.generation, .valid=dm_invite_async_current}, dm_invite_sent, job);
}
static void dm_invite_registered(void* data, int result, const char* link) {
    struct dm_invite_job* job = data; job->request->pending = NULL;
    if (result != CHAT_JOIN_OK || !link || !*link || !dm_invite_async_current(job)) {
        dm_invite_async_finish(job, result == CHAT_JOIN_CANCELLED || !dm_invite_async_current(job) ?
            DM_INVITE_CANCELLED : DM_INVITE_BUILD_ERROR); return;
    }
    size_t bytes = strlen(link);
    if (bytes >= DM_DATA_MAX || !(job->sql.link = u_strdup(link))) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "invite PM cannot copy registered link bytes=%zu", bytes);
        dm_invite_async_finish(job, DM_INVITE_BUILD_ERROR); return;
    }
    db_async_run(job->request->inst->db_worker, &(struct db_async_task){.name="dm-invite-metadata", .mode=DB_ASYNC_READ,
        .data=job, .work=dm_invite_metadata_work, .guard=dm_invite_async_guard, .done=dm_invite_metadata_done});
}
static void dm_invite_prepared(void* data, const struct db_async_result* result) {
    struct dm_invite_job* job = data; struct dm_invite_req* req = job->request;
    if (result->status || !dm_invite_async_current(job)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "invite PM prepare failed channel=%s status=%d rc=%d: %s", job->sql.channel,
            result->status, result->sqlite_code, result->message);
        dm_invite_async_finish(job, dm_invite_async_current(job) ? DM_INVITE_START_ERROR : DM_INVITE_CANCELLED); return;
    }
    req->conv_id = chat_numeric_id(job->sql.conv); req->generation = job->sql.generation;
    size_t length = strlen(job->sql.conv); uint8_t event[30]; event[0] = (uint8_t)length;
    memcpy(event + 1, job->sql.conv, length); memcpy(event + 1 + length, &req->generation, 8);
    chat_event_post(req->inst, CHAT_EVT_DM_CONV_UPDATED, event, (int)length + 9);
    if (!dm_invite_async_current(job)) { dm_invite_async_finish(job, DM_INVITE_CANCELLED); return; }
    req->pending = chat_invite_build_link(req->inst, topo_groups_channel_slot(req->inst->topo_groups, req->channel_id), NULL, dm_invite_registered, job);
    if (!req->pending) dm_invite_registered(job, CHAT_JOIN_ERROR, NULL);
}
static void dm_invite_async_submit(struct dm_invite_req* req) {
    struct dm_state* dm = dm_of(req->inst);
    if (req->cancel) {
        if (dm && dm->invite_job && dm->invite->request_id == req->request_id) dm_invite_async_cancel(dm, 0);
        u_free(req); return;
    }
    if (!chat_channel_valid(req->channel_id) || !memchr(req->source_ch_id, 0, sizeof(req->source_ch_id)) || !memchr(req->peer_name, 0, sizeof(req->peer_name))) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "invite PM unterminated request"); u_free(req); return;
    }
    int selected = !strncmp(req->source_ch_id, "dm:", 3);
    req->conv_id = selected ? chat_numeric_id(req->source_ch_id + 3) : 0;
    struct TOPO_GROUP* group = topo_groups_find_channel(req->inst->topo_groups, req->channel_id);
    uint8_t x25519[32];
    if (!dm || !dm->initialized || !req->request_id || !memcmp(req->peer_ed, req->inst->my_ed25519_pubkey, 32) ||
        sc_identity_public_x("DM invitation", req->peer_ed, x25519) != SC_OK ||
        (selected && !req->conv_id) || !group || group->stopping || group->group_type != TOPO_GROUP_TYPE_CHAT) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "invite PM invalid request/recipient/active group"); dm_invite_done(req, DM_INVITE_START_ERROR); return;
    }
    if (dm->invite) { DEBUG_WARN(DEBUG_CATEGORY_DM, "invite PM busy request=%llu", (unsigned long long)req->request_id);
        dm_invite_done(req, DM_INVITE_BUSY); return; }
    struct dm_invite_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "invite PM worker allocation failed"); dm_invite_done(req, DM_INVITE_START_ERROR); return; }
    job->request = req; job->owner = dm; job->group_generation = group->generation; job->sql.selected = selected;
    strcpy(job->sql.channel, group->channel_id);
    snprintf(job->sql.conv, sizeof(job->sql.conv), "%llu", (unsigned long long)req->conv_id);
    job->sql.start = (struct dm_start_write){.identity.node=req->inst->node_id, .peer=sc_derive_node_id_from_pubkey(x25519),
        .mode=DM_MODE_DEVICE, .supplied_name=1, .supplied_keys=1};
    if (!selected && chat_channel_valid(req->source_ch_id)) strcpy(job->sql.start.group, req->source_ch_id);
    memcpy(job->sql.start.peer_x, x25519, 32); memcpy(job->sql.start.peer_ed, req->peer_ed, 32);
    memcpy(job->sql.start.identity.x, req->inst->my_keys.public_key, 32); memcpy(job->sql.start.identity.ed, req->inst->my_ed25519_pubkey, 32);
    memcpy(job->sql.start.identity.private_key, req->inst->my_keys.private_key, 32);
    dm_store_copy_name(job->sql.start.name, req->peer_name, strlen(req->peer_name), job->sql.conv);
    dm->invite = req; dm->invite_job = job;
    db_async_run(req->inst->db_worker, &(struct db_async_task){.name="dm-invite-prepare", .mode=DB_ASYNC_WRITE,
        .data=job, .guard=dm_invite_async_guard, .work=dm_invite_prepare_work, .done=dm_invite_prepared});
}

/* ACK регистрации разрешает отправку ссылки; ошибка/отмена не создаёт сообщение. */
static void dm_invite_ready(void* arg, int result, const char* link) {
    struct dm_invite_req* req = arg;
    req->pending = NULL;
    if (result == CHAT_JOIN_CANCELLED) { dm_invite_done(req, DM_INVITE_CANCELLED); return; }
    if (result != CHAT_JOIN_OK || !link || !link[0]) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "invite PM: registration failed request=%llu ch=%s result=%d",
                    (unsigned long long)req->request_id, req->channel_id, result);
        dm_invite_done(req, DM_INVITE_BUILD_ERROR); return;
    }
    char conv[64];
    snprintf(conv, sizeof(conv), "%llu", (unsigned long long)req->conv_id);
    if (!dm_history_current(req->inst, conv, req->generation)) { dm_invite_done(req, DM_INVITE_CANCELLED); return; }
    size_t bytes = strlen(link);
    sqlite3_stmt* st = NULL;
    char channel[65], payload[DM_DATA_MAX];
    strcpy(channel, req->channel_id);
    struct dm_state* dm = dm_of(req->inst);
    int rc = sqlite3_prepare_v2(dm->db, "SELECT name FROM channels WHERE channel_id=?", -1, &st, NULL);
    if (rc == SQLITE_OK) { sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC); rc = sqlite3_step(st); }
    size_t name_bytes = rc == SQLITE_ROW ? (size_t)sqlite3_column_bytes(st, 0) : 0;
    if (rc != SQLITE_ROW || name_bytes + 1 + bytes > sizeof(payload)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "invite PM: group metadata failed ch=%s rc=%d name_bytes=%zu link_bytes=%zu sql=%s",
                    channel, rc, name_bytes, bytes, sqlite3_errmsg(dm->db));
        sqlite3_finalize(st); dm_invite_done(req, DM_INVITE_SEND_ERROR); return;
    }
    memcpy(payload, sqlite3_column_text(st, 0), name_bytes); payload[name_bytes] = '\0';
    memcpy(payload + name_bytes + 1, link, bytes);
    sqlite3_finalize(st);
    DEBUG_INFO(DEBUG_CATEGORY_DM, "invite PM: registered request=%llu peer=%s ch=%s name_bytes=%zu link_bytes=%zu ttl=%ds",
               (unsigned long long)req->request_id, sc_public_key_hex(req->peer_ed, (char[65]){0}),
               req->channel_id, name_bytes, bytes, JOIN_KEY_TTL_SECONDS);
    rc = dm_send(req->inst, conv, "invite", (const uint8_t*)payload, (uint32_t)(name_bytes + 1 + bytes));
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "invite PM: send failed request=%llu conv=%s rc=%d",
                        (unsigned long long)req->request_id, conv, rc);
    dm_invite_done(req, rc ? DM_INVITE_SEND_ERROR : DM_INVITE_OK);
}

/* Создать/найти PM, затем зарегистрировать свежий invite; все данные принадлежат запросу. */
void dm_invite_trampoline(void* arg) {
    struct dm_invite_req* req = arg;
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "invite PM: missing request"); return; }
    if (req->inst && db_async_can_write(req->inst->db_worker)) { dm_invite_async_submit(req); return; }
    struct dm_state* dm = dm_of(req->inst);
    if (req->cancel) {
        if (dm && dm->invite && dm->invite->request_id == req->request_id) {
            DEBUG_INFO(DEBUG_CATEGORY_DM, "invite PM: cancelling request=%llu", (unsigned long long)req->request_id);
            chat_invite_build_cancel(dm->invite->pending);
        } else DEBUG_DEBUG(DEBUG_CATEGORY_DM, "invite PM: cancel already completed request=%llu",
                           (unsigned long long)req->request_id);
        u_free(req); return;
    }
    if (!req->inst) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "invite PM: missing instance"); u_free(req); return; }
    int selected = !strncmp(req->source_ch_id, "dm:", 3);
    req->conv_id = selected ? chat_numeric_id(req->source_ch_id + 3) : 0;
    uint8_t x25519[32];
    if (!dm || !dm->initialized || !req->request_id || !memcmp(req->peer_ed, req->inst->my_ed25519_pubkey, 32) ||
        sc_identity_public_x("DM invitation", req->peer_ed, x25519) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "invite PM: invalid state/recipient request=%llu peer=%s",
                    (unsigned long long)req->request_id, sc_public_key_hex(req->peer_ed, (char[65]){0}));
        dm_invite_done(req, DM_INVITE_START_ERROR); return;
    }
    if (!selected) req->conv_id = dm_derive_conv_id(req->inst->my_ed25519_pubkey, req->peer_ed);
    if (!req->conv_id) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "invite PM: missing conversation identity peer=%s", sc_public_key_hex(req->peer_ed, (char[65]){0}));
        dm_invite_done(req, DM_INVITE_START_ERROR); return;
    }
    if (dm->invite) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "invite PM: busy request=%llu pending=%llu",
                   (unsigned long long)req->request_id, (unsigned long long)dm->invite->request_id);
        dm_invite_done(req, DM_INVITE_BUSY); return;
    }
    char channel[65], conv[64];
    strcpy(channel, req->channel_id);
    struct TOPO_GROUP* group = topo_groups_find_channel(req->inst->topo_groups, req->channel_id);
    if (!group || group->group_type != TOPO_GROUP_TYPE_CHAT ||
        !group_policy_member_key_allowed(req->inst, channel, req->inst->my_keys.public_key, req->inst->my_ed25519_pubkey, 0)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "invite PM: no active membership ch=%s", channel);
        dm_invite_done(req, DM_INVITE_BUILD_ERROR); return;
    }
    snprintf(conv, sizeof(conv), "%llu", (unsigned long long)req->conv_id);
    struct dm_conv c;
    enum dm_conv_result loaded = dm_conv_load(dm, conv, &c);
    if (loaded == DM_CONV_ERROR) { dm_invite_done(req, DM_INVITE_START_ERROR); return; }
    if ((selected && loaded != DM_CONV_FOUND) ||
        (loaded == DM_CONV_FOUND && (!dm_conv_active(dm, &c) || memcmp(c.peer_ed25519, req->peer_ed, 32)))) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "invite PM: selected conversation unavailable conv=%s", conv);
        dm_invite_done(req, DM_INVITE_START_ERROR); return;
    }
    if (loaded == DM_CONV_NOT_FOUND) {
        if (dm_start(req->inst, sc_derive_node_id_from_pubkey(x25519), x25519, req->peer_ed, req->peer_name, req->source_ch_id, DM_MODE_DEVICE, NULL) != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "invite PM: conversation failed peer=%s", sc_public_key_hex(req->peer_ed, (char[65]){0}));
            dm_invite_done(req, DM_INVITE_START_ERROR); return;
        }
    }
    if (dm_history_show(req->inst, conv)) { dm_invite_done(req, DM_INVITE_START_ERROR); return; }
    req->generation = dm_history_generation(req->inst, conv);
    if (!dm_history_current(req->inst, conv, req->generation)) { dm_invite_done(req, DM_INVITE_CANCELLED); return; }
    DEBUG_INFO(DEBUG_CATEGORY_DM, "invite PM: building request=%llu peer=%s source=%s ch=%s conv=%s",
               (unsigned long long)req->request_id, sc_public_key_hex(req->peer_ed, (char[65]){0}), req->source_ch_id, channel, conv);
    dm->invite = req;
    req->pending = chat_invite_build_link(req->inst, topo_groups_channel_slot(req->inst->topo_groups, req->channel_id), NULL, dm_invite_ready, req);
    if (!req->pending) dm_invite_ready(req, CHAT_JOIN_ERROR, NULL);
}

void dm_cancel_pending_history(struct UTUN_INSTANCE* inst, const char* conv) {
    if (!chat_numeric_id(conv)) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM history invalidation: invalid conversation ID"); return; }
    struct dm_state* dm = dm_of(inst);
    unsigned cancelled = 0;
    for (struct dm_json_job* job = dm ? dm->reads : NULL; job; job = job->next) {
        if (job->sql.conv[0] && strcmp(job->sql.conv, conv)) continue;
        job->invalidated = 1; cancelled++;
    }
    for (struct dm_members_job* job = dm ? dm->members : NULL; job; job = job->next) {
        if (strcmp(job->conv, conv)) continue;
        job->invalidated = 1; cancelled++;
    }
    for (struct dm_route_job* job = dm ? dm->routes : NULL; job; job = job->next)
        if (!strcmp(job->conv, conv)) { job->invalidated = 1; cancelled++; }
    if (cancelled) DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM history: invalidated snapshots after delete conv=%s reads=%u", conv, cancelled);
    if (dm && dm->invite && dm->invite->conv_id == strtoull(conv, NULL, 10)) {
        if (dm->invite_job) dm_invite_async_cancel(dm, 0);
        else chat_invite_build_cancel(dm->invite->pending);
    }
}

static int dm_delete_alive(struct dm_delete_job* job) {
    char root[DM_MEDIA_ROOT_SIZE];
    return job->owner && !job->inst->destroying && job->inst->dm == job->owner && job->owner->initialized &&
        job->inst->dm_media == job->media && !dm_media_root_snapshot(job->inst, root);
}
static int dm_delete_current(void* arg) {
    struct dm_delete_job* job = arg;
    return dm_delete_alive(job) && (!job->valid || job->valid(job->arg));
}
static int dm_delete_work(sqlite3* db, void* arg) {
    struct dm_delete_job* job = arg;
    return dm_history_delete_write(db, job->root, job->conv, job->generation);
}
static void dm_delete_cleanup(void* arg) {
    struct dm_delete_job* job = arg;
    if (job->owner) {
        struct dm_delete_job** slot = &job->owner->deletes;
        while (*slot && *slot != job) slot = &(*slot)->next;
        if (*slot) *slot = job->next;
        else DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM delete cleanup: lost owner conv=%s", job->conv);
    }
    u_free(job);
}
/* Инвалидация относится к фактическому commit, даже если caller отменил свой callback-результат. */
static void dm_delete_done(void* arg, const struct db_async_result* outcome) {
    struct dm_delete_job* job = arg;
    struct dm_delete_result result = {.status=-1, .sqlite_code=outcome->sqlite_code, .committed=!outcome->status};
    if (!outcome->status && dm_delete_alive(job)) {
        dm_cancel_pending_history(job->inst, job->conv);
        if (dm_delete_alive(job)) dm_media_forget_committed(job->inst, job->conv);
        if (dm_delete_current(job)) { result.status = 0; result.generation = job->generation + 1; }
        DEBUG_INFO(DEBUG_CATEGORY_DM, "DM history deleted conv=%s generation=%llu -> %llu committed=1 current=%d", job->conv,
            (unsigned long long)job->generation, (unsigned long long)(job->generation + 1), !result.status);
    }
    if (result.status) {
        if (!result.sqlite_code) result.sqlite_code = SQLITE_ABORT;
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM delete completion conv=%s generation=%llu status=%d sqlite=%d committed=%d: %s",
            job->conv, (unsigned long long)job->generation, outcome->status, result.sqlite_code, result.committed, outcome->message);
    }
    job->done(job->arg, &result);
}
void dm_delete_submit(struct UTUN_INSTANCE* inst, const struct dm_delete_query* query, dm_delete_done_fn done, void* arg) {
    struct dm_state* dm = dm_of(inst); struct dm_delete_result failed = {.status=-1, .sqlite_code=SQLITE_MISUSE};
    if (!done) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM delete: missing completion"); return; }
    if (!dm || !dm->initialized || inst->destroying || !query || !query->conv || strlen(query->conv) >= 64 ||
        !chat_numeric_id(query->conv) || !query->generation || query->generation >= INT64_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM delete: invalid owner/conversation/generation"); done(arg, &failed); return;
    }
    struct dm_delete_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM delete allocation failed"); failed.sqlite_code = SQLITE_NOMEM; done(arg, &failed); return; }
    job->inst = inst; job->owner = dm; job->media = inst->dm_media; job->done = done; job->arg = arg; job->valid = query->valid;
    job->generation = query->generation; snprintf(job->conv, sizeof(job->conv), "%s", query->conv);
    if (dm_media_root_snapshot(inst, job->root)) {
        u_free(job); failed.sqlite_code = SQLITE_ABORT; done(arg, &failed); return;
    }
    job->next = dm->deletes; dm->deletes = job;
    if (db_async_can_write(inst->db_worker)) {
        db_async_run(inst->db_worker, &(struct db_async_task){.name="dm-history-delete", .mode=DB_ASYNC_WRITE, .work=dm_delete_work,
            .data=job, .guard=dm_delete_current, .done=dm_delete_done, .cleanup=dm_delete_cleanup}); return;
    }
    sqlite3* db = dm->db; const char* file = db ? sqlite3_db_filename(db, "main") : NULL;
    int owns = db && sqlite3_get_autocommit(db) && (inst->db_worker || (file && !*file));
    int rc = !dm_delete_current(job) ? SQLITE_ABORT : owns ? sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) : SQLITE_MISUSE;
    if (!rc) rc = dm_delete_work(db, job);
    if (!rc && !dm_delete_current(job)) rc = SQLITE_ABORT;
    if (!rc) rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    struct db_async_result result = {.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK, .sqlite_code=rc};
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM delete legacy rc=%d: %s", rc, sqlite3_errstr(rc));
        if (owns && !sqlite3_get_autocommit(db)) {
            int rollback = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            if (rollback) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM delete rollback rc=%d: %s", rollback, sqlite3_errmsg(db));
            if (!sqlite3_get_autocommit(db)) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM delete left transaction open"); utun_instance_stop(inst); }
        }
    }
    dm_delete_done(job, &result); dm_delete_cleanup(job);
}
static int dm_delete_event_current(void* arg) {
    struct dm_delete_event* event = arg; struct UTUN_INSTANCE* inst = event->req->inst;
    return inst && !inst->destroying && inst->group_generation == event->group_generation &&
        event->owner && inst->dm == event->owner && event->owner->initialized;
}
static void dm_delete_event_done(void* arg, const struct dm_delete_result* result) {
    struct dm_delete_event* reply = arg; struct dm_delete_req* req = reply->req;
    int current = dm_delete_event_current(reply);
    if (reply->owner) {
        struct dm_delete_event** slot = &reply->owner->delete_events;
        while (*slot && *slot != reply) slot = &(*slot)->next;
        if (*slot) *slot = reply->next;
        else DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM delete result: lost owner request=%llu", (unsigned long long)req->request_id);
        reply->owner = NULL;
    }
    if (current) {
        uint64_t conv = strtoull(req->conv_id, NULL, 10); uint8_t event[25];
        memcpy(event, &req->request_id, 8); memcpy(event + 8, &conv, 8);
        memcpy(event + 16, &req->generation, 8); event[24] = !result->status;
        DEBUG_INFO(DEBUG_CATEGORY_DM, "DM delete result request=%llu conv=%s generation=%llu success=%d",
            (unsigned long long)req->request_id, req->conv_id, (unsigned long long)req->generation, !result->status);
        chat_event_post(req->inst, CHAT_EVT_DM_DELETE_RESULT, event, sizeof(event));
    }
    else DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM delete result suppressed after stop/restart request=%llu conv=%s",
        (unsigned long long)req->request_id, req->conv_id);
    u_free(req); u_free(reply);
}
void dm_delete_trampoline(void* arg) {
    struct dm_delete_req* req = arg;
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM delete: missing request"); return; }
    struct dm_delete_event* event = u_calloc(1, sizeof(*event));
    if (!event) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM delete result context allocation failed");
        uint64_t conv = strtoull(req->conv_id, NULL, 10); uint8_t failed[25];
        memcpy(failed, &req->request_id, 8); memcpy(failed + 8, &conv, 8); memcpy(failed + 16, &req->generation, 8); failed[24] = 0;
        if (req->inst && req->inst->dm && !req->inst->destroying) chat_event_post(req->inst, CHAT_EVT_DM_DELETE_RESULT, failed, sizeof(failed));
        u_free(req); return;
    }
    event->req = req; event->owner = dm_of(req->inst); event->group_generation = req->inst ? req->inst->group_generation : 0;
    if (event->owner) { event->next = event->owner->delete_events; event->owner->delete_events = event; }
    dm_delete_submit(req->inst, &(struct dm_delete_query){.conv=req->conv_id, .generation=req->generation,
        .valid=dm_delete_event_current}, dm_delete_event_done, event);
}

static int dm_route_current(void* data) {
    struct dm_route_job* job = data;
    return job->owner && job->owner->initialized && job->inst->dm == job->owner && !job->inst->destroying && !job->invalidated;
}
static int dm_route_work(sqlite3* db, void* data) {
    struct dm_route_job* job = data; uint64_t generation; int hidden;
    int rc = dm_history_state_read(db, job->conv, &generation, &hidden);
    if (rc) return rc;
    if (hidden || (job->generation && job->generation != generation)) return SQLITE_ABORT;
    struct dm_conv conv = {0}; int found = 0;
    rc = dm_store_conv_read(db, &job->identity, job->conv, &conv, &found);
    if (!rc && (!found || conv.closed)) rc = found ? SQLITE_AUTH : SQLITE_NOTFOUND;
    int member = 0;
    if (!rc) rc = dm_party_member_read(db, &job->identity, conv.parties, job->identity.node, &member);
    if (!rc && !member) rc = SQLITE_AUTH;
    if (!rc) rc = dm_identity_targets_read(db, &job->identity, conv.parties, job->targets, DM_TARGET_MAX, &job->count);
    if (!rc) { job->result.generation = generation; job->result.peer_group = conv.parties[1].kind == DM_PARTY_GROUP; strcpy(job->preferred, conv.group_id); }
    OPENSSL_cleanse(&conv, sizeof(conv)); return rc;
}
static void dm_route_done(void* data, const struct db_async_result* outcome) {
    struct dm_route_job* job = data; struct dm_call_route_result result = {.status=-1, .sqlite_code=outcome->sqlite_code};
    if (!outcome->status && dm_route_current(job)) {
        result = job->result;
        for (size_t i = 0; i < job->count; i++) {
            if (job->targets[i].side != 1 || !memcmp(job->targets[i].ed, job->inst->my_ed25519_pubkey, 32)) continue;
            memcpy(result.peer_ed, job->targets[i].ed, 32);
            if (dm_find_route_group(job->inst, 0, result.peer_ed, topo_groups_channel_slot(job->inst->topo_groups, job->preferred), &result.group, 1, ETCP_RT_ID_DM)) {
                result.available = 1; break;
            }
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "PM route READ complete conv=%s generation=%llu devices=%zu peer=%s group=%llu available=%d",
            job->conv, (unsigned long long)result.generation, job->count, sc_public_key_hex(result.peer_ed, (char[65]){0}),
            (unsigned long long)result.group, result.available);
    } else {
        result.cancelled = !dm_route_current(job) || result.sqlite_code == SQLITE_ABORT;
        if (!result.sqlite_code) result.sqlite_code = result.cancelled ? SQLITE_ABORT : SQLITE_ERROR;
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "PM route READ failed conv=%s status=%d rc=%d current=%d: %s", job->conv,
            outcome->status, result.sqlite_code, dm_route_current(job), outcome->message);
    }
    job->done(job->arg, &result);
}
static void dm_route_free(void* data) {
    struct dm_route_job* job = data;
    if (job->owner) {
        struct dm_route_job** slot = &job->owner->routes;
        while (*slot && *slot != job) slot = &(*slot)->next;
        if (*slot) *slot = job->next;
        else DEBUG_ERROR(DEBUG_CATEGORY_CALL, "PM route lost weak owner conv=%s", job->conv);
    }
    OPENSSL_cleanse(&job->identity, sizeof(job->identity)); u_free(job);
}
void dm_call_route_submit(struct UTUN_INSTANCE* inst, const char* conv, uint64_t generation, dm_call_route_done_fn done, void* arg) {
    struct dm_state* dm = dm_of(inst); struct dm_call_route_result failure = {.status=-1, .sqlite_code=SQLITE_MISUSE};
    if (!done) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "PM route requires completion"); return; }
    if (!dm || !dm->initialized || inst->destroying || !chat_numeric_id(conv)) {
        failure.cancelled = !dm || !dm->initialized || inst->destroying;
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "PM route invalid conversation/stopped owner"); done(arg, &failure); return;
    }
    struct dm_route_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "PM route allocation failed conv=%s", conv); failure.sqlite_code = SQLITE_NOMEM; done(arg, &failure); return; }
    job->inst = inst; job->owner = dm; job->done = done; job->arg = arg; job->generation = generation;
    snprintf(job->conv, sizeof(job->conv), "%s", conv); job->identity.node = inst->node_id;
    memcpy(job->identity.x, inst->my_keys.public_key, 32); memcpy(job->identity.ed, inst->my_ed25519_pubkey, 32);
    memcpy(job->identity.private_key, inst->my_keys.private_key, 32);
    job->next = dm->routes; dm->routes = job;
    if (db_async_can_write(inst->db_worker)) db_async_run(inst->db_worker, &(struct db_async_task){.name="dm-call-route", .mode=DB_ASYNC_READ,
        .data=job, .guard=dm_route_current, .work=dm_route_work, .done=dm_route_done, .cleanup=dm_route_free});
    else {
        int rc = dm->db ? dm_route_work(dm->db, job) : SQLITE_MISUSE;
        const struct db_async_result result = {.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK, .sqlite_code=rc};
        dm_route_done(job, &result); dm_route_free(job);
    }
}

static void dm_call_route_event(void* data, const struct dm_call_route_result* result) {
    struct dm_messages_req* req = data;
    if (req->inst->destroying || result->cancelled || result->sqlite_code == SQLITE_ABORT) { u_free(req); return; }
    struct TOPO_GROUP* group = result->group ? topo_groups_find(req->inst->topo_groups, result->group) : NULL;
    const uint8_t* key = topo_group_wire_id(group);
    size_t len = strlen(req->conv_id); uint8_t event[1 + sizeof(req->conv_id) + 64]; event[0] = (uint8_t)len;
    memcpy(event + 1, req->conv_id, len); memcpy(event + 1 + len, result->peer_ed, 32);
    memcpy(event + 33 + len, key ? key : (const uint8_t[32]){0}, 32);
    chat_event_post(req->inst, CHAT_EVT_DM_CALL_ROUTE, event, (int)len + 65); u_free(req);
}

void dm_call_route_trampoline(void* arg) {
    struct dm_messages_req* req = arg;
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "PM route: empty request"); return; }
    if (!memchr(req->conv_id, 0, sizeof(req->conv_id))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "PM route: unterminated conversation"); u_free(req); return;
    }
    dm_call_route_submit(req->inst, req->conv_id, req->generation, dm_call_route_event, req);
}

/* Публикация результата READ в старом формате события; req живёт до callback, SQL в loop нет. */
static void messages_event(void* arg, const struct dm_json_result* result) {
    struct dm_messages_req* req = arg;
    if (!result->status) {
        size_t cl = strlen(req->conv_id); uint8_t* event = u_malloc(9 + cl + result->length);
        if (!event) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM history event allocation failed conv=%s bytes=%zu", req->conv_id, result->length);
        else {
            event[0] = (uint8_t)cl; memcpy(event + 1, req->conv_id, cl); memcpy(event + 1 + cl, &result->generation, 8);
            memcpy(event + 9 + cl, result->json, result->length);
            chat_event_post(req->inst, CHAT_EVT_DM_MESSAGES, event, (int)(9 + cl + result->length)); u_free(event);
        }
    }
    u_free(req);
}

void dm_messages_trampoline(void* arg) {
    struct dm_messages_req* req = arg;
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "%s: missing history request", DM_ID); return; }
    dm_json_read(req->inst, &(struct dm_json_query){.conv=req->conv_id, .generation=req->generation, .capacity=1024 * 1024},
        messages_event, req);
}

static void conversations_event(void* arg, const struct dm_json_result* result) {
    if (!result->status) chat_event_post(arg, CHAT_EVT_DM_CONVERSATIONS, (const uint8_t*)result->json, (int)result->length);
}

void dm_conversations_trampoline(void* arg) {
    struct dm_conversations_req* req = arg;
    if (!req) return;
    struct UTUN_INSTANCE* inst = req->inst;
    u_free(req);
    dm_json_read(inst, &(struct dm_json_query){.capacity=256 * 1024}, conversations_event, inst);
}
