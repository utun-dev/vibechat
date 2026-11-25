#include "member_log.h"
#include "../db_changes.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../transport_layer/secure_channel.h"
#include "../../lib/debug_config.h"
#include <openssl/evp.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static const uint8_t log_domain[16] = "utun.memberlog3";

/* Числовые поля wire всегда big-endian. */
#define LOG_PREVIOUS_OFFSET 88
#define LOG_TIME_OFFSET 120
#define LOG_GRANT_OFFSET 160
#define LOG_LENGTH_OFFSET 192
static void put64(uint8_t* p, uint64_t value) {
    for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)value; value >>= 8; }
}
static uint64_t get64(const uint8_t* p) {
    uint64_t value = 0; for (unsigned i = 0; i < 8; i++) value = (value << 8) | p[i]; return value;
}
static int failure(sqlite3* db, const char* stage) {
    if (db && sqlite3_errcode(db) != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "member log %s: %s", stage, sqlite3_errmsg(db));
    else DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "member log %s", stage);
    return -1;
}
static int group_key(sqlite3* db, const char* channel, uint8_t out[32]) {

    if (!channel || !*channel || strnlen(channel, 65) == 65 || topo_node_sqlite_channel_get(db, channel, NULL, 0, NULL, NULL, out, NULL))
        return failure(db, "group key unavailable");
    return 0;
}
static int valid(const struct member_log_record* r) {
    return r && memcmp(r->group_ed, (uint8_t[32]){0}, 32) && memcmp(r->member, (uint8_t[32]){0}, 32) && r->seq && r->seq <= INT64_MAX &&
        r->time_us && r->time_us < INT64_MAX && r->body && r->body_size && r->body_size <= MEMBER_LOG_BODY_MAX;
}
/* Подписываем точные байты заголовка и payload; hash/signature в заголовок не входят. */
static int frame(const struct member_log_record* r, uint8_t* wire, size_t* size) {
    if (!valid(r) || !wire || !size || *size < MEMBER_LOG_HEADER_SIZE + r->body_size + 64)
        return failure(NULL, "encode arguments/capacity");
    memcpy(wire, log_domain, 16); memcpy(wire + MEMBER_LOG_GROUP_OFFSET, r->group_ed, 32); memcpy(wire + MEMBER_LOG_MEMBER_OFFSET, r->member, 32);
    put64(wire + MEMBER_LOG_SEQUENCE_OFFSET, r->seq); memcpy(wire + LOG_PREVIOUS_OFFSET, r->previous, 32); put64(wire + LOG_TIME_OFFSET, r->time_us);
    memcpy(wire + MEMBER_LOG_SCOPE_OFFSET, r->scope, 32); memcpy(wire + LOG_GRANT_OFFSET, r->grant, 32);
    uint32_t bytes = (uint32_t)r->body_size;
    for (int i = 3; i >= 0; i--) { wire[LOG_LENGTH_OFFSET + i] = (uint8_t)bytes; bytes >>= 8; }
    memcpy(wire + MEMBER_LOG_HEADER_SIZE, r->body, r->body_size);
    memcpy(wire + MEMBER_LOG_HEADER_SIZE + r->body_size, r->signature, 64);
    *size = MEMBER_LOG_HEADER_SIZE + r->body_size + 64; return 0;
}
int member_log_encode(const struct member_log_record* r, uint8_t* wire, size_t* size) { return frame(r, wire, size); }

static int decode(sqlite3* db, const char* channel, const uint8_t* wire, size_t size, const uint8_t key[32], struct member_log_record* out) {
    const char* reason = "invalid frame";
    const uint8_t* signature = NULL;
    if (out) memset(out, 0, sizeof(*out));
    if (!wire || !key || !out || size < MEMBER_LOG_WIRE_MIN || size > MEMBER_LOG_WIRE_MAX ||
        memcmp(wire, log_domain, 16)) goto invalid;
    uint32_t bytes = 0; for (unsigned i = LOG_LENGTH_OFFSET; i < MEMBER_LOG_HEADER_SIZE; i++) bytes = (bytes << 8) | wire[i];
    reason = "invalid signed body length";
    if (bytes > MEMBER_LOG_BODY_MAX || size != MEMBER_LOG_HEADER_SIZE + (size_t)bytes + 64) goto invalid;
    struct member_log_record r = {0}; memcpy(r.group_ed, wire + MEMBER_LOG_GROUP_OFFSET, 32); memcpy(r.member, wire + MEMBER_LOG_MEMBER_OFFSET, 32); r.seq = get64(wire + MEMBER_LOG_SEQUENCE_OFFSET);
    memcpy(r.previous, wire + LOG_PREVIOUS_OFFSET, 32); r.time_us = get64(wire + LOG_TIME_OFFSET);
    memcpy(r.scope, wire + MEMBER_LOG_SCOPE_OFFSET, 32); memcpy(r.grant, wire + LOG_GRANT_OFFSET, 32);
    r.body = wire + MEMBER_LOG_HEADER_SIZE; r.body_size = bytes; memcpy(r.signature, r.body + bytes, 64);
    signature = r.body + bytes;
    reason = "invalid author or signed header";
    if (!valid(&r) || memcmp(r.member, key, 32)) goto invalid;
    if (EVP_Digest(wire, MEMBER_LOG_HEADER_SIZE + bytes, r.hash, NULL, EVP_sha256(), NULL) != 1) return failure(db, "hash block");
    int rc = sc_ed25519_verify(key, r.hash, 32, r.signature);
    if (rc != SC_OK) {
        if (rc != SC_ERR_AUTH_FAILED) return failure(db, "verify block signature");
        reason = "invalid signature"; goto invalid;
    }
    *out = r; return 0;
invalid:
    if (channel) {
        char details[96] = "";
        if (signature) snprintf(details, sizeof(details), "seq=%llu scope_id=%s",
            (unsigned long long)get64(wire + MEMBER_LOG_SEQUENCE_OFFSET), sc_public_key_hex(wire + MEMBER_LOG_SCOPE_OFFSET, (char[65]){0}));
        db_changes_warn(db, NULL, channel, "Chat block", reason, key, signature, *details ? details : NULL);
    }
    return failure(db, reason);
}

