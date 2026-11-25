/* Подписанные блоки CHAT: rollback, пропуски, revoke/возврат, распространение развилки. */
#include "../src/chat/chat_log_priv.h"
#include "../src/chat/chat_log_sync.c"
#include "../src/group/group_protocol.h"
#include "../src/db_schema.h"
#include "../src/chat/group_info.h"
#include "../src/attachments/manifest.h"
#include "../src/attachments/store.h"
#include "../lib/strbuf.h"
#include "test_membership.h"
#include "test_utils.h"
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#define TEST_CHANNEL "bb5c672482b0dcca91a21a4ed63b15afde8aa1378da72cd01b349589d6e7dd6a"
static const uint8_t group_private[32] = {9};
static void sql(sqlite3* db, const char* query) {
    int rc = sqlite3_exec(db, query, NULL, NULL, NULL);
    if (rc) fprintf(stderr, "%s: %s\n", query, sqlite3_errmsg(db));
    assert(!rc);
}
static void public_key(const uint8_t seed[32], uint8_t key[32]) {
    EVP_PKEY* p = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32);
    size_t n = 32;
    assert(p && EVP_PKEY_get_raw_public_key(p, key, &n) == 1);
    EVP_PKEY_free(p);
}
static void setup_channel(struct UTUN_INSTANCE* inst, struct UASYNC* ua, const char* channel) {
    memset(inst, 0, sizeof(*inst));
    inst->ua = ua;
    assert(!sc_identity_generate(inst->my_keys.public_key, inst->my_keys.private_key, inst->my_ed25519_pubkey, inst->my_ed25519_privkey));
    inst->node_id = sc_derive_node_id_from_pubkey(inst->my_keys.public_key);

    char path[768]; snprintf(path, sizeof(path), "%s/utun-chat-log-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    assert(!sqlite3_open(path, &inst->topo_sqlite_db));
    sql(inst->topo_sqlite_db, "PRAGMA journal_mode=WAL;PRAGMA synchronous=FULL");
    assert(!topo_node_sqlite_init(inst->topo_sqlite_db));
    uint8_t x[32], x_private[32], ed[32], signature[64];
    assert(!sc_identity_from_seed(group_private, x, x_private, ed));
    assert(!test_group_signature(channel, "journal", ed, x, ed, group_private, signature));
    assert(!topo_node_sqlite_channel_put(inst->topo_sqlite_db, channel, "journal", ed, x, NULL, ed, group_private, signature));
    assert(!member_sync_init(inst));
    assert(!chat_feedback_init(inst));
    char table[96]; snprintf(table, sizeof(table), "msg_%s", channel);
    assert(!db_schema_history(inst->topo_sqlite_db, table));
}
static void setup(struct UTUN_INSTANCE* inst, struct UASYNC* ua) { setup_channel(inst, ua, TEST_CHANNEL); }
static void dispose(struct UTUN_INSTANCE* inst) {
    char path[768];
    snprintf(path, sizeof(path), "%s", sqlite3_db_filename(inst->topo_sqlite_db, "main"));
    member_sync_destroy(inst);
    assert(!sqlite3_close(inst->topo_sqlite_db));
    unlink(path);
}
static void seed(struct UTUN_INSTANCE* dst, struct UTUN_INSTANCE* author) {
    assert(test_member_seed(dst, TEST_CHANNEL, author->my_keys.public_key, author->my_ed25519_pubkey, author->my_ed25519_privkey, group_private, "{}", NULL,
                            NULL) >= 0);
}
static sqlite3_int64 number(sqlite3* db, const char* query) {
    sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(db, query, -1, &st, NULL) && sqlite3_step(st) == SQLITE_ROW);
    sqlite3_int64 n = sqlite3_column_int64(st, 0);
    assert(!sqlite3_finalize(st));
    return n;
}
struct journal_trace { int replay, clear_inventory, heads, scopes; };
static int trace_journal(unsigned event, void* arg, void* statement, void* expanded) {
    (void)expanded;
    assert(event == SQLITE_TRACE_STMT);
    struct journal_trace* trace = arg;
    const char* query = sqlite3_sql(statement);
    if (!query) return 0;
    trace->replay += !strncmp(query, "SELECT DISTINCT scope_id FROM member_log", sizeof("SELECT DISTINCT scope_id FROM member_log") - 1);
    trace->clear_inventory += !strncmp(query, "DELETE FROM chat_heads", sizeof("DELETE FROM chat_heads") - 1);
    trace->heads += !strncmp(query, "INSERT INTO chat_heads", sizeof("INSERT INTO chat_heads") - 1);
    trace->scopes += !strncmp(query, "SELECT id,ver,local_attrs,data,flags", sizeof("SELECT id,ver,local_attrs,data,flags") - 1);
    return 0;
}
static void trace_start(sqlite3* db, struct journal_trace* trace) {
    memset(trace, 0, sizeof(*trace));
    assert(!sqlite3_trace_v2(db, SQLITE_TRACE_STMT, trace_journal, trace));
}
static void trace_stop(sqlite3* db) {
    assert(!sqlite3_trace_v2(db, 0, NULL, NULL));
}
static void assert_full_rebuild_matches(sqlite3* db) {
    const char* tables[] = {"chat_heads", "msg_" TEST_CHANNEL, "feedback_" TEST_CHANNEL, "chat_effects"};
    uint8_t before[MT_HASH_SIZE], after[MT_HASH_SIZE];
    char ns[70], query[256];
    snprintf(ns, sizeof(ns), "%u:%s", MT_DATA_CHAT_LOG, TEST_CHANNEL);
    assert(!merkle_tree_get(db, ns, 0, 0, before));
    sql(db, "BEGIN IMMEDIATE");
    for (unsigned i = 0; i < sizeof(tables) / sizeof(*tables); i++) {
        snprintf(query, sizeof(query), "CREATE TEMP TABLE saved_%u AS SELECT * FROM %s", i, tables[i]);
        sql(db, query);
    }
    assert(!cl_rebuild_derived(db, TEST_CHANNEL, "test comparison"));
    assert(!merkle_tree_get(db, ns, 0, 0, after) && !memcmp(before, after, sizeof(before)));
    for (unsigned i = 0; i < sizeof(tables) / sizeof(*tables); i++) {
        snprintf(query, sizeof(query), "SELECT count(*) FROM (SELECT * FROM %s EXCEPT SELECT * FROM saved_%u)", tables[i], i);
        assert(!number(db, query));
        snprintf(query, sizeof(query), "SELECT count(*) FROM (SELECT * FROM saved_%u EXCEPT SELECT * FROM %s)", i, tables[i]);
        assert(!number(db, query));
    }
    sql(db, "ROLLBACK");
}
static void boundary(struct UTUN_INSTANCE* inst, const struct UTUN_INSTANCE* member, const char* head, unsigned epoch, int deleted) {
    char tags[256];
    uint8_t body[512], signature[64];
    snprintf(tags, sizeof(tags), "{\"ver\":\"%u\",\"_message_epoch\":\"%u\",\"_chat_seal\":\"%s\",\"deleted\":\"%s\"}", epoch + 1, epoch, head,
             deleted ? "yes" : "no");
    int n = member_sync_build_owner_msg(TEST_CHANNEL, member->my_ed25519_pubkey, tags, body, sizeof(body));
    assert(n > 0 && !sc_ed25519_sign(group_private, body, n, signature));
    sql(inst->topo_sqlite_db, "BEGIN IMMEDIATE");
    assert(!topo_node_sqlite_member_owner_put(inst->topo_sqlite_db, TEST_CHANNEL, member->my_ed25519_pubkey, tags, signature, 0));
    char query[200];
    snprintf(query, sizeof(query), "UPDATE peers_" TEST_CHANNEL " SET admitted=%d WHERE node_id=%llu", !deleted, (unsigned long long)member->node_id);
    sql(inst->topo_sqlite_db, query);
    assert(!chat_log_rebuild(inst->topo_sqlite_db, TEST_CHANNEL));
    sql(inst->topo_sqlite_db, "COMMIT");
}
static void message(struct UTUN_INSTANCE* author, struct db_sync_record* r, const char* text, int action) {
    static char json[4096];
    snprintf(json, sizeof(json), "{\"n\":%llu,\"ch\":\"" TEST_CHANNEL "\",\"ct\":\"text/plain\",\"d\":\"%s\"}", (unsigned long long)author->node_id, text);
    r->author = author->node_id;
    memcpy(r->author_ed, author->my_ed25519_pubkey, 32);
    r->timestamp = 100;
    r->data = json;
    r->size = strlen(json);
    r->action = action;
    r->revision.edited_at = action == CL_CREATE ? 0 : 200 + r->revision.ver;
    if (action == CL_CREATE) {
        memset(r->revision.nonce, 7, 16);
        assert(!group_policy_message_generation(author->topo_sqlite_db, TEST_CHANNEL, r->author, r->revision.generation));
    }
}
static void append_message(struct UTUN_INSTANCE* author, struct db_sync_record* r) {
    memcpy(r->author_ed, author->my_ed25519_pubkey, 32);
    sql(author->topo_sqlite_db, "BEGIN IMMEDIATE");
    assert(!chat_log_message(author->topo_sqlite_db, TEST_CHANNEL, r, author->my_ed25519_privkey, NULL));
    assert(!member_log_commit(author->topo_sqlite_db));
}
/* Как в Merkle, читаем только разные листья. */
static struct jl_pull* stage_heads(struct UTUN_INSTANCE* from, struct UTUN_INSTANCE* to) {
    sqlite3 *a = from->topo_sqlite_db, *b = to->topo_sqlite_db;
    sqlite3_stmt* st = NULL;
    sql(b, "BEGIN IMMEDIATE");
    void* state = NULL;
    assert(!jl_begin_write(b, TEST_CHANNEL, &state));
    sql(b, "COMMIT");
    assert(!sqlite3_prepare_v2(a, "SELECT DISTINCT bucket FROM chat_heads ORDER BY bucket DESC", -1, &st, NULL));
    while (sqlite3_step(st) == SQLITE_ROW) {
        uint64_t prefix = (uint64_t)sqlite3_column_int64(st, 0) << 39;
        uint8_t source_hash[32], dest_hash[32];
        assert(!merkle_sync_read_hash(from, MT_DATA_CHAT_LOG, TEST_CHANNEL, MT_MAX_LEVEL, prefix, source_hash));
        assert(!merkle_sync_read_hash(to, MT_DATA_CHAT_LOG, TEST_CHANNEL, MT_MAX_LEVEL, prefix, dest_hash));
        if (!memcmp(source_hash, dest_hash, 32)) continue;
        merkle_key_t after = {{0}}, next;
        int av = 0, more;
        do {
            uint8_t page[MT_PAGE_SIZE];
            size_t size = sizeof(page);
            assert(!jl_page_read(a, TEST_CHANNEL, prefix, av, &after, page, &size, &next, &more));
            sql(b, "BEGIN IMMEDIATE");
            int result; assert(!jl_stage_write(b, state, prefix, av, &after, &next, more, page, size, &result) && result == MT_OK);
            sql(b, "COMMIT");
            after = next;
            av = 1;
        } while (more);
    }
    assert(!sqlite3_finalize(st));
    return state;
}
static int transfer_until(struct UTUN_INSTANCE* from, struct UTUN_INSTANCE* to, int pause_on) {
    sqlite3 *a = from->topo_sqlite_db, *b = to->topo_sqlite_db;
    struct jl_pull* pull = stage_heads(from, to);
    int result = 0;
    for (unsigned rounds = 0;; rounds++) {
        assert(rounds < 100);
        sql(b, "BEGIN IMMEDIATE");
        int rc = jl_commit_write(b, pull);
        sql(b, rc ? "ROLLBACK" : "COMMIT");
        if (rc) {
            fprintf(stderr, "CHAT journal pull commit failed step=%u rc=%d\n", rounds, rc);
            result = -1;
            break;
        }
        int kind = pull->fetch.length ? pull->fetch.data[0] : 0;
        if (!kind || kind == pause_on) { result = kind; break; }
        struct merkle_sync_sql_request request = {.action=MT_SQL_SERVE_DEPENDENCY, .fetch=pull->fetch, .capacity=MT_PAGE_SIZE - MS_HEADER};
        strcpy(request.ns, TEST_CHANNEL);
        sql(a, "BEGIN");
        rc = jl_dependency_sql(a, &request);
        sql(a, rc ? "ROLLBACK" : "COMMIT");
        if (rc || !request.length) {
            fprintf(stderr, "CHAT dependency request failed kind=%d rc=%d result=%d\n", kind, rc, request.result);
            result = -1;
            break;
        }
        request.action = MT_SQL_APPLY_DEPENDENCY;
        request.pull = pull;
        sql(b, "BEGIN IMMEDIATE");
        rc = jl_dependency_sql(b, &request);
        sql(b, rc ? "ROLLBACK" : "COMMIT");
        if (rc) {
            fprintf(stderr, "CHAT dependency response refused kind=%d rc=%d result=%d\n", kind, rc, request.result);
            result = -1;
            break;
        }
    }
    sql(b, "BEGIN IMMEDIATE");
    assert(!jl_abort_write(b, pull));
    sql(b, "COMMIT");
    u_free(pull);
    return result;
}
static void transfer(struct UTUN_INSTANCE* from, struct UTUN_INSTANCE* to) {
    assert(!transfer_until(from, to, 0));
}

