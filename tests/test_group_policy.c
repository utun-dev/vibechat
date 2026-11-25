#define TEST_CHANNEL "41048d12a17c404084bdef649765aa5001cc268c7e9392795a23730e07952b36"
/* Реальные подписи, SQLite, Merkle страницы и одна реализация расчёта допуска. */
#include "test_join_id.h"
#include "../src/chat/member_sync.c"
#include "test_member_staging.h"
#include "../src/chat/db_sync.c"
#include "../src/chat/merkle_sync.c"
#include "../src/chat/chat_poll.h"
#include "../lib/db_async.h"
#include <assert.h>
#include <pthread.h>

static pthread_t loop_thread;
static int forbid_loop_sql;
static __thread int projection_fault;
static __thread int fail_admin_parse;
int __real_json_flat_parse(const char*, json_flat_cb, void*);
int __wrap_json_flat_parse(const char* json, json_flat_cb cb, void* arg) {
    if (fail_admin_parse && json && strstr(json, "\"admin\"")) { fail_admin_parse = 0; return -1; }
    return __real_json_flat_parse(json, cb, arg);
}
static __thread const char* author_fault_prefix;
static __thread int author_fault_code, author_fault_point;
static __thread unsigned author_fault_skip;
static __thread int block_retry_cleanup;
static int author_fault(const char* sql, int point) {
    if (!sql || !author_fault_prefix || point != author_fault_point || strncmp(sql, author_fault_prefix, strlen(author_fault_prefix))) return 0;
    if (author_fault_skip) { author_fault_skip--; return 0; }
    author_fault_prefix = NULL; return author_fault_code;
}
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int length, sqlite3_stmt** st, const char** tail) {
    assert(!pthread_equal(loop_thread, pthread_self()) || !forbid_loop_sql);
    int failure = author_fault(sql, 0); if (failure) { *st = NULL; return failure; }
    const char* target = projection_fault == 1 ? "UPDATE peers_" TEST_CHANNEL " SET admitted=" : projection_fault == 3 ?
        "SELECT node_id,x25519_pubkey,ed25519_pubkey,deleted,admitted,identity_verified,adm_tags" : "INSERT INTO archive_msg_" TEST_CHANNEL " SELECT *";
    if (projection_fault && !strncmp(sql, target, strlen(target))) {
        int rc = projection_fault == 2 ? SQLITE_IOERR : SQLITE_BUSY; projection_fault = 0; *st = NULL; return rc;
    }
    return __real_sqlite3_prepare_v2(db, sql, length, st, tail);
}
int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int injected = st ? author_fault(sqlite3_sql(st), 1) : 0; int rc = __real_sqlite3_finalize(st);
    return injected ? injected : rc;
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*fn)(void*, int, char**, char**), void* arg, char** error) {
    assert(!pthread_equal(loop_thread, pthread_self()) || !forbid_loop_sql);
    int failure = author_fault(sql, 2);
    if (failure && block_retry_cleanup) {
        assert(!strcmp(sql, "COMMIT"));
        block_retry_cleanup = 0;
        author_fault_prefix = "DELETE FROM member_sync_records WHERE receive_id=";
        author_fault_code = SQLITE_BUSY;
    }
    if (failure) return failure;
    return __real_sqlite3_exec(db, sql, fn, arg, error);
}

static uint8_t group_x[32] = {81}, group_private[32] = {82}, group_public[32];
struct person { uint64_t id; uint8_t x[32], ed[32], private[32], join[64], tree[64], role[64]; };
static struct person people[11];

static void public_key(const uint8_t private[32], uint8_t public[32]) {
    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, private, 32); assert(key);
    size_t size = 32; assert(EVP_PKEY_get_raw_public_key(key, public, &size) == 1); EVP_PKEY_free(key);
}

static void setup(struct UTUN_INSTANCE* inst, struct UASYNC* ua, int owner) {
    memset(inst, 0, sizeof(*inst)); inst->ua = ua; inst->node_id = people[owner ? 0 : 1].id;
    assert(sqlite3_open(":memory:", &inst->topo_sqlite_db) == SQLITE_OK);
    assert(!topo_node_sqlite_init(inst->topo_sqlite_db));
    uint8_t description[GROUP_INFO_MESSAGE_MAX], signature[64]; char json[GROUP_INFO_MAX + 1];
    assert(!group_info_initial("policy", json));
    int size = group_info_message(TEST_CHANNEL, json, people[0].ed, group_x, group_public, description, sizeof(description));
    assert(size > 0 && !sc_ed25519_sign(group_private, description, (size_t)size, signature));
    assert(!topo_node_sqlite_channel_put(inst->topo_sqlite_db, TEST_CHANNEL, "policy", people[0].ed,
        group_x, NULL, group_public, owner ? group_private : NULL, signature));
    assert(!member_sync_init(inst));
}

static void put_person(struct UTUN_INSTANCE* inst, unsigned i, unsigned parent) {
    struct person* p = &people[i];
    const char* role = i == 1 ? "{\"ver\":\"1\",\"admin\":\"e1\"}" : NULL;
    uint8_t body[128], update[64];
    if (role) {
        int n = member_sync_build_owner_msg(TEST_CHANNEL, p->ed, role, body, sizeof(body)); assert(n > 0);
        assert(sc_ed25519_sign(group_private, body, n, p->role) == SC_OK);
    }
    assert(member_sync_sign_join(i ? people[parent].private : group_private, TEST_CHANNEL, i ? people[parent].ed : NULL, p->x, p->ed,
        TEST_JOIN_ID(group_x, group_public, p->ed, p->x), 1, p->tree) == 0);
    int bytes = member_sync_build_update_msg(TEST_JOIN_ID(group_x, group_public, p->ed, p->x), 1, "{\"name\":\"member\"}", body, sizeof(body));
    assert(bytes > 0 && !sc_ed25519_sign(p->private, body, bytes, update));
    assert(member_sync_put(inst, TEST_CHANNEL, p->x, p->ed, p->join, 1, update, 1, "{\"name\":\"member\"}",
        role, role ? p->role : NULL, 0, i ? people[parent].ed : NULL, p->tree) >= 0);
}

static void token(struct UTUN_INSTANCE* inst, char out[65]) {
    uint8_t hash[32]; assert(!merkle_sync_read_hash(inst, MT_DATA_MEMBERS, TEST_CHANNEL, 0, 0, hash));
    for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", hash[i]);
}

static int admitted(struct UTUN_INSTANCE* inst, unsigned person) {
    int expected = topo_node_sqlite_member_allowed(inst->topo_sqlite_db, TEST_CHANNEL, people[person].id);
    assert(inst->group_access);
    assert(group_policy_member_access(inst, TEST_CHANNEL, people[person].id, 0) == expected);
    return expected;
}

/* Независимый profile READ обязан заново проверить owner-подпись и текущие ключи, без project перед ним. */
static void check_invite_proof_refresh(struct UTUN_INSTANCE* inst) {
    sqlite3* db = inst->topo_sqlite_db; char forged_role[192], replaced_member[192];
    snprintf(forged_role, sizeof(forged_role), "UPDATE peers_" TEST_CHANNEL " SET adm_tags_sig=CAST(x'01'||zeroblob(63) AS BLOB) WHERE node_id=%lld",
        (long long)people[1].id);
    snprintf(replaced_member, sizeof(replaced_member), "UPDATE peers_" TEST_CHANNEL " SET ed25519_pubkey=zeroblob(32) WHERE node_id=%lld",
        (long long)people[1].id);
    const char* mutations[] = {forged_role, replaced_member, "UPDATE channels SET ed25519_pubkey=zeroblob(32) WHERE channel_id='" TEST_CHANNEL "'",
        "UPDATE channels SET owner_node_id=NULL WHERE channel_id='" TEST_CHANNEL "'"};
    for (unsigned i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
        assert(group_policy_can_invite(inst, TEST_CHANNEL, people[1].ed));
        assert(!sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL));
        if (i == 2) {
            assert(sqlite3_exec(db, mutations[i], NULL, NULL, NULL) == SQLITE_CONSTRAINT);
            assert(!sqlite3_exec(db, "PRAGMA ignore_check_constraints=ON", NULL, NULL, NULL));
        }
        assert(!sqlite3_exec(db, mutations[i], NULL, NULL, NULL));
        if (i == 2) assert(!sqlite3_exec(db, "PRAGMA ignore_check_constraints=OFF", NULL, NULL, NULL));
        struct group_policy_cache* fresh = NULL; assert(!group_policy_snapshot_read(db, TEST_CHANNEL, &fresh) && fresh);
        group_policy_snapshot_publish(inst, fresh); inst->topo_sqlite_db = NULL;
        assert(!group_policy_can_invite(inst, TEST_CHANNEL, people[1].ed));
        if (i >= 2) assert(!group_policy_can_invite(inst, TEST_CHANNEL, people[0].ed));
        inst->topo_sqlite_db = db; assert(!sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL));
        assert(!group_policy_snapshot_read(db, TEST_CHANNEL, &fresh)); group_policy_snapshot_publish(inst, fresh);
        assert(group_policy_can_invite(inst, TEST_CHANNEL, people[1].ed));
    }
}

/* Точный IOERR/CORRUPT не теряется в enum даже если SQLite connection по-прежнему возвращает OK. */
static void check_author_errors(struct UTUN_INSTANCE* inst) {
    uint8_t join[64], sentinel[64]; memset(sentinel, 0x59, sizeof(sentinel)); memcpy(join, sentinel, sizeof(join));
    uint64_t epoch = 77; int found;
    for (int point = 0; point < 2; point++) {
        author_fault_point = point; author_fault_skip = 0; author_fault_code = point ? SQLITE_LOCKED : SQLITE_BUSY;
        author_fault_prefix = "SELECT join_sig, join_ts FROM";
        assert(topo_node_sqlite_member_join_read(inst->topo_sqlite_db, TEST_CHANNEL, people[0].ed, join, &epoch, &found) == author_fault_code);
        assert(!author_fault_prefix && !found && epoch == 77 && !memcmp(join, sentinel, sizeof(join)));
    }
    assert(!topo_node_sqlite_member_join_read(inst->topo_sqlite_db, TEST_CHANNEL, (uint8_t[32]){255}, join, &epoch, &found));
    assert(!found && epoch == 77 && !memcmp(join, sentinel, sizeof(join)));
    assert(!topo_node_sqlite_member_join_read(inst->topo_sqlite_db, TEST_CHANNEL, people[0].ed, join, &epoch, &found));
    assert(found && epoch == 1 && !memcmp(join, people[0].join, sizeof(join)));
    const char* prefixes[] = {"SELECT x25519_pubkey,ed25519_pubkey,signed_by,signature",
        "SELECT x25519_pubkey,ed25519_pubkey FROM channels", "SELECT x25519_pubkey,ed25519_pubkey FROM channels",
        "SELECT 1 FROM peers_" TEST_CHANNEL " WHERE ed25519_pubkey"};
    for (unsigned query = 0; query < 4; query++) for (int point = 0; point < 2; point++) {
        unsigned member = query == 2 ? 0 : query == 3 ? 3 : 1; uint8_t key[32], unchanged[32]; memset(key, 0x59, 32); memcpy(unchanged, key, 32);
        author_fault_point = point; author_fault_skip = query == 2 ? 1 : 0;
        author_fault_code = point ? SQLITE_CORRUPT : SQLITE_IOERR; author_fault_prefix = prefixes[query]; int code = SQLITE_OK;
        enum ms_author_status status = member_sync_author_key_ex(inst->topo_sqlite_db, TEST_CHANNEL, people[member].id, key, &code);
        if (status != MS_AUTHOR_ERROR) DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "author fault fixture query=%u point=%d member=%u status=%d code=%d armed=%d",
            query, point, member, status, code, !!author_fault_prefix);
        assert(status == MS_AUTHOR_ERROR);
        assert(!author_fault_prefix && code == author_fault_code && !memcmp(key, unchanged, 32));
        assert(member_sync_author_key_ex(inst->topo_sqlite_db, TEST_CHANNEL, people[member].id, key, &code) == MS_AUTHOR_ACTIVE);
        assert(code == SQLITE_OK && !memcmp(key, people[member].ed, 32));
    }
}
static void check_member_projection_errors(struct UTUN_INSTANCE* inst) {
    sqlite3* db = inst->topo_sqlite_db; struct member_saved saved;
    assert(member_read_saved_row(db, TEST_CHANNEL, 0, people[0].ed, 0, &saved) == 1);
    const char* json = "{\"name\":\"projection test\"}"; uint8_t body[MS_USERINFO_MAX + 73], signature[64];
    uint64_t version = saved.rec.update_ts + 1;
    int size = member_sync_build_update_msg(TEST_JOIN_ID(group_x, group_public, saved.ed, saved.x), version, json, body, sizeof(body));
    assert(size > 0 && !sc_ed25519_sign(people[0].private, body, (size_t)size, signature));
    struct ms_member_rec record = saved.rec; record.userinfo = json; record.update_ts = version; record.update_sig = signature;
    const char* queries[] = {"INSERT INTO nodes(node_id, name, x25519_pubkey", "SELECT addr_type FROM node_addresses WHERE node_x=?"};
    for (unsigned query = 0; query < 2; query++) for (int point = 0; point < 2; point++) {
        struct member_sync_write_context context = {.self=inst->node_id, .now=2000000};
        assert(!sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL));
        author_fault_prefix = queries[query]; author_fault_point = point; author_fault_skip = 0;
        author_fault_code = point ? SQLITE_CORRUPT : SQLITE_IOERR;
        assert(member_sync_record_write(db, &context, TEST_CHANNEL, NULL, &record) == MT_ERR_IO);
        assert(!author_fault_prefix && context.sqlite_code == author_fault_code);
        assert(!sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL));
        struct member_saved current; assert(member_read_saved_row(db, TEST_CHANNEL, 0, people[0].ed, 0, &current) == 1);
        assert(current.rec.update_ts == saved.rec.update_ts && !strcmp(current.info, saved.info));
    }
    puts("PASS member node projection: exact IOERR/CORRUPT prepare/finalize errors survive cleanup and preserve rollback");
}