int member_log_decode(const uint8_t* wire, size_t size, const uint8_t key[32], struct member_log_record* out) {
    return decode(NULL, NULL, wire, size, key, out);
}

int member_log_decode_group(sqlite3* db, const char* channel, const uint8_t* wire, size_t size, const uint8_t key[32],
    struct member_log_record* out) {
    uint8_t expected[32];
    if (decode(db, channel, wire, size, key, out) || group_key(db, channel, expected)) return -1;
    if (memcmp(out->group_ed, expected, 32)) {
        db_changes_warn(db, NULL, channel, "Chat block", "signed for a different group", key, out->signature, NULL);
        DEBUG_WARN(DEBUG_CATEGORY_GROUP, "member log group mismatch channel=%s expected=%s received=%s",
            channel, sc_public_key_hex(expected, (char[65]){0}), sc_public_key_hex(out->group_ed, (char[65]){0}));
        return -1;
    }
    strcpy(out->channel, channel);
    return 0;
}

int member_log_head(sqlite3* db, const char* channel, const uint8_t member[32], struct member_log_head* out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!db || !channel || !*channel || strnlen(channel, 65) == 65 || !member || !out) return failure(db, "head arguments");

    sqlite3_stmt* st = NULL; int rc = sqlite3_prepare_v2(db,
        "SELECT head_seq,head_hash FROM member_chains WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND member_id=?2", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, member, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int good = rc == SQLITE_DONE || (rc == SQLITE_ROW && sqlite3_column_int64(st, 0) > 0 && sqlite3_column_bytes(st, 1) == 32);
    if (good && rc == SQLITE_ROW) {
        out->seq = (uint64_t)sqlite3_column_int64(st, 0); memcpy(out->hash, sqlite3_column_blob(st, 1), 32);

    }
    int final = sqlite3_finalize(st);
    if (!good || final != SQLITE_OK) { memset(out, 0, sizeof(*out)); return failure(db, "read head"); }
    uint64_t fault=0;
    if(member_log_fault(db,channel,member,&fault,NULL,NULL,NULL,NULL)<0) return -1;
    out->blocked=fault!=0;
    return 0;
}

int member_log_head_time(sqlite3* db, const char* channel, const uint8_t member[32], uint64_t* time_us) {
    if (!db || !channel || !member || !time_us || sqlite3_get_autocommit(db)) {
        failure(db, "head time arguments/transaction"); return SQLITE_MISUSE;
    }
    *time_us = 0;
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT l.time_us FROM member_chains c LEFT JOIN member_log l"
        " ON l.group_id=c.group_id AND l.member_id=c.member_id AND l.seq=c.head_seq"
        " WHERE c.group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND c.member_id=?2", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, member, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        sqlite3_int64 previous = sqlite3_column_int64(st, 0);
        if (sqlite3_column_type(st, 0) != SQLITE_INTEGER || previous <= 0 || previous >= INT64_MAX) rc = SQLITE_CORRUPT;
        else { *time_us = (uint64_t)previous; rc = SQLITE_OK; }
    } else if (rc == SQLITE_DONE) rc = SQLITE_OK;
    int final = sqlite3_finalize(st);
    if (!rc) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "member log head time failed group=%s member=%s rc=%d: %s",
        channel, sc_public_key_hex(member, (char[65]){0}), rc, sqlite3_errstr(rc));
    return rc;
}

