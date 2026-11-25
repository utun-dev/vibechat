/* Настоящие подписи, MEMBERS и страницы Merkle: офлайн-правки, роли и срез отзыва. */
#include "../src/chat/member_sync.c"
#include "test_member_staging.h"
#include "../src/kanban/kanban_sync.c"
#include "../src/kanban/kanban_read.h"
#include "../src/kanban/kanban_attachment.h"
#include "../src/attachments/download.h"
#include "../src/attachments/group_file.h"
#include "../src/attachments/manifest.h"
#include "../src/attachments/recovery.h"
#include "../src/media_async/media_async.h"
#include "../src/media_async/attachment_send.h"
#include "../src/group/group_protocol.h"
#include "test_membership.h"
#include "test_utils.h"
#include "test_net.h"
#include "../src/transport_layer/node_conn_direct.h"
#include <assert.h>
#include <pthread.h>

static pthread_t kb_test_loop;
static int kb_forbid_loop;
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int length, sqlite3_stmt** st, const char** tail) {
    assert(!kb_forbid_loop || !pthread_equal(kb_test_loop, pthread_self()));
    return __real_sqlite3_prepare_v2(db, sql, length, st, tail);
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*cb)(void*, int, char**, char**), void* arg, char** error) {
    assert(!kb_forbid_loop || !pthread_equal(kb_test_loop, pthread_self())); return __real_sqlite3_exec(db, sql, cb, arg, error);
}

struct kb_file_job {
    struct merkle_sync_sql_request request;
    struct kb_sql_scope scope;
    uint64_t prefixes[128]; size_t count;
    int collect, veto, done, status, code;
};
static int kb_file_veto(void* unused) { (void)unused; return 1; }
static int kb_file_work(sqlite3* db, void* data) {
    struct kb_file_job* job = data; assert(!pthread_equal(kb_test_loop, pthread_self())); sqlite3_commit_hook(db, NULL, NULL);
    if (!job->collect) {
        int code = kb_sql_work(db, &job->scope, &job->request);
        if (!code && job->veto) sqlite3_commit_hook(db, kb_file_veto, NULL);
        return code;
    }
    sqlite3_stmt* st = NULL; int rc = sqlite3_prepare_v2(db,
        "SELECT DISTINCT bucket FROM kanban_ops WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?) AND active=1 ORDER BY bucket DESC", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, job->request.ns, -1, SQLITE_STATIC);
    if (!rc) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (job->count == 128) { rc = SQLITE_TOOBIG; break; }
        job->prefixes[job->count++] = (uint64_t)sqlite3_column_int64(st, 0) << 39;
    }
    int final = sqlite3_finalize(st); return rc == SQLITE_DONE ? final : rc;
}
static void kb_file_done(void* data, const struct db_async_result* result) {
    struct kb_file_job* job = data; assert(!job->done++); job->status = result->status; job->code = result->sqlite_code;
}
static void kb_file_wait(struct UASYNC* ua, int* done) {
    uint64_t end = get_time_us() + 5000000; while (!*done && get_time_us() < end) uasync_poll(ua, 1); assert(*done == 1);
}
static void kb_file_ready(void* data, const struct db_async_result* result) { assert(!result->status); *(int*)data = 1; }
static void kb_file_run(struct UTUN_INSTANCE* runtime, struct kb_file_job* job) {
    job->done = 0; memcpy(job->scope.self, runtime->my_ed25519_pubkey, 32); job->scope.generation = runtime->kanban_generation;
    db_async_run(runtime->db_worker, &(struct db_async_task){.name="kanban-frozen-round",
        .mode=job->collect || job->request.action == MT_SQL_PAGE ? DB_ASYNC_READ : DB_ASYNC_WRITE,
        .work=kb_file_work, .done=kb_file_done, .data=job}); kb_file_wait(runtime->ua, &job->done);
    if (!job->status && !job->collect) kb_sql_publish(runtime, &job->scope, &job->request);
}
static void frozen_file_round(struct UTUN_INSTANCE* source, const char* group) {
    struct UASYNC* ua = uasync_create(); assert(ua); char paths[2][512]; struct UTUN_INSTANCE runtime[2] = {{0}}; int ready[2] = {0};
    kb_test_loop = pthread_self();
    for (unsigned i = 0; i < 2; i++) {
        snprintf(paths[i], sizeof(paths[i]), "%s/utun-kanban-model-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
        int fd = mkstemp(paths[i]); assert(fd >= 0); close(fd); sqlite3* copy = NULL; assert(!sqlite3_open(paths[i], &copy));
        sqlite3_backup* backup = sqlite3_backup_init(copy, "main", source->topo_sqlite_db, "main"); assert(backup);
        assert(sqlite3_backup_step(backup, -1) == SQLITE_DONE && !sqlite3_backup_finish(backup));
        if (i) assert(!sqlite3_exec(copy, "DELETE FROM kanban_card_properties;DELETE FROM kanban_items;"
            "DELETE FROM kanban_state;DELETE FROM kanban_ops;DELETE FROM merkle_tree_hash WHERE namespace GLOB '3:*'", NULL, NULL, NULL));
        assert(!sqlite3_close(copy)); runtime[i].ua = ua; runtime[i].node_id = source->node_id;
        runtime[i].group_started = runtime[i].kanban_started = 1; runtime[i].kanban_generation = 1;
        runtime[i].db_worker = db_async_create(ua, paths[i], 16, kb_file_ready, ready + i); assert(runtime[i].db_worker);
    }
    kb_forbid_loop = 1; for (unsigned i = 0; i < 2; i++) kb_file_wait(ua, ready + i);
    struct kb_file_job catalog = {.collect=1}; strcpy(catalog.request.ns, group); kb_file_run(runtime, &catalog);
    assert(!catalog.status && catalog.count);
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        struct kb_file_job begin_job = {.request={.action=MT_SQL_BEGIN_PULL}}; strcpy(begin_job.request.ns, group);
        kb_file_run(runtime + 1, &begin_job); assert(!begin_job.status && begin_job.request.pull); void* pull = begin_job.request.pull;
        for (size_t i = 0; i < catalog.count; i++) {
            struct kb_file_job page_job = {.request={.action=MT_SQL_PAGE, .prefix=catalog.prefixes[i], .capacity=MT_PAGE_SIZE}};
            strcpy(page_job.request.ns, group);
            do {
                page_job.request.action = MT_SQL_PAGE; kb_file_run(runtime, &page_job); assert(!page_job.status);
                page_job.request.action = MT_SQL_STAGE_PAGE; page_job.request.pull = pull;
                kb_file_run(runtime + 1, &page_job); assert(!page_job.status);
                page_job.request.after = page_job.request.next; page_job.request.after_valid = 1;
            } while (page_job.request.more);
        }
        struct kb_file_job commit_job = {.request={.action=MT_SQL_COMMIT_PULL, .pull=pull}, .veto=!attempt}; strcpy(commit_job.request.ns, group);
        kb_file_run(runtime + 1, &commit_job);
        if (!attempt) assert(commit_job.status == DB_ASYNC_SQL && (commit_job.code & 255) == SQLITE_CONSTRAINT);
        else assert(!commit_job.status);
        struct kb_file_job abort_job = {.request={.action=MT_SQL_ABORT_PULL, .pull=pull}}; strcpy(abort_job.request.ns, group);
        kb_file_run(runtime + 1, &abort_job); assert(!abort_job.status); kb_sql_pull_free(pull);
    }
    for (unsigned i = 0; i < 2; i++) assert(!db_async_destroy(runtime[i].db_worker));
    kb_forbid_loop = 0; char ns[32]; snprintf(ns, sizeof(ns), "3:%s", group); uint8_t roots[2][32];
    for (unsigned i = 0; i < 2; i++) {
        sqlite3* db = NULL; assert(!sqlite3_open(paths[i], &db) && !merkle_tree_get(db, ns, 0, 0, roots[i]));
        assert(!sqlite3_close(db) && !remove(paths[i]));
    }
    assert(!memcmp(roots[0], roots[1], 32)); uasync_destroy(ua, 0);
    puts("PASS Kanban FILE frozen round: signed pages/seals, separate staging jobs, COMMIT rollback/retry, no loop SQL, converged root");
}