static void dependency_range(struct UASYNC* ua) {
    struct UTUN_INSTANCE source, receiver;
    setup(&source, ua); setup(&receiver, ua);
    seed(&source, &source); seed(&receiver, &source);
    struct db_sync_record record = {0};
    message(&source, &record, "first", CL_CREATE); append_message(&source, &record);
    message(&source, &record, "second", CL_EDIT); append_message(&source, &record);
    struct jl_pull* pull = stage_heads(&source, &receiver);
    sqlite3* db = receiver.topo_sqlite_db;
    sql(db, "BEGIN IMMEDIATE");
    assert(!jl_commit_write(db, pull) && pull->fetch.data[0] == CL_FETCH_TAIL);
    assert(cl_read64(pull->fetch.data + CL_SUBJECT) == 1 && cl_read64(pull->fetch.data + CL_SUBJECT + 8) == 2);
    sql(db, "COMMIT");
    message(&source, &record, "third", CL_EDIT); append_message(&source, &record);

    struct merkle_sync_sql_request request = {.action=MT_SQL_SERVE_DEPENDENCY, .fetch=pull->fetch, .capacity=MT_PAGE_SIZE - MS_HEADER};
    strcpy(request.ns, TEST_CHANNEL);
    cl_write64(request.fetch.data + CL_SUBJECT, 2);
    assert(!jl_dependency_work(&source, &request));
    request.action = MT_SQL_APPLY_DEPENDENCY; request.pull = pull; request.fetch = pull->fetch;
    assert(jl_dependency_work(&receiver, &request) && request.result == MT_ERR_DATA);
    assert(!number(db, "SELECT count(*) FROM member_log"));

    request.action = MT_SQL_SERVE_DEPENDENCY;
    assert(!jl_dependency_work(&source, &request));
    request.action = MT_SQL_APPLY_DEPENDENCY;
    assert(!jl_dependency_work(&receiver, &request) && number(db, "SELECT max(seq) FROM member_log") == 2);
    abort_pull(&receiver, pull);
    dispose(&source); dispose(&receiver);
    puts("PASS CHAT tail range: wrong first sequence rolls back; appended blocks preserve the requested prefix");
}
static void feedback(struct UTUN_INSTANCE* inst, const uint8_t message[32], const char* emoji, uint64_t expected) {
    struct member_sync_write_context ctx = {.self = inst->node_id, .now = 300000};
    memcpy(ctx.private_key, inst->my_ed25519_privkey, 32);
    memcpy(ctx.public_key, inst->my_ed25519_pubkey, 32);
    struct chat_feedback_record r = {.kind = CHAT_FEEDBACK_REACTION, .data_len = strlen(emoji)};
    memcpy(r.message_id, message, 32);
    memcpy(r.author_ed, inst->my_ed25519_pubkey, 32);
    if (r.data_len) {
        r.data_len = 2;
        r.data[0] = 1;
        r.data[1] = 0;
    }
    sql(inst->topo_sqlite_db, "BEGIN IMMEDIATE");
    assert(!chat_log_feedback(inst->topo_sqlite_db, &ctx, TEST_CHANNEL, &r, NULL));
    assert(!member_log_commit(inst->topo_sqlite_db) && r.ver == expected);
}