int member_log_make(sqlite3* db, const char* channel, const uint8_t member[32], uint64_t time_us,
    const uint8_t scope[32], const uint8_t grant[32], const uint8_t* body, size_t body_size,
    const uint8_t key[32], struct member_log_record* out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!db || sqlite3_get_autocommit(db) || !out || !scope || !grant || !key) return failure(db, "make transaction/arguments");
    struct member_log_head head;
    if (member_log_head(db, channel, member, &head) || head.seq == INT64_MAX || head.blocked)
        return failure(db, "make blocked head/exhausted sequence");
    struct member_log_record r = {.seq=head.seq + 1, .time_us=time_us, .body=body, .body_size=body_size};
    if (group_key(db, channel, r.group_ed)) return -1;
    strcpy(r.channel, channel);
    memcpy(r.member, member, 32);
    memcpy(r.previous, head.hash, 32); memcpy(r.scope, scope, 32); memcpy(r.grant, grant, 32);
    /* Поправка NTP не делает новую запись старше предыдущей при LWW. */
    if (head.seq) {
        uint64_t previous;
        if (member_log_head_time(db, channel, member, &previous) || !previous || previous >= INT64_MAX - 1) return failure(db, "make previous time");
        if (r.time_us <= previous) r.time_us = previous + 1;
    }
    uint8_t wire[MEMBER_LOG_WIRE_MAX]; size_t size = sizeof(wire);
    if (frame(&r, wire, &size) || EVP_Digest(wire, size - 64, r.hash, NULL, EVP_sha256(), NULL) != 1 ||
        sc_ed25519_sign(key, r.hash, 32, r.signature) != SC_OK) return failure(db, "make signature");
    *out = r; return 0;
}

int member_log_checkpoint(sqlite3* db, const char* channel, const uint8_t member[32], uint64_t seq, const uint8_t hash[32]) {
    uint8_t zero[32] = {0};
    if (!db || !channel || !*channel || strnlen(channel, 65) == 65 || !member || seq > INT64_MAX || !hash) {
        failure(db, "checkpoint arguments"); return -SQLITE_MISUSE;
    }
    if (!seq) return !memcmp(hash, zero, 32);
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT block_hash FROM member_log WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND member_id=?2 AND seq=?3", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, member, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)seq);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int result = 0;
    if (rc == SQLITE_ROW) {
        if (sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != 32) rc = SQLITE_CORRUPT;
        else { const void* value = sqlite3_column_blob(st, 0); if (!value) rc = SQLITE_NOMEM; else result = !memcmp(value, hash, 32); }
    }
    int final = sqlite3_finalize(st);
    if ((rc != SQLITE_ROW && rc != SQLITE_DONE) || final != SQLITE_OK) {
        int code = rc != SQLITE_ROW && rc != SQLITE_DONE ? rc : final;
        DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "member log checkpoint group=%s member=%s seq=%llu rc=%d finalize=%d: %s",
            channel, sc_public_key_hex(member, (char[65]){0}), (unsigned long long)seq, code, final, sqlite3_errstr(code)); return -code;
    }
    return result;
}

/* Читаем собственный сохранённый блок и проверяем его перед обвинением автора:
 * повреждение БД и неподтверждённый хеш пира не являются доказательством компрометации. */
int member_log_get(sqlite3* db, const char* channel, const uint8_t member[32], uint64_t seq, uint8_t* wire, size_t* size) {
    if (!db || !channel || !*channel || strnlen(channel, 65) == 65 || !member || !seq || seq > INT64_MAX || !wire || !size) {
        failure(db, "get arguments"); return -SQLITE_MISUSE;
    }
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT prev_hash,block_hash,time_us,scope_id,grant_id,body,signature FROM member_log"
        " WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND member_id=?2 AND seq=?3", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, member, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)seq);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int result = -SQLITE_CORRUPT;
    if (rc == SQLITE_DONE) { *size = 0; result = 0; }
    else if (rc == SQLITE_ROW) {
        const int cols[] = {0, 1, 3, 4, 6}, sizes[] = {32, 32, 32, 32, 64}; const void* blobs[5];
        for (unsigned i = 0; i < 5; i++) {
            if (sqlite3_column_type(st, cols[i]) != SQLITE_BLOB || sqlite3_column_bytes(st, cols[i]) != sizes[i]) goto get_done;
            blobs[i] = sqlite3_column_blob(st, cols[i]); if (!blobs[i]) { result = -SQLITE_NOMEM; goto get_done; }
        }
        if (sqlite3_column_type(st, 2) != SQLITE_INTEGER || sqlite3_column_type(st, 5) != SQLITE_BLOB) goto get_done;
        struct member_log_record r = {.seq=seq,
            .time_us=(uint64_t)sqlite3_column_int64(st, 2), .body=sqlite3_column_blob(st, 5), .body_size=(size_t)sqlite3_column_bytes(st, 5)};
        memcpy(r.member, member, 32);
        if (group_key(db, channel, r.group_ed)) goto get_done;
        if (!r.body) { result = -SQLITE_NOMEM; goto get_done; }
        if (MEMBER_LOG_HEADER_SIZE + r.body_size + 64 > *size) { result = -SQLITE_TOOBIG; goto get_done; }
        memcpy(r.previous, blobs[0], 32); memcpy(r.hash, blobs[1], 32);
        memcpy(r.scope, blobs[2], 32); memcpy(r.grant, blobs[3], 32); memcpy(r.signature, blobs[4], 64);
        uint8_t actual[32];
        if (!frame(&r, wire, size) && EVP_Digest(wire, *size - 64, actual, NULL, EVP_sha256(), NULL) == 1 &&
            !memcmp(actual, r.hash, 32)) result = 1;
    }