/* Снимок остаётся работоспособен вообще без SQL handle, в том числе для отозванных удостоверений. */
static void check_access_snapshot(struct UTUN_INSTANCE* inst) {
    assert(inst->group_access);
    struct group_policy_cache* copied = NULL;
    assert(group_policy_snapshot_read(inst->topo_sqlite_db, TEST_CHANNEL, &copied) == SQLITE_OK && copied);
    group_policy_snapshot_publish(inst, copied);
    uint8_t sentinel[32]; memset(sentinel, 0x7a, sizeof(sentinel));
    for (unsigned i = 0; i < 8; i++) {
        uint8_t x[32], ed[32], actual_x[32], actual_ed[32]; sqlite3* db = inst->topo_sqlite_db;
        int allowed = topo_node_sqlite_member_allowed(db, TEST_CHANNEL, people[i].id);
        int transport = topo_node_sqlite_member_transport_allowed(db, TEST_CHANNEL, people[i].id);
        for (int active = 0; active <= 1; active++) {
            int expected = topo_node_sqlite_member_keys(db, TEST_CHANNEL, people[i].id, active, x, ed);
            assert(expected != TOPO_SQLITE_LOOKUP_ERROR);
            inst->topo_sqlite_db = NULL;
            memset(actual_x, 0x7a, 32); memset(actual_ed, 0x7a, 32);
            int result = group_policy_member_keys(inst, TEST_CHANNEL, people[i].id, active, actual_x, actual_ed);
            assert(result == expected);
            if (!result) assert(!memcmp(x, actual_x, 32) && !memcmp(ed, actual_ed, 32));
            else assert(!memcmp(actual_x, sentinel, 32) && !memcmp(actual_ed, sentinel, 32));
            assert(group_policy_member_access(inst, TEST_CHANNEL, people[i].id, 0) == allowed);
            assert(group_policy_member_access(inst, TEST_CHANNEL, people[i].id, 1) == transport);
            assert(group_policy_member_key_allowed(inst, TEST_CHANNEL, people[i].x, people[i].ed, 0) == allowed);
            assert(!group_policy_member_key_allowed(inst, TEST_CHANNEL, people[i].x, sentinel, 0));
            assert(!topo_node_sqlite_member_key_allowed(db, TEST_CHANNEL, people[i].x, sentinel, 0));
            assert(!group_policy_member_access(inst, "9999", people[i].id, 0));
            inst->topo_sqlite_db = db;
        }
    }
}

static void page(struct UTUN_INSTANCE* src, struct UTUN_INSTANCE* dst, void* pull, uint64_t prefix) {
    uint8_t data[8192]; size_t size = sizeof(data); int more; merkle_key_t after = {{0}}, next;
    assert(!member_view_page(src->topo_sqlite_db, TEST_CHANNEL, prefix, 0, &after, data, &size, &next, &more,
        ((struct member_pull*)pull)->admin) && !more);
    assert(!member_stage_page(dst, pull, prefix, 0, &after, &next, more, data, size));
}

static void page_members(struct UTUN_INSTANCE* src, struct UTUN_INSTANCE* dst, void* pull) {
    sqlite3_stmt* leaves = NULL;
    char tree[70]; snprintf(tree, sizeof(tree), "%u:%s",
        ((struct member_pull*)pull)->admin ? MT_DATA_MEMBER_ADMIN : MT_DATA_MEMBERS, TEST_CHANNEL);
    assert(!sqlite3_prepare_v2(src->topo_sqlite_db,
        "SELECT prefix64 FROM merkle_tree_hash WHERE namespace=? AND level=5 AND prefix64<>0", -1, &leaves, NULL));
    assert(!sqlite3_bind_text(leaves, 1, tree, -1, SQLITE_STATIC));
    int rc;
    while ((rc = sqlite3_step(leaves)) == SQLITE_ROW) page(src, dst, pull, (uint64_t)sqlite3_column_int64(leaves, 0));
    assert(rc == SQLITE_DONE && !sqlite3_finalize(leaves));
}

static void complete(struct UTUN_INSTANCE* inst, void* pull) {
    assert(!member_commit_pull(inst, pull)); member_publish_pull(inst, pull); member_abort_pull(inst, pull);
}

static struct DB_SYNC_INSTANCE* history(struct UTUN_INSTANCE* inst) {
    struct DB_SYNC* db = u_calloc(1, sizeof(*db)); assert(db);
    struct DB_SYNC_INSTANCE* si = u_calloc(1, sizeof(*si)); assert(si);
    db->inst = inst; db->db = inst->topo_sqlite_db; db->enabled = 1; inst->db_sync = db;
    db->instances = u_calloc(1, sizeof(*db->instances)); assert(db->instances);
    db->instances[0] = si; db->instance_count = db->instance_capacity = 1;
    si->db_sync = db; si->enabled = 1; si->group_id = 1001; si->next_id = 1;
    strcpy(si->table_name, "msg_" TEST_CHANNEL ""); strcpy(si->ns, TEST_CHANNEL);
    assert(!si_table_init(si) && !chat_feedback_ensure_channel(inst->topo_sqlite_db, TEST_CHANNEL) && !db_sync_register_model(inst));
    assert(!db_sync_verify(si) && !chat_feedback_init(inst)); return si;
}

static struct db_record message(struct UTUN_INSTANCE* inst, uint64_t timestamp) {
    struct db_record r = {.timestamp = timestamp, .author = people[4].id, .data = "accepted history", .data_len = 16, .sig_len = 64,
        .revision = {.nonce = {1}, .ver = 1}};
    assert(!group_policy_message_generation(inst->topo_sqlite_db, TEST_CHANNEL, r.author, r.revision.generation));
    memcpy(r.accepted_key, people[4].ed, 32);
    assert(!db_sync_sign_message(inst->topo_sqlite_db, TEST_CHANNEL, timestamp, r.accepted_key, &r.revision, r.data, r.data_len, people[4].private, r.signature));
    return r;
}

static int archive_count(struct UTUN_INSTANCE* inst) {
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(inst->topo_sqlite_db, "SELECT count(*) FROM archive_msg_" TEST_CHANNEL "", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW); int count = sqlite3_column_int(st, 0); assert(sqlite3_finalize(st) == SQLITE_OK); return count;
}

static void owner_block(struct UTUN_INSTANCE* inst, const char* tags) {
    uint8_t body[256], signature[64]; struct person* p = &people[5];
    int length = member_sync_build_owner_msg(TEST_CHANNEL, p->ed, tags, body, sizeof(body)); assert(length > 0);
    assert(sc_ed25519_sign(group_private, body, length, signature) == SC_OK);
    assert(member_sync_put(inst, TEST_CHANNEL, p->x, p->ed, NULL, 0, NULL, 0, NULL, tags, signature, 0, 0, NULL) >= 0);
}