static void incremental_updates(struct UASYNC* ua) {
    struct UTUN_INSTANCE source, receiver;
    setup(&source, ua); setup(&receiver, ua);
    seed(&source, &source); seed(&receiver, &source); seed(&receiver, &receiver);
    sqlite3 *left = source.topo_sqlite_db, *right = receiver.topo_sqlite_db;
    struct db_sync_record r = {0}, other = {0};
    struct journal_trace trace;
    message(&receiver, &other, "unrelated", CL_CREATE); append_message(&receiver, &other);
    trace_start(left, &trace);
    message(&source, &r, "body", CL_CREATE); append_message(&source, &r);
    message(&source, &r, "edit", CL_EDIT); append_message(&source, &r);
    trace_stop(left);
    assert(trace.heads == 2 && trace.scopes == 2 && !trace.replay && !trace.clear_inventory);

    trace_start(right, &trace);
    transfer(&source, &receiver);
    trace_stop(right);
    assert(trace.heads == 2 && trace.scopes == 2 && !trace.replay && !trace.clear_inventory);
    assert(number(right, "SELECT updated AND initial FROM chat_effects WHERE version=2") == 1);
    assert(number(right, "SELECT count(*) FROM msg_" TEST_CHANNEL " WHERE json_extract(data,'$.d')='unrelated'") == 1);

    feedback(&source, r.message_id, "ok", 3);
    struct net_job push = {.type=CHAT_LOG_PUSH, .size=MEMBER_LOG_WIRE_MAX};
    strcpy(push.group, TEST_CHANNEL);
    assert(member_log_get(left, TEST_CHANNEL, source.my_ed25519_pubkey, 3, push.input, &push.size) == 1);
    sql(right, "CREATE TEMP TRIGGER fail_head BEFORE UPDATE ON main.chat_heads BEGIN SELECT RAISE(ABORT,'test head write'); END");
    sql(right, "BEGIN IMMEDIATE");
    assert(net_work(right, &push) && !push.changes);
    sql(right, "ROLLBACK");
    assert(number(right, "SELECT max(seq) FROM member_log") == 2 && !number(right, "SELECT count(*) FROM feedback_" TEST_CHANNEL));
    sql(right, "DROP TRIGGER fail_head");

    trace_start(right, &trace);
    sql(right, "BEGIN IMMEDIATE");
    assert(!net_work(right, &push) && push.changes == CL_CHANGE_BLOCK);
    assert(!net_work(right, &push) && !push.changes);
    sql(right, "COMMIT");
    trace_stop(right);
    assert(trace.heads == 1 && trace.scopes == 1 && !trace.replay && !trace.clear_inventory);
    assert(number(right, "SELECT ver FROM feedback_" TEST_CHANNEL) == 3);
    assert_full_rebuild_matches(right);
    dispose(&source); dispose(&receiver);
    puts("PASS CHAT incremental history: local, PUSH, and tail update only affected paths; rollback and full replay agree");
}

static struct jl_pull* stage_seal(sqlite3* source, sqlite3* receiver, const char* seal) {
    uint8_t page[2 + CL_SEAL_SIZE] = {0, CL_SEAL_SIZE};
    sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(source, "SELECT wire FROM chat_seals WHERE lower(hex(id))=?", -1, &st, NULL));
    assert(!sqlite3_bind_text(st, 1, seal, -1, SQLITE_STATIC) && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_bytes(st, 0) == CL_SEAL_SIZE);
    memcpy(page + 2, sqlite3_column_blob(st, 0), CL_SEAL_SIZE);
    assert(!sqlite3_finalize(st));
    merkle_key_t after = {{0}}, next;
    assert(!cl_inventory_key(page + 2, CL_SEAL_SIZE, next.bytes));
    void* pull;
    int result;
    sql(receiver, "BEGIN IMMEDIATE");
    assert(!jl_begin_write(receiver, TEST_CHANNEL, &pull));
    assert(!jl_stage_write(receiver, pull, merkle_sync_level_prefix(cl_read64(next.bytes), MT_MAX_LEVEL), 0, &after, &next, 0, page, sizeof(page),
        &result) && result == MT_OK);
    sql(receiver, "COMMIT");
    return pull;
}

static void staging_budget(struct UASYNC* ua) {
    struct UTUN_INSTANCE a, b;
    setup(&a, ua); setup(&b, ua); seed(&a, &a); seed(&b, &a);
    struct db_sync_record msg = {0};
    message(&a, &msg, "prefix", CL_CREATE); append_message(&a, &msg);
    transfer(&a, &b);
    message(&a, &msg, "continuation", CL_EDIT); append_message(&a, &msg);
    sqlite3* db = b.topo_sqlite_db;
    void* state = NULL;
    assert(!jl_begin_write(db, TEST_CHANNEL, &state));
    struct jl_pull* pull = state;
    uint8_t seal_page[2 + CL_SEAL_SIZE] = {0, CL_SEAL_SIZE, 2};
    merkle_key_t start = {{0}}, next;
    const size_t seal_cost = CL_SEAL_SIZE + 64 + 88;
    pull->staged_bytes = JL_STAGE_LIMIT - seal_cost;
    assert(!cl_inventory_key(seal_page + 2, CL_SEAL_SIZE, next.bytes));
    uint64_t prefix = merkle_sync_level_prefix(cl_read64(next.bytes), MT_MAX_LEVEL);
    assert(stage(&b, pull, prefix, 0, &start, &next, 0, seal_page, sizeof(seal_page)) == MT_OK);
    assert(pull->staged_bytes == JL_STAGE_LIMIT);
    sql(db, "INSERT INTO chat_seal_stage SELECT pull,id,0,zeroblob(32) FROM chat_head_stage");
    uint8_t head_page[2 + CL_HEAD_SIZE] = {0, CL_HEAD_SIZE, 1};
    memcpy(head_page + 3, a.my_ed25519_pubkey, 32);
    assert(!cl_inventory_key(head_page + 2, CL_HEAD_SIZE, next.bytes));
    prefix = merkle_sync_level_prefix(cl_read64(next.bytes), MT_MAX_LEVEL);
    assert(stage(&b, pull, prefix, 0, &start, &next, 0, head_page, sizeof(head_page)) == MT_ERR_LIMIT);
    assert(sqlite3_get_autocommit(db) && pull->staged_bytes == JL_STAGE_LIMIT);
    assert(number(db, "SELECT count(*) FROM chat_head_stage") == 1 && number(db, "SELECT count(*) FROM chat_seal_stage") == 1);
    abort_pull(&b, pull);
    assert(!number(db, "SELECT count(*) FROM chat_head_stage") && !number(db, "SELECT count(*) FROM chat_seal_stage"));
    assert(number(db, "SELECT ver FROM msg_" TEST_CHANNEL) == 1);
    transfer(&a, &b);
    assert(number(db, "SELECT ver FROM msg_" TEST_CHANNEL) == 2);
    dispose(&b); dispose(&a);
    puts("PASS CHAT staging budget counts seal cursor, cleans both tables and resumes from committed prefix");
}