get_done:
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) result = -rc;
    int final = sqlite3_finalize(st);
    if (result >= 0 && final != SQLITE_OK) result = -final;
    if (result < 0) DEBUG_ERROR(DEBUG_CATEGORY_GROUP,
        "member log get group=%s member=%s seq=%llu rc=%d finalize=%d: %s", channel,
        sc_public_key_hex(member, (char[65]){0}), (unsigned long long)seq, -result, final, sqlite3_errstr(-result));
    return result;
}

int member_log_verify_own_read(sqlite3* db, const uint8_t member[32]) {
    if (!db || !member || sqlite3_get_autocommit(db)) {
        failure(db, "startup verification requires caller snapshot/member/key"); return SQLITE_MISUSE;
    }
    sqlite3_stmt* st = NULL; unsigned chains = 0;
    int rc = sqlite3_prepare_v2(db, "SELECT ch.channel_id,c.head_seq,c.head_hash,"
        "(SELECT seq FROM member_log l WHERE l.group_id=c.group_id AND l.member_id=c.member_id ORDER BY seq DESC LIMIT 1)"
        " FROM member_chains c LEFT JOIN channels ch ON ch.ed25519_pubkey=c.group_id WHERE c.member_id=?1", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, member, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        uint64_t seq = (uint64_t)sqlite3_column_int64(st, 1);
        uint8_t expected[32];
        const char* channel = (const char*)sqlite3_column_text(st, 0);
        if (!channel || !*channel || strnlen(channel, 65) == 65 || (size_t)sqlite3_column_bytes(st, 0) != strlen(channel) ||
            !seq || seq > INT64_MAX ||
            sqlite3_column_bytes(st, 2) != 32 || sqlite3_column_type(st, 3) != SQLITE_INTEGER ||
            (uint64_t)sqlite3_column_int64(st, 3) != seq) {
            DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "own member log invalid head group=%s member=%s seq=%llu; startup refused",
                channel ? channel : "?", sc_public_key_hex(member, (char[65]){0}), (unsigned long long)seq);
            rc = SQLITE_CORRUPT; break;
        }
        const void* head = sqlite3_column_blob(st, 2);
        if (!head) { rc = SQLITE_NOMEM; break; }
        memcpy(expected, head, 32);
        uint64_t checked = seq < MEMBER_LOG_VERIFY_LAST ? seq : MEMBER_LOG_VERIFY_LAST, n = seq;
        for (; n > seq - checked; n--) {
            uint8_t wire[MEMBER_LOG_WIRE_MAX]; size_t size = sizeof(wire); struct member_log_record r;
            int found = member_log_get(db, channel, member, n, wire, &size);
            if (found != 1) { rc = found < 0 ? -found : SQLITE_CORRUPT; break; }
            if (member_log_decode(wire, size, member, &r) || memcmp(r.hash, expected, 32)) { rc = SQLITE_CORRUPT; break; }
            memcpy(expected, r.previous, 32);
        }
        int checkpoint = n == seq - checked ? member_log_checkpoint(db, channel, member, n, expected) : 0;
        if (n != seq - checked || checkpoint != 1) {
            DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "own member log verification failed group=%s member=%s seq=%llu head=%llu; "
                "startup refused, stored data retained", channel, sc_public_key_hex(member, (char[65]){0}),
                (unsigned long long)n, (unsigned long long)seq);
            if (rc == SQLITE_ROW) rc = checkpoint < 0 ? -checkpoint : SQLITE_CORRUPT;
            break;
        }
        chains++;
        DEBUG_INFO(DEBUG_CATEGORY_GROUP, "own member log verified group=%s member=%s head=%llu checked=%llu",
            channel, sc_public_key_hex(member, (char[65]){0}), (unsigned long long)seq, (unsigned long long)checked);
    }
    int final = sqlite3_finalize(st);
    if (rc == SQLITE_DONE && final == SQLITE_OK) {
        DEBUG_DEBUG(DEBUG_CATEGORY_GROUP, "own member log startup verification complete member=%s chains=%u limit=%u",
            sc_public_key_hex(member, (char[65]){0}), chains, MEMBER_LOG_VERIFY_LAST); return 0;
    }
    int code = rc == SQLITE_DONE ? final : rc;
    DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "member log startup verification failed member=%s rc=%d finalize=%d: %s",
        sc_public_key_hex(member, (char[65]){0}), code, final, sqlite3_errstr(code)); return code;
}

int member_log_verify_own(sqlite3* db, const uint8_t member[32]) {
    if (!db || !member || !sqlite3_get_autocommit(db)) return failure(db, "startup verification arguments/transaction");
    if (sqlite3_exec(db, "BEGIN /* member log startup verification */", NULL, NULL, NULL) != SQLITE_OK)
        return failure(db, "startup verification snapshot");
    int rc = member_log_verify_own_read(db, member);
    if (!rc) rc = sqlite3_exec(db, "COMMIT /* member log startup verification */", NULL, NULL, NULL);
    if (!rc) return 0;
    DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "member log startup snapshot failed rc=%d: %s", rc, sqlite3_errstr(rc));
    if (!sqlite3_get_autocommit(db) && sqlite3_exec(db, "ROLLBACK /* member log startup verification */", NULL, NULL, NULL) != SQLITE_OK)
        failure(db, "startup verification rollback");
    return -1;
}