static void copy_members(struct UTUN_INSTANCE* src, struct UTUN_INSTANCE* dst, const char* group) {
    void* pull = NULL; assert(!member_begin_pull(dst, group, src->my_ed25519_pubkey, 1, &pull));
    sqlite3_stmt* st = NULL; char ns[70]; snprintf(ns, sizeof(ns), "0:%s", group);
    assert(!sqlite3_prepare_v2(src->topo_sqlite_db, "SELECT prefix64 FROM merkle_tree_hash WHERE namespace=? AND level=?", -1, &st, NULL));
    assert(!sqlite3_bind_text(st, 1, ns, -1, SQLITE_STATIC) && !sqlite3_bind_int(st, 2, MT_MAX_LEVEL));
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        uint64_t prefix = (uint64_t)sqlite3_column_int64(st, 0);
        merkle_key_t after = {{0}}, next; int more, valid = 0;
        do {
            uint8_t bytes[MT_PAGE_SIZE]; size_t size = sizeof(bytes);
            assert(!_member_get_page(src, group, prefix, valid, &after, bytes, &size, &next, &more));
            assert(!member_stage_page(dst, pull, prefix, valid, &after, &next, more, bytes, size));
            after = next; valid = 1;
        } while (more);
    }
    assert(rc == SQLITE_DONE && !sqlite3_finalize(st));
    assert(!member_commit_pull(dst, pull)); member_publish_pull(dst, pull); member_abort_pull(dst, pull);
}
static void copy_board(struct UTUN_INSTANCE* src, struct UTUN_INSTANCE* dst, const char* group) {
    void* pull = NULL; assert(!begin(dst, group, src->my_ed25519_pubkey, 1, &pull));
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(src->topo_sqlite_db, "SELECT DISTINCT bucket FROM kanban_ops WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND active=1",
        -1, &st, NULL) == SQLITE_OK && sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC) == SQLITE_OK);
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        uint64_t prefix = (uint64_t)sqlite3_column_int64(st, 0) << 39; merkle_key_t after = {{0}}, next; int more, valid = 0;
        do {
            uint8_t bytes[MT_PAGE_SIZE]; size_t size = sizeof(bytes);
            assert(!page(src, group, prefix, valid, &after, bytes, &size, &next, &more));
            assert(!stage(dst, pull, prefix, valid, &after, &next, more, bytes, size));
            after = next; valid = 1;
        } while (more);
    }
    assert(rc == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
    assert(!commit(dst, pull)); publish(dst, pull); abort_pull(dst, pull);
}
static char* query(sqlite3* db, const char* json, const char* path) {
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT json_extract(?1,?2)", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    const char* text = (const char*)sqlite3_column_text(st, 0); char* out = text ? u_strdup(text) : NULL;
    assert(sqlite3_finalize(st) == SQLITE_OK); return out;
}
static char* snapshot(struct UTUN_INSTANCE* inst, const char* group) {
    char* json = NULL; assert(!kanban_snapshot(inst, group, &json) && json); return json;
}
static void equal_boards(struct UTUN_INSTANCE* a, struct UTUN_INSTANCE* b, const char* group) {
    char* x = snapshot(a, group), *y = snapshot(b, group);
    char* cx = query(a->topo_sqlite_db, x, "$.cards"), *cy = query(b->topo_sqlite_db, y, "$.cards");
    char* px = query(a->topo_sqlite_db, x, "$.columns"), *py = query(b->topo_sqlite_db, y, "$.columns");
    if (!cx || !cy || !px || !py || strcmp(cx, cy) || strcmp(px, py))
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "test board mismatch: cards=%s / %s columns=%s / %s", cx, cy, px, py);
    assert(cx && cy && px && py && !strcmp(cx, cy) && !strcmp(px, py));
    uint8_t hx[32], hy[32];
    assert(!merkle_sync_read_hash(a, MT_DATA_KANBAN, group, 0, 0, hx));
    assert(!merkle_sync_read_hash(b, MT_DATA_KANBAN, group, 0, 0, hy) && !memcmp(hx, hy, 32));
    u_free(x); u_free(y); u_free(cx); u_free(cy); u_free(px); u_free(py);
}
static char* command(struct UTUN_INSTANCE* inst, const char* group, const char* action, const char* entity,
    const char* column, const char* text, const char* parents, int expected) {
    struct strbuf json = {0};
    assert(strbuf_addf(&json, "{\"action\":\"%s\",\"entity\":\"%s\",\"column\":\"%s\",\"parents\":%s,\"text\":",
        action, entity ? entity : "", column ? column : "", parents ? parents : "[]") >= 0);
    assert(!strbuf_add_json_string(&json, text ? text : "") && strbuf_addf(&json, "}") >= 0);
    char* out = NULL; int rc = kanban_command(inst, group, strbuf_str(&json), &out);
    if ((rc == 0) != (expected == 0)) DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "test command action=%s response=%s", action, out ? out : "NULL");
    assert((rc == 0) == (expected == 0) && out);
    strbuf_free(&json); return out;
}
static void role(struct UTUN_INSTANCE* inst, const char* group, const uint8_t member[32], const char* value) {
    struct chat_member_tags* tags = chat_member_tags_load(inst, group, member); assert(tags);
    assert(!chat_member_tags_set(tags, "moder", value) && !chat_member_tags_commit(inst, tags)); chat_member_tags_free(tags);
}