static void ack_framing(void) {
    uint8_t ack[41] = {0};
    assert(!journal_ack_valid(ack, 40));
    cl_write64(ack + 32, (uint64_t)INT64_MAX + 1);
    assert(!journal_ack_valid(ack, 40));
    cl_write64(ack + 32, INT64_MAX);
    assert(journal_ack_valid(ack, 40) && !journal_ack_valid(ack, 39) && !journal_ack_valid(ack, 41));
}
static unsigned history_events;
static void history_event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int size) {
    assert(sqlite3_get_autocommit(inst->topo_sqlite_db));
    assert(type == CHAT_EVT_MSG_RECEIVED && size == 98 && data[0] == 64 && data[97] == 1);
    assert(!memcmp(data + 1, TEST_CHANNEL, 64));
    history_events++;
}
static void seal_projection_changes(struct UASYNC* ua) {
    struct UTUN_INSTANCE source, receiver;
    setup(&source, ua); setup(&receiver, ua);
    seed(&source, &source); seed(&receiver, &source);
    sqlite3 *left = source.topo_sqlite_db, *right = receiver.topo_sqlite_db;
    struct db_sync_record r = {0};
    message(&source, &r, "previous grant", CL_CREATE); append_message(&source, &r);
    uint8_t missing_scope[32] = {9};
    feedback(&source, missing_scope, "ok", 2);
    char first[65], second[65];
    sql(left, "BEGIN IMMEDIATE");
    assert(!chat_log_seal(left, TEST_CHANNEL, source.my_ed25519_pubkey, first));
    sql(left, "COMMIT");
    boundary(&receiver, &source, first, 1, 1);
    struct jl_pull* pull = stage_seal(left, right, first);
    sql(right, "BEGIN IMMEDIATE");
    assert(!jl_commit_write(right, pull) && pull->changed);
    sql(right, "COMMIT");
    abort_pull(&receiver, pull);

    sql(right, "BEGIN IMMEDIATE");
    uint8_t wire[MEMBER_LOG_WIRE_MAX];
    size_t n = sizeof(wire);
    int changes;
    assert(member_log_get(left, TEST_CHANNEL, source.my_ed25519_pubkey, 1, wire, &n) == 1);
    assert(!cl_accept(right, TEST_CHANNEL, wire, n, 1, &changes) && changes == CL_CHANGE_BLOCK);
    sql(right, "COMMIT");
    assert(!number(right, "SELECT count(*) FROM msg_" TEST_CHANNEL));
    assert(jl_finish_read(right, TEST_CHANNEL));
    pull = stage_heads(&source, &receiver);
    sql(right, "BEGIN IMMEDIATE");
    assert(!jl_commit_write(right, pull) && !pull->changed && pull->fetch.data[0] == CL_FETCH_TAIL);
    sql(right, "COMMIT");
    struct merkle_sync_sql_request request = {.action=MT_SQL_SERVE_DEPENDENCY, .fetch=pull->fetch, .capacity=MT_PAGE_SIZE - MS_HEADER};
    strcpy(request.ns, TEST_CHANNEL);
    assert(!jl_dependency_work(&source, &request));
    request.action = MT_SQL_APPLY_DEPENDENCY; request.pull = pull;
    receiver.chat_content_started = 1;
    history_events = 0;
    chat_event_set_handler(&receiver, history_event);
    sql(right, "BEGIN IMMEDIATE");
    assert(!jl_dependency_sql(right, &request) && request.changes == (CL_CHANGE_BLOCK | CL_CHANGE_HISTORY));
    assert(!history_events);
    sql(right, "COMMIT");
    jl_dependency_committed(&receiver, &request);
    assert(history_events == 1 && !jl_finish_read(right, TEST_CHANNEL));
    assert(number(right, "SELECT count(*) FROM msg_" TEST_CHANNEL) == 1);
    assert(number(right, "SELECT count(*) FROM feedback_" TEST_CHANNEL) == 1);
    abort_pull(&receiver, pull);

    boundary(&source, &source, first, 1, 0);
    feedback(&source, missing_scope, "", 3);
    sql(left, "BEGIN IMMEDIATE");
    assert(!chat_log_seal(left, TEST_CHANNEL, source.my_ed25519_pubkey, second));
    sql(left, "COMMIT");
    boundary(&receiver, &source, second, 2, 1);
    assert(!number(right, "SELECT count(*) FROM msg_" TEST_CHANNEL));
    uint8_t before[MT_HASH_SIZE], after[MT_HASH_SIZE];
    assert(!merkle_sync_read_hash(&receiver, MT_DATA_CHAT_LOG, TEST_CHANNEL, 0, 0, before));
    pull = stage_seal(left, right, second);
    sql(right, "BEGIN IMMEDIATE");
    assert(!jl_commit_write(right, pull) && pull->changed && history_events == 1);
    sql(right, "COMMIT");
    publish(&receiver, pull);
    assert(history_events == 2 && jl_finish_read(right, TEST_CHANNEL));
    assert(!merkle_sync_read_hash(&receiver, MT_DATA_CHAT_LOG, TEST_CHANNEL, 0, 0, after) && !memcmp(before, after, sizeof(before)));
    assert(number(right, "SELECT count(*) FROM msg_" TEST_CHANNEL) == 1);
    assert(number(right, "SELECT count(*) FROM feedback_" TEST_CHANNEL) == 1);
    struct journal_trace trace;
    trace_start(right, &trace);
    sql(right, "BEGIN IMMEDIATE");
    assert(!jl_commit_write(right, pull) && !pull->changed);
    sql(right, "COMMIT");
    trace_stop(right);
    publish(&receiver, pull);
    assert(history_events == 2 && !trace.replay && !trace.clear_inventory);
    assert_full_rebuild_matches(right);
    abort_pull(&receiver, pull);
    dispose(&source); dispose(&receiver);
    puts("PASS CHAT seals: checkpoint and previously missing seal link restore history, including with unchanged Merkle root");
}
static unsigned feedback_events;
static uint8_t feedback_message[32];
static void feedback_event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int size) {
    (void)inst;
    assert(type == CHAT_EVT_FEEDBACK_CHANGED && size == 98 && data[0] == 64 && data[97] == CHAT_FEEDBACK_REACTION);
    assert(!memcmp(data + 1, TEST_CHANNEL, 64) && !memcmp(data + 65, feedback_message, 32));
    feedback_events++;
}
static void message_and_feedback_checks(struct UASYNC* ua) {
    struct UTUN_INSTANCE a, b;
    setup(&a, ua); setup(&b, ua);
    seed(&a, &a); seed(&b, &a);
    sqlite3* db = a.topo_sqlite_db;
    struct db_sync_record r = {0};
    message(&a, &r, "original", CL_CREATE);
    r.data = "{\"ct\":\"text/plain\",\"d\":\"original\"}";
    r.size = strlen(r.data);
    append_message(&a, &r);
    const char* edits[] = {"{\"ct\":\"text/plain\"}", "{\"ct\":\"text/plain\",\"d\":{}}",
        "{\"ct\":\"text/plain\",\"d\":\"edited\",\"extra\":true}"};
    sql(db, "BEGIN IMMEDIATE");
    for (unsigned i = 0; i < sizeof(edits) / sizeof(*edits); i++) {
        uint8_t body[128] = {CL_EDIT}, wire[MEMBER_LOG_WIRE_MAX];
        size_t size = sizeof(wire), bytes = strlen(edits[i]);
        int added;
        struct member_log_record block;
        cl_write64(body + 1, r.timestamp);
        memcpy(body + 9, r.revision.nonce, 16);
        memcpy(body + 25, r.revision.generation, 32);
        memcpy(body + 57, edits[i], bytes);
        assert(!member_log_make(db, TEST_CHANNEL, a.my_ed25519_pubkey, 200000 + i, r.message_id, r.revision.generation,
            body, 57 + bytes, a.my_ed25519_privkey, &block));
        assert(!member_log_encode(&block, wire, &size));
        int valid = i == 2;
        assert(cl_accept(db, TEST_CHANNEL, wire, size, 1, &added) == (valid ? 0 : -SQLITE_MISMATCH) && added == valid);
    }
    sql(db, "COMMIT");
    feedback(&a, r.message_id, "ok", 3);
    feedback(&a, r.message_id, "", 4);
    transfer(&a, &b);
    assert(number(b.topo_sqlite_db, "SELECT ver FROM msg_" TEST_CHANNEL) == 1);
    assert(number(b.topo_sqlite_db, "SELECT json_extract(data,'$.d')='original' FROM msg_" TEST_CHANNEL) == 1);
    memcpy(feedback_message, r.message_id, 32);
    chat_event_set_handler(&b, feedback_event);
    for (uint64_t seq = 3; seq <= 4; seq++) {
        uint8_t wire[MEMBER_LOG_WIRE_MAX];
        size_t size = sizeof(wire);
        assert(member_log_get(db, TEST_CHANNEL, a.my_ed25519_pubkey, seq, wire, &size) == 1);
        publish_block(&b, TEST_CHANNEL, wire, size);
        assert(feedback_events == seq - 2);
    }
    dispose(&a); dispose(&b);
    puts("PASS CHAT message validation: reject malformed edit, retain unapplied valid edit, notify reaction and cancellation");
}

static void colliding_projections(struct UASYNC* ua) {
    struct UTUN_INSTANCE a, b, receiver;
    setup(&a, ua); setup(&b, ua); setup(&receiver, ua);
    seed(&a, &a); seed(&b, &b);
    seed(&receiver, &a); seed(&receiver, &b);
    sql(receiver.topo_sqlite_db, "UPDATE peers_" TEST_CHANNEL " SET node_id=17 WHERE record_state=0");
    struct db_sync_record r = {0};
    message(&a, &r, "two authors", CL_CREATE);
    append_message(&a, &r);
    feedback(&a, r.message_id, "ok", 2);
    feedback(&b, r.message_id, "ok", 1);
    transfer(&a, &receiver);
    transfer(&b, &receiver);
    sqlite3* db = receiver.topo_sqlite_db;
    assert(number(db, "SELECT count(*) FROM member_chains") == 2);
    assert(number(db, "SELECT count(DISTINCT author_ed) FROM feedback_" TEST_CHANNEL "") == 2);
    char projected[192];
    snprintf(projected, sizeof(projected), "SELECT count(*) FROM msg_" TEST_CHANNEL " WHERE accepted_key=x'%s'",
        sc_public_key_hex(r.author_ed, (char[65]){0}));
    assert(number(db, projected) == 1);
    dispose(&a); dispose(&b); dispose(&receiver);
    puts("PASS CHAT journal: full signing keys keep independent chains and reactions despite a colliding local projection");
}