int member_log_commit(sqlite3* db) {
    const char* path = db ? sqlite3_db_filename(db, "main") : NULL;
    if (!db || sqlite3_get_autocommit(db) || !path || !*path) return failure(db, "durable commit requires transaction and disk database");
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT journal_mode,synchronous FROM pragma_journal_mode,pragma_synchronous", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    const char* mode = rc == SQLITE_ROW ? (const char*)sqlite3_column_text(st, 0) : NULL;
    int full = rc == SQLITE_ROW && mode && !strcmp(mode, "wal") && sqlite3_column_int(st, 1) >= 2;
    if (!full) DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "member log durable commit refused file=%s journal=%s synchronous=%d; no publication",
        path, mode ? mode : "?", rc == SQLITE_ROW ? sqlite3_column_int(st, 1) : -1);
    int final = sqlite3_finalize(st);
    if (!full || final != SQLITE_OK) return failure(db, "durable commit configuration");
    if (sqlite3_exec(db, "COMMIT /* member log durable */", NULL, NULL, NULL) != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "member log disk commit failed file=%s rc=%d error=%s; publication forbidden",
            path, sqlite3_extended_errcode(db), sqlite3_errmsg(db)); return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_GROUP, "member log disk commit completed file=%s WAL synced; publication permitted", path); return 0;
}

/* Читает отдельную запись доказательства. Формат и подписи проверяет decode_fault. */
int member_log_fault(sqlite3* db,const char* channel,const uint8_t member[32],uint64_t* seq,
    uint8_t* first,size_t* first_size,uint8_t* second,size_t* second_size) {
    sqlite3_stmt* st=NULL;
    if(seq) *seq=0;
    int rc=sqlite3_prepare_v2(db,"SELECT seq,first_wire,second_wire FROM member_faults WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?) AND member_id=?",-1,&st,NULL);
    if(!rc) rc=sqlite3_bind_text(st,1,channel,-1,SQLITE_STATIC);
    if(!rc) rc=sqlite3_bind_blob(st, 2, member, 32, SQLITE_STATIC);
    if(!rc) rc=sqlite3_step(st);
    int found=rc==SQLITE_ROW;
    if(found) {
        if(seq) *seq=(uint64_t)sqlite3_column_int64(st,0);
        uint8_t* outs[]={first,second}; size_t* sizes[]={first_size,second_size};
        for(unsigned i=0;i<2;i++) if(outs[i]) {
            int bytes=sqlite3_column_bytes(st,1+i);
            if(!sizes[i] || bytes<MEMBER_LOG_WIRE_MIN || (size_t)bytes>*sizes[i]) { sqlite3_finalize(st); return failure(db,"fault output capacity"); }
            memcpy(outs[i],sqlite3_column_blob(st,1+i),(size_t)bytes); *sizes[i]=(size_t)bytes;
        }
    }
    int final=sqlite3_finalize(st);
    return (rc==SQLITE_ROW || rc==SQLITE_DONE)&&!final?found:failure(db,"read fault");
}
struct fork_block {
    const uint8_t* wire;
    size_t size;
    const uint8_t* hash;
};

static int decode_fault(sqlite3* db, const char* channel, const uint8_t member[32], uint64_t seq,
    uint8_t wire[2][MEMBER_LOG_WIRE_MAX], const size_t size[2], struct member_log_record records[2]) {
    if (member_log_decode_group(db, channel, wire[0], size[0], member, &records[0]) ||
        member_log_decode_group(db, channel, wire[1], size[1], member, &records[1]) ||
        records[0].seq != seq || records[1].seq != seq || memcmp(records[0].hash, records[1].hash, 32) >= 0)
        return failure(db, "stored fork proof");
    return 0;
}

int member_log_fault_hashes(sqlite3* db, const char* channel, const uint8_t member[32], uint64_t* seq, uint8_t hashes[64]) {
    if (!seq || !hashes) return failure(db, "fault hashes arguments");
    uint8_t wire[2][MEMBER_LOG_WIRE_MAX];
    size_t size[2] = {sizeof(wire[0]), sizeof(wire[1])};
    struct member_log_record records[2];
    memset(hashes, 0, 64);
    int found = member_log_fault(db, channel, member, seq, wire[0], &size[0], wire[1], &size[1]);
    if (found <= 0) return found;
    if (decode_fault(db, channel, member, *seq, wire, size, records)) return -1;
    memcpy(hashes, records[0].hash, 32);
    memcpy(hashes + 32, records[1].hash, 32);
    return 1;
}

static void choose_fork_block(struct fork_block best[2], struct fork_block candidate) {
    int cmp = best[0].hash ? memcmp(candidate.hash, best[0].hash, 32) : -1;
    if (cmp < 0) {
        best[1] = best[0];
        best[0] = candidate;
    } else if (cmp > 0 && (!best[1].hash || memcmp(candidate.hash, best[1].hash, 32) < 0)) {
        best[1] = candidate;
    }
}