static void owner_equal(struct UTUN_INSTANCE* a, struct UTUN_INSTANCE* b, const char* expected) {
    struct UTUN_INSTANCE* nodes[] = {a, b};
    for (unsigned i = 0; i < 2; i++) {
        sqlite3_stmt* st = NULL;
        assert(sqlite3_prepare_v2(nodes[i]->topo_sqlite_db, "SELECT adm_tags FROM peers_" TEST_CHANNEL " WHERE node_id=?", -1, &st, NULL) == SQLITE_OK);
        assert(sqlite3_bind_int64(st, 1, (sqlite3_int64)people[5].id) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
        assert(!strcmp((const char*)sqlite3_column_text(st, 0), expected)); assert(sqlite3_finalize(st) == SQLITE_OK);
    }
}

static int projection_veto(void* arg) { (void)arg; return 1; }
static void projection_ready(void* arg, const struct db_async_result* result) {
    assert(pthread_equal(loop_thread, pthread_self()) && !result->status); int* done = arg; assert(!(*done)++);
}
static void projection_wait(struct UASYNC* ua, int* done) {
    uint64_t end = get_time_us() + 10000000;
    while (!*done && get_time_us() < end) uasync_poll(ua, 1);
    assert(*done == 1);
}
struct bootstrap_job {
    uint64_t self;
    struct member_sync_prepared* prepared;
    int done, status, code, veto, fault;
};
static int bootstrap_work(sqlite3* db, void* arg) {
    struct bootstrap_job* job = arg; assert(!pthread_equal(loop_thread, pthread_self()));
    sqlite3_commit_hook(db, NULL, NULL); projection_fault = job->fault;
    int code = member_sync_prepare(db, job->self, &job->prepared);
    assert(!projection_fault);
    if (!code && job->veto) sqlite3_commit_hook(db, projection_veto, NULL);
    return code;
}
static void bootstrap_done(void* arg, const struct db_async_result* result) {
    struct bootstrap_job* job = arg; assert(pthread_equal(loop_thread, pthread_self()) && !job->done++);
    job->status = result->status; job->code = result->sqlite_code;
}
static void bootstrap_run(struct UASYNC* ua, struct db_async* db, struct bootstrap_job* job) {
    member_sync_prepared_free(job->prepared); job->prepared = NULL; job->done = 0;
    db_async_run(db, &(struct db_async_task){.name="member-bootstrap", .mode=DB_ASYNC_WRITE,
        .work=bootstrap_work, .done=bootstrap_done, .data=job}); projection_wait(ua, &job->done);
}
static void bootstrap_worker_regression(struct UASYNC* ua, struct UTUN_INSTANCE* source) {
    char path[512]; snprintf(path, sizeof(path), "%s/utun-members-bootstrap-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    int fd = mkstemp(path); assert(fd >= 0); close(fd); sqlite3* copy = NULL;
    assert(sqlite3_open(path, &copy) == SQLITE_OK);
    sqlite3_backup* backup = sqlite3_backup_init(copy, "main", source->topo_sqlite_db, "main"); assert(backup);
    assert(sqlite3_backup_step(backup, -1) == SQLITE_DONE && !sqlite3_backup_finish(backup));
    assert(!sqlite3_exec(copy, "INSERT INTO peers_" TEST_CHANNEL "(node_id,x25519_pubkey,ed25519_pubkey,join_sig,join_ts,record_state)"
        " SELECT node_id,x25519_pubkey,ed25519_pubkey,join_sig,join_ts,2 FROM peers_" TEST_CHANNEL " WHERE record_state=0 LIMIT 1;"
        " INSERT INTO merkle_tree_hash(namespace,level,prefix64,hash) VALUES('1:preserve',0,0,zeroblob(32))", NULL, NULL, NULL));
    char sql[256]; snprintf(sql, sizeof(sql), "UPDATE peers_" TEST_CHANNEL " SET join_sig=randomblob(64) WHERE node_id=%lld AND record_state=0",
        (long long)people[5].id); assert(!sqlite3_exec(copy, sql, NULL, NULL, NULL)); assert(!sqlite3_close(copy));
    forbid_loop_sql = 1; int ready = 0;
    struct db_async* db = db_async_create(ua, path, 16, projection_ready, &ready); assert(db); projection_wait(ua, &ready);
    struct bootstrap_job job = {.self=source->node_id, .veto=1}; bootstrap_run(ua, db, &job);
    assert(job.status == DB_ASYNC_SQL && (job.code & 255) == SQLITE_CONSTRAINT && job.prepared);
    job.veto = 0; job.fault = 3; bootstrap_run(ua, db, &job);
    assert(job.status == DB_ASYNC_SQL && job.code == SQLITE_BUSY && !job.prepared);
    struct bootstrap_job invalid = {0}; bootstrap_run(ua, db, &invalid);
    assert(invalid.status == DB_ASYNC_SQL && invalid.code == SQLITE_MISUSE && !invalid.prepared);
    job.fault = 0; bootstrap_run(ua, db, &job); assert(!job.status && job.prepared);
    struct UTUN_INSTANCE runtime = {.ua=ua, .node_id=source->node_id, .db_worker=db, .group_started=1};
    assert(!member_sync_activate(&runtime, job.prepared)); member_sync_prepared_free(job.prepared); job.prepared = NULL;
    assert(runtime.msync && !runtime.topo_sqlite_db && group_policy_can_invite(&runtime, TEST_CHANNEL, people[0].ed));
    assert(!group_policy_member_access(&runtime, TEST_CHANNEL, people[5].id, 1)); member_sync_destroy(&runtime);
    assert(!db_async_destroy(db)); forbid_loop_sql = 0;
    assert(sqlite3_open(path, &copy) == SQLITE_OK); sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(copy, "SELECT (SELECT count(*) FROM peers_" TEST_CHANNEL " WHERE record_state<>0),"
        "(SELECT count(*) FROM peers_" TEST_CHANNEL " WHERE record_state=0),(SELECT count(*) FROM merkle_tree_hash WHERE namespace='1:preserve')",
        -1, &st, NULL) && sqlite3_step(st) == SQLITE_ROW);
    assert(!sqlite3_column_int(st, 0) && sqlite3_column_int(st, 1) == 5 && sqlite3_column_int(st, 2) == 1);
    assert(!sqlite3_finalize(st) && !sqlite3_close(copy) && !remove(path));
    puts("PASS member bootstrap FILE/RW: no loop SQL/runtime, invalid record purge, staging cleanup, rollback/retry, independent indexes");
}

struct member_round_job {
    struct merkle_sync_sql_request request;
    void* scope;
    int done, status, code, veto, fail_snapshot;
    const char* fault_sql; int fault_code, fault_point; unsigned fault_skip;
};
static int member_round_work(sqlite3* db, void* data) {
    struct member_round_job* job = data; assert(!pthread_equal(loop_thread, pthread_self()));
    sqlite3_commit_hook(db, NULL, NULL);
    if (job->fail_snapshot) projection_fault = 3;
    if (job->fault_sql) {
        author_fault_prefix = job->fault_sql; author_fault_point = job->fault_point;
        author_fault_skip = job->fault_skip; author_fault_code = job->fault_code;
    }
    int code = member_sql_work(db, job->scope, &job->request);
    assert(!projection_fault && !author_fault_prefix);
    if (!code && job->veto) sqlite3_commit_hook(db, projection_veto, NULL);
    return code;
}
static void member_round_done(void* data, const struct db_async_result* result) {
    struct member_round_job* job = data; assert(pthread_equal(loop_thread, pthread_self()) && !job->done++);
    job->status = result->status; job->code = result->sqlite_code;
}
static void member_round_run(struct UTUN_INSTANCE* runtime, struct member_round_job* job) {
    assert(!job->scope && !member_sql_snapshot(runtime, TEST_CHANNEL, &job->scope)); job->done = 0;
    strcpy(job->request.ns, TEST_CHANNEL);
    db_async_run(runtime->db_worker, &(struct db_async_task){.name="member-frozen-round",
        .mode=job->request.action == MT_SQL_PAGE ? DB_ASYNC_READ : DB_ASYNC_WRITE,
        .work=member_round_work, .done=member_round_done, .data=job}); projection_wait(runtime->ua, &job->done);
    if (!job->status) member_sql_publish(runtime, job->scope, &job->request);
    member_sql_free(job->scope); job->scope = NULL;
}
static int clipboard_events, expected_clipboard;
static void clipboard_event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int size) {
    if (type == CHAT_EVT_CHANNEL_UPDATED) {
        assert(size == 65 && data[0] == 64 && !memcmp(data + 1, TEST_CHANNEL, 64));
        assert(group_policy_clipboard_members(inst, TEST_CHANNEL) == expected_clipboard);
        clipboard_events++;
    }
}

/* Проверяем обе ранние ветки commit: пустой лист и страница с прежним мембером. */
static void clipboard_description_rounds(struct UTUN_INSTANCE* source, struct UTUN_INSTANCE* receiver, const char* path) {
    chat_event_set_handler(receiver, clipboard_event); clipboard_events = 0;
    sqlite3* disk = NULL; assert(!sqlite3_open_v2(path, &disk, SQLITE_OPEN_READONLY, NULL));
    for (int round = 1; round <= 8; round++) {
        struct group_info_record info;
        snprintf(info.json, sizeof(info.json), "{\"name\":\"policy\",\"group_type\":\"chat\",\"ver\":%d,\"clipboard_members\":%s}",
            round + 1, round == 1 || round == 6 ? "true" : "false");
        uint8_t body[GROUP_INFO_MESSAGE_MAX];
        int n = group_info_message(TEST_CHANNEL, info.json, people[0].ed, group_x, group_public, body, sizeof(body));
        assert(n > 0 && !sc_ed25519_sign(group_private, body, n, info.signature));
        uint64_t prefix = 0; /* Описание всегда находится в выделенном листе. */
        struct member_round_job page = {.request={.action=MT_SQL_STAGE_PAGE, .prefix=prefix, .length=MT_PAGE_SIZE}};
        forbid_loop_sql = 0; sqlite3* db = source->topo_sqlite_db;
        assert(!sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) && group_info_apply(db, TEST_CHANNEL, &info) == 1);
        assert(!_member_get_page(source, TEST_CHANNEL, prefix, 0, &page.request.after, page.request.data, &page.request.length,
            &page.request.next, &page.request.more) && !page.request.more);
        assert(!sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL)); forbid_loop_sql = 1;

        struct member_round_job begin = {.request={.action=MT_SQL_BEGIN_PULL, .round=round}};
        memcpy(begin.request.peer, source->my_ed25519_pubkey, 32);
        member_round_run(receiver, &begin); assert(!begin.status && begin.request.pull);
        void* pull = begin.request.pull;
        page.request.pull = pull;
        member_round_run(receiver, &page); assert(!page.status);
        expected_clipboard = round < 5;
        struct member_round_job commit = {.request={.action=MT_SQL_COMMIT_PULL, .pull=pull},
            .veto=round == 2, .fail_snapshot=round == 6,
            .fault_sql=round == 3 || round == 4 ? "SELECT description_json,signature,owner_node_id" : NULL,
            .fault_code=round == 3 ? SQLITE_AUTH : SQLITE_LOCKED, .fault_point=round == 4};
        if (round == 7 || round == 8) {
            commit.fault_sql = round == 7 ? "UPDATE channels SET description_json=" : "SELECT ed25519_pubkey FROM channels";
            commit.fault_point = round == 7; commit.fault_code = SQLITE_BUSY; commit.fault_skip = 0;
        }
        member_round_run(receiver, &commit);
        if (round == 2) assert(commit.status == DB_ASYNC_SQL && (commit.code & 255) == SQLITE_CONSTRAINT);
        else if (round == 3 || round == 4 || round >= 7)
            assert(commit.status == DB_ASYNC_SQL && commit.code == commit.fault_code && commit.request.result == MT_ERR_IO);
        else if (round == 6) assert(commit.status == DB_ASYNC_SQL && commit.code == SQLITE_BUSY && commit.request.result == MT_ERR_IO);
        else assert(!commit.status);
        assert(group_policy_clipboard_members(receiver, TEST_CHANNEL) == expected_clipboard && clipboard_events == (round < 5 ? 1 : 2));
        forbid_loop_sql = 0;
        struct group_info_record saved; struct group_info_profile profile;
        assert(!group_info_get(disk, TEST_CHANNEL, &saved) && !group_info_parse(disk, saved.json, &profile));
        assert(profile.clipboard_members == expected_clipboard && profile.version == (round < 5 ? 2 : 6));
        forbid_loop_sql = 1;
        struct member_round_job abort = {.request={.action=MT_SQL_ABORT_PULL, .pull=pull}};
        member_round_run(receiver, &abort); assert(!abort.status); member_pull_free(pull);
    }
    assert(!sqlite3_close(disk)); chat_event_set_handler(receiver, NULL);
}

/* Запрещаем чтение/запись конкретного поля на соединении, которое исполняет MEMBERS. */
struct page_denial { const char* field; const char* table; int action, hits, done; };
static int deny_page_field(void* data, int action, const char* table, const char* field, const char* schema, const char* trigger) {
    struct page_denial* deny = data; (void)schema; (void)trigger;
    if (action == deny->action && table && field && !strcmp(table, deny->table ? deny->table : "channels") && !strcmp(field, deny->field)) {
        deny->hits++; return SQLITE_DENY;
    }
    return SQLITE_OK;
}
static int install_page_denial(sqlite3* db, void* data) {
    struct page_denial* deny = data;
    return sqlite3_set_authorizer(db, deny->field ? deny_page_field : NULL, deny->field ? deny : NULL);
}
static void page_denial_done(void* data, const struct db_async_result* result) {
    assert(result->status == DB_ASYNC_OK); ((struct page_denial*)data)->done++;
}
static void set_page_denial(struct UTUN_INSTANCE* receiver, struct page_denial* deny) {
    if (!receiver->db_worker) { assert(!install_page_denial(receiver->topo_sqlite_db, deny)); return; }
    deny->done = 0;
    db_async_run(receiver->db_worker, &(struct db_async_task){.name="MEMBERS page authorizer", .mode=DB_ASYNC_READ,
        .work=install_page_denial, .done=page_denial_done, .data=deny});
    projection_wait(receiver->ua, &deny->done);
}