static void deleted_attachment_history(struct UASYNC* ua) {
    struct UTUN_INSTANCE a, b; setup(&a, ua); setup(&b, ua); seed(&a, &a); seed(&b, &a);
    uint8_t blocks[16] = {1}, signatures[64] = {0};
    struct media_index_result file = {.media_id={2}, .content_hash={3}, .file_size=1, .block_size=1,
        .num_blocks=1, .block_ids=blocks, .block_sigs=signatures};
    char* body = attachment_manifest_format("file", 4, &file, 0, 0, 0); assert(body);
    struct strbuf json = {0};
    assert(strbuf_addf(&json, "{\"n\":%llu,\"ch\":\"" TEST_CHANNEL "\",\"ct\":\"application/octet-stream\",\"d\":",
        (unsigned long long)a.node_id) >= 0 && !strbuf_add_json_string(&json, body) && strbuf_addf(&json, "}") >= 0);
    struct db_sync_record r = {.author=a.node_id, .timestamp=100, .action=CL_CREATE,
        .data=strbuf_str(&json), .size=json.len};
    r.revision.nonce[0] = 8;
    assert(!group_policy_message_generation(a.topo_sqlite_db, TEST_CHANNEL, a.node_id, r.revision.generation));
    append_message(&a, &r); assert(attachment_store_public_read(a.topo_sqlite_db, TEST_CHANNEL, file.media_id) == 1);
    char tombstone[256];
    snprintf(tombstone, sizeof(tombstone), "{\"n\":%llu,\"ch\":\"" TEST_CHANNEL "\",\"ct\":\"text/plain\",\"d\":\"\",\"deleted\":1}",
        (unsigned long long)a.node_id);
    r.action = CL_DELETE; r.data = tombstone; r.size = strlen(tombstone); r.revision.edited_at = 200; append_message(&a, &r);
    transfer(&a, &b);
    sqlite3* db = b.topo_sqlite_db;
    assert(number(db, "SELECT count(*) FROM attachment_refs WHERE released=1") == 1);
    assert(attachment_store_public_read(db, TEST_CHANNEL, file.media_id) == 1);
    sql(db, "BEGIN IMMEDIATE;DELETE FROM attachment_refs;DELETE FROM attachment_objects");
    assert(!chat_log_prepare(db)); sql(db, "COMMIT");
    assert(number(db, "SELECT count(*) FROM attachment_refs WHERE released=1") == 1);
    assert(attachment_store_public_read(db, TEST_CHANNEL, file.media_id) == 1);
    assert(number(db, "SELECT json_extract(data,'$.deleted') FROM msg_" TEST_CHANNEL "") == 1);
    strbuf_free(&json); u_free(body);

    /* Описание уже зарегистрированного объекта неизменно, даже если сообщение с другим описанием корректно. */
    file.content_hash[0] = 4;
    body = attachment_manifest_format("other", 5, &file, 0, 0, 0); assert(body);
    json = (struct strbuf){0};
    assert(strbuf_addf(&json, "{\"ct\":\"application/octet-stream\",\"d\":") >= 0 &&
        !strbuf_add_json_string(&json, body) && strbuf_addf(&json, "}") >= 0);
    r.action = CL_CREATE; r.data = strbuf_str(&json); r.size = json.len;
    r.revision.nonce[0] = 9; r.revision.edited_at = 0; r.timestamp = 300;
    append_message(&a, &r); transfer(&a, &b);
    assert(number(db, "SELECT count(*) FROM member_log") == 3);
    assert(number(db, "SELECT count(*) FROM msg_" TEST_CHANNEL) == 2);
    assert(number(db, "SELECT count(*) FROM attachment_refs") == 1);
    assert(number(db, "SELECT count(*) FROM attachment_objects WHERE substr(hash,1,1)=x'03'") == 1);
    strbuf_free(&json); u_free(body); dispose(&a); dispose(&b);
    puts("PASS CHAT attachment history: CREATE/DELETE sync and startup rebuild retain publication proof with released reference");
}

/* Принимающий SQL-путь PUSH и Merkle должны одинаково отличать ошибку записи от ошибки проекции. */
static void record_validation(struct UASYNC* ua) {
    struct UTUN_INSTANCE source, live, pull;
    setup(&source, ua); setup(&live, ua); setup(&pull, ua);
    seed(&source, &source); seed(&live, &source); seed(&pull, &source);
    sqlite3 *db = source.topo_sqlite_db, *live_db = live.topo_sqlite_db;
    struct db_sync_record record = {.timestamp = 100, .action = CL_CREATE};
    memcpy(record.author_ed, source.my_ed25519_pubkey, 32);
    assert(!group_policy_key_generation(db, TEST_CHANNEL, record.author_ed, record.revision.generation));
    record.revision.nonce[0] = 1;
    assert(!db_sync_message_id(record.timestamp, record.author_ed, &record.revision, record.message_id));
    uint8_t body[57 + DB_SYNC_MESSAGE_MAX] = {CL_CREATE};
    cl_write64(body + 1, record.timestamp);
    memcpy(body + 9, record.revision.nonce, 16);
    memcpy(body + 25, record.revision.generation, 32);
    const char* data[] = {"{}", "{\"ct\":\"application/octet-stream\",\"d\":\"unrecognized manifest\"}"};
    struct net_job packet = {.type = CHAT_LOG_PUSH};
    strcpy(packet.group, TEST_CHANNEL);
    uint8_t root[32], unchanged[32];
    assert(!merkle_sync_read_hash(&live, MT_DATA_CHAT_LOG, TEST_CHANNEL, 0, 0, root));
    for (unsigned i = 0; i < 2; i++) {
        struct member_log_record block;
        size_t bytes = strlen(data[i]); memcpy(body + 57, data[i], bytes);
        sql(db, "BEGIN IMMEDIATE");
        assert(!member_log_make(db, TEST_CHANNEL, record.author_ed, record.timestamp * 1000, record.message_id, record.revision.generation,
            body, 57 + bytes, source.my_ed25519_privkey, &block));
        packet.size = sizeof(packet.input); assert(!member_log_encode(&block, packet.input, &packet.size));
        if (i) {
            assert(!cl_accept(db, TEST_CHANNEL, packet.input, packet.size, 0, NULL));
            sql(db, "COMMIT");
        } else sql(db, "ROLLBACK");
        sql(live_db, "BEGIN IMMEDIATE");
        int rc = net_work(live_db, &packet);
        assert(rc == (i ? SQLITE_OK : SQLITE_MISMATCH) && !packet.reply && packet.changes == (i ? CL_CHANGE_BLOCK : 0));
        sql(live_db, "COMMIT");
        assert(number(live_db, "SELECT count(*) FROM member_log") == i);
        assert(!number(live_db, "SELECT count(*) FROM msg_" TEST_CHANNEL));
        if (!i) assert(!merkle_sync_read_hash(&live, MT_DATA_CHAT_LOG, TEST_CHANNEL, 0, 0, unchanged) && !memcmp(root, unchanged, 32));
    }
    memset(&record, 0, sizeof(record)); message(&source, &record, "next valid message", CL_CREATE); append_message(&source, &record);
    packet.size = sizeof(packet.input);
    assert(member_log_get(db, TEST_CHANNEL, source.my_ed25519_pubkey, 2, packet.input, &packet.size) == 1);
    sql(live_db, "BEGIN IMMEDIATE"); assert(!net_work(live_db, &packet) && packet.reply == CHAT_LOG_ACK); sql(live_db, "COMMIT");
    transfer(&source, &pull);
    assert(!merkle_sync_read_hash(&source, MT_DATA_CHAT_LOG, TEST_CHANNEL, 0, 0, root));
    struct UTUN_INSTANCE* replicas[] = {&live, &pull};
    for (unsigned i = 0; i < 2; i++) {
        struct UTUN_INSTANCE* replica = replicas[i];
        assert(!merkle_sync_read_hash(replica, MT_DATA_CHAT_LOG, TEST_CHANNEL, 0, 0, unchanged) && !memcmp(root, unchanged, 32));
        char path[768]; snprintf(path, sizeof(path), "%s", sqlite3_db_filename(replica->topo_sqlite_db, "main"));
        assert(!sqlite3_close(replica->topo_sqlite_db) && !sqlite3_open(path, &replica->topo_sqlite_db));
        sqlite3* rebuilt = replica->topo_sqlite_db;
        sql(rebuilt, "BEGIN IMMEDIATE;DELETE FROM msg_" TEST_CHANNEL);
        assert(!chat_log_prepare(rebuilt)); sql(rebuilt, "COMMIT");
        assert(number(rebuilt, "SELECT max(head_seq) FROM member_chains") == 2);
        assert(number(rebuilt, "SELECT count(*) FROM msg_" TEST_CHANNEL) == 1);
        assert(number(rebuilt, "SELECT ver FROM msg_" TEST_CHANNEL) == 2);
        assert(!merkle_sync_read_hash(replica, MT_DATA_CHAT_LOG, TEST_CHANNEL, 0, 0, unchanged) && !memcmp(root, unchanged, 32));
    }
    dispose(&source); dispose(&live); dispose(&pull);
    puts("PASS CHAT validation: reject malformed action, retain unprojected manifest, continue and rebuild PUSH/Merkle history");
}

static int fork_proof_matches(sqlite3* db, const uint8_t author[32], uint64_t seq, const uint8_t first[32], const uint8_t second[32]) {
    uint8_t hashes[64];
    uint64_t fault = 0;
    int found = member_log_fault_hashes(db, TEST_CHANNEL, author, &fault, hashes);
    if (memcmp(first, second, 32) > 0) { const uint8_t* tmp = first; first = second; second = tmp; }
    return found == 1 && fault == seq && !memcmp(hashes, first, 32) && !memcmp(hashes + 32, second, 32);
}