/* Одинаковые timestamp задаём через штатный persistent clock, подписи создаёт production API. */
static void clock_at(struct UTUN_INSTANCE* inst, const char* group, sqlite3_int64 time) {
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(inst->topo_sqlite_db, "UPDATE kanban_clock SET last_time=?1 WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?2) AND author=?3",
        -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_int64(st, 1, time - 1) == SQLITE_OK && sqlite3_bind_text(st, 2, group, -1, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 3, inst->my_ed25519_pubkey, 32, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_DONE);
    assert(sqlite3_changes(inst->topo_sqlite_db) == 1 && sqlite3_finalize(st) == SQLITE_OK);
}
static void move_card(struct UTUN_INSTANCE* inst, const char* group, const char* card, const char* column, const char* before) {
    char json[512], *out = NULL;
    snprintf(json, sizeof(json), "{\"action\":\"card_move\",\"entity\":\"%s\",\"column\":\"%s\",\"before\":\"%s\"}", card, column, before);
    assert(!kanban_command(inst, group, json, &out)); u_free(out);
}
static void positions(struct UTUN_INSTANCE** n, const char* group, const char* first) {
    char* board = snapshot(n[0], group), *second = query(n[0]->topo_sqlite_db, board, "$.columns[1].id"); u_free(board);
    char* out = command(n[0], group, "card_create", NULL, first, "anchor", NULL, 0);
    char* a = query(n[0]->topo_sqlite_db, out, "$.id"); u_free(out);
    out = command(n[0], group, "card_create", NULL, first, "moving", NULL, 0);
    char* b = query(n[0]->topo_sqlite_db, out, "$.id"); u_free(out); assert(a && b && second);
    copy_board(n[0], n[1], group);
    move_card(n[0], group, b, first, a);
    board = snapshot(n[0], group); assert(strstr(board, b) < strstr(board, a)); u_free(board);
    copy_board(n[0], n[1], group);
    move_card(n[0], group, a, first, "");
    out = command(n[0], group, "card_create", NULL, first, "created at bottom after moves", NULL, 0);
    char* appended = query(n[0]->topo_sqlite_db, out, "$.id"); u_free(out); assert(appended);
    board = snapshot(n[0], group); assert(strstr(board, b) < strstr(board, a) && strstr(board, a) < strstr(board, appended)); u_free(board);
    copy_board(n[0], n[1], group); equal_boards(n[0], n[1], group);
    sqlite3_int64 time = ntp_time_get_us(n[0]) + 1000000000;
    clock_at(n[0], group, time); clock_at(n[1], group, time);
    move_card(n[0], group, b, first, ""); move_card(n[1], group, b, second, "");
    copy_board(n[0], n[1], group); copy_board(n[1], n[0], group); equal_boards(n[0], n[1], group);
    char match[180]; snprintf(match, sizeof(match), "\"id\":\"%s\",\"column\":\"%s\"", b,
        memcmp(n[0]->my_ed25519_pubkey, n[1]->my_ed25519_pubkey, 32) > 0 ? first : second);
    board = snapshot(n[0], group); assert(strstr(board, match)); u_free(board);
    clock_at(n[0], group, time + 100); clock_at(n[1], group, time + 101);
    move_card(n[0], group, b, second, ""); move_card(n[1], group, b, first, a);
    copy_board(n[1], n[0], group); copy_board(n[0], n[1], group); equal_boards(n[0], n[1], group);
    board = snapshot(n[0], group); assert(strstr(board, b) < strstr(board, a)); u_free(board);
    out = command(n[1], group, "column_create", NULL, NULL, "Forbidden", NULL, -1); u_free(out);
    out = command(n[0], group, "column_delete", first, NULL, NULL, NULL, -1); u_free(out);
    out = command(n[0], group, "column_create", NULL, NULL, "Extra", NULL, 0);
    char* extra = query(n[0]->topo_sqlite_db, out, "$.id"); u_free(out); assert(extra);
    out = command(n[0], group, "column_rename", extra, NULL, "Renamed", NULL, 0); u_free(out);
    out = command(n[0], group, "column_delete", extra, NULL, NULL, NULL, 0); u_free(out);
    copy_board(n[0], n[1], group); equal_boards(n[0], n[1], group);
    board = snapshot(n[0], group);
    assert(sqlite3_exec(n[0]->topo_sqlite_db, "CREATE TEMP TRIGGER reject_kanban BEFORE INSERT ON kanban_ops BEGIN"
        " SELECT RAISE(ABORT,'injected kanban write failure'); END", NULL, NULL, NULL) == SQLITE_OK);
    out = command(n[0], group, "card_create", NULL, first, "must roll back", NULL, -1); u_free(out);
    char* after = snapshot(n[0], group); assert(!strcmp(board, after) && sqlite3_get_autocommit(n[0]->topo_sqlite_db));
    assert(sqlite3_exec(n[0]->topo_sqlite_db, "DROP TRIGGER reject_kanban", NULL, NULL, NULL) == SQLITE_OK);
    u_free(board); u_free(after);
    /* Порча личной подписи отбрасывается до записи страницы и не меняет снимок. */
    sqlite3_stmt* st = NULL; assert(sqlite3_prepare_v2(n[0]->topo_sqlite_db,
        "SELECT wire FROM kanban_ops WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND action=1 LIMIT 1", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    size_t size = (size_t)sqlite3_column_bytes(st, 0); uint8_t bytes[KB_WIRE_MAX + 2];
    bytes[0] = (uint8_t)(size >> 8); bytes[1] = (uint8_t)size; memcpy(bytes + 2, sqlite3_column_blob(st, 0), size);
    assert(sqlite3_finalize(st) == SQLITE_OK); bytes[size - 63] ^= 1;
    void* pull = NULL; assert(!begin(n[1], group, n[0]->my_ed25519_pubkey, 2, &pull));
    merkle_key_t key = {{0}};
    assert(stage(n[1], pull, 0, 0, &key, &key, 0, bytes, size + 2) == MT_ERR_DATA); abort_pull(n[1], pull);
    equal_boards(n[0], n[1], group);
    u_free(a); u_free(b); u_free(second); u_free(extra); u_free(appended);
}

static void color_edit(struct UTUN_INSTANCE* inst, const char* group, const char* card, const char* color, int expected) {
    char json[512], *out = NULL;
    snprintf(json, sizeof(json), "{\"action\":\"card_edit\",\"entity\":\"%s\",\"text\":\"color test\",\"color\":\"%s\"}", card, color);
    assert((kanban_command(inst, group, json, &out) == 0) == (expected == 0)); u_free(out);
}
static void has_color(struct UTUN_INSTANCE* inst, const char* group, const char* card, const char* expected) {
    char* board = snapshot(inst, group); sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(inst->topo_sqlite_db, "SELECT json_extract(value,'$.color') FROM json_each(?1,'$.cards')"
        " WHERE json_extract(value,'$.id')=?2", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, board, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_bind_text(st, 2, card, -1, SQLITE_STATIC) == SQLITE_OK);
    int rc = sqlite3_step(st); const char* actual = rc == SQLITE_ROW ? (const char*)sqlite3_column_text(st, 0) : NULL;
    if (!actual || strcmp(actual, expected)) fprintf(stderr, "color mismatch node=%016llx card=%s expected=%s actual=%s rc=%d\n",
        (unsigned long long)inst->node_id, card, expected, actual ? actual : "NULL", rc);
    assert(actual && !strcmp(actual, expected));
    assert(sqlite3_finalize(st) == SQLITE_OK); u_free(board);
}
static void colors(struct UTUN_INSTANCE** n, const char* group, const char* column) {
    char* out = command(n[0], group, "card_create", NULL, column, "color test", NULL, 0);
    char* card = query(n[0]->topo_sqlite_db, out, "$.id"); u_free(out); assert(card);
    char* board = snapshot(n[0], group), *palette = query(n[0]->topo_sqlite_db, board, "$.colors");
    struct kb_fields fields = {0};
    for (unsigned i = 0; i < KB_COLOR_COUNT; i++) {
        assert(kb_color(i) && strstr(palette, kb_color(i)));
        for (unsigned j = 0; j < i; j++) assert(strcmp(kb_color(i), kb_color(j)));
    }
    assert(!kb_color(KB_COLOR_COUNT)); u_free(board); u_free(palette);
    has_color(n[0], group, card, kb_color(0)); copy_board(n[0], n[1], group);
    color_edit(n[1], group, card, kb_color(8), 0); copy_board(n[1], n[0], group);
    has_color(n[0], group, card, kb_color(8));
    out = command(n[0], group, "card_edit", card, NULL, "color test", NULL, 0); u_free(out);
    has_color(n[0], group, card, kb_color(8)); copy_board(n[0], n[1], group);
    sqlite3_int64 time = ntp_time_get_us(n[0]) + 2000000000;
    clock_at(n[0], group, time); clock_at(n[1], group, time);
    color_edit(n[0], group, card, kb_color(1), 0); color_edit(n[1], group, card, kb_color(5), 0);
    copy_board(n[1], n[0], group); copy_board(n[0], n[1], group); equal_boards(n[0], n[1], group);
    has_color(n[0], group, card, kb_color(memcmp(n[0]->my_ed25519_pubkey, n[1]->my_ed25519_pubkey, 32) > 0 ? 1 : 5));
    color_edit(n[0], group, card, "#000000", -1);
    assert(kb_fields_read(n[0]->topo_sqlite_db, "{\"action\":\"column_create\",\"text\":\"x\",\"color\":\"#e45b5b\"}", &fields) < 0);
    assert(!kanban_history(n[0], group, card, &out) && strstr(out, "\"color\":\"#547ddb\"")); u_free(out); u_free(card);
}

/* Настоящий UDP и общий Merkle: READY запускает именно канбан при остановленном chat content. */
static void network(struct UASYNC* ua, struct UTUN_INSTANCE** n, const char* group, const char* column) {
    unsigned port = ss_get_port(&test_etcp_bound_socket(n[1], "s1")->local_addr);
    struct TOPO_ADDR4 address = {.addr = {127, 0, 0, 1}, .port = port, .protocol = TOPO_PROTO_UDP};
    struct TOPO_NODE node = {.node_id = n[1]->node_id, .v4_addrs = &address};
    memcpy(node.public_key, n[1]->my_keys.public_key, 32); struct NODE_CONN_DIRECT* transport = NULL;
    assert(node_conn_direct_open_node(n[0], node.node_id, NULL, NULL, &transport, &node, NULL) != NCD_ERR);
    uint64_t deadline = get_time_tb() + 150000;
    while (get_time_tb() < deadline) {
        struct ETCP_CONN* a = node_conn_direct_get_conn(transport), *b = instance_find_conn(n[1], n[0]->node_id);
        if (a && b && a->initialized && b->initialized && a->links_up && b->links_up) break;
        uasync_poll(ua, 5);
    }
    assert(get_time_tb() < deadline);
    struct TOPO_PEER_REQUEST* handles[2] = {0};
    for (unsigned i = 0; i < 2; i++) assert(!topo_group_peer_open(topo_groups_find_channel(n[i]->topo_groups, group), n[1 - i]->node_id, &handles[i]));
    deadline = get_time_tb() + 150000;
    int converged = 0;
    while (get_time_tb() < deadline) {
        char* board = snapshot(n[1], group); converged = strstr(board, "without chat") != NULL; u_free(board);
        if (converged) break;
        uasync_poll(ua, 5);
    }
    assert(converged && topo_group_peer_phase(handles[0]) == TOPO_PEER_READY && topo_group_peer_phase(handles[1]) == TOPO_PEER_READY);
    /* Проверка каждого пакета использует подготовленный протокол и committed ключи, вообще без DB handle. */
    for (unsigned i = 0; i < 2; i++) {
        sqlite3* db = n[i]->topo_sqlite_db; n[i]->topo_sqlite_db = NULL;
        struct ETCP_CONN* peer = instance_find_conn_by_key(n[i], n[1 - i]->my_keys.public_key);
        assert(group_protocol_current(n[i], group) == GROUP_PROTOCOL_KANBAN && eligible(n[i], group, peer));
        n[i]->topo_sqlite_db = db;
    }
    char* out = command(n[1], group, "card_create", NULL, column, "network return", NULL, 0); u_free(out);
    deadline = get_time_tb() + 150000; converged = 0;
    while (get_time_tb() < deadline) {
        char* board = snapshot(n[0], group); converged = strstr(board, "network return") != NULL; u_free(board);
        if (converged) break;
        uasync_poll(ua, 5);
    }
    assert(converged && !db_sync_instance_find(n[0], topo_groups_channel_slot(n[0]->topo_groups, group)));
    /* Снимок появляется до завершающего CHECK/DONE; дождаться подтверждения обеих корневых сумм. */
    while (get_time_tb() < deadline) {
        uint8_t a[32], b[32];
        assert(!merkle_sync_read_hash(n[0], MT_DATA_KANBAN, group, 0, 0, a) && !merkle_sync_read_hash(n[1], MT_DATA_KANBAN, group, 0, 0, b));
        if (!memcmp(a, b, 32)) break;
        uasync_poll(ua, 5);
    }
    equal_boards(n[0], n[1], group);
    topo_group_peer_close(handles[0]); topo_group_peer_close(handles[1]); node_conn_direct_close(transport);
}

/* Реальные команды карточки: подпись, проверка прав, текущие коллекции и Merkle. */
static char* feature_command(struct UTUN_INSTANCE* inst, const char* group, const char* card, const char* action,
    const uint8_t* target, const char* text, const char* before, int success) {
    struct strbuf json = {0};
    assert(strbuf_addf(&json, "{\"action\":\"%s\",\"entity\":\"%s\",\"before\":\"%s\"", action, card, before ? before : "") >= 0);
    if (target) assert(strbuf_addf(&json, ",\"target\":\"%s\"", sc_public_key_hex(target, (char[65]){0})) >= 0);
    if (text) assert(strbuf_addf(&json, ",\"text\":") >= 0 && !strbuf_add_json_string(&json, text));
    assert(strbuf_addf(&json, "}") >= 0);
    char* result = NULL; int rc = kanban_command(inst, group, strbuf_str(&json), &result);
    if ((rc == 0) != success) DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "feature command action=%s result=%s", action, result ? result : "NULL");
    assert((rc == 0) == success && result); strbuf_free(&json); return result;
}

static void card_field(struct UTUN_INSTANCE* inst, const char* group, const char* card, const char* field, const char* expected) {
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(inst->topo_sqlite_db, "SELECT json_extract(data_json,?3) FROM kanban_items WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND id=?2",
        -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_bind_text(st, 2, card, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_bind_text(st, 3, field, -1, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW); const char* value = (const char*)sqlite3_column_text(st, 0);
    if (!value || strcmp(value, expected)) DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "card field mismatch card=%s field=%s expected=%s actual=%s",
        card, field, expected, value ? value : "NULL");
    assert(value && !strcmp(value, expected)); assert(sqlite3_finalize(st) == SQLITE_OK);
}

static void inactive(struct UTUN_INSTANCE* inst, const char* group, const uint8_t member[32], int state) {
    struct chat_member_tags* tags = chat_member_tags_load(inst, group, member); assert(tags);
    assert(!chat_member_tags_set(tags, "deleted", state ? "yes" : "no") && !chat_member_tags_commit(inst, tags));
    chat_member_tags_free(tags);
}

static void card_details(struct UTUN_INSTANCE** n, const char* group, const char* column) {
    char* result = command(n[0], group, "card_create", NULL, column, "details fixture", NULL, 0);
    char* card = query(n[0]->topo_sqlite_db, result, "$.id"); u_free(result); assert(card);
    result = feature_command(n[0], group, card, "card_due", 0, "253402300799000000", NULL, 1); u_free(result);
    card_field(n[0], group, card, "$.due", "253402300799000000");
    result = feature_command(n[0], group, card, "card_due", 0, "9223372036854775808", NULL, 0); u_free(result);
    result = feature_command(n[0], group, card, "card_due", 0, "not-a-time", NULL, 0); u_free(result);
    result = feature_command(n[0], group, card, "card_due", 0, "1900000000000000", NULL, 1); u_free(result);
    result = feature_command(n[0], group, card, "card_author", n[1]->my_ed25519_pubkey, NULL, NULL, 1); u_free(result);
    result = feature_command(n[0], group, card, "card_due", 0, "", NULL, 0); u_free(result);
    copy_board(n[0], n[1], group); equal_boards(n[0], n[1], group);
    result = feature_command(n[1], group, card, "card_due", 0, "", NULL, 1); u_free(result);
    result = feature_command(n[1], group, card, "assign", n[0]->my_ed25519_pubkey, NULL, NULL, 1); u_free(result);
    result = feature_command(n[1], group, card, "unassign", n[0]->my_ed25519_pubkey, NULL, NULL, 1); u_free(result);
    result = feature_command(n[1], group, card, "assign", n[1]->my_ed25519_pubkey, NULL, NULL, 1); u_free(result);
    result = feature_command(n[1], group, card, "unassign", n[1]->my_ed25519_pubkey, NULL, NULL, 1); u_free(result);
    copy_board(n[1], n[0], group);
    result = feature_command(n[0], group, card, "assign", n[1]->my_ed25519_pubkey, NULL, NULL, 0); u_free(result);
    result = feature_command(n[0], group, card, "assign", n[0]->my_ed25519_pubkey, NULL, NULL, 1); u_free(result);
    result = feature_command(n[0], group, card, "message_create", 0, "hello https://example.com <script>", NULL, 1);
    char* message = query(n[0]->topo_sqlite_db, result, "$.id"); u_free(result); assert(message);
    copy_board(n[0], n[1], group);
    result = feature_command(n[1], group, card, "message_edit", 0, "forged", message, 0); u_free(result);
    result = feature_command(n[1], group, card, "message_delete", 0, NULL, message, 0); u_free(result);
    result = feature_command(n[0], group, card, "message_edit", 0, "edited", message, 1); u_free(result);
    result = feature_command(n[0], group, card, "message_delete", 0, NULL, message, 1); u_free(result);
    result = feature_command(n[0], group, card, "message_edit", 0, "resurrect", message, 0); u_free(result);
    for (int i = 0; i < 52; i++) {
        result = feature_command(n[0], group, card, "message_create", 0, "page", NULL, 1); u_free(result);
    }
    copy_board(n[0], n[1], group); copy_board(n[1], n[0], group); equal_boards(n[0], n[1], group);
    struct strbuf read = {0}; assert(strbuf_addf(&read, "{\"entity\":\"%s\"}", card) >= 0);
    assert(kanban_messages_read(n[0]->topo_sqlite_db, group, strbuf_str(&read), &result) == SQLITE_OK);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(n[0]->topo_sqlite_db, "SELECT json_array_length(?1,'$.messages'),json_extract(?1,'$.messages[49].id')", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, result, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_int(st, 0) == 51); char* cursor = u_strdup((const char*)sqlite3_column_text(st, 1));
    assert(sqlite3_finalize(st) == SQLITE_OK); u_free(result); strbuf_free(&read);
    assert(strbuf_addf(&read, "{\"entity\":\"%s\",\"before\":\"%s\"}", card, cursor) >= 0);
    assert(kanban_messages_read(n[0]->topo_sqlite_db, group, strbuf_str(&read), &result) == SQLITE_OK);
    assert(strstr(result, "\"deleted\":true") && !strstr(result, "hello"));
    u_free(result); u_free(cursor); strbuf_free(&read);
    /* Старая цель загружается одним ограниченным окном, включая tombstone; чужая карточка не подменяет контекст. */
    assert(strbuf_addf(&read, "{\"entity\":\"%s\",\"around\":\"%s\"}", card, message) >= 0);
    assert(!kanban_messages_read(n[0]->topo_sqlite_db, group, strbuf_str(&read), &result));
    char* found = query(n[0]->topo_sqlite_db, result, "$.target_found"); assert(found && !strcmp(found, "1")); u_free(found);
    assert(strstr(result, message) && strstr(result, "\"deleted\":true")); u_free(result); strbuf_free(&read);
    assert(strbuf_addf(&read, "{\"entity\":\"%s\",\"around\":\"%s\"}", column, message) >= 0);
    assert(!kanban_messages_read(n[0]->topo_sqlite_db, group, strbuf_str(&read), &result));
    assert(strstr(result, "\"target_found\":false") && strstr(result, "\"messages\":[]")); u_free(result); strbuf_free(&read);
    assert(strbuf_addf(&read, "{\"entity\":\"%s\",\"around\":\"%s\",\"before\":\"%s\"}", card, message, message) >= 0);
    assert(kanban_messages_read(n[0]->topo_sqlite_db, group, strbuf_str(&read), &result) == SQLITE_MISMATCH && !result);
    strbuf_free(&read);
    result = snapshot(n[0], group); assert(!strstr(result, "\"messages\"")); u_free(result);
    /* Неактивность закрывает историю по подписанному срезу, но сохраняет идентичность и назначения. */
    result = command(n[1], group, "card_create", NULL, column, "recovery fixture", NULL, 0);
    char* recovery = query(n[1]->topo_sqlite_db, result, "$.id"); u_free(result); assert(recovery);
    result = feature_command(n[1], group, card, "message_create", 0, "before inactive cutoff", NULL, 1); u_free(result);
    copy_board(n[1], n[0], group);
    result = feature_command(n[1], group, card, "message_create", 0, "beyond inactive cutoff", NULL, 1);
    char* rejected = query(n[1]->topo_sqlite_db, result, "$.id"); u_free(result);
    inactive(n[0], group, n[1]->my_ed25519_pubkey, 1); copy_members(n[0], n[1], group); copy_board(n[0], n[1], group);
    char owner0[65], owner1[65]; kb_hex(n[0]->my_ed25519_pubkey, owner0); kb_hex(n[1]->my_ed25519_pubkey, owner1);
    card_field(n[0], group, card, "$.author", owner1); card_field(n[1], group, card, "$.author", owner1);
    card_field(n[0], group, card, "$.author_active", "0");
    sqlite3_stmt* rejected_op = NULL;
    assert(sqlite3_prepare_v2(n[1]->topo_sqlite_db, "SELECT active FROM kanban_ops WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND lower(hex(id))=?2",
        -1, &rejected_op, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(rejected_op, 1, group, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_bind_text(rejected_op, 2, rejected, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(rejected_op) == SQLITE_ROW);
    assert(sqlite3_column_int(rejected_op, 0) == 0 && sqlite3_finalize(rejected_op) == SQLITE_OK); u_free(rejected);
    result = feature_command(n[1], group, card, "card_due", 0, "", NULL, 0); u_free(result);
    result = feature_command(n[0], group, card, "card_due", 0, "2000000000000000", NULL, 1); u_free(result);
    result = feature_command(n[0], group, recovery, "card_author", n[0]->my_ed25519_pubkey, NULL, NULL, 1); u_free(result);
    inactive(n[0], group, n[1]->my_ed25519_pubkey, 0); copy_members(n[0], n[1], group);
    copy_board(n[0], n[1], group); copy_board(n[1], n[0], group); equal_boards(n[0], n[1], group);
    card_field(n[0], group, card, "$.author", owner1); card_field(n[1], group, card, "$.due", "2000000000000000");
    card_field(n[0], group, recovery, "$.author", owner0); card_field(n[1], group, recovery, "$.author", owner0);
    result = feature_command(n[0], group, card, "card_due", 0, "", NULL, 0); u_free(result);
    u_free(recovery);
    u_free(message); u_free(card);
}

struct card_attachment_result { int calls, success; };
static void card_attachment_done(void* data, int success) {
    struct card_attachment_result* result = data; result->calls++; result->success = success;
}
static void card_download_done(void* data, int error, const char* path) {
    struct card_attachment_result* result = data; result->calls++; result->success = !error;
    assert(error ? !path : path && ma_file_size(path) == 15);
}
static struct { int calls; uint8_t group[32]; char* json; } card_gui_reply;
/* Проверяем настоящий wire-ответ GUI, а не подменённый результат загрузчика. */
static void card_gui_event(struct UTUN_INSTANCE* inst, int type, const uint8_t* bytes, int size) {
    (void)inst;
    if (type != CHAT_EVT_KANBAN_RESULT) return;
    assert(bytes && size > 40 && !card_gui_reply.calls++);
    uint64_t request = 0;
    for (unsigned i = 0; i < 8; i++) request |= (uint64_t)bytes[i] << (i * 8);
    assert(request == 17 && !memcmp(bytes + 8, card_gui_reply.group, 32));
    card_gui_reply.json = u_malloc((size_t)size - 39); assert(card_gui_reply.json);
    memcpy(card_gui_reply.json, bytes + 40, (size_t)size - 40); card_gui_reply.json[size - 40] = 0;
}
/* Локальный проверенный файл, неправильная/освобождённая ссылка и поздний callback после restart. */
static void card_gui_download(struct UTUN_INSTANCE* inst, const char* group, const char* message, const uint8_t object[16],
    const char* expected_path, int restart) {
    char uuid[33], json[256];
    for (unsigned i = 0; i < 16; i++) snprintf(uuid + i * 2, 3, "%02x", object[i]);
    snprintf(json, sizeof(json), "{\"action\":\"download\",\"entity\":\"%s\",\"object\":\"%s\"}", message, uuid);
    struct kanban_request* req = u_calloc(1, sizeof(*req) + strlen(json) + 1); assert(req);
    req->inst = inst; req->request = 17; strcpy(req->group, group); strcpy(req->json, json);
    memset(&card_gui_reply, 0, sizeof(card_gui_reply)); assert(!sc_hex_to_binary(group, card_gui_reply.group, 32));
    chat_event_set_handler(inst, card_gui_event); kanban_gui_command(req);
    if (restart) { kanban_service_stop(inst); assert(!kanban_service_start(inst)); }
    kb_file_wait(inst->ua, &card_gui_reply.calls); chat_event_set_handler(inst, NULL);
    assert(card_gui_reply.json && strstr(card_gui_reply.json, expected_path ? "\"ok\":true" : "\"ok\":false"));
    if (expected_path) {
        char* path = query(inst->topo_sqlite_db, card_gui_reply.json, "$.path");
        assert(path && !strcmp(path, expected_path) && ma_file_size(path) == 15); u_free(path);
    }
    u_free(card_gui_reply.json); card_gui_reply.json = NULL;
}
static void card_ready_done(void* data, int error, const struct media_index_result* result) {
    struct card_attachment_result* completion = data; completion->calls++; completion->success = !error && result != NULL;
}
/* Настоящий файл и подписанный журнал: независимость от chat stop, неизменность manifest и освобождение ссылки. */
static void card_attachment(struct UTUN_INSTANCE** nodes, const char* group, const char* column) {
    struct UTUN_INSTANCE* inst = nodes[0]; assert(!inst->chat_content_started && inst->kanban_started);
    char* result = command(inst, group, "card_create", NULL, column, "file fixture", NULL, 0);
    char* card = query(inst->topo_sqlite_db, result, "$.id"); u_free(result); assert(card);
    char media[1024], directory[1024], path[1024]; const char* base = chat_core_media_base(inst);
    assert(base && snprintf(media, sizeof(media), "%s/media", base) < (int)sizeof(media));
    assert(!utun_mkdir(media, 0700) || errno == EEXIST);
    assert(snprintf(directory, sizeof(directory), "%s/%s", media, group) < (int)sizeof(directory));
    assert(!utun_mkdir(directory, 0700) || errno == EEXIST);
    assert(snprintf(path, sizeof(path), "%s/card.file", directory) < (int)sizeof(path));
    FILE* source = fopen(path, "wb"); assert(source && fwrite("card attachment", 1, 15, source) == 15 && !fclose(source));
    struct attachment_info info = {.kind=ATTACHMENT_FILE, .name="card.txt"}; struct card_attachment_result completion = {0};
    kanban_attachment_send(inst, group, card, path, &info, "application/octet-stream", 0, card_attachment_done, &completion);
    kb_file_wait(inst->ua, &completion.calls); assert(completion.success);
    sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(inst->topo_sqlite_db, "SELECT lower(hex(message)),id,object FROM attachment_jobs WHERE owner=2 AND state=3", -1, &st, NULL));
    assert(sqlite3_step(st) == SQLITE_ROW); char* message = u_strdup((const char*)sqlite3_column_text(st, 0));
    uint8_t job[16], object[16]; memcpy(job, sqlite3_column_blob(st, 1), 16); memcpy(object, sqlite3_column_blob(st, 2), 16);
    assert(!sqlite3_finalize(st));
    struct attachment_ref ref = {.owner=ATTACHMENT_OWNER_KANBAN}; strcpy(ref.space, group);
    assert(!kb_unhex(message, ref.message)); memcpy(ref.object, object, 16); completion = (struct card_attachment_result){0};
    attachment_group_download(inst, &ref, card_download_done, &completion); kb_file_wait(inst->ua, &completion.calls); assert(completion.success);
    card_gui_download(inst, group, message, object, path, 0);
    card_gui_download(inst, group, "invalid", object, NULL, 0);
    uint8_t wrong_object[16]; memcpy(wrong_object, object, 16); wrong_object[0] ^= 1;
    card_gui_download(inst, group, message, wrong_object, NULL, 0);
    card_gui_download(inst, group, message, object, NULL, 1);
    struct strbuf command_json = {0}; assert(strbuf_addf(&command_json, "{\"entity\":\"%s\"}", card) >= 0);
    assert(!kanban_messages_read(inst->topo_sqlite_db, group, strbuf_str(&command_json), &result));
    char* manifest = query(inst->topo_sqlite_db, result, "$.messages[0].d"); assert(manifest && strstr(result, "application/octet-stream")); u_free(result);
    result = feature_command(inst, group, card, "message_edit", 0, "caption", message, 1); u_free(result);
    assert(!kanban_messages_read(inst->topo_sqlite_db, group, strbuf_str(&command_json), &result));
    char* edited = query(inst->topo_sqlite_db, result, "$.messages[0].d"); assert(edited && !strcmp(manifest, edited)); u_free(result); u_free(edited);
    copy_board(inst, nodes[1], group); equal_boards(inst, nodes[1], group);
    /* Повтор READY/PUBLISHED intent возвращает тот же ID даже после изменения caption. */
    struct strbuf replay = {0}; assert(strbuf_addf(&replay, "{\"action\":\"message_create\",\"entity\":\"%s\",\"ct\":\"application/octet-stream\",\"d\":", card) >= 0);
    assert(!strbuf_add_json_string(&replay, manifest) && strbuf_addf(&replay, "}") >= 0);
    struct kanban_write write = {.group=group, .json=strbuf_str(&replay), .writer={.now=ntp_time_get_us(inst)}};
    memcpy(write.writer.public_key, inst->my_ed25519_pubkey, 32); memcpy(write.writer.private_key, inst->my_ed25519_privkey, 32);
    memcpy(write.attachment_job, job, 16); memcpy(write.attachment_object, object, 16);
    assert(!sqlite3_exec(inst->topo_sqlite_db, "BEGIN IMMEDIATE", NULL, NULL, NULL) && !kanban_command_write(inst->topo_sqlite_db, &write));
    assert(!sqlite3_exec(inst->topo_sqlite_db, "COMMIT", NULL, NULL, NULL));
    char* repeated = query(inst->topo_sqlite_db, write.result, "$.id"); assert(repeated && !strcmp(message, repeated));
    OPENSSL_cleanse(&write.writer, sizeof(write.writer)); u_free(write.result); u_free(repeated); strbuf_free(&replay);
    result = feature_command(inst, group, card, "message_delete", 0, NULL, message, 1); u_free(result);
    assert(!kanban_messages_read(inst->topo_sqlite_db, group, strbuf_str(&command_json), &result));
    assert(!strstr(result, "\"ct\"") && !strstr(result, "\"d\"")); u_free(result);
    assert(!sqlite3_prepare_v2(inst->topo_sqlite_db, "SELECT released FROM attachment_refs WHERE owner=2 AND object=?1", -1, &st, NULL));
    assert(!sqlite3_bind_blob(st, 1, object, 16, SQLITE_STATIC) && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1);
    assert(!sqlite3_finalize(st)); u_free(manifest); u_free(card); strbuf_free(&command_json);
    assert(attachment_store_public_read(inst->topo_sqlite_db, ref.space, ref.object) == 1);
    completion = (struct card_attachment_result){0};
    attachment_group_download(inst, &ref, card_download_done, &completion); kb_file_wait(inst->ua, &completion.calls); assert(!completion.success);
    card_gui_download(inst, group, message, object, NULL, 0); u_free(message);
    /* READY переживает полную остановку групп; новый runtime проверяет файл и публикует ровно один раз. */
    result = command(inst, group, "card_create", NULL, column, "recovered file", NULL, 0);
    card = query(inst->topo_sqlite_db, result, "$.id"); u_free(result); assert(card);
    attachment_recovery_stop(inst); completion = (struct card_attachment_result){0};
    struct attachment_group_file intent = {.owner=ATTACHMENT_OWNER_KANBAN, .group=group, .card=card, .source=path,
        .destination=path, .type="application/octet-stream", .description="recovered.txt"};
    attachment_group_register(inst, &intent, job, NULL, card_ready_done, &completion);
    kb_file_wait(inst->ua, &completion.calls); assert(completion.success);
    enum attachment_job_state state; uint8_t recovered[65];
    assert(!attachment_store_job_read(inst->topo_sqlite_db, job, &state, recovered) && state == ATTACHMENT_JOB_READY);
    group_service_stop(inst); assert(!group_service_start(inst) && !kanban_service_start(inst));
    uint64_t end = get_time_us() + 5000000;
    while (get_time_us() < end) {
        uasync_poll(inst->ua, 1); assert(!attachment_store_job_read(inst->topo_sqlite_db, job, &state, recovered));
        if (state == ATTACHMENT_JOB_PUBLISHED) break;
    }
    assert(state == ATTACHMENT_JOB_PUBLISHED);
    assert(strbuf_addf(&command_json, "{\"entity\":\"%s\"}", card) >= 0);
    assert(!kanban_messages_read(inst->topo_sqlite_db, group, strbuf_str(&command_json), &result));
    char* length = query(inst->topo_sqlite_db, result, "$.messages"); assert(length);
    assert(!sqlite3_prepare_v2(inst->topo_sqlite_db, "SELECT json_array_length(?1,'$.messages')", -1, &st, NULL));
    assert(!sqlite3_bind_text(st, 1, result, -1, SQLITE_STATIC) && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1);
    assert(!sqlite3_finalize(st)); u_free(length); u_free(result); u_free(card); strbuf_free(&command_json);
    DEBUG_INFO(DEBUG_CATEGORY_KANBAN, "PASS card attachment: chat stopped, real file/index, signed peer replay, immutable manifest, idempotent publication and reference release");
}

static struct card_attachment_result video_submission;
static void card_video_event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int length) {
    (void)inst;
    if (type != CHAT_EVT_MSG_SUBMITTED) return;
    assert(length == 9); uint64_t request = 0;
    for (unsigned i = 0; i < 8; i++) request |= (uint64_t)data[i] << (8 * i);
    assert(request == 7); video_submission.calls++; video_submission.success = data[8];
}
static void card_video_download(void* data, int error, const char* path) {
    card_download_done(data, error, path); assert(!error && strlen(path) >= 4 && !strcmp(path + strlen(path) - 4, ".mkv"));
    FILE* file = fopen(path, "rb"); char bytes[15];
    assert(file && fread(bytes, 1, sizeof(bytes), file) == sizeof(bytes) && !fclose(file) && !memcmp(bytes, "card attachment", sizeof(bytes)));
}
/* Запись проходит общий sender без перекодирования: MKV, метаданные, исходная карточка и подписи второго узла. */
static void card_video_recording(struct UTUN_INSTANCE** nodes, const char* group, const char* column) {
    struct UTUN_INSTANCE* inst = nodes[0]; assert(!inst->chat_content_started && inst->kanban_started);
    char* result = command(inst, group, "card_create", NULL, column, "video fixture", NULL, 0);
    char* card = query(inst->topo_sqlite_db, result, "$.id"); u_free(result); assert(card);
    char source[1024]; assert(snprintf(source, sizeof(source), "%s/recording-snapshot.mkv", chat_core_media_base(inst)) < (int)sizeof(source));
    FILE* file = fopen(source, "wb"); assert(file && fwrite("card attachment", 1, 15, file) == 15 && !fclose(file));
    struct attachment_send_req* request = u_calloc(1, sizeof(*request)); assert(request);
    request->inst = inst; request->request_id = 7; request->temporary_source = 1;
    strcpy(request->target, group); strcpy(request->card, card); strcpy(request->source, source);
    request->info = (struct attachment_info){.kind=ATTACHMENT_VIDEO, .name="recording.mkv", .duration_ms=1234, .width=1920, .height=1080};
    strcpy(request->content_type, "video/x-matroska"); chat_event_set_handler(inst, card_video_event);
    attachment_send_trampoline(request);
    kb_file_wait(inst->ua, &video_submission.calls); assert(video_submission.success);
    chat_event_set_handler(inst, NULL); assert(ma_file_size(source) < 0);
    struct strbuf read = {0}; assert(strbuf_addf(&read, "{\"entity\":\"%s\"}", card) >= 0);
    assert(!kanban_messages_read(inst->topo_sqlite_db, group, strbuf_str(&read), &result));
    char* type = query(inst->topo_sqlite_db, result, "$.messages[0].ct"); assert(type && !strcmp(type, "video/x-matroska")); u_free(type);
    char* body = query(inst->topo_sqlite_db, result, "$.messages[0].d"), *message = query(inst->topo_sqlite_db, result, "$.messages[0].id"); u_free(result);
    assert(body && message && strstr(body, "|dur=1234|w=1920|h=1080"));
    struct media_index_result indexed; assert(!attachment_manifest_parse(body, &indexed) && indexed.file_size == 15);
    struct attachment_ref ref = {.owner=ATTACHMENT_OWNER_KANBAN}; strcpy(ref.space, group);
    assert(!kb_unhex(message, ref.message)); memcpy(ref.object, indexed.media_id, 16);
    struct card_attachment_result downloaded = {0}; attachment_group_download(inst, &ref, card_video_download, &downloaded);
    kb_file_wait(inst->ua, &downloaded.calls); assert(downloaded.success);
    copy_board(inst, nodes[1], group); equal_boards(inst, nodes[1], group);
    assert(!kanban_messages_read(nodes[1]->topo_sqlite_db, group, strbuf_str(&read), &result));
    char* remote = query(nodes[1]->topo_sqlite_db, result, "$.messages[0].d"); assert(remote && !strcmp(body, remote));
    u_free(result); u_free(remote); u_free(body); u_free(message); u_free(card); strbuf_free(&read); media_index_result_free(&indexed);
    DEBUG_INFO(DEBUG_CATEGORY_KANBAN, "PASS card recording: MKV sender, original card, unchanged bytes, duration/dimensions and signed peer replay");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_KANBAN, DEBUG_LEVEL_INFO);
    size_t allocations = u_get_allocated_count();
    utun_instance_set_tun_init_enabled(0);
    struct UASYNC* ua = uasync_create(); assert(ua);
    struct UTUN_INSTANCE* nodes[2]; char directory[512]; const char* temporary = getenv("TMPDIR");
    assert(snprintf(directory, sizeof(directory), "%s/utun-kanban-XXXXXX", temporary && *temporary ? temporary : "/tmp") < (int)sizeof(directory));
    assert(!test_mkdtemp(directory)); DEBUG_INFO(DEBUG_CATEGORY_KANBAN, "Test data directory: %s", directory);
    for (unsigned i = 0; i < 2; i++) {
        uint8_t device_x[32], device_seed[32]; char pub[65], priv[65], config[2048]; assert(!sc_identity_generate_config(device_x, device_seed));
        kb_hex(device_x, pub); kb_hex(device_seed, priv);
        snprintf(config, sizeof(config), "[global]\nmy_private_key=%s\nmy_public_key=%s\ndb_path=%s/node%u\n"
            "[server:s1]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=1\n[chatserver]\n"
            "group_autoconnect=0\nstorage_autoload=0\nmember_files_enabled=0\n[ntp]\nenabled=no\n", priv, pub, directory, i);
        nodes[i] = utun_instance_create_from_str(ua, config); assert(nodes[i]);
        if (!i) assert(!utun_core_start(nodes[i]) && !kanban_service_start(nodes[i]) && !nodes[i]->chat_started);
        else assert(!utun_instance_init(nodes[i]));
    }
    uint64_t id = group_core_create_auto(nodes[0], "Board", "kanban"); assert(id);
    char group[65]; strcpy(group, topo_groups_channel(nodes[0]->topo_groups, id));
    assert(!db_sync_instance_find(nodes[0], id) && !nodes[0]->chat_content_started);
    assert(!chat_service_start(nodes[0]) && !db_sync_instance_find(nodes[0], id));
    uint8_t x[32], ed[32], private[32]; uint8_t owner[32]; struct group_info_record info;
    assert(!group_info_get(nodes[0]->topo_sqlite_db, group, &info));
    assert(!topo_node_sqlite_channel_get(nodes[0]->topo_sqlite_db, group, NULL, 0, owner, x, ed, NULL));
    assert(!topo_node_sqlite_channel_get_priv(nodes[0]->topo_sqlite_db, group, private));
    assert(!topo_node_sqlite_channel_put_description(nodes[1]->topo_sqlite_db, group, info.json, owner, x, NULL, ed, NULL, info.signature));
    assert(!chat_core_ensure_channel_ready(nodes[1], group));
    assert(test_member_seed(nodes[0], group, nodes[1]->my_keys.public_key, nodes[1]->my_ed25519_pubkey,
        nodes[1]->my_ed25519_privkey, private, "{\"name\":\"Second\"}", NULL, NULL) >= 0);
    copy_members(nodes[0], nodes[1], group);
    char* board = snapshot(nodes[0], group), *column = query(nodes[0]->topo_sqlite_db, board, "$.columns[0].id"); u_free(board); assert(column);
    char* response = command(nodes[0], group, "card_create", NULL, column, "first\nsecond\nthird\n", NULL, 0);
    char* card = query(nodes[0]->topo_sqlite_db, response, "$.id"); u_free(response); assert(card);
    copy_board(nodes[0], nodes[1], group); equal_boards(nodes[0], nodes[1], group);
    response = command(nodes[0], group, "card_edit", card, NULL, "FIRST\nsecond\nthird\n", NULL, 0); u_free(response);
    response = command(nodes[1], group, "card_edit", card, NULL, "first\nsecond\nTHIRD\n", NULL, 0); u_free(response);
    copy_board(nodes[0], nodes[1], group); copy_board(nodes[1], nodes[0], group); equal_boards(nodes[0], nodes[1], group);
    board = snapshot(nodes[0], group); char* text = query(nodes[0]->topo_sqlite_db, board, "$.cards[0].text");
    assert(text && !strcmp(text, "FIRST\nsecond\nTHIRD\n")); u_free(text); u_free(board);
    response = command(nodes[0], group, "card_edit", card, NULL, "left", NULL, 0); u_free(response);
    response = command(nodes[1], group, "card_edit", card, NULL, "right", NULL, 0); u_free(response);
    copy_board(nodes[0], nodes[1], group); copy_board(nodes[1], nodes[0], group);
    board = snapshot(nodes[0], group); text = query(nodes[0]->topo_sqlite_db, board, "$.cards[0].conflict");
    assert(text && !strcmp(text, "1")); u_free(text); u_free(board);
    response = command(nodes[0], group, "card_edit", card, NULL, "resolved", NULL, 0); u_free(response);
    copy_board(nodes[0], nodes[1], group); equal_boards(nodes[0], nodes[1], group);
    response = command(nodes[1], group, "card_delete", card, NULL, NULL, NULL, -1); u_free(response);
    role(nodes[0], group, nodes[1]->my_ed25519_pubkey, "yes"); copy_members(nodes[0], nodes[1], group);
    response = command(nodes[1], group, "card_delete", card, NULL, NULL, NULL, 0); u_free(response);
    copy_board(nodes[1], nodes[0], group);
    response = command(nodes[1], group, "card_create", NULL, column, "late offline card", NULL, 0);
    char* late = query(nodes[1]->topo_sqlite_db, response, "$.id"); u_free(response);
    role(nodes[0], group, nodes[1]->my_ed25519_pubkey, "no"); copy_members(nodes[0], nodes[1], group);
    /* MEMBERS может прийти раньше самих частей среза. */
    board = snapshot(nodes[1], group);
    assert(strstr(board, "\"deleted\":true") && strstr(board, "\"pending_history\":true")); u_free(board);
    copy_board(nodes[0], nodes[1], group); copy_board(nodes[1], nodes[0], group); equal_boards(nodes[0], nodes[1], group);
    assert(!kanban_history(nodes[1], group, late, &response) && strstr(response, "\"active\":false")); u_free(response);
    board = snapshot(nodes[0], group); assert(strstr(board, "\"deleted\":true") && !strstr(board, "late offline card")); u_free(board);
    positions(nodes, group, column);
    colors(nodes, group, column);
    card_details(nodes, group, column);
    chat_service_stop(nodes[0]);
    assert(nodes[0]->group_started && nodes[0]->kanban_started && !nodes[0]->chat_content_started);
    card_attachment(nodes, group, column);
    card_video_recording(nodes, group, column);
    response = command(nodes[0], group, "card_create", NULL, column, "without chat", NULL, 0); u_free(response);
    kanban_service_stop(nodes[0]); assert(nodes[0]->group_started && nodes[0]->chat_core && nodes[0]->chat_sync);
    assert(!kanban_service_start(nodes[0]));
    response = snapshot(nodes[0], group); assert(strstr(response, "without chat")); u_free(response);
    network(ua, nodes, group, column);
    frozen_file_round(nodes[0], group);
    for (unsigned i = 0; i < 2; i++) utun_instance_destroy(nodes[i]);
    uasync_destroy(ua, 1); u_free(column); u_free(card); u_free(late); OPENSSL_cleanse(private, sizeof(private));
    assert(!test_remove_tree(directory)); DEBUG_INFO(DEBUG_CATEGORY_KANBAN, "Test data removed: %s", directory);
    assert(u_get_allocated_count() == allocations);
    puts("PASS kanban: independent group lifecycle, signed sync, parallel text merge, moderation, immutable history and revocation seal");
    return 0;
}