/* Все кандидаты подписаны одним автором. Для заданного номера сохраняем два минимальных разных хеша. */
static int merge_fault(sqlite3* db, const char* channel, const uint8_t member[32], uint64_t seq,
    const struct fork_block* incoming, unsigned count) {
    uint8_t saved[2][MEMBER_LOG_WIRE_MAX], local_wire[MEMBER_LOG_WIRE_MAX];
    size_t size[2] = {sizeof(saved[0]), sizeof(saved[1])}, local_size = sizeof(local_wire);
    uint64_t prior_seq = 0;
    int found = member_log_fault(db, channel, member, &prior_seq, saved[0], &size[0], saved[1], &size[1]);
    if (found < 0) return -1;
    if (found && prior_seq < seq) return 0;
    struct member_log_record prior[2], local;
    struct fork_block best[2] = {{0}};
    if (found && prior_seq == seq) {
        if (decode_fault(db, channel, member, seq, saved, size, prior)) return -1;
        for (unsigned i = 0; i < 2; i++) choose_fork_block(best, (struct fork_block){saved[i], size[i], prior[i].hash});
    }
    for (unsigned i = 0; i < count; i++) choose_fork_block(best, incoming[i]);
    int has_local = member_log_get(db, channel, member, seq, local_wire, &local_size);
    if (has_local < 0 || (has_local && member_log_decode_group(db, channel, local_wire, local_size, member, &local))) return -1;
    if (has_local) choose_fork_block(best, (struct fork_block){local_wire, local_size, local.hash});
    if (!best[1].hash) return failure(db, "fork proof missing second block");
    if (prior_seq == seq && !memcmp(best[0].hash, prior[0].hash, 32) && !memcmp(best[1].hash, prior[1].hash, 32)) return 0;

    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO member_faults VALUES((SELECT ed25519_pubkey FROM channels WHERE channel_id=?1),?2,?3,?4,?5)"
        " ON CONFLICT(group_id,member_id) DO UPDATE SET seq=excluded.seq,first_wire=excluded.first_wire,second_wire=excluded.second_wire", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, member, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)seq);
    if (!rc) rc = sqlite3_bind_blob(st, 4, best[0].wire, (int)best[0].size, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 5, best[1].wire, (int)best[1].size, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final) return failure(db, "persist fork proof");
    DEBUG_WARN(DEBUG_CATEGORY_GROUP, "member fork proof staged group=%s member=%s seq=%llu previous=%llu first=%s second=%s",
        channel, sc_public_key_hex(member, (char[65]){0}), (unsigned long long)seq, (unsigned long long)prior_seq,
        sc_public_key_hex(best[0].hash, (char[65]){0}), sc_public_key_hex(best[1].hash, (char[65]){0}));
    return 1;
}

int member_log_prove(sqlite3* db, const char* channel, const uint8_t key[32],
    const uint8_t* first, size_t first_size, const uint8_t* second, size_t second_size) {
    struct member_log_record a, b;
    if (!db || sqlite3_get_autocommit(db) || member_log_decode_group(db, channel, first, first_size, key, &a) ||
        member_log_decode_group(db, channel, second, second_size, key, &b) || a.seq != b.seq || !memcmp(a.hash, b.hash, 32))
        return failure(db, "invalid fork proof");
    struct fork_block blocks[2] = {{first, first_size, a.hash}, {second, second_size, b.hash}};
    return merge_fault(db, channel, key, a.seq, blocks, 2);
}

/* Сравнить два подтверждённых блока одного номера. Идентичность подписи не заменяет
 * проверку содержимого: хеш локального блока пересчитывается из сохранённых байтов. */
static int compare_record(sqlite3* db, const struct member_log_record* remote, const uint8_t key[32],
    const uint8_t* wire, size_t size) {
    uint8_t local_wire[MEMBER_LOG_WIRE_MAX]; size_t local_size = sizeof(local_wire); struct member_log_record local;
    if (member_log_get(db, remote->channel, remote->member, remote->seq, local_wire, &local_size) != 1 ||
        member_log_decode(local_wire, local_size, key, &local)) return failure(db, "verify local conflict evidence");
    if (!memcmp(local.hash, remote->hash, 32)) return 0;
    struct fork_block blocks[2] = {{local_wire, local_size, local.hash}, {wire, size, remote->hash}};
    return merge_fault(db, remote->channel, key, remote->seq, blocks, 2) < 0 ? -1 : MEMBER_LOG_BLOCKED;
}