struct fork_variant {
    uint8_t wire[MEMBER_LOG_WIRE_MAX];
    uint8_t hash[32];
    size_t size;
};
static int variant_cmp(const void* a, const void* b) {
    const struct fork_variant* left = a;
    const struct fork_variant* right = b;
    return memcmp(left->hash, right->hash, 32);
}
static void fork_variants(struct UTUN_INSTANCE* a, struct fork_variant* variants, unsigned count, uint64_t seq) {
    uint8_t body[] = {CL_FEEDBACK, CHAT_FEEDBACK_REACTION, 1, 0}, scope[32] = {1}, grant[32];
    assert(!group_policy_key_generation(a->topo_sqlite_db, TEST_CHANNEL, a->my_ed25519_pubkey, grant));
    for (unsigned i = 0; i < count; i++) {
        struct member_log_record block;
        variants[i].size = sizeof(variants[i].wire);
        assert(!member_log_make(a->topo_sqlite_db, TEST_CHANNEL, a->my_ed25519_pubkey, seq * 1000000 + i, scope, grant, body,
            sizeof(body), a->my_ed25519_privkey, &block) && block.seq == seq);
        assert(!member_log_encode(&block, variants[i].wire, &variants[i].size));
        memcpy(variants[i].hash, block.hash, 32);
    }
    qsort(variants, count, sizeof(*variants), variant_cmp);
}
static void fork_priorities(struct UASYNC* ua) {
    struct UTUN_INSTANCE a, b;
    setup(&a, ua); setup(&b, ua);
    seed(&a, &a); seed(&b, &a);
    sqlite3 *left = a.topo_sqlite_db, *right = b.topo_sqlite_db;
    const uint8_t* author = a.my_ed25519_pubkey;
    struct fork_variant early[4], late[2];
    int added;
    sql(left, "BEGIN IMMEDIATE");
    fork_variants(&a, early, 4, 1);
    assert(!cl_accept(left, TEST_CHANNEL, early[3].wire, early[3].size, 1, &added) && added == 1);
    fork_variants(&a, late, 2, 2);
    assert(member_log_prove(left, TEST_CHANNEL, author, late[0].wire, late[0].size, late[1].wire, late[1].size) == 1);
    assert(!cl_reindex(left, TEST_CHANNEL));
    sql(left, "COMMIT");
    sql(right, "BEGIN IMMEDIATE");
    assert(member_log_prove(right, TEST_CHANNEL, author, early[3].wire, early[3].size, early[2].wire, early[2].size) == 1);
    assert(!cl_reindex(right, TEST_CHANNEL));
    sql(right, "COMMIT");

    struct journal_trace trace;
    trace_start(left, &trace);
    transfer(&b, &a);
    trace_stop(left);
    assert(trace.replay == 1 && trace.clear_inventory == 1);
    assert(fork_proof_matches(left, author, 1, early[2].hash, early[3].hash));
    sql(left, "BEGIN IMMEDIATE");
    assert(member_log_prove(left, TEST_CHANNEL, author, late[0].wire, late[0].size, late[1].wire, late[1].size) == 0);
    assert(!cl_accept(left, TEST_CHANNEL, early[0].wire, early[0].size, 1, &added) && added == CL_CHANGE_PROOF);
    sql(left, "COMMIT");
    sql(right, "BEGIN IMMEDIATE");
    assert(!cl_accept(right, TEST_CHANNEL, early[1].wire, early[1].size, 1, &added) && added == CL_CHANGE_PROOF);
    sql(right, "COMMIT");
    assert(fork_proof_matches(left, author, 1, early[0].hash, early[2].hash));
    assert(fork_proof_matches(right, author, 1, early[1].hash, early[2].hash));
    struct merkle_sync_sql_request old = {.action=MT_SQL_SERVE_DEPENDENCY, .capacity=MT_PAGE_SIZE - MS_HEADER};
    strcpy(old.ns, TEST_CHANNEL);
    old.fetch.length = CL_PROOF_QUERY_SIZE; old.fetch.data[0] = CL_FETCH_PROOF;
    memcpy(old.fetch.data + 1, author, 32); cl_write64(old.fetch.data + CL_SUBJECT, 1);
    memcpy(old.fetch.data + CL_SUBJECT + 8, early[2].hash, 32); memcpy(old.fetch.data + CL_SUBJECT + 40, early[3].hash, 32);
    assert(jl_dependency_work(&b, &old) && old.result == MT_ERR_STALE && !old.length);

    trace_start(left, &trace);
    transfer(&b, &a);
    trace_stop(left);
    assert(trace.heads == 1 && !trace.replay && !trace.clear_inventory);
    transfer(&a, &b);
    assert(fork_proof_matches(left, author, 1, early[0].hash, early[1].hash));
    assert(fork_proof_matches(right, author, 1, early[0].hash, early[1].hash));
    struct member_log_head head;
    assert(!cl_valid_head(left, TEST_CHANNEL, author, &head, NULL) && head.blocked && !head.seq);
    assert(!cl_valid_head(right, TEST_CHANNEL, author, &head, NULL) && head.blocked && !head.seq);
    uint8_t a_root[32], b_root[32];
    assert(!merkle_sync_read_hash(&a, MT_DATA_CHAT_LOG, TEST_CHANNEL, 0, 0, a_root));
    assert(!merkle_sync_read_hash(&b, MT_DATA_CHAT_LOG, TEST_CHANNEL, 0, 0, b_root) && !memcmp(a_root, b_root, 32));
    dispose(&a); dispose(&b);
    puts("PASS CHAT fork priority: earliest sequence, two lowest hashes, propagation and equal Merkle roots");
}

/* При разной длине журналов узел должен заморозить цепочку и передать доказательство дальше. */
static int unequal_fork(struct UASYNC* ua, struct UTUN_INSTANCE* source, const uint8_t* alternate, size_t size) {
    struct UTUN_INSTANCE shorter;
    setup(&shorter, ua);
    seed(&shorter, source);
    sqlite3* db = shorter.topo_sqlite_db;
    const uint8_t* author = source->my_ed25519_pubkey;
    struct member_log_head head;
    struct member_log_record original, fork;
    uint8_t original_wire[MEMBER_LOG_WIRE_MAX];
    size_t original_size = sizeof(original_wire);
    assert(!member_log_head(source->topo_sqlite_db, TEST_CHANNEL, author, &head) && head.seq == 4);
    assert(member_log_get(source->topo_sqlite_db, TEST_CHANNEL, author, 3, original_wire, &original_size) == 1);
    assert(!member_log_decode_group(db, TEST_CHANNEL, original_wire, original_size, author, &original) && original.seq == 3);
    assert(!member_log_decode_group(db, TEST_CHANNEL, alternate, size, author, &fork) && fork.seq == 3);
    assert(memcmp(original.hash, fork.hash, 32));
    sql(db, "BEGIN IMMEDIATE");
    for (uint64_t seq = 1; seq <= 2; seq++) {
        uint8_t wire[MEMBER_LOG_WIRE_MAX];
        size_t bytes = sizeof(wire);
        int added;
        assert(member_log_get(source->topo_sqlite_db, TEST_CHANNEL, author, seq, wire, &bytes) == 1);
        assert(!cl_accept(db, TEST_CHANNEL, wire, bytes, 1, &added) && added == 1);
    }
    int added;
    assert(!cl_accept(db, TEST_CHANNEL, alternate, size, 1, &added) && added == 1);
    sql(db, "COMMIT");
    int first = transfer_until(source, &shorter, 0);
    int second = first ? transfer_until(source, &shorter, 0) : first;
    uint64_t fault;
    assert(!cl_valid_head(db, TEST_CHANNEL, author, &head, &fault));
    sqlite3_int64 visible = number(db, "SELECT coalesce(max(ver),0) FROM msg_" TEST_CHANNEL);
    int proof = fork_proof_matches(db, author, 3, original.hash, fork.hash), forwarded = 0;
    if (proof) {
        struct UTUN_INSTANCE third;
        setup(&third, ua);
        seed(&third, source);
        struct member_log_head third_head;
        int transferred = transfer_until(&shorter, &third, 0);
        assert(!cl_valid_head(third.topo_sqlite_db, TEST_CHANNEL, author, &third_head, NULL));
        forwarded = !transferred && third_head.blocked && third_head.seq == 2 &&
            fork_proof_matches(third.topo_sqlite_db, author, 3, original.hash, fork.hash);
        dispose(&third);
    }
    int ok = !second && head.blocked && fault == 3 && head.seq == 2 && visible == 1 && proof && forwarded;
    fprintf(ok ? stdout : stderr,
        "%s CHAT unequal fork: first=%d second=%d fault=%llu frozen=%d safe_head=%llu message_ver=%lld proof=%d forwarded=%d\n",
        ok ? "PASS" : "FAIL", first, second, (unsigned long long)fault, head.blocked, (unsigned long long)head.seq, (long long)visible, proof, forwarded);
    dispose(&shorter);
    return !ok;
}