static struct ll_entry* member_wire_frame(struct ms_session* s, uint8_t type, uint32_t request, size_t payload) {
    uint16_t size = (uint16_t)(MS_HEADER + payload);
    struct ll_entry* entry = ll_alloc_lldgram(size); assert(entry); entry->len = size;
    uint8_t* d = entry->dgram; memset(d, 0, size);
    d[0] = s->ms->svc_id; memcpy(d + MS_SCOPE_OFF, s->group_key, 32); d[MS_TYPE_OFF] = type; ms_write64(d + MS_ROUND_OFF, s->round);
    ms_write32(d + MS_REQUEST_OFF, request); d[MS_DATASET_OFF] = MT_DATA_MEMBERS;
    memcpy(d + MS_ROUND_ID_OFF, s->round_id, MS_TOKEN_SIZE); memcpy(d + MS_ACCEPT_ID_OFF, s->accept_id, MS_TOKEN_SIZE);
    return entry;
}
static struct ll_entry* member_wire_reply(struct ETCP_CONN* conn) {
    uint64_t until = get_time_us() + (getenv("UTUN_TEST_MEMBERS_LARGE") ? 30000000 : 5000000);
    while (!conn->send_input_q->head && get_time_us() < until) uasync_poll(conn->instance->ua, 1);
    struct ll_entry* sent = queue_data_get(conn->send_input_q); assert(sent); return sent;
}

static void member_timeout_retry(struct ETCP_CONN* conn, uint64_t group_id) {
    struct UTUN_INSTANCE* inst = conn->instance;
    for (int leader = 0; leader <= 1; leader++) {
        struct ms_session* s = ms_create(inst->msync, conn, NULL, MT_DATA_MEMBERS, group_id, MS_EXCHANGE);
        assert(s); s->leader = leader;
        assert(!member_sync_start(inst, conn, TEST_CHANNEL, NULL, NULL));
        struct ll_entry* packet = member_wire_reply(conn);
        int type = leader ? MS_BEGIN : MS_WAKE;
        assert(packet->dgram[MS_TYPE_OFF] == type);
        queue_dgram_free(packet); queue_entry_free(packet);
        uint64_t round = s->round;
        assert(s->state == (leader ? MS_BEGIN_WAIT : MS_WAKE_WAIT) && s->deadline);
        ms_cancel_deadline(s);
        ms_timeout(s);
        assert(s->state == MS_FAILED && s->restart);
        packet = member_wire_reply(conn);
        assert(packet->dgram[MS_TYPE_OFF] == type && (!leader || s->round > round));
        queue_dgram_free(packet); queue_entry_free(packet);
        merkle_sync_cancel(inst, MT_DATA_MEMBERS, conn->peer_ed25519_pubkey, TEST_CHANNEL);
    }
}
static int stalled_member_page(void* ctx, const char* ns, uint64_t prefix, int valid, const merkle_key_t* after,
    uint8_t* buf, size_t* len, merkle_key_t* next, int* more) {
    (void)ctx; (void)ns; (void)prefix; assert(valid && *len); *next = *after; *more = 1; buf[0] = 0; *len = 1; return 0;
}
static int stalled_member_sql(sqlite3* db, void* scope, struct merkle_sync_sql_request* req) {
    (void)db; (void)scope; assert(req->action == MT_SQL_PAGE); req->length = req->capacity;
    return stalled_member_page(NULL, req->ns, req->prefix, req->after_valid, &req->after, req->data, &req->length, &req->next, &req->more);
}
static void member_wire_query_results(struct ETCP_CONN* conn, struct merkle_sync_data_ops* ops, sqlite3* inspect) {
    struct UTUN_INSTANCE* inst = conn->instance;
    struct merkle_sync_sql_ops stalled = member_sql_ops; stalled.work = stalled_member_sql;
    for (int i = 0; i < 5; i++) {
        struct ms_session* s = ms_create(inst->msync, conn, NULL, MT_DATA_MEMBERS, topo_groups_channel_slot(inst->topo_groups, TEST_CHANNEL), MS_EXCHANGE); assert(s);
        s->round = 20 + i; ms_write64(s->round_id, s->round); memset(s->accept_id, 8, MS_TOKEN_SIZE);
        s->state = MS_SERVE; s->served_root_known = 1;
        assert(!merkle_tree_get(inspect, "0:" TEST_CHANNEL, 0, 0, s->served_root));
        struct ll_entry* entry = i == 4 ? member_wire_frame(s, MS_SOURCE_CHECK, 0, MT_HASH_SIZE)
            : member_wire_frame(s, MS_QUERY, 1, MS_QUERY_SIZE);
        uint8_t* query = entry->dgram + MS_HEADER;
        if (i == 4) memcpy(query, s->served_root, MT_HASH_SIZE);
        else { query[0] = MT_MAX_LEVEL; query[9] = 1; }
        if (i == 0) query[10] = 0x80; /* Курсор другого листа. */
        if (i >= 3) {
            /* Источник опубликовал изменение после чтения первой ветви. */
            s->served_root[0] ^= 1;
            if (i == 4) query[0] ^= 1;
        }
        if (i == 1) { ops->sql = &stalled; ops->get_page = stalled_member_page; }
        struct page_denial deny = {.table="member_sync_items", .field="wire", .action=SQLITE_READ};
        if (i == 2) set_page_denial(inst, &deny);
        ms_receive(conn, entry); struct ll_entry* sent = member_wire_reply(conn);
        int expected = i == 0 ? MT_ERR_PROTOCOL : i >= 3 ? MT_ERR_STALE : MT_ERR_IO;
        assert(sent->dgram[MS_TYPE_OFF] == MS_ERROR && sent->len == MS_HEADER + 4 && ms_read32(sent->dgram + MS_HEADER) == (uint32_t)-expected);
        if (i == 2) assert(s->state == MS_FAILED && !s->restart);
        queue_dgram_free(sent); queue_entry_free(sent);
        if (i == 2) { struct page_denial clear = {0}; set_page_denial(inst, &clear); assert(deny.hits > 0); }
        ops->sql = &member_sql_ops; ops->get_page = _member_get_page;
        merkle_sync_cancel(inst, MT_DATA_MEMBERS, conn->peer_ed25519_pubkey, TEST_CHANNEL);
    }
}

static int allow_member_recovery(void* ctx, const char* ns, struct ETCP_CONN* conn) {
    (void)ctx; (void)ns; (void)conn; return 1;
}

static void member_wire_readonly(struct ETCP_CONN* conn, sqlite3* inspect) {
    struct UTUN_INSTANCE* inst = conn->instance, source;
    setup(&source, inst->ua, 1);
    unsigned child = memcmp(people[9].ed, people[10].ed, 32) < 0 ? 9 : 10, parent = 19 - child;
    put_person(&source, 0, 0); put_person(&source, parent, 0); put_person(&source, child, parent);
    uint8_t before[MT_HASH_SIZE], source_root[MT_HASH_SIZE], after_root[MT_HASH_SIZE];
    assert(!merkle_tree_get(source.topo_sqlite_db, "0:" TEST_CHANNEL, 0, 0, source_root));
    assert(!merkle_tree_get(inspect, "0:" TEST_CHANNEL, 0, 0, before));
    for (int conflict = 4; conflict >= 0; conflict--) {
        struct ms_session* s = ms_create(inst->msync, conn, NULL, MT_DATA_MEMBERS,
            topo_groups_channel_slot(inst->topo_groups, TEST_CHANNEL), MS_READER);
        assert(s);
        s->round = 90 + (unsigned)conflict;
        ms_write64(s->round_id, s->round); memset(s->accept_id, 7, MS_TOKEN_SIZE);
        assert(!ms_pull_start(s));
        int done = 0;
        while (!done) {
            struct ll_entry* query = member_wire_reply(conn), *reply = NULL;
            const uint8_t* body = query->dgram + MS_HEADER;
            uint8_t type = query->dgram[MS_TYPE_OFF] & ~MS_READ_ONLY;
            if (type == MS_ERROR) {
                assert(conflict && ms_read32(body) == (uint32_t)-(conflict == 4 ? MT_ERR_STALE : MT_ERR_CONFLICT));
                done = 1;
            } else if (type == MS_SOURCE_CHECK) {
                assert(query->len == MS_HEADER + MT_HASH_SIZE && !memcmp(body, source_root, MT_HASH_SIZE));
                reply = member_wire_frame(s, MS_SOURCE_OK, 0, MT_HASH_SIZE);
                memcpy(reply->dgram + MS_HEADER, source_root, MT_HASH_SIZE);
                if (conflict == 3) reply->dgram[MS_HEADER] ^= 1;
                if (conflict == 4) s->local_root[0] ^= 1;
                done = !conflict;
            } else {
                assert(type == MS_QUERY);
                uint8_t level = body[0]; uint64_t prefix = ms_read64(body + 1);
                uint32_t id = ms_read32(query->dgram + MS_REQUEST_OFF);
                if (level < MT_MAX_LEVEL) {
                    uint8_t hashes[MT_BUCKETS][MT_HASH_SIZE];
                    assert(!merkle_tree_children(source.topo_sqlite_db, "0:" TEST_CHANNEL, level, prefix, hashes));
                    if (level == 1 && conflict == 1) hashes[0][0] ^= 1;
                    reply = member_wire_frame(s, MS_HASHES, id, sizeof(hashes));
                    memcpy(reply->dgram + MS_HEADER, hashes, sizeof(hashes));
                } else {
                    uint8_t page[MT_PAGE_SIZE]; size_t size = MT_PAGE_SIZE - MS_HEADER - MS_PAGE_HEADER;
                    merkle_key_t after, next; int more;
                    memcpy(after.bytes, body + 10, 32);
                    assert(!member_page_read(source.topo_sqlite_db, TEST_CHANNEL, prefix, body[9], &after, page, &size, &next, &more));
                    if (conflict == 2 && prefix == member_sync_leaf_prefix(people[child].ed)) {
                        size = MEMBER_PAGE_HEADER; page[1] = page[2] = 0; more = 0; memset(&next, 0, sizeof(next));
                    }
                    reply = member_wire_frame(s, MS_PAGE, id, MS_PAGE_HEADER + size);
                    uint8_t* payload = reply->dgram + MS_HEADER;
                    payload[0] = more; memcpy(payload + 1, next.bytes, 32); memcpy(payload + MS_PAGE_HEADER, page, size);
                }
            }
            queue_dgram_free(query); queue_entry_free(query);
            if (reply) { reply->dgram[MS_TYPE_OFF] |= MS_READ_ONLY | MS_RESPONSE; ms_receive(conn, reply); }
        }
        uint64_t until = get_time_us() + 5000000;
        while (s->sql_job && get_time_us() < until) uasync_poll(inst->ua, 1);
        assert(!s->sql_job && s->state == (conflict ? MS_FAILED : MS_COMPLETE));
        assert(!merkle_tree_get(inspect, "0:" TEST_CHANNEL, 0, 0, after_root));
        if (conflict) assert(!memcmp(before, after_root, MT_HASH_SIZE));
        else {
            assert(memcmp(before, after_root, MT_HASH_SIZE) && memcmp(source_root, after_root, MT_HASH_SIZE));
            assert(group_policy_member_access(inst, TEST_CHANNEL, people[child].id, 0));
        }
        merkle_sync_cancel(inst, MT_DATA_MEMBERS, conn->peer_ed25519_pubkey, TEST_CHANNEL);
    }
    member_sync_destroy(&source); assert(!sqlite3_close(source.topo_sqlite_db));
    printf("PASS MEMBERS %s read-only source proof and atomic commit\n", inst->db_worker ? "FILE/RW" : "RAM");
}