/* Единственный путь записи после криптографической проверки входа. */
static int accept_checked(sqlite3* db, const struct member_log_record* r, const uint8_t* wire, size_t size,
    const uint8_t key[32], member_log_apply_fn apply, void* context) {
    struct member_log_head head;
    if (member_log_head(db, r->channel, r->member, &head)) return -1;
    if (head.blocked) {
        uint64_t fault;
        if (member_log_fault(db, r->channel, r->member, &fault, NULL, NULL, NULL, NULL) != 1) return failure(db, "blocked head missing proof");
        if (r->seq >= fault) {
            struct fork_block block = {wire, size, r->hash};
            if (r->seq == fault && merge_fault(db, r->channel, key, r->seq, &block, 1) < 0) return -1;
            return MEMBER_LOG_BLOCKED;
        }
    }
    if (r->seq <= head.seq) {
        int conflict = compare_record(db, r, key, wire, size);
        if (!conflict) {
            DEBUG_DEBUG(DEBUG_CATEGORY_GROUP, "member log duplicate group=%s member=%s seq=%llu",
                r->channel, sc_public_key_hex(r->member, (char[65]){0}), (unsigned long long)r->seq);
            return head.blocked ? MEMBER_LOG_BLOCKED : 0;
        }
        return conflict;
    }
    if (head.seq == INT64_MAX || r->seq != head.seq + 1) {
        DEBUG_WARN(DEBUG_CATEGORY_GROUP, "member log unexpected sequence group=%s member=%s head=%llu received=%llu",
            r->channel, sc_public_key_hex(r->member, (char[65]){0}), (unsigned long long)head.seq, (unsigned long long)r->seq);
        return -1;
    }
    if (memcmp(r->previous, head.hash, 32)) {
        char details[192];
        snprintf(details, sizeof(details), "seq=%llu expected=%s received=%s", (unsigned long long)r->seq,
            sc_public_key_hex(head.hash, (char[65]){0}), sc_public_key_hex(r->previous, (char[65]){0}));
        db_changes_warn(db, NULL, r->channel, "Chat block", "previous hash mismatch", r->member, r->signature, details);
        DEBUG_WARN(DEBUG_CATEGORY_GROUP, "member log previous hash differs group=%s member=%s seq=%llu local=%s received=%s",
            r->channel, sc_public_key_hex(r->member, (char[65]){0}), (unsigned long long)r->seq,
            sc_public_key_hex(head.hash, (char[65]){0}), sc_public_key_hex(r->previous, (char[65]){0}));
        return MEMBER_LOG_DIVERGED;
    }
    const char* channel = r->channel; const uint8_t* member = r->member; sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "INSERT INTO member_log(group_id,member_id,seq,prev_hash,block_hash,time_us,scope_id,grant_id,body,signature,applied)"
        " VALUES((SELECT ed25519_pubkey FROM channels WHERE channel_id=?1),?2,?3,?4,?5,?6,?7,?8,?9,?10,0)", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, member, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)r->seq);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 4, r->previous, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 5, r->hash, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 6, (sqlite3_int64)r->time_us);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 7, r->scope, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 8, r->grant, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 9, r->body, (int)r->body_size, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 10, r->signature, 64, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK) return failure(db, "insert record");
    int applied = apply(context, r);
    if (applied < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_GROUP, "member log application deferred/failed group=%s member=%s seq=%llu result=%d; rollback required",
            r->channel, sc_public_key_hex(r->member, (char[65]){0}), (unsigned long long)r->seq, applied);
        return applied == MEMBER_LOG_DEFERRED ? MEMBER_LOG_DEFERRED : -1;
    }
    if (applied > 1) return failure(db, "invalid application result");
    rc = sqlite3_prepare_v2(db, "UPDATE member_log SET applied=?4 WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND member_id=?2 AND seq=?3", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, member, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)r->seq);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(st, 4, applied);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK) return failure(db, "record application result");
    rc = sqlite3_prepare_v2(db, "INSERT INTO member_chains(group_id,member_id,head_seq,head_hash) VALUES((SELECT ed25519_pubkey FROM channels WHERE channel_id=?1),?2,?3,?4)"
        " ON CONFLICT(group_id,member_id) DO UPDATE SET head_seq=excluded.head_seq,head_hash=excluded.head_hash", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, member, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)r->seq);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 4, r->hash, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK) return failure(db, "advance head");
    DEBUG_DEBUG(DEBUG_CATEGORY_GROUP, "member log staged group=%s member=%s seq=%llu applied=%d",
        r->channel, sc_public_key_hex(r->member, (char[65]){0}), (unsigned long long)r->seq, applied);
    return 1;
}

int member_log_accept(sqlite3* db, const struct member_log_record* r, const uint8_t key[32], member_log_apply_fn apply, void* context) {
    if (!db || sqlite3_get_autocommit(db) || !valid(r) || !key || !apply) return failure(db, "accept transaction/arguments");
    uint8_t wire[MEMBER_LOG_WIRE_MAX]; size_t size = sizeof(wire); struct member_log_record verified;
    if (frame(r, wire, &size) || member_log_decode_group(db, r->channel, wire, size, key, &verified) || memcmp(r->hash, verified.hash, 32))
        return failure(db, "accept changed record/hash");
    return accept_checked(db, r, wire, size, key, apply, context);
}

int member_log_accept_wire(sqlite3* db, const char* channel, const uint8_t member[32], const uint8_t* wire, size_t size,
    const uint8_t key[32], member_log_apply_fn apply, void* context) {
    if (!db || sqlite3_get_autocommit(db) || !member || !apply) return failure(db, "accept wire transaction/arguments");
    struct member_log_record r;
    if (member_log_decode_group(db, channel, wire, size, key, &r) || memcmp(r.member, member, 32))
        return failure(db, "accept wire identity/signature");
    return accept_checked(db, &r, wire, size, key, apply, context);
}