/* Прерываем при первом запросе цепочки среза и начинаем новый pull с Merkle-сверки. */
static int interrupted_seal(struct UASYNC* ua, struct UTUN_INSTANCE* source, struct UTUN_INSTANCE* author, const char* seal) {
    struct UTUN_INSTANCE receiver;
    setup(&receiver, ua);
    seed(&receiver, author);
    boundary(&receiver, author, seal, 1, 1);
    int interrupted = transfer_until(source, &receiver, CL_FETCH_SEAL);
    assert(jl_finish_read(receiver.topo_sqlite_db, TEST_CHANNEL) < 0);
    int resumed = interrupted == CL_FETCH_SEAL ? transfer_until(source, &receiver, 0) : -1;
    uint8_t source_root[32], receiver_root[32];
    assert(!merkle_sync_read_hash(source, MT_DATA_CHAT_LOG, TEST_CHANNEL, 0, 0, source_root));
    assert(!merkle_sync_read_hash(&receiver, MT_DATA_CHAT_LOG, TEST_CHANNEL, 0, 0, receiver_root));
    int same_root = !memcmp(source_root, receiver_root, 32);
    sqlite3* db = receiver.topo_sqlite_db;
    sqlite3_int64 message_ver = number(db, "SELECT coalesce(max(ver),0) FROM msg_" TEST_CHANNEL);
    sqlite3_int64 feedback_ver = number(db, "SELECT coalesce(max(ver),0) FROM feedback_" TEST_CHANNEL);
    int ok = interrupted == CL_FETCH_SEAL && !resumed && same_root && message_ver == 1 && feedback_ver == 2 && !jl_finish_read(db, TEST_CHANNEL);
    fprintf(ok ? stdout : stderr, "%s CHAT interrupted seal: paused=%d resumed=%d equal_roots=%d message_ver=%lld feedback_ver=%lld\n",
        ok ? "PASS" : "FAIL", interrupted, resumed, same_root, (long long)message_ver, (long long)feedback_ver);
    dispose(&receiver);
    return !ok;
}

