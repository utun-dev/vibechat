/* Сверка проверенных голов через Merkle; блоки передаются непрерывными ограниченными хвостами.
 * Непроверенные заявления головы живут только в staging текущего pull. */
#include "chat_log_priv.h"
#include "../db_changes.h"

#include "chat_event.h"
#include "../group/group_protocol.h"
#include "../routing_layer/topo_group.h"
#include "../transport_layer/etcp_connections.h"
#include "../transport_layer/etcp.h"
#include "../../lib/db_async.h"
#include <openssl/rand.h>
#include <limits.h>

/* Запросы Merkle: тип, субъект (автор или срез), далее числа BE и хеши.
 * Ответ начинается с того же типа. Объём/идентичность запроса хранятся в сессии Merkle. */
enum { CL_FETCH_TAIL = 1, CL_FETCH_PROOF, CL_FETCH_SEAL };
enum { CL_SUBJECT = 33, CL_TAIL_QUERY_SIZE = 89, CL_PROOF_QUERY_SIZE = 105, CL_SEAL_QUERY_SIZE = 73 };
typedef char jl_proof_fits_merkle[(MS_HEADER + 1 + 4 + 2 * MEMBER_LOG_WIRE_MAX <= MT_PAGE_SIZE) ? 1 : -1];
#define JL_STAGE_LIMIT (32u * 1024u * 1024u)
struct jl_pull {
    char group[65];
    uint8_t id[16]; /* Только ключ временных SQL-строк. */
    struct merkle_sync_dependency fetch;
    size_t staged_bytes; /* Тела, ключи, поля головы и резерв курсоров срезов. */
    int changed;
};
static int eligible(void* ctx, const char* group, struct ETCP_CONN* conn) {
    struct UTUN_INSTANCE* inst = ctx;
    if (!inst || !inst->chat_content_started || inst->destroying) return 0;
    struct TOPO_GROUP* topology = inst->topo_groups ? topo_groups_find_channel(inst->topo_groups, group) : NULL;
    return group_protocol_current(inst, group) == GROUP_PROTOCOL_CHAT && topo_group_connection_ready(topology, conn) && conn && conn->initialized &&
           conn->links_up && !conn->close_requested && topo_group_chat_connection_allowed(topology, conn);
}
static int leaf(void* ctx, const char* group, uint8_t level, uint64_t prefix, EVP_MD_CTX* hash) {
    return level == MT_MAX_LEVEL ? cl_leaf(((struct UTUN_INSTANCE*)ctx)->topo_sqlite_db, group, prefix, hash) : -SQLITE_MISUSE;
}
static int jl_page_read(sqlite3* db, const char* group, uint64_t prefix, int av, const merkle_key_t* after, uint8_t* out, size_t* len,
                        merkle_key_t* next, int* more) {
    sqlite3_stmt* st = NULL;
    size_t used = 0;
    *more = 0;
    memset(next, 0, sizeof(*next));
    int rc = sqlite3_prepare_v2(db, "SELECT id,wire FROM chat_heads WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND bucket=?2 AND (?3=0 OR id>?4) ORDER BY id", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int64(st, 2, (sqlite3_int64)(prefix >> 39));
    if (!rc) rc = sqlite3_bind_int(st, 3, av);
    if (!rc) rc = sqlite3_bind_blob(st, 4, after->bytes, 32, SQLITE_STATIC);
    if (!rc)
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            size_t n = (size_t)sqlite3_column_bytes(st, 1);
            if (n != CL_HEAD_SIZE && n != CL_SEAL_SIZE) {
                rc = SQLITE_CORRUPT;
                break;
            }
            if (n + 2 > *len - used) {
                *more = 1;
                rc = used ? SQLITE_DONE : SQLITE_TOOBIG;
                break;
            }
            out[used++] = (uint8_t)(n >> 8);
            out[used++] = (uint8_t)n;
            memcpy(out + used, sqlite3_column_blob(st, 1), n);
            used += n;
            memcpy(next->bytes, sqlite3_column_blob(st, 0), 32);
        }
    int code = cl_finish(db, st, rc, "head page");
    if (!code) *len = used;
    return code;
}
static int page(void* ctx, const char* group, uint64_t prefix, int av, const merkle_key_t* after, uint8_t* out, size_t* len, merkle_key_t* next,
                int* more) {
    return jl_page_read(((struct UTUN_INSTANCE*)ctx)->topo_sqlite_db, group, prefix, av, after, out, len, next, more);
}
int cl_stage_tables(sqlite3* db) {
    int code = cl_exec(db, "CREATE TEMP TABLE IF NOT EXISTS chat_head_stage(pull BLOB,id BLOB,wire BLOB,checked INTEGER NOT NULL DEFAULT 0,"
                           "scan INTEGER NOT NULL DEFAULT 0,"
                           "PRIMARY KEY(pull,id)) WITHOUT ROWID");
    if (!code)
        code = cl_exec(db,
                       "CREATE TEMP TABLE IF NOT EXISTS chat_seal_stage(pull BLOB,id BLOB,seq INTEGER,hash BLOB,PRIMARY KEY(pull,id)) WITHOUT ROWID");
    return code;
}
static int jl_begin_write(sqlite3* db, const char* group, void** out) {
    *out = NULL;
    struct jl_pull* p = u_calloc(1, sizeof(*p));
    if (!p) return cl_error(db, "pull allocation", SQLITE_NOMEM);
    if (!chat_channel_valid(group) || RAND_bytes(p->id, 16) != 1) {
        u_free(p);
        return cl_error(db, "pull token", SQLITE_ERROR);
    }
    strcpy(p->group, group);
    int code = cl_stage_tables(db);
    if (code) {
        u_free(p);
        return code;
    }
    *out = p;
    return 0;
}
static int begin(void* ctx, const char* group, const uint8_t peer[32], uint64_t round, void** out) {
    (void)peer; (void)round;
    return jl_begin_write(((struct UTUN_INSTANCE*)ctx)->topo_sqlite_db, group, out);
}
static int jl_bad_page(sqlite3* db, const char* group, const char* reason, const uint8_t* author) {
    db_changes_warn(db, NULL, group, "Chat journal page", reason, author, NULL, NULL);
    return cl_error(db, reason, SQLITE_MISMATCH);
}
static int jl_stage_write(sqlite3* db, struct jl_pull* p, uint64_t prefix, int av, const merkle_key_t* after, const merkle_key_t* next, int more,
                          const uint8_t* data, size_t len, int* result) {
    *result = MT_ERR_DATA;
    uint8_t previous[32] = {0};
    if (av) memcpy(previous, after->bytes, 32);
    size_t off = 0, staged = p->staged_bytes;
    unsigned count = 0;
    while (off < len) {
        if (len - off < 2) return jl_bad_page(db, p->group, "invalid head length", NULL);
        size_t n = (size_t)data[off] * 256 + data[off + 1];
        off += 2;
        uint8_t key[32];
        if (n > len - off || cl_inventory_key(data + off, n, key) || merkle_sync_level_prefix(cl_read64(key), MT_MAX_LEVEL) != prefix ||
            ((count || av) && memcmp(key, previous, 32) <= 0))
            return jl_bad_page(db, p->group, "invalid head or page position", NULL);
        size_t charge = n + sizeof(p->id) + sizeof(key) + 2 * sizeof(uint64_t);
        /* Один временный курсор на срез: pull, id, seq и hash. */
        if (n == CL_SEAL_SIZE) charge += sizeof(p->id) + sizeof(key) + sizeof(uint64_t) + MT_HASH_SIZE;
        if (charge > JL_STAGE_LIMIT - staged) {
            DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "journal staging limit ch=%s bytes=%zu next=%zu max=%u",
                p->group, staged, charge, JL_STAGE_LIMIT);
            *result = MT_ERR_LIMIT;
            return -SQLITE_TOOBIG;
        }
        if (n == CL_HEAD_SIZE) {
            const uint8_t* head = data + off;
            const uint8_t zero[64] = {0};
            const uint8_t* hashes = head + CL_HEAD_FAULT_HASHES;
            uint64_t seq = cl_read64(head + CL_HEAD_SEQ), fault = cl_read64(head + CL_HEAD_FAULT);
            if (!memcmp(head + 1, zero, 32) || seq > INT64_MAX || fault > INT64_MAX ||
                (fault && (seq >= fault || memcmp(hashes, hashes + 32, 32) >= 0)) ||
                (!fault && memcmp(hashes, zero, 64)) || (!seq && memcmp(head + CL_HEAD_HASH, zero, 32))) {
                DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "journal invalid head ch=%s member=%s seq=%llu fault=%llu",
                    p->group, sc_public_key_hex(head + 1, (char[65]){0}), (unsigned long long)seq, (unsigned long long)fault);
                return jl_bad_page(db, p->group, "inconsistent head or fork fields", head + 1);
            }
        }
        sqlite3_stmt* st = NULL;
        int rc = sqlite3_prepare_v2(db, "INSERT INTO chat_head_stage(pull,id,wire) VALUES(?1,?2,?3) ON CONFLICT DO NOTHING", -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_blob(st, 1, p->id, 16, SQLITE_STATIC);
        if (!rc) rc = sqlite3_bind_blob(st, 2, key, 32, SQLITE_STATIC);
        if (!rc) rc = sqlite3_bind_blob(st, 3, data + off, (int)n, SQLITE_STATIC);
        if (!rc) rc = sqlite3_step(st);
        int code = cl_finish(db, st, rc, "stage head");
        if (code) { *result = MT_ERR_IO; return code; }
        if (sqlite3_changes(db)) staged += charge;
        memcpy(previous, key, 32);
        off += n;
        count++;
    }
    if (memcmp(previous, next->bytes, 32) || (!count && more)) return jl_bad_page(db, p->group, "invalid head page cursor", NULL);
    p->staged_bytes = staged;
    *result = MT_OK; return 0;
}
static int stage(void* ctx, void* p, uint64_t prefix, int av, const merkle_key_t* after, const merkle_key_t* next, int more, const uint8_t* data,
                 size_t len) {
    sqlite3* db = ((struct UTUN_INSTANCE*)ctx)->topo_sqlite_db;
    if (cl_exec(db, "BEGIN IMMEDIATE")) return MT_ERR_IO;
    int result;
    int code = jl_stage_write(db, p, prefix, av, after, next, more, data, len, &result);
    if (!code) {
        code = cl_exec(db, "COMMIT");
        if (code) result = MT_ERR_IO;
    }
    if (code && !sqlite3_get_autocommit(db) && cl_exec(db, "ROLLBACK")) return MT_ERR_IO;
    return result;
}
static uint8_t* jl_fetch(struct jl_pull* p, uint8_t kind, size_t size, const uint8_t subject[32]) {
    p->fetch.length = size;
    p->fetch.data[0] = kind;
    memcpy(p->fetch.data + 1, subject, 32);
    return p->fetch.data + CL_SUBJECT;
}
static int jl_commit_write(sqlite3* db, struct jl_pull* p) {
    p->fetch.length = 0;
    p->changed = 0;
    sqlite3_stmt* st = NULL;
    int new_seal = 0, new_anchor = 0;
    int rc = sqlite3_prepare_v2(db, "SELECT wire,checked,scan FROM chat_head_stage WHERE pull=? ORDER BY substr(wire,1,1) DESC,id", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, p->id, 16, SQLITE_STATIC);
    if (!rc)
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            const uint8_t* w = sqlite3_column_blob(st, 0);
            size_t n = (size_t)sqlite3_column_bytes(st, 0);
            int code = 0;
            if (n == CL_SEAL_SIZE) {
                int inserted = cl_store_seal(db, p->group, w, n);
                if (inserted < 0) { rc = -inserted; break; }
                new_seal |= inserted;
                if (inserted) {
                    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "journal stored seal ch=%s member=%s cut=%llu", p->group,
                        sc_public_key_hex(w + CL_SEAL_MEMBER, (char[65]){0}), (unsigned long long)cl_read64(w + CL_SEAL_SEQ));
                }
            } else {
                const uint8_t* member = w + 1;
                uint64_t seq = cl_read64(w + CL_HEAD_SEQ), fault = cl_read64(w + CL_HEAD_FAULT), ours = 0;
                struct member_log_head head;
                code = cl_valid_head(db, p->group, member, &head, &ours);
                if (code) {
                    rc = -code;
                    break;
                }
                int fetch_proof = fault && (!ours || fault < ours);
                if (fault && fault == ours) {
                    uint8_t local[64];
                    if (member_log_fault_hashes(db, p->group, member, &ours, local) != 1) {
                        rc = SQLITE_CORRUPT;
                        break;
                    }
                    const uint8_t* remote = w + CL_HEAD_FAULT_HASHES;
                    /* Первый удалённый хеш, который ещё может войти в локальную пару. */
                    const uint8_t* candidate = !memcmp(remote, local, 32) ? remote + 32 : remote;
                    fetch_proof = memcmp(candidate, local + 32, 32) < 0;
                }
                if (fetch_proof) {
                    uint8_t* args = jl_fetch(p, CL_FETCH_PROOF, CL_PROOF_QUERY_SIZE, member);
                    cl_write64(args, fault);
                    memcpy(args + 8, w + CL_HEAD_FAULT_HASHES, 64);
                    break;
                }
                /* При компрометации сравниваем только разрешённый префикс. Другой узел также получит proof через обратный проход. */
                uint64_t upto = ours && seq >= ours ? ours - 1 : seq;
                int checkpoint = seq == upto ? member_log_checkpoint(db, p->group, member, seq, w + CL_HEAD_HASH) : head.seq >= upto;
                if (checkpoint < 0) {
                    rc = -checkpoint;
                    break;
                }
                if (!checkpoint && upto) {
                    int scan = sqlite3_column_int(st, 2);
                    uint64_t from = !scan && head.seq < upto ? head.seq + 1 : (uint64_t)sqlite3_column_int64(st, 1) + 1;
                    if (from > upto) {
                        rc = SQLITE_MISMATCH;
                        break;
                    }
                    uint8_t* args = jl_fetch(p, CL_FETCH_TAIL, CL_TAIL_QUERY_SIZE, member);
                    cl_write64(args, from);
                    cl_write64(args + 8, upto);
                    cl_write64(args + 16, seq);
                    memcpy(args + 24, w + CL_HEAD_HASH, 32);
                    break;
                }
            }
            if (code) {
                rc = -code;
                break;
            }
        }
    if (p->fetch.length && rc == SQLITE_ROW) rc = SQLITE_DONE;
    int code = cl_finish(db, st, rc, "verify advertised heads");
    if (!code && !p->fetch.length) {
        rc = sqlite3_prepare_v2(db, "SELECT id,wire FROM chat_head_stage WHERE pull=? AND substr(wire,1,1)=x'02'", -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_blob(st, 1, p->id, 16, SQLITE_STATIC);
        if (!rc)
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                const uint8_t* id = sqlite3_column_blob(st, 0);
                const uint8_t* w = sqlite3_column_blob(st, 1);
                const uint8_t* member = w + CL_SEAL_MEMBER;
                uint64_t cut = cl_read64(w + CL_SEAL_SEQ), fault = 0;
                struct member_log_head head;
                code = cl_valid_head(db, p->group, member, &head, &fault);
                if (code) {
                    rc = -code;
                    break;
                }
                int valid = cl_seal_checkpoint(db, p->group, id, w);
                if (valid < 0) {
                    rc = -valid;
                    break;
                }
                if (valid || !fault || cut < fault || head.seq != fault - 1) continue;
                sqlite3_stmt* cursor = NULL;
                int cr = sqlite3_prepare_v2(db, "INSERT INTO chat_seal_stage VALUES(?1,?2,?3,?4) ON CONFLICT DO NOTHING", -1, &cursor, NULL);
                if (!cr) cr = sqlite3_bind_blob(cursor, 1, p->id, 16, SQLITE_STATIC);
                if (!cr) cr = sqlite3_bind_blob(cursor, 2, id, 32, SQLITE_STATIC);
                if (!cr) cr = sqlite3_bind_int64(cursor, 3, (sqlite3_int64)cut);
                if (!cr) cr = sqlite3_bind_blob(cursor, 4, w + CL_SEAL_HASH, 32, SQLITE_STATIC);
                if (!cr) cr = sqlite3_step(cursor);
                code = cl_finish(db, cursor, cr, "seal ancestry cursor");
                if (code) {
                    rc = -code;
                    break;
                }
                cr = sqlite3_prepare_v2(db, "SELECT seq,hash FROM chat_seal_stage WHERE pull=? AND id=?", -1, &cursor, NULL);
                if (!cr) cr = sqlite3_bind_blob(cursor, 1, p->id, 16, SQLITE_STATIC);
                if (!cr) cr = sqlite3_bind_blob(cursor, 2, id, 32, SQLITE_STATIC);
                if (!cr) cr = sqlite3_step(cursor);
                if (cr != SQLITE_ROW) {
                    sqlite3_finalize(cursor);
                    rc = cr;
                    break;
                }
                uint64_t seq = (uint64_t)sqlite3_column_int64(cursor, 0);
                uint8_t hash[32];
                memcpy(hash, sqlite3_column_blob(cursor, 1), 32);
                int cf = sqlite3_finalize(cursor);
                if (cf) {
                    rc = cf;
                    break;
                }
                if (seq == head.seq) {
                    int same = member_log_checkpoint(db, p->group, member, seq, hash);
                    if (same != 1) {
                        rc = same < 0 ? -same : SQLITE_AUTH;
                        break;
                    }
                    cr = sqlite3_prepare_v2(db, "INSERT INTO chat_seal_anchors VALUES((SELECT ed25519_pubkey FROM channels WHERE channel_id=?1),?2,?3,?4) ON CONFLICT DO NOTHING", -1, &cursor, NULL);
                    if (!cr) cr = sqlite3_bind_text(cursor, 1, p->group, -1, SQLITE_STATIC);
                    if (!cr) cr = sqlite3_bind_blob(cursor, 2, id, 32, SQLITE_STATIC);
                    if (!cr) cr = sqlite3_bind_int64(cursor, 3, (sqlite3_int64)seq);
                    if (!cr) cr = sqlite3_bind_blob(cursor, 4, hash, 32, SQLITE_STATIC);
                    if (!cr) cr = sqlite3_step(cursor);
                    if (cr == SQLITE_DONE) new_anchor |= sqlite3_changes(db);
                    code = cl_finish(db, cursor, cr, "confirmed seal ancestry");
                    if (code) {
                        rc = -code;
                        break;
                    }
                    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "historical seal linked to safe prefix ch=%s member=%s cut=%llu prefix=%llu", p->group,
                               sc_public_key_hex(member, (char[65]){0}), (unsigned long long)cut, (unsigned long long)seq);
                } else if (seq > head.seq) {
                    uint8_t* args = jl_fetch(p, CL_FETCH_SEAL, CL_SEAL_QUERY_SIZE, id);
                    cl_write64(args, seq);
                    memcpy(args + 8, hash, 32);
                    break;
                } else {
                    rc = SQLITE_CORRUPT;
                    break;
                }
            }
        if (p->fetch.length && rc == SQLITE_ROW) rc = SQLITE_DONE;
        code = cl_finish(db, st, rc, "closed checkpoint ancestry");
    }
    if (!code && (new_seal || new_anchor)) {
        code = cl_rebuild_derived(db, p->group, new_anchor ? "confirmed seal ancestry" : "new seal");
        p->changed = !code;
    }
    return code;
}
static int commit(void* ctx, void* p) {
    struct UTUN_INSTANCE* inst = ctx;
    sqlite3* db = inst->topo_sqlite_db;
    int code = cl_exec(db, "BEGIN IMMEDIATE");
    if (!code) code = jl_commit_write(db, p);
    if (!code) code = cl_exec(db, "COMMIT");
    if (code && !sqlite3_get_autocommit(db)) cl_exec(db, "ROLLBACK");
    return code ? MT_ERR_DATA : ((struct jl_pull*)p)->fetch.length ? MT_APPLY_PENDING : MT_OK;
}
static void publish(void* ctx, void* data) {
    struct jl_pull* p = data;
    char channel[65];
    if (p->changed) { strcpy(channel, p->group); chat_log_publish(ctx, channel); }
}
static int jl_dependency(const void* data, struct merkle_sync_dependency* request) {
    *request = ((const struct jl_pull*)data)->fetch;
    return 0;
}
static int jl_abort_write(sqlite3* db, struct jl_pull* p) {
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "DELETE FROM chat_head_stage WHERE pull=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, p->id, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int code = cl_finish(db, st, rc, "abort head pull");
    if (code) return code;
    rc = sqlite3_prepare_v2(db, "DELETE FROM chat_seal_stage WHERE pull=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, p->id, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    return cl_finish(db, st, rc, "abort seal proof");
}
static void abort_pull(void* ctx, void* p) {
    jl_abort_write(((struct UTUN_INSTANCE*)ctx)->topo_sqlite_db, p);
    u_free(p);
}
static void result(void* ctx, const char* group, const uint8_t peer[32], int status) {
    if (status == MT_OK) {
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "CHAT journal synchronized ch=%s peer=%s", group, sc_public_key_hex(peer, (char[65]){0}));
        cl_effects_start(ctx, group);
        db_sync_journal_synchronized(ctx, group, peer);
    } else
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "CHAT journal sync failed ch=%s peer=%s result=%d", group, sc_public_key_hex(peer, (char[65]){0}), status);
}
static int jl_finish_read(sqlite3* db, const char* group) {
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT s.id FROM chat_seals s LEFT JOIN chat_heads h ON h.group_id=s.group_id AND h.id=s.id"
        " WHERE s.group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?) AND h.id IS NULL LIMIT 1", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const uint8_t* id = sqlite3_column_blob(st, 0);
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "journal completion deferred ch=%s seal=%s: checkpoint unresolved", group,
            id && sqlite3_column_bytes(st, 0) == 32 ? sc_public_key_hex(id, (char[65]){0}) : "?");
        rc = SQLITE_CONSTRAINT;
    }
    return cl_finish(db, st, rc, "journal finish");
}
static int finish(void* ctx, const char* group) {
    return jl_finish_read(((struct UTUN_INSTANCE*)ctx)->topo_sqlite_db, group);
}
struct jl_sql_scope {
    uint64_t generation;
};
static int jl_dependency_sql(sqlite3* db, struct merkle_sync_sql_request* r);
static int jl_dependency_work(void* ctx, struct merkle_sync_sql_request* r);
static void jl_dependency_committed(void* ctx, const struct merkle_sync_sql_request* r);
static int jl_sql_snapshot(void* ctx, const char* group, void** out) {
    struct UTUN_INSTANCE* inst = ctx;
    *out = NULL;
    if (!inst->chat_content_started || group_protocol_current(inst, group) != GROUP_PROTOCOL_CHAT) return -1;
    struct jl_sql_scope* s = u_calloc(1, sizeof(*s));
    if (!s) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "journal snapshot allocation");
        return -1;
    }
    s->generation = chat_core_generation(inst);
    *out = s;
    return 0;
}
static int jl_sql_work(sqlite3* db, void* data, struct merkle_sync_sql_request* r) {
    (void)data;
    int code = 0;
    switch (r->action) {
    case MT_SQL_PAGE:
        r->length = r->capacity;
        code = jl_page_read(db, r->ns, r->prefix, r->after_valid, &r->after, r->data, &r->length, &r->next, &r->more);
        break;
    case MT_SQL_BEGIN_PULL:
        code = jl_begin_write(db, r->ns, &r->pull);
        break;
    case MT_SQL_STAGE_PAGE:
        code = jl_stage_write(db, r->pull, r->prefix, r->after_valid, &r->after, &r->next, r->more, r->data, r->length, &r->result);
        return code < 0 ? -code : code;
    case MT_SQL_COMMIT_PULL:
        code = jl_commit_write(db, r->pull);
        break;
    case MT_SQL_ABORT_PULL:
        code = jl_abort_write(db, r->pull);
        break;
    case MT_SQL_FINISH:
        code = jl_finish_read(db, r->ns);
        break;
    case MT_SQL_SERVE_DEPENDENCY:
    case MT_SQL_APPLY_DEPENDENCY:
        return jl_dependency_sql(db, r);
    default:
        code = cl_error(db, "unknown journal SQL action", SQLITE_MISUSE);
        break;
    }
    if (code && (r->action == MT_SQL_PAGE || r->action == MT_SQL_BEGIN_PULL)) r->result = MT_ERR_IO;
    else r->result = code ? MT_ERR_DATA : r->action == MT_SQL_COMMIT_PULL && ((struct jl_pull*)r->pull)->fetch.length ? MT_APPLY_PENDING : MT_OK;
    return code < 0 ? -code : code;
}
static void jl_sql_publish(void* ctx, void* data, struct merkle_sync_sql_request* r) {
    struct jl_sql_scope* scope = data;
    struct UTUN_INSTANCE* inst = ctx;
    if (!inst->chat_content_started || inst->destroying || chat_core_generation(inst) != scope->generation) return;
    if (r->action == MT_SQL_COMMIT_PULL) publish(ctx, r->pull);
    if (r->action == MT_SQL_APPLY_DEPENDENCY) jl_dependency_committed(ctx, r);
}
static void free_data(void* data) {
    u_free(data);
}
static const struct merkle_sync_sql_ops sql_ops = {
    .snapshot = jl_sql_snapshot, .work = jl_sql_work, .publish = jl_sql_publish, .free = free_data, .pull_free = free_data};