/* Длинный проход через настоящий автомат QUERY/HASHES/PAGE в RAM и FILE/RW. */
static void member_wire_large(struct ETCP_CONN* conn, sqlite3* inspect) {
    struct UTUN_INSTANCE source;
    setup(&source, conn->instance->ua, 1);
    sqlite3* db = source.topo_sqlite_db;
    sqlite3_backup* copy = sqlite3_backup_init(db, "main", inspect, "main");
    assert(copy && sqlite3_backup_step(copy, -1) == SQLITE_DONE && !sqlite3_backup_finish(copy));
    char info[MS_USERINFO_MAX + 1];
    memcpy(info, "{\"pad\":\"", 8);
    memset(info + 8, 'p', sizeof(info) - 11);
    memcpy(info + sizeof(info) - 3, "\"}", 3);
    assert(!sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL));
    struct member_sync_write_context writer = {.self=source.node_id};
    for (unsigned i = 0; i < 1400; i++) {
        uint8_t seed[32], x[32], xp[32], ed[32], body[MS_USERINFO_MAX + 128], join[64], update[64], cert[64], id[32];
        assert(!sc_identity_generate(x, xp, ed, seed));
        int n = member_sync_build_join_msg(group_x, group_public, ed, x, body, sizeof(body));
        assert(n == MEMBER_JOIN_MSG_SIZE && !sc_ed25519_sign(seed, body, n, join));
        assert(!member_sync_join_id(body, id) && !member_sync_sign_join(group_private, TEST_CHANNEL, NULL, x, ed, id, 1, cert));
        n = member_sync_build_update_msg(id, 1, info, body, sizeof(body));
        assert(n > 0 && !sc_ed25519_sign(seed, body, n, update));
        struct ms_member_rec member = {.node_id=sc_derive_node_id_from_pubkey(x),.x25519=x,.ed25519=ed,.join_sig=join,.join_ts=1,
            .update_sig=update,.update_ts=1,.userinfo=info,.signature=cert};
        assert(member_sync_record_write(db, &writer, TEST_CHANNEL, NULL, &member) == MS_APPLY_CHANGED);
    }
    assert(!sqlite3_exec(db, "COMMIT", NULL, NULL, NULL));

    struct UTUN_INSTANCE* inst = conn->instance;
    struct ms_session* s = ms_create(inst->msync, conn, NULL, MT_DATA_MEMBERS,
        topo_groups_channel_slot(inst->topo_groups, TEST_CHANNEL), MS_EXCHANGE);
    assert(s && s->leader);
    s->round = 77; ms_write64(s->round_id, s->round); memset(s->accept_id, 7, MS_TOKEN_SIZE);
    assert(!ms_pull_start(s));
    debug_level_t old_level = g_debug_config.category_levels[DEBUG_CATEGORY_MEMBER_SYNC];
    debug_set_category_level(DEBUG_CATEGORY_MEMBER_SYNC, DEBUG_LEVEL_INFO);
    size_t received = 0; unsigned queries = 0;
    for (;;) {
        struct ll_entry* query = member_wire_reply(conn);
        uint8_t type = query->dgram[MS_TYPE_OFF];
        if (type == MS_TURN) { queue_dgram_free(query); queue_entry_free(query); break; }
        if (type == MS_SOURCE_CHECK) {
            struct ll_entry* answer = member_wire_frame(s, MS_SOURCE_OK, 0, MT_HASH_SIZE);
            assert(!merkle_tree_get(db, "0:" TEST_CHANNEL, 0, 0, answer->dgram + MS_HEADER));
            assert(!memcmp(query->dgram + MS_HEADER, answer->dgram + MS_HEADER, MT_HASH_SIZE));
            queue_dgram_free(query); queue_entry_free(query); ms_receive(conn, answer);
            continue;
        }
        assert(type == MS_QUERY);
        queries++;
        if (queries % 1000 == 0) DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "large MEMBERS mode=%s requests=%u received=%zu",
            inst->db_worker ? "FILE/RW" : "RAM", queries, received);
        const uint8_t* body = query->dgram + MS_HEADER;
        uint8_t level = body[0]; uint64_t prefix = ms_read64(body + 1);
        uint32_t request = ms_read32(query->dgram + MS_REQUEST_OFF);
        struct ll_entry* answer;
        if (level < MT_MAX_LEVEL) {
            uint8_t hashes[MT_BUCKETS][MT_HASH_SIZE];
            assert(!merkle_tree_children(db, "0:" TEST_CHANNEL, level, prefix, hashes));
            answer = member_wire_frame(s, MS_HASHES, request, sizeof(hashes));
            memcpy(answer->dgram + MS_HEADER, hashes, sizeof(hashes));
        } else {
            uint8_t page[MT_PAGE_SIZE]; size_t size = MT_PAGE_SIZE - MS_HEADER - MS_PAGE_HEADER;
            merkle_key_t after, next; int more;
            memcpy(after.bytes, body + 10, 32);
            assert(!member_page_read(db, TEST_CHANNEL, prefix, body[9], &after, page, &size, &next, &more));
            received += size;
            answer = member_wire_frame(s, MS_PAGE, request, MS_PAGE_HEADER + size);
            uint8_t* payload = answer->dgram + MS_HEADER;
            payload[0] = more; memcpy(payload + 1, next.bytes, 32); memcpy(payload + MS_PAGE_HEADER, page, size);
        }
        queue_dgram_free(query); queue_entry_free(query);
        ms_receive(conn, answer);
    }
    uint8_t expected[32], actual[32];
    assert(received > 4 * 1024 * 1024 && queries > 4096);
    assert(!merkle_tree_get(db, "0:" TEST_CHANNEL, 0, 0, expected));
    assert(!merkle_tree_get(inspect, "0:" TEST_CHANNEL, 0, 0, actual) && !memcmp(expected, actual, 32));
    merkle_sync_cancel(inst, MT_DATA_MEMBERS, conn->peer_ed25519_pubkey, TEST_CHANNEL);
    member_sync_destroy(&source); assert(!sqlite3_close(db));
    debug_set_category_level(DEBUG_CATEGORY_MEMBER_SYNC, old_level);
    printf("PASS MEMBERS %s large wire: %zu bytes, %u queries, roots equal\n", inst->db_worker ? "FILE/RW" : "RAM", received, queries);
}

static void member_wire_registration(struct ETCP_CONN* conn, struct TOPO_GROUP* group,
    const uint8_t* data, size_t len) {
    struct UTUN_INSTANCE* inst = conn->instance;
    struct ms_session* s = ms_create(inst->msync, conn, NULL, MT_DATA_MEMBERS, group->group_id, MS_EXCHANGE);
    assert(s);
    s->round = 73;
    ms_write64(s->round_id, s->round); memset(s->accept_id, 7, MS_TOKEN_SIZE);
    assert(!ms_pull_start(s));
    struct ll_entry* sent = member_wire_reply(conn);
    queue_dgram_free(sent); queue_entry_free(sent);
    s->stack[0].level = MT_MAX_LEVEL;
    struct ll_entry* page = member_wire_frame(s, MS_PAGE, s->request_id, MS_PAGE_HEADER + len);
    memcpy(page->dgram + MS_HEADER + MS_PAGE_HEADER, data, len);
    struct group_policy_cache* access = inst->group_access;
    group->generation++; /* Тот же полный ключ и новый экземпляр группы. */
    ms_receive(conn, page);
    if (inst->db_worker) {
        uint64_t until = get_time_us() + 5000000;
        while (s->sql_job && get_time_us() < until) uasync_poll(inst->ua, 1);
        assert(!s->sql_job && !conn->send_input_q->head);
    } else {
        struct ll_entry* error = member_wire_reply(conn);
        assert(error->dgram[MS_TYPE_OFF] == MS_ERROR && ms_read32(error->dgram + MS_HEADER) == (uint32_t)-MT_ERR_DISCONNECTED);
        queue_dgram_free(error); queue_entry_free(error);
    }
    assert(inst->group_access == access);
    merkle_sync_cancel(inst, MT_DATA_MEMBERS, conn->peer_ed25519_pubkey, TEST_CHANNEL);
}

static void member_wire_results(struct UTUN_INSTANCE* receiver, sqlite3* inspect, const uint8_t* page, size_t len) {
    struct TOPO_GROUP group = {.group_id=1001, .group_type=TOPO_GROUP_TYPE_CHAT, .instance=receiver, .generation=1};
    strcpy(group.channel_id, TEST_CHANNEL); assert(!sc_hex_to_binary(TEST_CHANNEL, group.wire_id, 32));
    group.ll.size = sizeof(group) - sizeof(group.ll);
    struct ll_queue* groups = queue_new(receiver->ua, 16, 0, 8, "MEMBERS test groups"); assert(groups);
    assert(queue_data_put_with_index(groups, &group.ll) >= 0);
    struct TOPO_GROUPS routes = {.instance=receiver, .group_list=groups};
    struct TOPO_GROUPS* saved_routes = receiver->topo_groups; receiver->topo_groups = &routes;
    struct merkle_sync_data_ops ops = g_member_ops; ops.validate_peer = NULL; ops.keep_transport = 0; ops.on_result = NULL;
    ops.validate_request = allow_member_recovery;
    struct ms_dataset* dataset = &receiver->msync->datasets[MT_DATA_MEMBERS]; dataset->ops = &ops;
    struct ETCP_CONN conn = {.instance=receiver, .peer_node_id=UINT64_MAX, .initialized=1, .links_up=1};
    memset(conn.peer_ed25519_pubkey, 0xff, 32);
    conn.send_input_q = queue_new(receiver->ua, 0, 0, 0, "MEMBERS reply"); assert(conn.send_input_q);
    queue_set_waiter_defer(conn.send_input_q, 1);
    member_timeout_retry(&conn, group.group_id);
    uint8_t root[32], current[32]; struct group_info_record saved, after;
    assert(!merkle_tree_get(inspect, "0:" TEST_CHANNEL, 0, 0, root) && !group_info_get(inspect, TEST_CHANNEL, &saved));
    struct group_policy_cache* access = receiver->group_access;
    int events = clipboard_events; chat_event_set_handler(receiver, clipboard_event);
    const size_t info = MEMBER_PAGE_HEADER + MEMBER_ITEM_HEADER + GROUP_POLICY_WIRE_SIZE;
    const size_t sig = info + 2 + (size_t)page[info] * 256 + page[info + 1]; assert(sig + 64 == len);
    uint8_t source_leaf[32]; EVP_MD_CTX* digest = EVP_MD_CTX_new(); assert(digest);
    assert(merkle_tree_digest_begin(digest, MT_MAX_LEVEL));
    assert(member_hash_item(digest, page + MEMBER_PAGE_HEADER, page + MEMBER_PAGE_HEADER + MEMBER_ITEM_HEADER,
        len - MEMBER_PAGE_HEADER - MEMBER_ITEM_HEADER));
    assert(EVP_DigestFinal_ex(digest, source_leaf, NULL) == 1); EVP_MD_CTX_free(digest);
    for (unsigned i = 0; i < 6; i++) {
        struct ms_session* session = ms_create(receiver->msync, &conn, NULL, MT_DATA_MEMBERS, topo_groups_channel_slot(conn.instance->topo_groups, TEST_CHANNEL), MS_EXCHANGE); assert(session);
        session->round = 10 + i; ms_write64(session->round_id, session->round); memset(session->accept_id, 8, MS_TOKEN_SIZE);
        assert(!ms_pull_start(session));
        uint64_t until = get_time_us() + 5000000;
        while (session->sql_job && get_time_us() < until) uasync_poll(receiver->ua, 1);
        assert(session->pull && session->waiting && !session->sql_job);
        ms_discard_tx(session);
        struct ll_entry* sent;
        while ((sent = queue_data_get(conn.send_input_q))) { queue_dgram_free(sent); queue_entry_free(sent); }
        session->stack[0].level = MT_MAX_LEVEL;
        memcpy(session->source_expected, source_leaf, 32);
        struct ll_entry* entry = member_wire_frame(session, MS_PAGE, session->request_id, MS_PAGE_HEADER + len);
        uint8_t* body = entry->dgram + MS_HEADER + MS_PAGE_HEADER; memcpy(body, page, len);
        if (i == 0) body[sig] ^= 1;
        if (i == 1) body[MEMBER_PAGE_HEADER + MEMBER_ITEM_HEADER + 9] ^= 1;
        if (i == 3) entry->dgram[MS_HEADER] = 2; /* Неизвестное значение more. */
        struct page_denial deny = {.field=i == 2 ? "description_json" : NULL, .action=SQLITE_READ};
        if (deny.field) set_page_denial(receiver, &deny);
        ms_receive(&conn, entry);
        sent = member_wire_reply(&conn);
        if (i < 4) {
            int expected = i < 2 ? MT_ERR_CONFLICT : i == 2 ? MT_ERR_IO : MT_ERR_PROTOCOL;
            assert(sent->dgram[MS_TYPE_OFF] == MS_ERROR && sent->len == MS_HEADER + 4 && ms_read32(sent->dgram + MS_HEADER) == (uint32_t)-expected);
        } else {
            assert(sent->dgram[MS_TYPE_OFF] == MS_SOURCE_CHECK);
            if (i == 4) {
                assert(session->pull && session->state == MS_SOURCE_WAIT);
                ms_cancel_deadline(session);
                ms_timeout(session);
                assert(!session->pull && session->restart);
                struct ll_entry* retry = member_wire_reply(&conn);
                assert(retry->dgram[MS_TYPE_OFF] == MS_BEGIN && !ms_cleanup_pending(session));
                queue_dgram_free(retry); queue_entry_free(retry);
                sqlite3_stmt* staging = NULL;
                assert(!sqlite3_prepare_v2(inspect, "SELECT count(*) FROM member_sync_records", -1, &staging, NULL));
                assert(sqlite3_step(staging) == SQLITE_ROW && sqlite3_column_int(staging, 0) == 0);
                assert(!sqlite3_finalize(staging));
            } else {
                struct ll_entry* ok = member_wire_frame(session, MS_SOURCE_OK, 0, 32);
                memcpy(ok->dgram + MS_HEADER, sent->dgram + MS_HEADER, 32);
                ms_receive(&conn, ok);
                struct ll_entry* turn = member_wire_reply(&conn);
                assert(turn->dgram[MS_TYPE_OFF] == MS_TURN && session->state == MS_SERVE);
                queue_dgram_free(turn); queue_entry_free(turn);
            }
        }
        queue_dgram_free(sent); queue_entry_free(sent);
        if (deny.field) { struct page_denial clear = {0}; set_page_denial(receiver, &clear); assert(deny.hits > 0); }
        merkle_sync_cancel(receiver, MT_DATA_MEMBERS, conn.peer_ed25519_pubkey, TEST_CHANNEL);
        assert(!merkle_tree_get(inspect, "0:" TEST_CHANNEL, 0, 0, current) && !memcmp(root, current, sizeof(root)));
        assert(!group_info_get(inspect, TEST_CHANNEL, &after) && !strcmp(saved.json, after.json) && !memcmp(saved.signature, after.signature, 64));
        assert(clipboard_events == events && receiver->group_access == access);
    }
    member_wire_query_results(&conn, &ops, inspect);
    chat_event_set_handler(receiver, NULL);
    member_wire_registration(&conn, &group, page, len);
    if (getenv("UTUN_TEST_MEMBERS_LARGE")) member_wire_large(&conn, inspect);
    member_wire_readonly(&conn, inspect);
    queue_free(conn.send_input_q); dataset->ops = &g_member_ops; assert(!conn.ref_count);
    receiver->topo_groups = saved_routes; queue_remove_data(groups, &group.ll); queue_free(groups);
    printf("PASS MEMBERS %s wire: peer cursor -> PROTOCOL, signed data -> CONFLICT, page/stalled cursor/begin failure -> IO\n",
        receiver->db_worker ? "FILE/RW" : "RAM");
}