/* Срез старше обнаруженной развилки: новый узел проверяет ancestry, затем сам выдаёт proof следующему. */
static int seal_after_fork(struct UASYNC* ua) {
    struct UTUN_INSTANCE a, b, c;
    setup(&a, ua);
    setup(&b, ua);
    setup(&c, ua);
    seed(&a, &a);
    seed(&b, &a);
    seed(&c, &a);
    sqlite3* db = a.topo_sqlite_db;
    struct db_sync_record r = {0};
    message(&a, &r, "safe", CL_CREATE);
    append_message(&a, &r);
    feedback(&a, r.message_id, "ok", 2);
    message(&a, &r, "disputed", CL_EDIT);
    append_message(&a, &r);
    feedback(&a, r.message_id, "", 4);
    uint8_t wire[MEMBER_LOG_WIRE_MAX], body[57 + DB_SYNC_MESSAGE_MAX], grant[32];
    size_t size = sizeof(wire);
    struct member_log_record fork;
    message(&a, &r, "other branch", CL_EDIT);
    body[0] = CL_EDIT;
    cl_write64(body + 1, r.timestamp);
    memcpy(body + 9, r.revision.nonce, 16);
    memcpy(body + 25, r.revision.generation, 32);
    memcpy(body + 57, r.data, r.size);
    memcpy(grant, r.revision.generation, 32);
    sql(db, "BEGIN IMMEDIATE");
    sql(db, "DELETE FROM member_log WHERE seq>=3;UPDATE member_chains SET head_seq=2,head_hash=(SELECT block_hash FROM member_log WHERE seq=2)");
    assert(!member_log_make(db, TEST_CHANNEL, a.my_ed25519_pubkey, 1000000, r.message_id, grant, body, 57 + r.size, a.my_ed25519_privkey, &fork));
    assert(!member_log_encode(&fork, wire, &size));
    sql(db, "ROLLBACK");
    int failed = unequal_fork(ua, &a, wire, size);
    char seal[65];
    sql(db, "BEGIN IMMEDIATE");
    assert(!chat_log_seal(db, TEST_CHANNEL, a.my_ed25519_pubkey, seal));
    sql(db, "COMMIT");
    boundary(&a, &a, seal, 1, 1);
    boundary(&b, &a, seal, 1, 1);
    boundary(&c, &a, seal, 1, 1);
    sql(db, "BEGIN IMMEDIATE");
    int added = 0;
    assert(!cl_accept(db, TEST_CHANNEL, wire, size, 1, &added) && added == (CL_CHANGE_PROOF | CL_CHANGE_HISTORY));
    sql(db, "COMMIT");
    transfer(&a, &b);
    transfer(&b, &c);
    assert(number(b.topo_sqlite_db, "SELECT max(seq) FROM member_log") == 2 && number(c.topo_sqlite_db, "SELECT max(seq) FROM member_log") == 2);
    assert(number(b.topo_sqlite_db, "SELECT count(*) FROM chat_seal_evidence") == 2 &&
           number(c.topo_sqlite_db, "SELECT count(*) FROM chat_seal_evidence") == 2);
    assert(number(c.topo_sqlite_db, "SELECT ver FROM msg_" TEST_CHANNEL "") == 1 && number(c.topo_sqlite_db, "SELECT ver FROM feedback_" TEST_CHANNEL "") == 2);
    failed += interrupted_seal(ua, &b, &a, seal);
    dispose(&a);
    dispose(&b);
    dispose(&c);
    puts("PASS closed checkpoint beyond fork: verified safe prefix survives through two fresh peers");
    return failed;
}
static void invitation_audit(struct UASYNC* ua) {
    struct UTUN_INSTANCE member, replica;
    setup(&member, ua); setup(&replica, ua);
    member.group_started = 1;
    sql(member.topo_sqlite_db, "CREATE TRIGGER audit_fault BEFORE INSERT ON member_log BEGIN SELECT RAISE(ABORT,'audit unavailable'); END");
    seed(&member, &member); seed(&replica, &member);
    assert(topo_node_sqlite_member_allowed(member.topo_sqlite_db, TEST_CHANNEL, member.node_id));
    assert(number(member.topo_sqlite_db, "SELECT count(*) FROM member_log") == 0);
    sql(member.topo_sqlite_db, "DROP TRIGGER audit_fault");
    member_sync_audit_invitation(&member, TEST_CHANNEL, NULL);
    member_sync_audit_invitation(&member, TEST_CHANNEL, NULL);
    uint8_t wire[MEMBER_LOG_WIRE_MAX], group_ed[32], gx[32], join[64], json[MEMBER_SIGN_MSG_SIZE];
    size_t size = sizeof(wire); struct member_log_record block;
    assert(member_log_get(member.topo_sqlite_db, TEST_CHANNEL, member.my_ed25519_pubkey, 1, wire, &size) == 1);
    assert(!member_log_decode(wire, size, member.my_ed25519_pubkey, &block));
    assert(block.body_size > MEMBER_LOG_INVITATION_PREFIX && block.body[0] == MEMBER_LOG_INVITATION);
    assert(!topo_node_sqlite_channel_get(member.topo_sqlite_db, TEST_CHANNEL, NULL, 0, NULL, gx, group_ed, NULL));
    assert(!memcmp(block.body + 1, group_ed, 32));
    uint64_t stamp;
    assert(!topo_node_sqlite_member_get_join(member.topo_sqlite_db, TEST_CHANNEL, member.my_ed25519_pubkey, join, &stamp));
    int n = member_sync_build_sign_msg(TEST_CHANNEL, 0, member.my_keys.public_key, member.my_ed25519_pubkey,
        TEST_JOIN_ID(gx, group_ed, member.my_ed25519_pubkey, member.my_keys.public_key), stamp, json, sizeof(json));
    assert(n > 0 && block.body_size == MEMBER_LOG_INVITATION_PREFIX + (size_t)n && !memcmp(json, block.body + MEMBER_LOG_INVITATION_PREFIX, n));
    assert(!sc_ed25519_verify(group_ed, json, n, block.body + 33));
    uint8_t roots[2][32]; assert(!merkle_sync_read_hash(&member, MT_DATA_MEMBERS, TEST_CHANNEL, 0, 0, roots[0]));
    uint8_t envelope[MEMBER_LOG_INVITATION_PREFIX + MEMBER_SIGN_MSG_SIZE] = {MEMBER_LOG_INVITATION}, sig[2][64];
    memcpy(envelope + 1, group_ed, 32);
    for (unsigned i = 0; i < 2; i++) {
        n = member_sync_build_sign_msg(TEST_CHANNEL, 0, member.my_keys.public_key, member.my_ed25519_pubkey,
            TEST_JOIN_ID(gx, group_ed, member.my_ed25519_pubkey, member.my_keys.public_key), stamp+1+i,
            envelope + MEMBER_LOG_INVITATION_PREFIX, MEMBER_SIGN_MSG_SIZE);
        assert(n > 0 && !sc_ed25519_sign(group_private, envelope + MEMBER_LOG_INVITATION_PREFIX, n, sig[i]));
    }
    /* Второе приглашение гарантированно проиграет. Оба журналируются ровно один раз. */
    unsigned first = memcmp(sig[0], sig[1], 64) < 0 ? 0 : 1;
    for (unsigned pass = 0; pass < 4; pass++) {
        unsigned i = first ^ (pass & 1);
        assert(member_sync_put(&member, TEST_CHANNEL, member.my_keys.public_key, member.my_ed25519_pubkey, join,
            stamp+1+i, NULL, 0, NULL, NULL, NULL, 0, 0, sig[i]) >= 0);
    }
    assert(number(member.topo_sqlite_db, "SELECT count(*) FROM member_log WHERE substr(body,1,1)=x'05'") == 3);
    assert(!merkle_sync_read_hash(&member, MT_DATA_MEMBERS, TEST_CHANNEL, 0, 0, roots[1]));
    assert((memcmp(sig[first], block.body + 33, 64) < 0) == !!memcmp(roots[0], roots[1], 32));
    transfer(&member, &replica);
    assert(number(replica.topo_sqlite_db, "SELECT count(*) FROM member_log WHERE substr(body,1,1)=x'05'") == 3);
    assert(number(replica.topo_sqlite_db, "SELECT count(*) FROM msg_" TEST_CHANNEL "") == 0);
    dispose(&member); dispose(&replica);
}
static void canonical_history_scope(struct UASYNC* ua) {
    struct UTUN_INSTANCE source, replica;
    setup(&source, ua); setup_channel(&replica, ua, TEST_CHANNEL);
    seed(&source, &source);
    assert(test_member_seed(&replica, TEST_CHANNEL, source.my_keys.public_key, source.my_ed25519_pubkey, source.my_ed25519_privkey,
        group_private, "{}", NULL, NULL) >= 0);
    struct db_sync_record record = {0};
    message(&source, &record, "same group", CL_CREATE);
    append_message(&source, &record);
    uint8_t wire[MEMBER_LOG_WIRE_MAX]; size_t size = sizeof(wire);
    assert(member_log_get(source.topo_sqlite_db, TEST_CHANNEL, source.my_ed25519_pubkey, 1, wire, &size) == 1);
    sqlite3* db = replica.topo_sqlite_db;
    int added = 0;
    sql(db, "BEGIN IMMEDIATE");
    assert(!cl_accept(db, TEST_CHANNEL, wire, size, 1, &added) && added == 1);
    sql(db, "COMMIT");
    assert(chat_log_message_check(db, TEST_CHANNEL, &record) == 1);
    assert(number(db, "SELECT count(*) FROM msg_" TEST_CHANNEL " WHERE json_extract(data,'$.d')='same group'") == 1);
    dispose(&source); dispose(&replica);
    puts("PASS CHAT signed history resolves the same public key on both nodes");
}
int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_WARN);
    debug_enable_console(1);
    struct UASYNC* ua = uasync_create();
    assert(ua);
    ack_framing();
    staging_budget(ua);
    invitation_audit(ua);
    canonical_history_scope(ua);
    struct UTUN_INSTANCE a, b, c;
    setup(&a, ua);
    setup(&b, ua);
    setup(&c, ua);
    seed(&a, &a);
    seed(&b, &a);
    seed(&c, &a);
    sqlite3* db = a.topo_sqlite_db;
    struct db_sync_record r = {0};
    message(&a, &r, "original", CL_CREATE);
    sql(db, "BEGIN IMMEDIATE");
    assert(!chat_log_message(db, TEST_CHANNEL, &r, a.my_ed25519_privkey, NULL));
    sql(db, "ROLLBACK");
    assert(!number(db, "SELECT count(*) FROM member_log") && !number(db, "SELECT count(*) FROM chat_heads"));
    append_message(&a, &r);
    assert(r.revision.ver == 1 && chat_log_message_check(db, TEST_CHANNEL, &r) == 1);
    feedback(&a, r.message_id, "ok", 2);
    transfer(&a, &b);
    transfer(&a, &b);
    assert(number(b.topo_sqlite_db, "SELECT ver FROM msg_" TEST_CHANNEL "") == 1 && number(b.topo_sqlite_db, "SELECT ver FROM feedback_" TEST_CHANNEL "") == 2);
    char seal[65];
    sql(b.topo_sqlite_db, "BEGIN IMMEDIATE");
    assert(!chat_log_seal(b.topo_sqlite_db, TEST_CHANNEL, a.my_ed25519_pubkey, seal));
    sql(b.topo_sqlite_db, "COMMIT");
    message(&a, &r, "excluded edit", CL_EDIT);
    append_message(&a, &r);
    assert(r.revision.ver == 3);
    feedback(&a, r.message_id, "", 4);
    transfer(&a, &b);
    assert(number(b.topo_sqlite_db, "SELECT ver FROM msg_" TEST_CHANNEL "") == 3);
    boundary(&b, &a, seal, 1, 1);
    assert(number(b.topo_sqlite_db, "SELECT ver FROM msg_" TEST_CHANNEL "") == 1);
    assert(number(b.topo_sqlite_db, "SELECT ver FROM feedback_" TEST_CHANNEL "") == 2);
    transfer(&b, &a);
    boundary(&a, &a, seal, 1, 1);
    boundary(&a, &a, seal, 1, 0);
    message(&a, &r, "returned", CL_EDIT);
    append_message(&a, &r);
    assert(r.revision.ver == 5);
    boundary(&b, &a, seal, 1, 0);
    transfer(&a, &b);
    assert(number(b.topo_sqlite_db, "SELECT ver FROM msg_" TEST_CHANNEL "") == 5);
    /* Две подписи seq=5 создаются без изменения сохранённого журнала. */
    uint8_t original[MEMBER_LOG_WIRE_MAX], alternative[MEMBER_LOG_WIRE_MAX], body[57 + DB_SYNC_MESSAGE_MAX], grant[32];
    size_t first_size = sizeof(original), second_size = sizeof(alternative);
    assert(member_log_get(db, TEST_CHANNEL, a.my_ed25519_pubkey, 5, original, &first_size) == 1);
    struct member_log_record fork;
    message(&a, &r, "fork", CL_EDIT);
    body[0] = CL_EDIT;
    cl_write64(body + 1, r.timestamp);
    memcpy(body + 9, r.revision.nonce, 16);
    memcpy(body + 25, r.revision.generation, 32);
    memcpy(body + 57, r.data, r.size);
    sql(db, "BEGIN IMMEDIATE");
    sql(db, "DELETE FROM member_log WHERE seq=5;UPDATE member_chains SET head_seq=4,head_hash=(SELECT block_hash FROM member_log WHERE seq=4)");
    assert(!group_policy_message_generation(db, TEST_CHANNEL, a.node_id, grant));
    assert(!member_log_make(db, TEST_CHANNEL, a.my_ed25519_pubkey, 900000, r.message_id, grant, body, 57 + r.size, a.my_ed25519_privkey, &fork));
    assert(!member_log_encode(&fork, alternative, &second_size));
    sql(db, "ROLLBACK");
    sql(db, "BEGIN IMMEDIATE");
    int added = 0;
    assert(!cl_accept(db, TEST_CHANNEL, alternative, second_size, 1, &added));
    sql(db, "COMMIT");
    assert(number(db, "SELECT seq FROM member_faults") == 5 && number(db, "SELECT ver FROM msg_" TEST_CHANNEL "") == 1);
    message(&a, &r, "after compromise", CL_EDIT);
    sql(db, "BEGIN IMMEDIATE");
    assert(chat_log_message(db, TEST_CHANNEL, &r, a.my_ed25519_privkey, NULL));
    sql(db, "ROLLBACK");
    transfer(&a, &b);
    transfer(&b, &c); /* C получает proof прежде отсутствовавшего префикса. */
    assert(number(c.topo_sqlite_db, "SELECT seq FROM member_faults") == 5 && number(c.topo_sqlite_db, "SELECT max(seq) FROM member_log") == 4);
    boundary(&c, &a, seal, 1, 0);
    assert(number(c.topo_sqlite_db, "SELECT ver FROM msg_" TEST_CHANNEL "") == 1);
    /* Реакция на ещё отсутствующее сообщение не останавливает линейную цепочку. */
    seed(&c, &c);
    uint8_t pending[32] = {99};
    feedback(&c, pending, "ok", 1);
    assert(number(c.topo_sqlite_db, "SELECT count(*) FROM feedback_" TEST_CHANNEL "") == 2);
    dispose(&a);
    dispose(&b);
    dispose(&c);
    fork_priorities(ua);
    dependency_range(ua);
    incremental_updates(ua);
    seal_projection_changes(ua);
    message_and_feedback_checks(ua);
    int failed = seal_after_fork(ua);
    deleted_attachment_history(ua);
    record_validation(ua);
    colliding_projections(ua);
    uasync_destroy(ua, 1);
    puts("PASS CHAT journal baseline: atomic rollback, one sequence, head/tail sync, revoke/readmission, independent fork proof, pending target");
    if (failed) fprintf(stderr, "FAIL CHAT journal: %d regression checks failed\n", failed);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