int member_log_probe_begin(sqlite3* db, const char* channel, const uint8_t member[32], uint64_t peer_seq, struct member_log_probe* out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!out || peer_seq > INT64_MAX) return failure(db, "probe arguments");
    struct member_log_head head; if (member_log_head(db, channel, member, &head)) return -1;
    strcpy(out->channel, channel); memcpy(out->member, member, 32); out->different = head.seq < peer_seq ? head.seq : peer_seq;
    out->query = out->different;
    return out->query ? 1 : 0;
}

int member_log_probe_reply(sqlite3* db, struct member_log_probe* p, const uint8_t key[32], const uint8_t* wire, size_t size) {
    if (!db || sqlite3_get_autocommit(db) || !p || !p->query || !key) return failure(db, "probe reply transaction/arguments");
    struct member_log_record r;
    if (member_log_decode_group(db, p->channel, wire, size, key, &r) || memcmp(r.member, p->member, 32) || r.seq != p->query)
        return failure(db, "probe reply identity/sequence/signature");
    int rc = compare_record(db, &r, key, wire, size); if (rc < 0) return -1;
    struct member_log_probe next = *p;
    if (!rc) {
        next.same = next.query;
        if (!next.mismatch) { next.query = 0; *p = next; return 0; }
    } else { next.different = next.query; next.mismatch = 1; }
    DEBUG_DEBUG(DEBUG_CATEGORY_GROUP, "member log binary compare group=%s member=%s same=%llu different=%llu",
        p->channel, sc_public_key_hex(p->member, (char[65]){0}),
        (unsigned long long)next.same, (unsigned long long)next.different);
    if (next.different - next.same == 1) { next.query = 0; *p = next; return MEMBER_LOG_BLOCKED; }
    next.query = next.same + (next.different - next.same) / 2; *p = next; return 1;
}

int member_log_tail(sqlite3* db, const char* channel, const uint8_t member[32], uint64_t from,
    uint8_t* out, size_t* size, uint64_t* next, int* more) {
    if (!db || !channel || !*channel || strnlen(channel, 65) == 65 || !member || !from || from > INT64_MAX || !out || !size || !next || !more)
        return failure(db, "tail arguments");
    size_t capacity = *size, used = 0; *size = 0; *more = 0; *next = from;
    sqlite3_stmt* st = NULL;
    uint8_t group_ed[32];
    if (group_key(db, channel, group_ed)) return -1;
    int rc = sqlite3_prepare_v2(db, "SELECT seq,prev_hash,block_hash,time_us,scope_id,grant_id,body,signature FROM member_log"
        " WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND member_id=?2 AND seq>=?3 ORDER BY seq", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, member, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)from);
    if (rc != SQLITE_OK) { sqlite3_finalize(st); return failure(db, "prepare tail"); }
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        struct member_log_record r = {.seq=(uint64_t)sqlite3_column_int64(st, 0),
            .time_us=(uint64_t)sqlite3_column_int64(st, 3), .body=sqlite3_column_blob(st, 6), .body_size=(size_t)sqlite3_column_bytes(st, 6)};
        memcpy(r.member, member, 32);
        memcpy(r.group_ed, group_ed, 32);
        if (r.seq != *next || sqlite3_column_bytes(st, 1) != 32 || sqlite3_column_bytes(st, 2) != 32 ||
            sqlite3_column_bytes(st, 4) != 32 || sqlite3_column_bytes(st, 5) != 32 || sqlite3_column_bytes(st, 7) != 64) { rc = SQLITE_CORRUPT; break; }
        memcpy(r.previous, sqlite3_column_blob(st, 1), 32); memcpy(r.hash, sqlite3_column_blob(st, 2), 32);
        memcpy(r.scope, sqlite3_column_blob(st, 4), 32); memcpy(r.grant, sqlite3_column_blob(st, 5), 32);
        memcpy(r.signature, sqlite3_column_blob(st, 7), 64);
        size_t bytes = MEMBER_LOG_HEADER_SIZE + r.body_size + 64;
        if (bytes + 2 > capacity - used) {
            if (!used) rc = SQLITE_TOOBIG;
            else { *more = 1; rc = SQLITE_DONE; } break;
        }
        size_t available = capacity - used - 2;
        if (frame(&r, out + used + 2, &available)) { rc = SQLITE_CORRUPT; break; }
        uint8_t stored_hash[32];
        if (EVP_Digest(out + used + 2, available - 64, stored_hash, NULL, EVP_sha256(), NULL) != 1 ||
            memcmp(stored_hash, r.hash, 32)) { rc = SQLITE_CORRUPT; break; }
        out[used] = (uint8_t)(available >> 8); out[used + 1] = (uint8_t)available;
        used += available + 2; *next = r.seq + 1;
    }
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK) return failure(db, "read tail/capacity");
    *size = used; return 0;
}