static void member_file_open(struct UTUN_INSTANCE* inst, struct UASYNC* ua, char path[512], sqlite3* seed,
    uint64_t self, uint64_t omit) {
    int saved = forbid_loop_sql; forbid_loop_sql = 0;
    snprintf(path, 512, "%s/utun-members-model-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    int fd = mkstemp(path); assert(fd >= 0); close(fd);
    sqlite3* copy = NULL; assert(!sqlite3_open(path, &copy));
    sqlite3_backup* backup = sqlite3_backup_init(copy, "main", seed, "main"); assert(backup);
    assert(sqlite3_backup_step(backup, -1) == SQLITE_DONE && !sqlite3_backup_finish(backup));
    if (omit) {
        char sql[160]; snprintf(sql, sizeof(sql), "DELETE FROM peers_" TEST_CHANNEL " WHERE node_id=%lld", (long long)omit);
        assert(!sqlite3_exec(copy, sql, NULL, NULL, NULL));
    }
    assert(!sqlite3_close(copy));
    forbid_loop_sql = 1;
    inst->ua = ua; inst->node_id = self; inst->group_started = 1;
    int ready = 0;
    inst->db_worker = db_async_create(ua, path, 16, projection_ready, &ready); assert(inst->db_worker);
    projection_wait(ua, &ready);
    struct bootstrap_job boot = {.self=self};
    bootstrap_run(ua, inst->db_worker, &boot); assert(!boot.status && !member_sync_activate(inst, boot.prepared));
    member_sync_prepared_free(boot.prepared);
    forbid_loop_sql = saved;
}

struct member_retry_fault {
    const char* sql;
    int point, code, block_cleanup, done;
};

static int member_set_retry_fault(sqlite3* db, void* arg) {
    (void)db;
    struct member_retry_fault* fault = arg;
    assert(!author_fault_prefix && !block_retry_cleanup);
    author_fault_prefix = fault->sql; author_fault_point = fault->point;
    author_fault_code = fault->code; author_fault_skip = 0; block_retry_cleanup = fault->block_cleanup;
    return SQLITE_OK;
}

static void member_retry_fault_ready(void* arg, const struct db_async_result* result) {
    assert(!result->status);
    ((struct member_retry_fault*)arg)->done++;
}

static void member_arm_retry_fault(struct UTUN_INSTANCE* inst, const char* sql, int point, int code, int block_cleanup) {
    struct member_retry_fault fault = {.sql=sql, .point=point, .code=code, .block_cleanup=block_cleanup};
    db_async_run(inst->db_worker, &(struct db_async_task){.name="member-retry-fault", .mode=DB_ASYNC_DIRECT,
        .work=member_set_retry_fault, .done=member_retry_fault_ready, .data=&fault});
    projection_wait(inst->ua, &fault.done);
}

static void member_retry_result(const uint8_t peer[32], const char* channel, int result, void* arg) {
    (void)peer;
    assert(!strcmp(channel, TEST_CHANNEL) && result == MT_ERR_DB_RETRY);
    (*(int*)arg)++;
}

static void member_wire_retry(struct UASYNC* ua, sqlite3* source, sqlite3* receiver, uint8_t dataset) {
    struct UTUN_INSTANCE inst[2] = {{0}}; struct ETCP_CONN conn[2] = {{0}};
    struct TOPO_GROUP group[2] = {{0}}; struct TOPO_GROUPS routes[2] = {{0}};
    struct merkle_sync_data_ops ops[2]; char path[2][512];
    int members = dataset == MT_DATA_MEMBERS;
    for (int i = 0; i < 2; i++) {
        struct person* peer = &people[1 - i];
        memcpy(inst[i].my_ed25519_pubkey, people[i].ed, 32); memcpy(inst[i].my_keys.public_key, people[i].x, 32);
        member_file_open(&inst[i], ua, path[i], i ? receiver : source, people[i].id, i && members ? people[5].id : 0);
        group[i] = (struct TOPO_GROUP){.group_id=1001, .group_type=TOPO_GROUP_TYPE_CHAT, .instance=&inst[i], .generation=1};
        strcpy(group[i].channel_id, TEST_CHANNEL); assert(!sc_hex_to_binary(TEST_CHANNEL, group[i].wire_id, 32));
        group[i].ll.size = sizeof(group[i]) - sizeof(group[i].ll);
        routes[i].instance = &inst[i]; routes[i].group_list = queue_new(ua, 16, 0, 8, "member retry groups");
        assert(routes[i].group_list && !queue_data_put_with_index(routes[i].group_list, &group[i].ll));
        inst[i].topo_groups = &routes[i];
        ops[i] = *inst[i].msync->datasets[dataset].ops;
        ops[i].keep_transport = 0; ops[i].on_result = NULL; /* Очереди заменяют физический ETCP. */
        inst[i].msync->datasets[dataset].ops = &ops[i];
        conn[i] = (struct ETCP_CONN){.instance=&inst[i], .peer_node_id=peer->id, .initialized=1, .links_up=1, .state=1};
        memcpy(conn[i].peer_ed25519_pubkey, peer->ed, 32); memcpy(conn[i].crypto_ctx.peer_public_key, peer->x, 32);
        conn[i].send_input_q = queue_new(ua, 0, 0, 0, "member retry wire"); assert(conn[i].send_input_q);
        queue_set_waiter_defer(conn[i].send_input_q, 1);
    }
    int leader = memcmp(people[0].ed, people[1].ed, 32) > 0, callbacks = 0;
    int failed_peer = members ? leader : 1;
    assert(group_policy_member_access(&inst[1], TEST_CHANNEL, people[members ? 5 : 4].id, 0) == !members);
    forbid_loop_sql = 1;
    assert(!merkle_sync_start(&inst[failed_peer], dataset, &conn[failed_peer], TEST_CHANNEL, member_retry_result, &callbacks));
    unsigned root_faults = 0; int commit_fault = 0, done_fault = 0, wakes_after_done = 0, cleanup_wait = 0, retries_grew = 0, converged = 0;
    struct ll_entry* replay = NULL;
    uint64_t until = get_time_us() + 15000000;
    while (!converged && get_time_us() < until) {
        for (int i = 0; i < 2; i++) {
            struct ll_entry* packet = queue_data_get(conn[i].send_input_q);
            if (!packet) continue;
            int to = 1 - i, type = packet->dgram[MS_TYPE_OFF];
            assert(type != MS_ERROR);
            if (members && to == leader && type == MS_HASHES && ms_read32(packet->dgram + MS_REQUEST_OFF) == 1 && root_faults < 2) {
                root_faults++;
                member_arm_retry_fault(&inst[to], "SELECT hash FROM merkle_tree_hash", 0, SQLITE_BUSY_SNAPSHOT, 0);
            }
            if (!members && to == 1 && type == MS_SOURCE_OK && !commit_fault) {
                commit_fault = 1;
                member_arm_retry_fault(&inst[to], "COMMIT", 2, SQLITE_LOCKED_SHAREDCACHE, 1);
            }
            if (members && type == MS_DONE && !done_fault) {
                done_fault = 1;
                replay = ll_alloc_lldgram(packet->len); assert(replay);
                replay->len = packet->len; memcpy(replay->dgram, packet->dgram, packet->len);
                member_arm_retry_fault(&inst[to], "SELECT hash FROM merkle_tree_hash", 0, SQLITE_BUSY, 0);
            } else if (done_fault && type == MS_WAKE) {
                assert(!wakes_after_done++ && replay);
                ms_receive(&conn[i], replay); replay = NULL;
                assert(inst[i].msync->sessions->state == MS_WAKE_WAIT);
            }
            ms_receive(&conn[to], packet);
        }
        uasync_poll(ua, 1);
        struct ms_session* s = inst[leader].msync->sessions;
        if (s && s->restart && s->restart_delay == 4 * MS_ROUND_RETRY_MIN_TB) retries_grew = 1;
        struct ms_sql_job* cleanup = inst[1].msync->cleanups;
        if (cleanup && cleanup->continuation == MS_DB_ABORT && cleanup->retry_timer) cleanup_wait = 1;
        converged = 1;
        for (int i = 0; i < 2; i++) {
            s = inst[i].msync->sessions;
            converged &= s && s->state == MS_IDLE && !s->dirty && !s->tx && !s->restart && !s->restart_delay &&
                !conn[i].send_input_q->head && !db_async_pending(inst[i].db_worker);
        }
    }
    assert(converged && callbacks == 1);
    assert(members ? root_faults == 2 && retries_grew && done_fault && wakes_after_done == 1 : commit_fault && cleanup_wait);
    assert(group_policy_member_access(&inst[1], TEST_CHANNEL, people[members ? 5 : 4].id, 0) == members);
    int follower = 1 - leader;
    struct ms_session* s = inst[follower].msync->sessions;
    uasync_poll(ua, 0);
    size_t timers = timeout_heap_get_size(ua->timeout_heap);
    s->state = MS_DONE_WAIT;
    ms_timeout(s);
    assert(s->state == MS_FAILED && s->restart && !s->tx && timeout_heap_get_size(ua->timeout_heap) == timers + 1);
    merkle_sync_cancel(&inst[follower], dataset, conn[follower].peer_ed25519_pubkey, TEST_CHANNEL);
    uasync_poll(ua, 0);
    assert(!inst[follower].msync->sessions && timeout_heap_get_size(ua->timeout_heap) <= timers);
    forbid_loop_sql = 0;
    uint8_t root[2][MT_HASH_SIZE];
    for (int i = 0; i < 2; i++) {
        sqlite3* inspect = NULL; assert(!sqlite3_open_v2(path[i], &inspect, SQLITE_OPEN_READONLY, NULL));
        char tree[70]; snprintf(tree, sizeof(tree), "%u:%s", dataset, TEST_CHANNEL);
        assert(!merkle_tree_get(inspect, tree, 0, 0, root[i]));
        sqlite3_stmt* st = NULL;
        assert(!sqlite3_prepare_v2(inspect, "SELECT (SELECT count(*) FROM member_sync_receives),"
            "(SELECT count(*) FROM member_sync_records)", -1, &st, NULL));
        assert(sqlite3_step(st) == SQLITE_ROW && !sqlite3_column_int(st, 0) && !sqlite3_column_int(st, 1));
        assert(!sqlite3_finalize(st) && !sqlite3_close(inspect));
        member_sync_destroy(&inst[i]); assert(!db_async_destroy(inst[i].db_worker) && !remove(path[i]));
        assert(!conn[i].ref_count); queue_free(conn[i].send_input_q);
        queue_remove_data(routes[i].group_list, &group[i].ll); queue_free(routes[i].group_list);
    }
    assert(!memcmp(root[0], root[1], sizeof(root[0])));
    printf("PASS %s FILE/RW retry: stable connection, converged roots and empty staging\n", members ? "MEMBERS" : "MEMBER_ADMIN");
}

static void members_worker_regression(struct UASYNC* ua, struct UTUN_INSTANCE* source) {
    char paths[2][512]; struct UTUN_INSTANCE runtime[2] = {{0}};
    for (unsigned i = 0; i < 2; i++) member_file_open(runtime + i, ua, paths[i], source->topo_sqlite_db, source->node_id, i ? people[5].id : 0);
    forbid_loop_sql = 1;
    assert(!group_policy_member_access(runtime + 1, TEST_CHANNEL, people[5].id, 1));
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        struct member_round_job begin = {.request={.action=MT_SQL_BEGIN_PULL, .round=attempt + 1}};
        memcpy(begin.request.peer, people[0].ed, 32);
        member_round_run(runtime + 1, &begin); assert(!begin.status && begin.request.pull);
        void* pull = begin.request.pull;
        sqlite3_stmt* leaves = NULL;
        assert(!__real_sqlite3_prepare_v2(source->topo_sqlite_db,
            "SELECT prefix64 FROM merkle_tree_hash WHERE namespace='0:" TEST_CHANNEL "' AND level=5", -1, &leaves, NULL));
        int step;
        while ((step = sqlite3_step(leaves)) == SQLITE_ROW) {
            struct member_round_job page = {.request={.action=MT_SQL_PAGE, .prefix=(uint64_t)sqlite3_column_int64(leaves, 0), .capacity=MT_PAGE_SIZE}};
            member_round_run(runtime, &page); assert(!page.status && !page.request.more);
            page.request.action = MT_SQL_STAGE_PAGE; page.request.pull = pull;
            member_round_run(runtime + 1, &page); assert(!page.status);
        }
        assert(step == SQLITE_DONE && !__real_sqlite3_finalize(leaves));
        struct member_round_job commit = {.request={.action=MT_SQL_COMMIT_PULL, .pull=pull}, .veto=!attempt};
        member_round_run(runtime + 1, &commit);
        if (!attempt) assert(commit.status == DB_ASYNC_SQL && (commit.code & 255) == SQLITE_CONSTRAINT &&
            !group_policy_member_access(runtime + 1, TEST_CHANNEL, people[5].id, 1));
        else assert(!commit.status && group_policy_member_access(runtime + 1, TEST_CHANNEL, people[5].id, 1));
        struct member_round_job abort = {.request={.action=MT_SQL_ABORT_PULL, .pull=pull}};
        member_round_run(runtime + 1, &abort); assert(!abort.status); member_pull_free(pull);
    }
    forbid_loop_sql = 0; uint8_t roots[2][32];
    for (unsigned i = 0; i < 2; i++) {
        sqlite3* db = NULL; assert(!sqlite3_open_v2(paths[i], &db, SQLITE_OPEN_READONLY, NULL));
        assert(!merkle_tree_get(db, "0:" TEST_CHANNEL, 0, 0, roots[i]) && !sqlite3_close(db));
    }
    assert(!memcmp(roots[0], roots[1], 32));
    merkle_key_t after = {{0}}, next; size_t length = MT_PAGE_SIZE; uint8_t page[MT_PAGE_SIZE]; int more;
    assert(!_member_get_page(source, TEST_CHANNEL, 0, 0, &after, page, &length, &next, &more) && !more && page[0] == MEMBER_PAGE_VERSION && !page[1] && page[2] == 1);
    sqlite3* disk; assert(!sqlite3_open_v2(paths[1], &disk, SQLITE_OPEN_READONLY, NULL));
    struct UTUN_INSTANCE ram = {.ua=ua, .node_id=source->node_id, .group_started=1};
    assert(!sqlite3_open(":memory:", &ram.topo_sqlite_db));
    sqlite3_backup* copy = sqlite3_backup_init(ram.topo_sqlite_db, "main", source->topo_sqlite_db, "main");
    assert(copy && sqlite3_backup_step(copy, -1) == SQLITE_DONE && !sqlite3_backup_finish(copy));
    assert(!member_sync_init(&ram));
    member_wire_results(&ram, ram.topo_sqlite_db, page, length);
    member_sync_destroy(&ram); assert(!sqlite3_close(ram.topo_sqlite_db));
    forbid_loop_sql = 1;
    clipboard_description_rounds(source, runtime + 1, paths[1]);
    forbid_loop_sql = 0;
    member_wire_results(runtime + 1, disk, page, length); assert(!sqlite3_close(disk));
    for (unsigned i = 0; i < 2; i++) {
        member_sync_destroy(runtime + i); assert(!db_async_destroy(runtime[i].db_worker) && !remove(paths[i]));
    }
    forbid_loop_sql = 0;
    puts("PASS MEMBERS frozen FILE round: signed pages/staging, atomic commit rollback, no loop SQL/runtime, postcommit access and equal roots");
}
/* SQL-образ содержит настоящие подписи и историю, но worker не получает runtime исходного экземпляра. */
/* Вход компаратора — уже проверенные записи. Проверяем обе перестановки и повтор. */
static void policy_order(void) {
    struct group_policy first = {.version=1, .all_members=0, .signature={2}};
    struct group_policy second = {.version=1, .all_members=0, .signature={1}};
    assert(group_policy_newer(&second, &first) && !group_policy_newer(&first, &second));
    assert(!group_policy_newer(&first, &first));
    second.all_members = 1;
    assert(group_policy_newer(&first, &second) && !group_policy_newer(&second, &first));
    second.version++;
    assert(group_policy_newer(&second, &first) && !group_policy_newer(&first, &second));
}