static const struct merkle_sync_data_ops ops = {.group_scoped = 1, .update_bucket_hash = leaf,
                                                .get_page = page,
                                                .validate_peer = eligible,
                                                .can_run = db_sync_journal_can_run,
                                                .on_result = result,
                                                .begin_pull = begin,
                                                .stage_page = stage,
                                                .commit_pull = commit,
                                                .publish_pull = publish,
                                                .abort_pull = abort_pull,
                                                .finish = finish,
                                                .dependency = jl_dependency,
                                                .dependency_work = jl_dependency_work,
                                                .dependency_committed = jl_dependency_committed,
                                                .sql = &sql_ops};

/* Каждый сетевой SQL job содержит только скопированный вход; runtime проверяется до dispatch и после commit. */
struct net_job {
    struct UTUN_INSTANCE* inst;
    uint64_t generation;
    uint8_t self_ed[32];
    uint8_t peer[32];
    uint64_t peer_node;
    char group[65];
    uint8_t type, reply;
    size_t size, output_size;
    int changes, gap;
    uint8_t input[MEMBER_LOG_WIRE_MAX], output[MEMBER_LOG_WIRE_MAX];
};
static int net_guard(void* data) {
    struct net_job* j = data;
    return chat_core_generation(j->inst) == j->generation && eligible(j->inst, j->group, instance_find_conn_by_ed(j->inst, j->peer));
}
static int accept_live(sqlite3* db, struct net_job* j) {
    int code = 0;
    code = cl_accept(db, j->group, j->input, j->size, 0, &j->changes);
    if (code == MEMBER_LOG_DEFERRED || code == CL_DIVERGED) {
        j->gap = 1;
        code = 0;
    }
    if (!code && !j->gap && j->size > MEMBER_LOG_HEADER_SIZE + 64 && j->input[MEMBER_LOG_HEADER_SIZE] != CL_FEEDBACK &&
        j->input[MEMBER_LOG_HEADER_SIZE] != MEMBER_LOG_INVITATION) {
        uint64_t seq = cl_read64(j->input + MEMBER_LOG_SEQUENCE_OFFSET);
        int visible = chat_log_effect_current(db, j->group, j->input + MEMBER_LOG_SCOPE_OFFSET, seq);
        if (visible < 0) return -visible;
        if (visible) {
            memcpy(j->output, j->input + MEMBER_LOG_SCOPE_OFFSET, 32);
            cl_write64(j->output + 32, seq);
            j->output_size = 40;
            j->reply = CHAT_LOG_ACK;
        }
    }
    return code < 0 ? -code : code;
}
static int tail_scan(sqlite3* db, const uint8_t id[16], const uint8_t member[32], int* scan) {
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT scan FROM chat_head_stage WHERE pull=? AND substr(wire,2,32)=? AND substr(wire,1,1)=x'01'", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, id, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, member, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int found = rc == SQLITE_ROW;
    if (found && scan) *scan = sqlite3_column_int(st, 0);
    int final = sqlite3_finalize(st);
    return (rc == SQLITE_ROW || rc == SQLITE_DONE) && !final ? found : cl_error(db, "tail stage cursor", rc);
}
static int jl_stale(struct merkle_sync_sql_request* r, const char* reason) {
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "journal dependency changed ch=%s kind=%u: %s", r->ns, r->fetch.data[0], reason);
    r->result = MT_ERR_STALE;
    return SQLITE_ABORT;
}
static int jl_serve_tail(sqlite3* db, struct merkle_sync_sql_request* r) {
    const uint8_t* author = r->fetch.data + 1;
    const uint8_t* args = r->fetch.data + CL_SUBJECT;
    uint64_t from = cl_read64(args), upto = cl_read64(args + 8), anchor = cl_read64(args + 16);
    struct member_log_head head;
    int code = cl_valid_head(db, r->ns, author, &head, NULL);
    if (code) return -code;
    if (anchor > head.seq) return jl_stale(r, "tail exceeds current prefix");
    int matches = member_log_checkpoint(db, r->ns, author, anchor, args + 24);
    if (matches < 0) return -matches;
    if (!matches) return jl_stale(r, "tail prefix hash");
    size_t off = 1;
    for (uint64_t seq = from; seq <= upto; seq++) {
        uint8_t wire[MEMBER_LOG_WIRE_MAX];
        size_t n = sizeof(wire);
        if (member_log_get(db, r->ns, author, seq, wire, &n) != 1)
            return -cl_error(db, "tail source missing block", SQLITE_CORRUPT);
        if (n + 2 > r->capacity - off) break;
        r->data[off++] = (uint8_t)(n >> 8);
        r->data[off++] = (uint8_t)n;
        memcpy(r->data + off, wire, n);
        off += n;
    }
    if (off == 1) return -cl_error(db, "tail page cannot advance", SQLITE_TOOBIG);
    r->length = off;
    return SQLITE_OK;
}
static int tail_progress(sqlite3* db, const struct jl_pull* pull, const uint8_t author[32], uint64_t last, int scan) {
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "UPDATE chat_head_stage SET checked=CASE WHEN ?4 THEN 0 ELSE max(checked,?3) END,scan=max(scan,?4)"
                                " WHERE pull=?1 AND substr(wire,2,32)=?2 AND substr(wire,1,1)=x'01'", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, pull->id, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, author, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)last);
    if (!rc) rc = sqlite3_bind_int(st, 4, scan);
    if (!rc) rc = sqlite3_step(st);
    return -cl_finish(db, st, rc, "tail diagnostic progress");
}
static int jl_apply_tail(sqlite3* db, struct merkle_sync_sql_request* r) {
    struct jl_pull* pull = r->pull;
    const uint8_t* author = r->fetch.data + 1;
    const uint8_t* args = r->fetch.data + CL_SUBJECT;
    uint64_t from = cl_read64(args), upto = cl_read64(args + 8), anchor = cl_read64(args + 16), seq = from;
    int scan = 0;
    int known = tail_scan(db, pull->id, author, &scan);
    if (known <= 0) return known < 0 ? -known : -cl_error(db, "tail author was not staged", SQLITE_MISMATCH);
    if (r->length < 1 + 2 + MEMBER_LOG_WIRE_MIN) return -jl_bad_page(db, r->ns, "empty tail response", author);
    /* Проверяем диапазон всей страницы до записи и до перехода к поиску развилки. */
    for (size_t off = 1; off < r->length; seq++) {
        if (r->length - off < 2) return -jl_bad_page(db, r->ns, "invalid tail item length", author);
        size_t n = (size_t)r->data[off] * 256 + r->data[off + 1];
        off += 2;
        const uint8_t* wire = r->data + off;
        if (n < MEMBER_LOG_WIRE_MIN || n > MEMBER_LOG_WIRE_MAX || n > r->length - off || seq > upto ||
            cl_read64(wire + MEMBER_LOG_SEQUENCE_OFFSET) != seq || memcmp(wire + MEMBER_LOG_MEMBER_OFFSET, author, 32))
            return -jl_bad_page(db, r->ns, "tail outside requested author or sequence range", author);
        if (seq == anchor) {
            struct member_log_record block;
            if (member_log_decode_group(db, r->ns, wire, n, author, &block)) return -cl_error(db, "tail anchor validation", SQLITE_AUTH);
            if (memcmp(block.hash, args + 24, 32)) {
                db_changes_warn_hash(db, NULL, r->ns, "Chat tail", "anchor hash mismatch", author, block.signature, args + 24, block.hash);
                return -cl_error(db, "tail response anchor hash", SQLITE_AUTH);
            }
        }
        off += n;
    }
    for (size_t off = 1; off < r->length;) {
        size_t n = (size_t)r->data[off] * 256 + r->data[off + 1];
        off += 2;
        int changes = 0;
        int code = cl_accept(db, r->ns, r->data + off, n, 1, &changes);
        if (code == CL_DIVERGED) {
            if (scan || off != 3 || from < 2) return -cl_error(db, "tail disagrees during fork search", SQLITE_MISMATCH);
            DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "journal comparing overlapping blocks ch=%s member=%s continuation=%llu",
                r->ns, sc_public_key_hex(author, (char[65]){0}), (unsigned long long)from);
            return tail_progress(db, pull, author, 0, 1);
        }
        if (code) return code == MEMBER_LOG_DEFERRED ? -cl_error(db, "tail response has gap", SQLITE_CONSTRAINT) : code < 0 ? -code : code;
        r->changes |= changes;
        off += n;
    }
    return tail_progress(db, pull, author, seq - 1, 0);
}
static int jl_serve_proof(sqlite3* db, struct merkle_sync_sql_request* r) {
    if (r->capacity < 1 + 4 + 2 * MEMBER_LOG_WIRE_MAX) return -cl_error(db, "fork proof capacity", SQLITE_TOOBIG);
    const uint8_t* author = r->fetch.data + 1;
    const uint8_t* args = r->fetch.data + CL_SUBJECT;
    uint8_t hashes[64];
    uint64_t fault;
    int found = member_log_fault_hashes(db, r->ns, author, &fault, hashes);
    if (found < 0) return SQLITE_ERROR;
    if (!found || fault != cl_read64(args) || memcmp(hashes, args + 8, 64)) return jl_stale(r, "canonical fork proof");
    size_t a = MEMBER_LOG_WIRE_MAX, b = MEMBER_LOG_WIRE_MAX;
    uint8_t second[MEMBER_LOG_WIRE_MAX];
    if (member_log_fault(db, r->ns, author, &fault, r->data + 3, &a, second, &b) != 1)
        return -cl_error(db, "read verified fork proof", SQLITE_CORRUPT);
    r->data[1] = (uint8_t)(a >> 8); r->data[2] = (uint8_t)a;
    r->data[3 + a] = (uint8_t)(b >> 8); r->data[4 + a] = (uint8_t)b;
    memcpy(r->data + 5 + a, second, b);
    r->length = 5 + a + b;
    return SQLITE_OK;
}
static int jl_apply_proof(sqlite3* db, struct merkle_sync_sql_request* r) {
    const uint8_t* author = r->fetch.data + 1;
    const uint8_t* args = r->fetch.data + CL_SUBJECT;
    if (r->length < 5) return -cl_error(db, "fork proof framing", SQLITE_MISMATCH);
    size_t a = (size_t)r->data[1] * 256 + r->data[2];
    if (a > r->length - 5) return -cl_error(db, "fork proof first length", SQLITE_MISMATCH);
    size_t b = (size_t)r->data[3 + a] * 256 + r->data[4 + a];
    if (b != r->length - 5 - a) return -cl_error(db, "fork proof second length", SQLITE_MISMATCH);
    const uint8_t *first = r->data + 3, *second = r->data + 5 + a;
    struct member_log_record left, right;
    uint64_t local_id, fault = cl_read64(args);
    if (topo_node_sqlite_member_local_id(db, r->ns, author, 0, &local_id, NULL) ||
        member_log_decode_group(db, r->ns, first, a, author, &left) || member_log_decode_group(db, r->ns, second, b, author, &right) ||
        left.seq != fault || right.seq != fault || memcmp(left.hash, args + 8, 32) || memcmp(right.hash, args + 40, 32))
        return -cl_error(db, "fork proof differs from advertised hashes", SQLITE_AUTH);
    int syntax = cl_validate_block(db, &left);
    if (!syntax) syntax = cl_validate_block(db, &right);
    if (syntax) return -syntax;
    uint64_t prior_fault = 0;
    if (member_log_fault(db, r->ns, author, &prior_fault, NULL, NULL, NULL, NULL) < 0) return SQLITE_ERROR;
    int changed = member_log_prove(db, r->ns, author, first, a, second, b);
    if (changed <= 0) return changed < 0 ? SQLITE_CONSTRAINT : SQLITE_OK;
    return -cl_refresh_fault(db, r->ns, author, prior_fault, fault, &r->changes);
}
static int jl_seal_read(sqlite3* db, const char* group, const uint8_t id[32], uint8_t seal[CL_SEAL_SIZE]) {
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT wire FROM chat_seals WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?) AND id=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, id, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int found = rc == SQLITE_ROW;
    if (found) {
        const void* wire = sqlite3_column_blob(st, 0);
        if (!wire || sqlite3_column_bytes(st, 0) != CL_SEAL_SIZE) rc = SQLITE_CORRUPT;
        else memcpy(seal, wire, CL_SEAL_SIZE);
    }
    int final = sqlite3_finalize(st);
    return (rc == SQLITE_ROW || rc == SQLITE_DONE) && !final ? found : cl_error(db, "read checkpoint seal", rc == SQLITE_ROW ? final : rc);
}
static int jl_serve_seal(sqlite3* db, struct merkle_sync_sql_request* r, const uint8_t seal[CL_SEAL_SIZE]) {
    const uint8_t *id = r->fetch.data + 1, *args = r->fetch.data + CL_SUBJECT, *author = seal + CL_SEAL_MEMBER;
    uint64_t seq = cl_read64(args);
    if (seq > cl_read64(seal + CL_SEAL_SEQ)) return -cl_error(db, "checkpoint request range", SQLITE_MISMATCH);
    if (r->capacity < 1 + MEMBER_LOG_WIRE_MAX) return -cl_error(db, "checkpoint reply capacity", SQLITE_TOOBIG);
    uint8_t* wire = r->data + 1;
    size_t n = MEMBER_LOG_WIRE_MAX;
    int found = member_log_get(db, r->ns, author, seq, wire, &n);
    struct member_log_record block;
    if (found < 0) return -found;
    if (found != 1 || member_log_decode_group(db, r->ns, wire, n, author, &block) || memcmp(block.hash, args + 8, 32)) {
        sqlite3_stmt* st = NULL;
        int rc = sqlite3_prepare_v2(db, "SELECT wire FROM chat_seal_evidence WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?) AND seal_id=? AND seq=?", -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_text(st, 1, r->ns, -1, SQLITE_STATIC);
        if (!rc) rc = sqlite3_bind_blob(st, 2, id, 32, SQLITE_STATIC);
        if (!rc) rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)seq);
        if (!rc) rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            const void* value = sqlite3_column_blob(st, 0);
            n = (size_t)sqlite3_column_bytes(st, 0);
            if (!value || n < MEMBER_LOG_WIRE_MIN || n > MEMBER_LOG_WIRE_MAX) rc = SQLITE_CORRUPT;
            else memcpy(wire, value, n);
        }
        int final = sqlite3_finalize(st);
        if ((rc != SQLITE_ROW && rc != SQLITE_DONE) || final) return -cl_error(db, "checkpoint ancestry read", rc == SQLITE_ROW ? final : rc);
        if (rc == SQLITE_DONE) return jl_stale(r, "checkpoint ancestry unavailable");
    }
    if (member_log_decode_group(db, r->ns, wire, n, author, &block) || block.seq != seq || memcmp(block.hash, args + 8, 32))
        return -cl_error(db, "checkpoint ancestry hash", SQLITE_AUTH);
    r->length = 1 + n;
    return SQLITE_OK;
}
static int jl_apply_seal(sqlite3* db, struct merkle_sync_sql_request* r, const uint8_t seal[CL_SEAL_SIZE]) {
    const struct jl_pull* pull = r->pull;
    const uint8_t *id = r->fetch.data + 1, *args = r->fetch.data + CL_SUBJECT;
    const uint8_t* wire = r->data + 1;
    size_t n = r->length - 1;
    struct member_log_record block;
    if (member_log_decode_group(db, r->ns, wire, n, seal + CL_SEAL_MEMBER, &block))
        return -cl_error(db, "checkpoint reply signature or format", SQLITE_AUTH);
    if (block.seq != cl_read64(args) || memcmp(block.hash, args + 8, 32)) {
        db_changes_warn_hash(db, NULL, r->ns, "Chat checkpoint", "reply differs from requested block",
            block.member, block.signature, args + 8, block.hash);
        return -cl_error(db, "checkpoint reply differs from requested block", SQLITE_AUTH);
    }
    int syntax = cl_validate_block(db, &block);
    if (syntax) return -syntax;
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "UPDATE chat_seal_stage SET seq=?1,hash=?2 WHERE pull=?3 AND id=?4 AND seq=?5 AND hash=?6", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_int64(st, 1, (sqlite3_int64)block.seq - 1);
    if (!rc) rc = sqlite3_bind_blob(st, 2, block.previous, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 3, pull->id, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 4, id, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int64(st, 5, (sqlite3_int64)block.seq);
    if (!rc) rc = sqlite3_bind_blob(st, 6, block.hash, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int updated = sqlite3_changes(db);
    int code = cl_finish(db, st, rc, "advance checkpoint ancestry");
    if (code) return -code;
    if (updated != 1) return -cl_error(db, "checkpoint cursor is no longer active", SQLITE_MISMATCH);
    rc = sqlite3_prepare_v2(db, "INSERT INTO chat_seal_evidence VALUES((SELECT ed25519_pubkey FROM channels WHERE channel_id=?1),?2,?3,?4) ON CONFLICT DO NOTHING", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, r->ns, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, id, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)block.seq);
    if (!rc) rc = sqlite3_bind_blob(st, 4, wire, (int)n, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    return -cl_finish(db, st, rc, "retain checkpoint evidence");
}
static int jl_dependency_sql(sqlite3* db, struct merkle_sync_sql_request* r) {
    int serve = r->action == MT_SQL_SERVE_DEPENDENCY;
    uint8_t kind = r->fetch.data[0];
    size_t expected = kind == CL_FETCH_TAIL ? CL_TAIL_QUERY_SIZE : kind == CL_FETCH_PROOF ? CL_PROOF_QUERY_SIZE :
        kind == CL_FETCH_SEAL ? CL_SEAL_QUERY_SIZE : 0;
    const uint8_t* args = r->fetch.data + CL_SUBJECT;
    uint64_t seq = cl_read64(args);
    if (!expected || r->fetch.length != expected || !seq || seq > INT64_MAX ||
        (serve ? !r->capacity || r->capacity > sizeof(r->data) : !r->pull || !r->length || r->length > sizeof(r->data) || r->data[0] != kind)) {
        r->result = MT_ERR_PROTOCOL;
        return -cl_error(db, "journal dependency framing", SQLITE_MISMATCH);
    }
    r->result = serve ? MT_ERR_IO : MT_ERR_DATA;
    if (serve) r->data[0] = kind;
    int rc;
    if (kind == CL_FETCH_TAIL) {
        uint64_t upto = cl_read64(args + 8), anchor = cl_read64(args + 16);
        if (seq > upto || upto > anchor || anchor > INT64_MAX) return -cl_error(db, "journal dependency tail range", SQLITE_MISMATCH);
        rc = serve ? jl_serve_tail(db, r) : jl_apply_tail(db, r);
    } else if (kind == CL_FETCH_PROOF) {
        if (memcmp(args + 8, args + 40, 32) >= 0) return -cl_error(db, "journal dependency fork hash order", SQLITE_MISMATCH);
        rc = serve ? jl_serve_proof(db, r) : jl_apply_proof(db, r);
    } else {
        uint8_t seal[CL_SEAL_SIZE];
        int found = jl_seal_read(db, r->ns, r->fetch.data + 1, seal);
        if (found < 0) return -found;
        if (!found) return serve ? jl_stale(r, "checkpoint seal unavailable") : -cl_error(db, "checkpoint seal absent", SQLITE_AUTH);
        rc = serve ? jl_serve_seal(db, r, seal) : jl_apply_seal(db, r, seal);
    }
    if (!rc) r->result = MT_OK;
    return rc;
}
static int jl_dependency_work(void* ctx, struct merkle_sync_sql_request* r) {
    sqlite3* db = ((struct UTUN_INSTANCE*)ctx)->topo_sqlite_db;
    int code = cl_exec(db, r->action == MT_SQL_SERVE_DEPENDENCY ? "BEGIN" : "BEGIN IMMEDIATE");
    if (code) return -code;
    code = jl_dependency_sql(db, r);
    if (!code) code = -cl_exec(db, "COMMIT");
    if (code && !sqlite3_get_autocommit(db)) cl_exec(db, "ROLLBACK");
    return code;
}
/* ACK хранит номер блока в знаковом INTEGER SQLite. */
static int journal_ack_valid(const uint8_t* data, size_t size) {
    if (size != 40) return 0;
    uint64_t seq = cl_read64(data + 32);
    return seq && seq <= INT64_MAX;
}
static int accept_ack(sqlite3* db, struct net_job* j) {
    if (!journal_ack_valid(j->input, j->size)) return -cl_error(db, "journal ACK framing", SQLITE_MISMATCH);
    return db_sync_journal_ack_write(db, j->group, j->self_ed, j->input, cl_read64(j->input + 32), j->peer);
}
static int net_work(sqlite3* db, void* data) {
    struct net_job* j = data;
    j->output_size = 0;
    j->reply = 0;
    struct db_changes_origin source = db_changes_source(db, j->peer, j->peer_node, j->type == CHAT_LOG_ACK ? "chat delivery ACK" : "chat push");
    int code = j->type == CHAT_LOG_PUSH ? accept_live(db, j) : j->type == CHAT_LOG_ACK ? accept_ack(db, j)
        : -cl_error(db, "unknown journal packet", SQLITE_MISMATCH);
    db_changes_restore(db, source);
    return code;
}
static void publish_block(struct UTUN_INSTANCE* inst, const char* group, const uint8_t* wire, size_t size) {
    if (size >= MEMBER_LOG_HEADER_SIZE + 2 + 64 && wire[MEMBER_LOG_HEADER_SIZE] == CL_FEEDBACK) {
        struct chat_feedback_record record = {.kind = wire[MEMBER_LOG_HEADER_SIZE + 1]};
        memcpy(record.message_id, wire + MEMBER_LOG_SCOPE_OFFSET, 32);
        chat_feedback_received(inst, group, &record);
    }
}
static void publish_history_event(struct UTUN_INSTANCE* inst, const char* group) {
    if (inst && inst->chat_content_started && !inst->destroying) chat_event_message(inst, group, NULL, 1);
}
static void jl_dependency_committed(void* ctx, const struct merkle_sync_sql_request* r) {
    struct UTUN_INSTANCE* inst = ctx;
    if (!r->changes) return;
    chat_log_changed(inst, r->ns);
    cl_effects_start(inst, r->ns);
    uint64_t generation = chat_core_generation(inst);
    for (size_t off = 1; (r->changes & CL_CHANGE_BLOCK) && off + 2 < r->length && inst->chat_content_started && !inst->destroying &&
        chat_core_generation(inst) == generation && group_protocol_current(inst, r->ns) == GROUP_PROTOCOL_CHAT;) {
        size_t n = (size_t)r->data[off] * 256 + r->data[off + 1];
        off += 2;
        publish_block(inst, r->ns, r->data + off, n);
        off += n;
    }
    if ((r->changes & CL_CHANGE_HISTORY) && chat_core_generation(inst) == generation) publish_history_event(inst, r->ns);
}
static void net_done(void* data, const struct db_async_result* r) {
    struct net_job* j = data;
    if (!net_guard(j)) return;
    if (r->status) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "journal packet failed ch=%s peer=%s type=%u rc=%d: %s", j->group, sc_public_key_hex(j->peer, (char[65]){0}),
                   j->type, r->sqlite_code, r->message);
        return;
    }
    if (j->reply) db_sync_journal_send(j->inst, j->group, j->peer, j->reply, j->output, j->output_size);
    if (j->changes) {
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "journal packet committed ch=%s peer=%s type=%u", j->group, sc_public_key_hex(j->peer, (char[65]){0}), j->type);
        chat_log_changed(j->inst, j->group);
        cl_effects_start(j->inst, j->group);
        if (j->changes & CL_CHANGE_BLOCK) {
            publish_block(j->inst, j->group, j->input, j->size);
            cl_broadcast(j->inst, j->group, j->peer, j->input, j->size);
        }
        if ((j->changes & CL_CHANGE_HISTORY) && chat_core_generation(j->inst) == j->generation)
            publish_history_event(j->inst, j->group);
    }
    if (j->gap) chat_log_peer_ready(j->inst, j->group, j->peer);
}
static void net_run(struct net_job* j) {
    if (db_async_can_write(j->inst->db_worker)) {
        struct db_async_task task = {.name = "chat-journal-packet",
                                     .mode = DB_ASYNC_WRITE,
                                     .work = net_work,
                                     .guard = net_guard,
                                     .done = net_done,
                                     .cleanup = free_data,
                                     .data = j};
        db_async_run(j->inst->db_worker, &task);
        return;
    }
    sqlite3* db = j->inst->topo_sqlite_db;
    int code = cl_exec(db, "BEGIN IMMEDIATE");
    if (!code) code = net_work(db, j);
    if (!code) code = cl_exec(db, "COMMIT");
    if (code && !sqlite3_get_autocommit(db)) cl_exec(db, "ROLLBACK");
    struct db_async_result r = {.status = code ? DB_ASYNC_SQL : 0, .sqlite_code = code < 0 ? -code : code};
    net_done(j, &r);
    u_free(j);
}
void chat_log_receive(struct ETCP_CONN* conn, const char* group, uint8_t type, const uint8_t* data, size_t size) {
    struct UTUN_INSTANCE* inst = conn ? conn->instance : NULL;
    if (!data || (type == CHAT_LOG_PUSH ? size < MEMBER_LOG_WIRE_MIN || size > MEMBER_LOG_WIRE_MAX
                                      : type != CHAT_LOG_ACK || !journal_ack_valid(data, size))) {
        if (conn) {
            struct db_changes_origin source = {.node=conn->peer_node_id, .via="chat journal"};
            memcpy(source.ed, conn->peer_ed25519_pubkey, 32);
            db_changes_warn(NULL, &source, group, "Chat journal packet", "invalid push or ACK frame", NULL, NULL, NULL);
        }
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "journal packet framing denied ch=%s peer=%s type=%u bytes=%zu",
            group ? group : "?", sc_public_key_hex(conn ? conn->peer_ed25519_pubkey : NULL, (char[65]){0}), type, size);
        return;
    }
    if (!eligible(inst, group, conn) || !db_sync_journal_can_run(inst, group, conn)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "journal packet admission denied ch=%s peer=%s type=%u",
            group ? group : "?", sc_public_key_hex(conn ? conn->peer_ed25519_pubkey : NULL, (char[65]){0}), type);
        return;
    }
    struct net_job* j = u_calloc(1, sizeof(*j));
    if (!j) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "journal packet allocation");
        return;
    }
    j->inst = inst;
    j->generation = chat_core_generation(inst);
    memcpy(j->self_ed, inst->my_ed25519_pubkey, 32);
    memcpy(j->peer, conn->peer_ed25519_pubkey, 32);
    j->peer_node = conn->peer_node_id;
    j->type = type;
    j->size = size;
    strcpy(j->group, group);
    memcpy(j->input, data, size);
    net_run(j);
}
void cl_broadcast(struct UTUN_INSTANCE* inst, const char* group, const uint8_t except[32], const uint8_t* wire, size_t size) {
    struct TOPO_GROUP* topology = inst->topo_groups ? topo_groups_find_channel(inst->topo_groups, group) : NULL;
    /* READY-пиры доступны через общий registry; send дополнительно проверяет актуальные права и sleep. */
    (void)topology;
    for (struct ll_entry* entry = inst->connections ? inst->connections->head : NULL; entry; entry = entry->next) {
        struct ETCP_CONN* conn = ((struct conn_queue_entry*)entry->data)->conn;
        if (conn && (!except || memcmp(conn->peer_ed25519_pubkey, except, 32)) && eligible(inst, group, conn))
            db_sync_journal_send(inst, group, conn->peer_ed25519_pubkey, CHAT_LOG_PUSH, wire, size);
    }
}
static int own_work(sqlite3* db, void* data) {
    struct net_job* j = data;
    j->output_size = MEMBER_LOG_WIRE_MAX;
    uint64_t seq = cl_read64(j->input);
    int found = member_log_get(db, j->group, j->self_ed, seq, j->output, &j->output_size);
    if (found == 1) return SQLITE_OK;
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "committed CHAT block missing or unreadable ch=%s author=%s seq=%llu result=%d",
        j->group, sc_public_key_hex(j->self_ed, (char[65]){0}), (unsigned long long)seq, found);
    return found < 0 ? -found : SQLITE_NOTFOUND;
}
static int own_guard(void* data) {
    struct net_job* j = data;
    return j->inst->chat_content_started && !j->inst->destroying && chat_core_generation(j->inst) == j->generation;
}
static void own_done(void* data, const struct db_async_result* r) {
    struct net_job* j = data;
    if (!own_guard(j)) return;
    if (r->status) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "committed CHAT block read failed ch=%s rc=%d", j->group, r->sqlite_code);
        return;
    }
    cl_broadcast(j->inst, j->group, 0, j->output, j->output_size);
}
static void publish_local_block(struct UTUN_INSTANCE* inst, const char* group, uint64_t seq) {
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "CHAT action durable ch=%s author=%s seq=%llu", group, sc_public_key_hex(inst->my_ed25519_pubkey, (char[65]){0}),
                (unsigned long long)seq);
    struct net_job* j = u_calloc(1, sizeof(*j));
    if (!j) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "own CHAT block allocation");
        return;
    }
    j->inst = inst;
    j->generation = chat_core_generation(inst);
    memcpy(j->self_ed, inst->my_ed25519_pubkey, 32);
    strcpy(j->group, group);
    cl_write64(j->input, seq);
    if (db_async_can_write(inst->db_worker)) {
        struct db_async_task task = {.name = "chat-journal-live",
                                     .mode = DB_ASYNC_READ,
                                     .work = own_work,
                                     .guard = own_guard,
                                     .done = own_done,
                                     .cleanup = free_data,
                                     .data = j};
        db_async_run(inst->db_worker, &task);
    } else {
        int code = own_work(inst->topo_sqlite_db, j);
        struct db_async_result r = {.status = code ? DB_ASYNC_SQL : 0, .sqlite_code = code};
        own_done(j, &r);
        u_free(j);
    }
}
void chat_log_local_committed(struct UTUN_INSTANCE* inst, const char* group, uint64_t seq, int history_changed) {
    chat_log_changed(inst, group);
    cl_effects_start(inst, group);
    if (seq) publish_local_block(inst, group, seq);
    if (history_changed) publish_history_event(inst, group);
}
int chat_log_activate(struct UTUN_INSTANCE* inst) {
    int rc = merkle_sync_registered(inst, MT_DATA_CHAT_LOG) ? 0 : merkle_sync_register(inst, MT_DATA_CHAT_LOG, &ops, inst);
    if (!rc && inst->chat_started) cl_effects_start(inst, NULL);
    return rc;
}
void chat_log_peer_ready(struct UTUN_INSTANCE* inst, const char* group, const uint8_t peer[32]) {
    struct ETCP_CONN* conn = instance_find_conn_by_ed(inst, peer);
    if (eligible(inst, group, conn) && merkle_sync_start(inst, MT_DATA_CHAT_LOG, conn, group, NULL, NULL) < 0)
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "journal sync unavailable ch=%s peer=%s", group, sc_public_key_hex(peer, (char[65]){0}));
    cl_effects_start(inst, group);
}
void chat_log_changed(struct UTUN_INSTANCE* inst, const char* group) {
    if (inst && inst->chat_content_started && !inst->destroying) merkle_sync_changed(inst, MT_DATA_CHAT_LOG, group);
}
void chat_log_publish(struct UTUN_INSTANCE* inst, const char* group) {
    chat_log_changed(inst, group);
    cl_effects_start(inst, group);
    publish_history_event(inst, group);
}