int main(void) {
    policy_order();
    loop_thread = pthread_self(); /* Единственная запись до создания любых worker threads. */
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    struct UASYNC* ua = uasync_create(); assert(ua); public_key(group_private, group_public);
    for (unsigned i = 0; i < sizeof(people) / sizeof(*people); i++) {
        struct person* p = &people[i]; uint8_t private_x[32];
        assert(!sc_identity_generate(p->x, private_x, p->ed, p->private)); p->id = sc_derive_node_id_from_pubkey(p->x);
        uint8_t body[MEMBER_JOIN_MSG_SIZE]; assert(member_sync_build_join_msg(group_x, group_public, p->ed, p->x, body, sizeof(body)) == MEMBER_JOIN_MSG_SIZE);
        assert(sc_ed25519_sign(p->private, body, sizeof(body), p->join) == SC_OK);
    }
    struct UTUN_INSTANCE source, target; setup(&source, ua, 1); setup(&target, ua, 0);
    const unsigned parents[] = {0,0,0,2,3,1};
    for (unsigned i = 0; i < 6; i++) { put_person(&source, i, parents[i]); put_person(&target, i, parents[i]); }
    assert(admitted(&source, 0) && admitted(&source, 1) && admitted(&source, 2) && admitted(&source, 5));
    assert(!admitted(&source, 3) && !admitted(&source, 4));
    assert(group_policy_can_invite(&source, TEST_CHANNEL, people[0].ed) == 1);
    assert(group_policy_can_invite(&source, TEST_CHANNEL, people[1].ed) == 1);
    assert(group_policy_can_invite(&source, TEST_CHANNEL, people[2].ed) == 0);
    struct group_policy_update* rejected = NULL; fail_admin_parse = 1;
    assert(!sqlite3_exec(source.topo_sqlite_db, "BEGIN IMMEDIATE", NULL, NULL, NULL));
    assert(group_policy_project(source.topo_sqlite_db, source.node_id, TEST_CHANNEL, &rejected) < 0 && !rejected && !fail_admin_parse);
    assert(!sqlite3_exec(source.topo_sqlite_db, "ROLLBACK", NULL, NULL, NULL));
    assert(admitted(&source, 5) && group_policy_can_invite(&source, TEST_CHANNEL, people[1].ed) == 1);
    check_invite_proof_refresh(&source);
    char baseline[65], current[65], *json = NULL; token(&source, baseline);
    assert(!group_policy_command(&source, TEST_CHANNEL, 1, baseline, NULL, &json)); u_free(json);
    assert(admitted(&source, 3) && admitted(&source, 4) && group_policy_can_invite(&source, TEST_CHANNEL, people[2].ed) == 1);
    check_author_errors(&source);
    check_member_projection_errors(&source);
    bootstrap_worker_regression(ua, &source);
    member_wire_retry(ua, source.topo_sqlite_db, source.topo_sqlite_db, MT_DATA_MEMBERS);
    members_worker_regression(ua, &source);
    check_access_snapshot(&source);
    /* Расчёт нового допуска до commit/после rollback не меняет опубликованные права. */
    struct group_policy_cache* saved_view = source.group_access;
    assert(sqlite3_exec(source.topo_sqlite_db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    char deletion[128]; snprintf(deletion, sizeof(deletion), "UPDATE peers_" TEST_CHANNEL " SET deleted=1 WHERE node_id=%lld",
        (long long)people[4].id);
    assert(sqlite3_exec(source.topo_sqlite_db, deletion, NULL, NULL, NULL) == SQLITE_OK);
    struct group_policy_update* pending_view = NULL;
    assert(!group_policy_project(source.topo_sqlite_db, source.node_id, TEST_CHANNEL, &pending_view) && pending_view);
    assert(source.group_access == saved_view && group_policy_member_access(&source, TEST_CHANNEL, people[4].id, 0));
    assert(sqlite3_exec(source.topo_sqlite_db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    group_policy_discard(pending_view); assert(source.group_access == saved_view); check_access_snapshot(&source);
    void* pull; assert(!member_begin_pull(&target, TEST_CHANNEL, source.my_ed25519_pubkey, 1, &pull));
    page(&source, &target, pull, 0); complete(&target, pull);
    assert(admitted(&target, 3) && admitted(&target, 4));
    struct UTUN_INSTANCE missed; setup(&missed, ua, 0); missed.node_id = people[5].id;
    assert(!member_begin_pull(&missed, TEST_CHANNEL, source.my_ed25519_pubkey, 1, &pull));
    page(&source, &missed, pull, 0);
    page_members(&source, &missed, pull);
    complete(&missed, pull); assert(admitted(&missed, 4));
    struct UTUN_INSTANCE admin_target; setup(&admin_target, ua, 0);
    assert(!member_begin_pull(&admin_target, TEST_CHANNEL, people[0].ed, 1, &pull));
    page(&source, &admin_target, pull, 0);
    page_members(&source, &admin_target, pull);
    complete(&admin_target, pull);
    struct DB_SYNC_INSTANCE *a = history(&source), *b = history(&target), *c = history(&missed);
    assert(!group_policy_command(&source, TEST_CHANNEL, 0, NULL, NULL, &json));
    assert(strstr(json, "\"affected\":[{") && strstr(json, "\"can_edit\":true")); u_free(json);
    token(&source, baseline);
    assert(group_policy_command(&target, TEST_CHANNEL, 0, baseline, NULL, &json) < 0 && strstr(json, "forbidden")); u_free(json);
    assert(group_policy_command(&source, TEST_CHANNEL, 0, "stale", NULL, &json) == -2); u_free(json);
    assert(group_policy_command(&source, TEST_CHANNEL, 0, baseline, "{}", &json) < 0); u_free(json);
    char keep[72]; snprintf(keep, sizeof(keep), "[\"%s\"]", sc_public_key_hex(people[3].ed, (char[65]){0}));
    assert(sqlite3_exec(source.topo_sqlite_db, "CREATE TRIGGER reject_policy BEFORE UPDATE ON group_policy "
        "BEGIN SELECT RAISE(ABORT,'rollback test'); END", NULL, NULL, NULL) == SQLITE_OK);
    assert(group_policy_command(&source, TEST_CHANNEL, 0, baseline, keep, &json) < 0); u_free(json);
    token(&source, current); assert(!strcmp(current, baseline) && admitted(&source, 4));
    uint8_t signature[64]; uint8_t signer[32];
    assert(!topo_node_sqlite_member_get_sign(source.topo_sqlite_db, TEST_CHANNEL, people[3].ed, signer, signature));
    assert(!memcmp(signer, people[2].ed, 32));
    assert(sqlite3_exec(source.topo_sqlite_db, "DROP TRIGGER reject_policy", NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_exec(source.topo_sqlite_db, "CREATE TRIGGER reject_archive BEFORE INSERT ON chat_seals"
        " BEGIN SELECT RAISE(ABORT,'archive rollback test'); END", NULL, NULL, NULL) == SQLITE_OK);
    assert(group_policy_command(&source, TEST_CHANNEL, 0, baseline, keep, &json) < 0); u_free(json);
    token(&source, current); assert(!strcmp(current, baseline) && admitted(&source, 4) && db_sync_count(a) == 0 && !archive_count(&source));
    check_access_snapshot(&source);
    assert(sqlite3_exec(source.topo_sqlite_db, "DROP TRIGGER reject_archive", NULL, NULL, NULL) == SQLITE_OK);
    assert(!group_policy_command(&source, TEST_CHANNEL, 0, baseline, keep, &json)); u_free(json);
    assert(admitted(&source, 3) && !admitted(&source, 4));
    member_wire_retry(ua, source.topo_sqlite_db, admin_target.topo_sqlite_db, MT_DATA_MEMBER_ADMIN);
    assert(!member_admin_begin(&admin_target, TEST_CHANNEL, people[0].ed, 2, &pull));
    page(&source, &admin_target, pull, 0);
    member_abort_pull(&admin_target, pull);
    assert(admitted(&admin_target, 3) && admitted(&admin_target, 4));
    assert(!member_admin_begin(&admin_target, TEST_CHANNEL, people[0].ed, 3, &pull));
    {
        uint8_t data[8192]; size_t size = sizeof(data); int more; merkle_key_t after = {{0}}, next;
        assert(!_member_get_page(&source, TEST_CHANNEL, 0, 0, &after, data, &size, &next, &more) && !more);
        assert(member_stage_page(&admin_target, pull, 0, 0, &after, &next, more, data, size) == MT_ERR_CONFLICT);
    }
    page(&source, &admin_target, pull, 0);
    page_members(&source, &admin_target, pull);
    assert(admitted(&admin_target, 3) && admitted(&admin_target, 4));
    complete(&admin_target, pull);
    assert(admitted(&admin_target, 3) && !admitted(&admin_target, 4));
    uint8_t admin_root[32], admin_remote[32];
    assert(!merkle_sync_read_hash(&source, MT_DATA_MEMBER_ADMIN, TEST_CHANNEL, 0, 0, admin_root));
    assert(!merkle_sync_read_hash(&admin_target, MT_DATA_MEMBER_ADMIN, TEST_CHANNEL, 0, 0, admin_remote));
    assert(!memcmp(admin_root, admin_remote, sizeof(admin_root)));
    member_sync_destroy(&admin_target);
    assert(!sqlite3_close(admin_target.topo_sqlite_db));
    check_access_snapshot(&source);
    assert(!db_sync_count(a) && archive_count(&source) == 0);
    assert(!topo_node_sqlite_member_get_sign(source.topo_sqlite_db, TEST_CHANNEL, people[3].ed, signer, signature) && !member_sync_signer(signer));
    /* Политика раньше переподписи: текущий допуск сохраняется до полной контрольной точки. */
    assert(!member_begin_pull(&target, TEST_CHANNEL, source.my_ed25519_pubkey, 2, &pull));
    page(&source, &target, pull, 0); assert(admitted(&target, 3) && admitted(&target, 4));
    member_sync_destroy(&target); assert(!member_sync_init(&target) && !chat_feedback_init(&target));
    member_abort_pull(&target, pull);
    assert(admitted(&target, 3) && admitted(&target, 4));
    assert(!member_begin_pull(&target, TEST_CHANNEL, source.my_ed25519_pubkey, 3, &pull));
    page(&source, &target, pull, 0);
    page_members(&source, &target, pull);
    assert(admitted(&target, 3) && admitted(&target, 4));
    complete(&target, pull);
    assert(admitted(&target, 3) && !admitted(&target, 4));
    check_access_snapshot(&target);
    assert(!db_sync_count(b) && archive_count(&target) == 0);
    token(&source, baseline); token(&target, current); assert(!strcmp(baseline, current));
    /* Поздняя старая подпись не вытесняет подпись группы той же эпохи. */
    put_person(&target, 3, 2); token(&target, current); assert(!strcmp(baseline, current));
    member_sync_destroy(&target); assert(!member_sync_init(&target) && !chat_feedback_init(&target));
    assert(admitted(&target, 3) && !admitted(&target, 4));
    /* Другое join_sig не может использовать подтверждение прежнего JOIN. */
    struct ms_member_rec forged = {.node_id = people[3].id, .x25519 = people[3].x, .ed25519 = people[3].ed, .join_sig = people[4].join,
        .signature = signature, .signed_by = 0};
    assert(member_sync_verify_signature(source.topo_sqlite_db, TEST_CHANNEL, &forged) == 0);
    put_person(&source, 6, 7); assert(!admitted(&source, 6));
    put_person(&source, 7, 6); assert(!admitted(&source, 6) && !admitted(&source, 7));
    token(&source, baseline);
    assert(!group_policy_command(&source, TEST_CHANNEL, 1, baseline, NULL, &json)); u_free(json);
    assert(!admitted(&source, 6) && !admitted(&source, 7));
    assert(admitted(&source, 4));
    /* Третий участник пропустил отзыв и видит только восстановление: новое поколение всё равно закрывает старую историю. */
    assert(!member_begin_pull(&missed, TEST_CHANNEL, source.my_ed25519_pubkey, 2, &pull)); page(&source, &missed, pull, 0);
    page_members(&source, &missed, pull);
    complete(&missed, pull); assert(admitted(&missed, 4) && !db_sync_count(c) && archive_count(&missed) == 0);
    uint8_t root_a[32],root_c[32]; assert(!db_sync_verify(a) && !db_sync_verify(c));
    assert(!db_sync_read_root(a,root_a) && !db_sync_read_root(c,root_c) && !memcmp(root_a,root_c,32));
    assert(!archive_count(&source) && !archive_count(&missed));
    const char *left = "{\"ver\":\"1\",\"storage\":\"no\"}", *right = "{\"ver\":\"1\",\"storage\":\"yes\"}";
    owner_block(&source, left); owner_block(&source, right); owner_block(&target, right); owner_block(&target, left);
    uint8_t sig_left[64], sig_right[64], owner_msg[256];
    int len = member_sync_build_owner_msg(TEST_CHANNEL, people[5].ed, left, owner_msg, sizeof(owner_msg));
    assert(len > 0 && !sc_ed25519_sign(group_private, owner_msg, len, sig_left));
    len = member_sync_build_owner_msg(TEST_CHANNEL, people[5].ed, right, owner_msg, sizeof(owner_msg));
    assert(len > 0 && !sc_ed25519_sign(group_private, owner_msg, len, sig_right));
    owner_equal(&source, &target, memcmp(sig_left, sig_right, 64) < 0 ? left : right);
    const char *closed = "{\"ver\":\"1\",\"_message_epoch\":\"1\",\"storage\":\"no\"}",
        *stale = "{\"ver\":\"99\",\"storage\":\"yes\"}";
    owner_block(&source, stale); owner_block(&source, closed); owner_block(&target, closed); owner_block(&target, stale);
    owner_equal(&source, &target, closed);
    const char* removed = "{\"ver\":\"1\",\"_message_epoch\":\"1\",\"deleted\":\"yes\"}";
    owner_block(&source, removed); owner_block(&source, closed);
    owner_block(&target, closed); owner_block(&target, removed);
    owner_equal(&source, &target, removed);
    uint64_t invalid_epoch;
    assert(member_sync_owner_epoch("{\"_message_epoch\":\"9223372036854775808\"}", &invalid_epoch) < 0);
    puts("PASS owner concurrency: same-version updates converge in opposite orders; higher metadata version cannot undo a revocation epoch");
    puts("PASS policy seals: atomic rollback, different local histories, multiple MEMBERS receivers, missed revocation, generation-bound IDs and converged live roots");
    db_sync_destroy(&source); db_sync_destroy(&target); db_sync_destroy(&missed); member_sync_destroy(&missed);
    assert(sqlite3_close(missed.topo_sqlite_db) == SQLITE_OK);
    member_sync_destroy(&source); member_sync_destroy(&target);
    assert(!source.group_access && !target.group_access && !missed.group_access);
    assert(sqlite3_close(source.topo_sqlite_db) == SQLITE_OK && sqlite3_close(target.topo_sqlite_db) == SQLITE_OK);
    uasync_destroy(ua, 0); puts("PASS: default, roles, cascade, selection, rollback, stale preview, page order, signature precedence, restart");
    return 0;
}
