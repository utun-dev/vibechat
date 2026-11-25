/* Проверяем границы автомата на точных wire-пакетах. Включение реализации позволяет
 * задать состояние без копирования private-структур и без подмены очередей/таймеров. */
#include "../lib/platform_compat.h"
static int fail_random;
static int join_allowed;
struct UTUN_INSTANCE;
struct chat_join_path;
static int t_random_bytes(uint8_t* data, size_t len);
static int t_join_allowed(struct UTUN_INSTANCE* inst, const struct chat_join_path* path, const char* channel);
#define random_bytes t_random_bytes
#define chat_sync_join_members_allowed t_join_allowed
#include "../src/chat/merkle_sync.c"
#undef chat_sync_join_members_allowed
#undef random_bytes
#include <assert.h>

static int t_random_bytes(uint8_t* data, size_t len) {
    if (fail_random) { fail_random = 0; return -1; }
    return random_bytes(data, len);
}

static int t_join_allowed(struct UTUN_INSTANCE* inst, const struct chat_join_path* path, const char* channel) {
    (void)inst; (void)path; (void)channel;
    return join_allowed;
}

struct fixture {
    struct UTUN_INSTANCE inst;
    struct ETCP_CONN conn;
    int callbacks, result, apply_error, authorized, applied, running;
};

static int t_hash(void* arg, const char* ns, uint8_t level, uint64_t prefix, EVP_MD_CTX* hash) {
    (void)arg; (void)ns; (void)level; (void)prefix;
    return EVP_DigestUpdate(hash, "record", 6) == 1 ? 1 : -1;
}

static int t_page(void* arg, const char* ns, uint64_t prefix, int has_after, const merkle_key_t* after,
        uint8_t* data, size_t* len, merkle_key_t* next, int* more) {
    (void)arg; (void)ns; (void)prefix; (void)has_after; (void)after;
    memset(data, 0, *len); memset(next, 0, sizeof(*next)); ms_write64(next->bytes, 1); *more = 1; return 0;
}

static int t_apply(void* arg, const char* ns, const uint8_t peer[32], const uint8_t* data, size_t len) {
    (void)ns; (void)peer; (void)data; (void)len;
    struct fixture* f = arg;
    f->applied++; return f->apply_error;
}

static const struct merkle_sync_data_ops t_ops = {
    .update_bucket_hash = t_hash, .get_page = t_page, .apply_items = t_apply
};

static void t_done(const uint8_t peer[32], const char* ns, int result, void* arg) {
    (void)peer; (void)ns;
    struct fixture* f = arg;
    f->callbacks++; f->result = result;
}

static void t_init(struct fixture* f) {
    memset(f, 0, sizeof(*f));
    f->inst.node_id = 1; f->inst.my_ed25519_pubkey[31] = 1;
    f->inst.ua = uasync_create(); assert(f->inst.ua);
    assert(sqlite3_open(":memory:", &f->inst.topo_sqlite_db) == SQLITE_OK);
    f->inst.connections = queue_new(f->inst.ua, 16, offsetof(struct conn_queue_entry, x25519), SC_PUBKEY_SIZE, "test connections"); assert(f->inst.connections);
    f->conn.instance = &f->inst; f->conn.peer_node_id = 2; f->conn.peer_ed25519_pubkey[31] = 2; f->conn.initialized = 1; f->conn.links_up = 1;
    f->conn.send_input_q = queue_new(f->inst.ua, 0, 0, 0, "test send"); assert(f->conn.send_input_q);
    queue_set_waiter_defer(f->conn.send_input_q, 1);
    struct ll_entry* e = queue_entry_new(sizeof(struct conn_queue_entry)); assert(e);
    struct conn_queue_entry* ce = (struct conn_queue_entry*)e->data;
    ce->peer_node_id = 2; ce->conn = &f->conn;
    assert(queue_data_put_with_index(f->inst.connections, e) == 0);
    assert(merkle_sync_init(&f->inst, 0x72, &t_ops, f) == 0);
}

static void t_clear_queue(struct ll_queue* q) {
    struct ll_entry* e;
    while ((e = queue_data_get(q))) { queue_dgram_free(e); queue_entry_free(e); }
    queue_free(q);
}

static void t_destroy(struct fixture* f) {
    merkle_sync_destroy(&f->inst);
    assert(f->conn.ref_count == 0);
    t_clear_queue(f->conn.send_input_q); t_clear_queue(f->inst.connections);
    sqlite3_close(f->inst.topo_sqlite_db);
    uasync_poll(f->inst.ua, 0); uasync_destroy(f->inst.ua, 0);
}

static struct ms_session* t_session(struct fixture* f) {
    struct ms_session* s = ms_create(f->inst.msync, &f->conn, NULL, MT_DATA_MEMBERS, 1001, MS_EXCHANGE); assert(s);
    s->round = 7; s->state = MS_PULL; s->request_id = 1; s->waiting = 1;
    ms_write64(s->round_id, 7); memset(s->accept_id, 0xa7, sizeof(s->accept_id));
    s->requests = u_calloc(1, sizeof(*s->requests)); assert(s->requests);
    s->requests->cb = t_done; s->requests->arg = f;
    return s;
}

static void t_receive_tokens(struct fixture* f, uint8_t type, uint64_t round, uint32_t request,
        const uint8_t* round_id, const uint8_t* accept_id, const uint8_t* data, size_t len) {
    struct ll_entry* e = ll_alloc_lldgram((uint16_t)(MS_HEADER + len)); assert(e);
    e->len = (uint16_t)(MS_HEADER + len); e->dgram[0] = 0x72; e->dgram[MS_DATASET_OFF] = MT_DATA_MEMBERS;
    topo_group_utun_wire_id(e->dgram + MS_SCOPE_OFF, 1001); e->dgram[MS_TYPE_OFF] = type;
    ms_write64(e->dgram + MS_ROUND_OFF, round); ms_write32(e->dgram + MS_REQUEST_OFF, request);
    memset(e->dgram + MS_ROUND_ID_OFF, 0, 2 * MS_TOKEN_SIZE);
    if (round_id) memcpy(e->dgram + MS_ROUND_ID_OFF, round_id, MS_TOKEN_SIZE);
    if (accept_id) memcpy(e->dgram + MS_ACCEPT_ID_OFF, accept_id, MS_TOKEN_SIZE);
    if (len) memcpy(e->dgram + MS_HEADER, data, len);
    ms_receive(&f->conn, e);
}

static void t_receive(struct fixture* f, uint8_t type, uint64_t round, uint32_t request, const uint8_t* data, size_t len) {
    struct ms_session* s = ms_find(f->inst.msync, &f->conn, NULL, MT_DATA_MEMBERS, 1001, MS_EXCHANGE);
    uint8_t id[MS_TOKEN_SIZE] = {0}; ms_write64(id, round);
    t_receive_tokens(f, type, round, request,
        type == MS_WAKE ? NULL : s && round == s->round ? s->round_id : id,
        type == MS_WAKE || type == MS_BEGIN ? NULL : s ? s->accept_id : NULL, data, len);
}

/* Старый BEGIN может получить новый challenge, но прежние PAGE/DONE/ERROR ему не соответствуют. */
static void test_reattach_tokens(void) {
    struct fixture f; t_init(&f); f.inst.my_ed25519_pubkey[31] = 3;
    struct ms_session* s = t_session(&f);
    uint8_t old_round[MS_TOKEN_SIZE], old_accept[MS_TOKEN_SIZE];
    memcpy(old_round, s->round_id, sizeof(old_round)); memcpy(old_accept, s->accept_id, sizeof(old_accept));
    merkle_sync_cancel(&f.inst, MT_DATA_MEMBERS, f.conn.peer_ed25519_pubkey, "1001");
    assert(merkle_sync_start(&f.inst, MT_DATA_MEMBERS, &f.conn, "1001", t_done, &f) == 0);
    s = f.inst.msync->sessions;
    t_receive_tokens(&f, MS_BEGIN, 7, 0, old_round, NULL, NULL, 0);
    assert(s->state == MS_SERVE && memcmp(old_accept, s->accept_id, sizeof(old_accept)) && s->tx);
    uint64_t deadline = s->round_deadline_tb;
    uint8_t current_accept[MS_TOKEN_SIZE]; memcpy(current_accept, s->accept_id, sizeof(current_accept));
    t_receive_tokens(&f, MS_BEGIN, 7, 0, old_round, NULL, NULL, 0);
    assert(deadline && s->round_deadline_tb == deadline && !memcmp(current_accept, s->accept_id, sizeof(current_accept)));
    ms_discard_tx(s); s->state = MS_PULL; s->waiting = 1; s->request_id = 1; s->stack[0].level = MT_MAX_LEVEL;
    uint8_t page[MS_PAGE_HEADER + 1] = {0}, root[MT_HASH_SIZE] = {0}, error[4]; ms_write32(error, 3);
    t_receive_tokens(&f, MS_PAGE, 7, 1, old_round, old_accept, page, sizeof(page));
    t_receive_tokens(&f, MS_DONE, 7, 0, old_round, old_accept, root, sizeof(root));
    t_receive_tokens(&f, MS_ERROR, 7, 0, old_round, old_accept, error, sizeof(error));
    assert(!f.applied && !f.callbacks && s->waiting && s->state == MS_PULL);
    t_receive_tokens(&f, MS_PAGE, 7, 1, old_round, current_accept, page, sizeof(page));
    assert(f.applied == 1 && s->state == MS_DONE_WAIT && !f.callbacks);
    ms_discard_tx(s);
    t_receive_tokens(&f, MS_DONE, 7, 0, old_round, current_accept, root, sizeof(root));
    assert(f.callbacks == 1 && f.result == MT_OK);
    t_destroy(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "PASS fresh responder token fences old pages, completion and errors after cancel/start");
}

static void test_handshake_failures(void) {
    for (int follower = 0; follower < 2; follower++) {
        struct fixture f; t_init(&f); if (follower) f.inst.my_ed25519_pubkey[31] = 3;
        struct ms_session* s = t_session(&f);
        uint8_t original[MS_TOKEN_SIZE]; memcpy(original, s->round_id, sizeof(original));
        fail_random = 1;
        if (follower) {
            uint8_t id[MS_TOKEN_SIZE] = {8};
            t_receive_tokens(&f, MS_BEGIN, 8, 0, id, NULL, NULL, 0);
        } else { s->state = MS_IDLE; s->dirty = 1; ms_work(s); }
        assert(!fail_random && !memcmp(original, s->round_id, sizeof(original)) && !f.applied);
        uasync_poll(f.inst.ua, 0);
        assert(f.callbacks == 1 && f.result == MT_ERR_IO && s->state == MS_FAILED);
        t_destroy(&f);
    }
    struct fixture f; t_init(&f); struct ms_session* s = t_session(&f);
    s->state = MS_BEGIN_WAIT; s->request_id = 0; memset(s->accept_id, 0, sizeof(s->accept_id));
    uint8_t accepted[MS_TOKEN_SIZE]; memset(accepted, 0xbc, sizeof(accepted));
    t_receive_tokens(&f, MS_READY, 7, 0, s->round_id, accepted, NULL, 0);
    assert(s->state == MS_PULL && s->waiting && s->tx);
    t_receive_tokens(&f, MS_READY, 7, 0, s->round_id, accepted, NULL, 0);
    assert(s->state == MS_PULL && s->request_id == 1 && !f.callbacks);
    t_destroy(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "PASS handshake RNG failures cannot install a partial round; duplicate READY is idempotent");
}

static void test_malformed_and_old_round(void) {
    struct fixture f; t_init(&f);
    struct ms_session* s = t_session(&f);
    uint8_t short_hashes[4] = {1};
    t_receive(&f, MS_HASHES, 6, 1, short_hashes, sizeof(short_hashes));
    assert(!f.callbacks && s->waiting && s->round == 7);
    t_receive(&f, MS_HASHES, 7, 1, short_hashes, sizeof(short_hashes));
    assert(!f.callbacks && s->state == MS_FAILING);
    uasync_poll(f.inst.ua, 0);
    assert(f.callbacks == 1 && f.result == MT_ERR_PROTOCOL && s->state == MS_FAILED);
    t_destroy(&f);
    puts("PASS: stale round ignored; truncated hashes rejected before reading");
}

static void test_control_admission(void) {
    struct fixture f; t_init(&f);
    uint8_t invalid_error[4]; ms_write32(invalid_error, (uint32_t)-MT_ERR_LIMIT + 1);
    t_receive(&f, MS_BEGIN, 8, 0, NULL, 0); /* BEGIN идёт к отвечающему. */
    t_receive(&f, MS_WAKE, 0, 1, NULL, 0);
    assert(!f.inst.msync->sessions && !f.conn.ref_count);
    struct ms_session* s = t_session(&f);
    assert(!ms_send(s, MS_QUERY, 1, NULL, 0));
    struct ll_entry* tx = s->tx;
    void* deadline = s->deadline;
    t_receive(&f, MS_ERROR, 7, 0, invalid_error, sizeof(invalid_error));
    t_receive(&f, MS_ERROR, 7, 0, NULL, 0);
    t_receive(&f, MS_BEGIN, 8, 0, NULL, 0);
    assert(!f.callbacks && s->state == MS_PULL && s->round == 7 && !s->received_bytes && s->tx == tx && s->deadline == deadline);
    t_destroy(&f);
    puts("PASS: invalid Merkle controls leave sessions, pending output and timers untouched");
}

static void test_absolute_deadline(void) {
    struct fixture f; t_init(&f);
    assert(!merkle_sync_start(&f.inst, MT_DATA_MEMBERS, &f.conn, "1001", t_done, &f));
    struct ms_session* s = f.inst.msync->sessions;
    assert(s && !s->round_deadline_tb);
    uint64_t before = get_time_tb();
    for (int i = 0; !s->round_deadline_tb && i < 4; i++) uasync_poll(f.inst.ua, 0);
    assert(s->round_deadline_tb >= before + MS_ROUND_TIMEOUT_TB && s->round_deadline_tb <= get_time_tb() + MS_ROUND_TIMEOUT_TB);
    uint64_t deadline = get_time_tb() + 100; /* Ускоряем только абсолютный срок, интервал прогресса штатный. */
    s->round_deadline_tb = deadline;
    assert(!ms_progress(s) && !ms_progress(s) && s->round_deadline_tb == deadline);
    while (!f.callbacks && get_time_tb() < deadline + 2000) uasync_poll(f.inst.ua, 200);
    assert(f.callbacks == 1 && f.result == MT_ERR_TIMEOUT && s->state == MS_FAILED && !s->deadline && !s->round_deadline_tb);
    t_destroy(&f);
    puts("PASS: timer refresh stays capped by the absolute deadline from BEGIN");
}

static void test_rejected_data(void) {
    struct fixture f; t_init(&f);
    struct ms_session* s = t_session(&f); s->stack[0].level = MT_MAX_LEVEL;
    f.apply_error = MT_ERR_DATA;
    uint8_t page[MS_PAGE_HEADER + 2] = {0};
    t_receive(&f, MS_PAGE, 7, 1, page, sizeof(page));
    assert(!f.callbacks && s->state == MS_FAILING);
    uasync_poll(f.inst.ua, 0);
    assert(f.callbacks == 1 && f.result == MT_ERR_DATA && s->state == MS_FAILED);
    t_destroy(&f);
    puts("PASS: failed apply cannot produce success");
}

static void test_sibling_walk(void) {
    struct fixture f; t_init(&f);
    struct ms_session* s = t_session(&f);
    uint8_t hashes[32 * 32] = {0}; hashes[0] = 1; hashes[32] = 1;
    assert(ms_receive_hashes(s, 1, hashes, sizeof(hashes)) == 0);
    assert(s->depth == 1 && s->stack[1].prefix == 0 && s->stack[0].children == 2);
    ms_discard_tx(s);
    memset(hashes, 0, sizeof(hashes));
    assert(ms_receive_hashes(s, 2, hashes, sizeof(hashes)) == 0);
    assert(s->depth == 1 && s->stack[1].prefix == (UINT64_C(1) << 59));
    assert(s->waiting && !f.callbacks);
    t_destroy(&f);
    puts("PASS: completing one branch preserves the sibling request");
}

static void test_cancel_waiters(void) {
    for (int blocked = 0; blocked < 2; blocked++) {
        struct fixture f; t_init(&f);
        struct ms_session* s = t_session(&f);
        if (blocked) { struct ll_entry* e = queue_entry_new(0); assert(e); assert(queue_data_put(f.conn.send_input_q, e) == 0); }
        assert(ms_send(s, MS_QUERY, 1, NULL, 0) == 0);
        assert(s->waiter.internal || s->waiter.call_soon_id);
        f.conn.links_up = 0;
        merkle_sync_cancel_peer(&f.inst, f.conn.peer_ed25519_pubkey);
        assert(!f.inst.msync->sessions && !f.conn.send_input_q->waiter_head);
        uasync_poll(f.inst.ua, 0);
        assert(!f.callbacks);
        t_destroy(&f);
    }
    puts("PASS: cancel after links_down removes queued and deferred waiters");
}

static void test_page_size_and_cursor(void) {
    struct fixture f; t_init(&f);
    struct ms_session* s = t_session(&f); s->state = MS_SERVE;
    uint8_t query[MS_QUERY_SIZE] = { MT_MAX_LEVEL };
    assert(ms_serve_query(s, 1, query, sizeof(query)) == 0);
    assert(s->tx && s->tx->len == MT_PAGE_SIZE);
    ms_discard_tx(s);
    query[9] = 1; ms_write64(query + 10, 1);
    assert(ms_serve_query(s, 2, query, sizeof(query)) == MT_ERR_IO);
    query[0] = 255;
    assert(ms_serve_query(s, 3, query, sizeof(query)) == MT_ERR_PROTOCOL);
    t_destroy(&f);
    puts("PASS: page bounded; non-advancing cursor and invalid level rejected");
}

static void test_readonly_tree(void) {
    struct fixture f; t_init(&f);
    assert(sqlite3_exec(f.inst.topo_sqlite_db, "PRAGMA query_only=ON", NULL, NULL, NULL) == SQLITE_OK);
    assert(merkle_sync_recompute_path(&f.inst, MT_DATA_MEMBERS, "1001", 0) < 0);
    uint8_t hash[32], empty[32] = {0};
    assert(merkle_sync_read_hash(&f.inst, MT_DATA_MEMBERS, "1001", 0, 0, hash) == 0 && !memcmp(hash, empty, 32));
    t_destroy(&f);
    puts("PASS: SQLite failure is reported and cannot publish a root");
}

static void test_backpressure_and_disconnect(void) {
    struct fixture f; t_init(&f);
    struct ll_entry* blocker = queue_entry_new(0); assert(blocker);
    assert(queue_data_put(f.conn.send_input_q, blocker) == 0);
    assert(merkle_sync_start(&f.inst, MT_DATA_MEMBERS, &f.conn, "1001", t_done, &f) == 0);
    uasync_poll(f.inst.ua, 0);
    struct ms_session* s = f.inst.msync->sessions;
    assert(s->tx && s->waiter.internal);
    for (int i = 0; i < 1000; i++) merkle_sync_changed(&f.inst, MT_DATA_MEMBERS, "1001");
    assert(s->tx && queue_entry_count(f.conn.send_input_q) == 1 && s->dirty);
    f.conn.links_up = 0;
    ms_conn_status(&f.conn, ETCP_CONN_STATUS_DOWN, f.inst.msync);
    assert(!f.inst.msync->sessions && f.callbacks == 1 && f.result == MT_ERR_DISCONNECTED);
    t_destroy(&f);
    puts("PASS: 1000 changes coalesce behind backpressure; disconnect completes request with error");
}

static void test_peer_restart(void) {
    struct fixture f; t_init(&f);
    struct ms_session* s = t_session(&f);
    assert(ms_send(s, MS_QUERY, 1, NULL, 0) == 0);
    t_receive(&f, MS_WAKE, 0, 0, NULL, 0);
    assert(s->round > 7 && s->state == MS_BEGIN_WAIT && s->tx && s->tx->dgram[MS_TYPE_OFF] == MS_BEGIN);
    uint8_t hashes[32 * 32] = {0};
    t_receive(&f, MS_HASHES, 7, 1, hashes, sizeof(hashes));
    assert(!f.callbacks && s->state == MS_BEGIN_WAIT);
    t_destroy(&f);
    puts("PASS: reattaching follower restarts the round; old responses cannot affect it");
}

static void test_send_rejection(void) {
    struct fixture f; t_init(&f);
    struct ms_session* s = t_session(&f);
    queue_set_size_limit(f.conn.send_input_q, 0);
    assert(ms_send(s, MS_QUERY, 1, NULL, 0) == 0);
    uasync_poll(f.inst.ua, 0);
    assert(f.callbacks == 1 && f.result == MT_ERR_IO && !s->tx);
    t_destroy(&f);
    puts("PASS: transport rejection retains ownership; packet is freed exactly once");
}

static void test_transport_refill(void) {
    struct fixture f; t_init(&f);
    struct ms_session* s = t_session(&f);
    struct ll_queue* q = f.conn.send_input_q;
    assert(!ms_send(s, MS_DEP_QUERY, 2, (uint8_t[]){1}, 1));
    struct ll_entry* pending = s->tx;
    void* deadline = s->deadline;
    struct ll_entry* blocker = queue_entry_new(0); assert(blocker && !queue_data_put(q, blocker));
    uasync_poll(f.inst.ua, 0);
    assert(s->tx == pending && s->waiter.internal && s->deadline == deadline && !f.callbacks);
    assert(queue_data_get(q) == blocker); queue_entry_free(blocker);
    uasync_poll(f.inst.ua, 0);
    assert(!s->tx && !s->waiter.internal && s->deadline == deadline && !f.callbacks);
    assert(queue_data_get(q) == pending && !queue_entry_count(q));
    queue_dgram_free(pending); queue_entry_free(pending);
    t_destroy(&f);
    puts("PASS: deferred transport notification rearms on refill and sends the same packet once");
}
static void test_checkpoint_revision(void) {
    for (int changed = 0; changed < 2; changed++) {
        struct fixture f; t_init(&f);
        struct ms_session* s = t_session(&f); s->state = MS_SERVE; s->waiting = 0;
        uint8_t root[32] = {0};
        t_receive(&f, MS_CHECK, 7, 0, root, sizeof(root));
        assert(s->tx && s->tx->dgram[MS_TYPE_OFF] == MS_DONE && !f.callbacks);
        if (changed) merkle_sync_changed(&f.inst, MT_DATA_MEMBERS, "1001");
        uasync_poll(f.inst.ua, 0);
        if (changed) assert(!f.callbacks && s->requests && s->revision != s->verified_revision);
        else assert(f.callbacks == 1 && f.result == MT_OK && !s->requests);
        t_destroy(&f);
    }
    puts("PASS: change between CHECK and DONE handoff retains the request for a new checkpoint");
}

static void t_cancel_done(const uint8_t peer[32], const char* ns, int result, void* arg) {
    struct fixture* f = arg;
    t_done(peer, ns, result, arg);
    merkle_sync_cancel(&f->inst, MT_DATA_MEMBERS, peer, ns);
}

static void test_error_handoff(void) {
    struct fixture f; t_init(&f);
    struct ms_session* s = t_session(&f); s->requests->cb = t_cancel_done;
    s->stack[0].level = MT_MAX_LEVEL; f.apply_error = MT_ERR_DATA;
    uint8_t page[MS_PAGE_HEADER + 2] = {0};
    t_receive(&f, MS_PAGE, 7, 1, page, sizeof(page));
    assert(!f.callbacks && s->state == MS_FAILING);
    uasync_poll(f.inst.ua, 0);
    assert(f.callbacks == 1 && f.result == MT_ERR_DATA && !f.inst.msync->sessions);
    assert(f.conn.send_input_q->head && f.conn.send_input_q->head->dgram[MS_TYPE_OFF] == MS_ERROR);
    t_destroy(&f);
    puts("PASS: ERROR reaches transport before cancellation inside callback");
}

static void test_blocked_error_timeout(void) {
    struct fixture f; t_init(&f);
    struct ll_entry* blocker = queue_entry_new(0); assert(blocker);
    assert(queue_data_put(f.conn.send_input_q, blocker) == 0);
    struct ms_session* s = t_session(&f); s->requests->cb = t_cancel_done;
    ms_fail(s, MT_ERR_DATA, "injected apply failure", 1);
    assert(s->deadline && s->state == MS_FAILING && !f.callbacks);
    ms_stop_deadline(s); ms_timeout(s);
    assert(f.callbacks == 1 && f.result == MT_ERR_TIMEOUT && !f.inst.msync->sessions);
    uasync_poll(f.inst.ua, 0);
    assert(f.callbacks == 1 && !f.conn.send_input_q->waiter_head);
    t_destroy(&f);
    puts("PASS: blocked terminal packet times out once and releases waiter/session");
}

static int pull_created, pull_committed, pull_released;
static int t_begin_pull(void* ctx, const char* ns, const uint8_t peer[32], uint64_t round, void** pull) {
    (void)ctx; (void)ns; (void)peer; (void)round;
    *pull = u_malloc(1); assert(*pull); pull_created++; return 0;
}
static int t_commit_pull(void* ctx, void* pull) { assert(pull); pull_committed++; return ((struct fixture*)ctx)->apply_error; }
static void t_abort_pull(void* ctx, void* pull) { (void)ctx; assert(pull); pull_released++; u_free(pull); }
static int pull_published;
static void t_publish_pull(void* ctx, void* pull) {
    struct fixture* f = ctx;
    assert(pull && f->callbacks == 1 && !f->inst.msync->sessions);
    merkle_sync_cancel_ns(&f->inst, "1001"); pull_published++;
}
static int t_stage_page(void* ctx, void* pull, uint64_t prefix, int av, const merkle_key_t* after,
        const merkle_key_t* next, int more, const uint8_t* data, size_t len) {
    (void)ctx; (void)pull; (void)prefix; (void)av; (void)after; (void)next; (void)more; (void)data; (void)len; return 0;
}
static void test_pull_lifecycle(void) {
    struct fixture f; t_init(&f);
    struct merkle_sync_data_ops ops = t_ops;
    ops.begin_pull = t_begin_pull; ops.commit_pull = t_commit_pull; ops.abort_pull = t_abort_pull; ops.stage_page = t_stage_page;
    f.inst.msync->datasets[MT_DATA_MEMBERS].ops = &ops;
    struct ms_session* s = t_session(&f);
    assert(ms_pull_start(s) == 0 && s->pull);
    ms_discard_tx(s); s->stack[0].level = MT_MAX_LEVEL;
    assert(ms_walk_next(s) == 0 && !s->pull && pull_committed == 1 && pull_released == 1);
    ms_discard_tx(s);
    assert(ms_pull_start(s) == 0);
    ms_discard_tx(s); assert(ms_begin(s) == 0 && !s->pull && pull_released == 2);
    ms_discard_tx(s); assert(ms_pull_start(s) == 0);
    ms_conn_status(&f.conn, ETCP_CONN_STATUS_DOWN, f.inst.msync);
    assert(pull_created == 3 && pull_committed == 1 && pull_released == 3);
    t_destroy(&f);
    puts("PASS: pull commits once; restart and disconnect abort exactly their own context");
}

/* Сессии и корни двух моделей одного канала не влияют друг на друга. */
static void test_dataset_isolation(void) {
    struct fixture f; t_init(&f);
    assert(merkle_sync_register(&f.inst, MT_DATA_FEEDBACK, &t_ops, &f) == 0);
    assert(merkle_sync_register(&f.inst, MT_DATA_FEEDBACK, &t_ops, &f) < 0);
    assert(merkle_sync_recompute_path(&f.inst, MT_DATA_MEMBERS, "1001", 0) == 1);
    uint8_t member[32], feedback[32], empty[32] = {0};
    assert(merkle_sync_read_hash(&f.inst, MT_DATA_MEMBERS, "1001", 0, 0, member) == 0);
    assert(merkle_sync_read_hash(&f.inst, MT_DATA_FEEDBACK, "1001", 0, 0, feedback) == 0);
    assert(memcmp(member, empty, 32) && !memcmp(feedback, empty, 32));
    assert(merkle_sync_recompute_path(&f.inst, MT_DATA_FEEDBACK, "1001", 0) == 1);
    struct ms_session* a = ms_create(f.inst.msync, &f.conn, NULL, MT_DATA_MEMBERS, 1001, MS_EXCHANGE); assert(a);
    struct ms_session* b = ms_create(f.inst.msync, &f.conn, NULL, MT_DATA_FEEDBACK, 1001, MS_EXCHANGE); assert(b);
    assert(a != b && ms_find(f.inst.msync, &f.conn, NULL, MT_DATA_MEMBERS, 1001, MS_EXCHANGE) == a);
    a->state = b->state = MS_SERVE;
    merkle_sync_changed(&f.inst, MT_DATA_FEEDBACK, "1001");
    assert(!a->dirty && !a->revision && b->dirty && b->revision == 1);
    merkle_sync_cancel(&f.inst, MT_DATA_FEEDBACK, f.conn.peer_ed25519_pubkey, "1001");
    assert(f.inst.msync->sessions == a && !a->next);
    merkle_sync_cancel_ns(&f.inst, "1001"); assert(!f.inst.msync->sessions);
    t_destroy(&f);
    puts("PASS: dataset roots, sessions, changes and cancellation are independent");
}

static int t_authorized(void* ctx, const char* ns, struct ETCP_CONN* peer) {
    (void)ns; (void)peer; return ((struct fixture*)ctx)->authorized;
}
static int t_can_run(void* ctx, const char* ns, struct ETCP_CONN* peer) {
    (void)ns; (void)peer; return ((struct fixture*)ctx)->running;
}
static void test_model_policy(void) {
    struct fixture f; t_init(&f); f.conn.peer_sleep_phase = 1;
    assert(merkle_sync_start(&f.inst, MT_DATA_MEMBERS, &f.conn, "1001", t_done, &f) < 0 && !f.callbacks);
    struct merkle_sync_data_ops ops = t_ops; ops.validate_peer = t_authorized; ops.can_run = t_can_run;
    f.inst.msync->datasets[MT_DATA_MEMBERS].ops = &ops; f.running = 1;
    assert(merkle_sync_start(&f.inst, MT_DATA_MEMBERS, &f.conn, "1001", t_done, &f) < 0 && !f.callbacks);
    f.authorized = 1;
    assert(merkle_sync_start(&f.inst, MT_DATA_MEMBERS, &f.conn, "1001", t_done, &f) == 0);
    struct ms_session* s = ms_find(f.inst.msync, &f.conn, NULL, MT_DATA_MEMBERS, 1001, MS_EXCHANGE); assert(s && s->work);
    f.running = 0; merkle_sync_recheck_peer(&f.conn);
    assert(s->state == MS_FAILED && !s->work && f.callbacks == 1 && f.result == MT_ERR_DISCONNECTED);
    merkle_sync_changed(&f.inst, MT_DATA_MEMBERS, "1001");
    assert(s->state == MS_IDLE && !s->work && s->dirty);
    f.running = 1;
    assert(merkle_sync_start(&f.inst, MT_DATA_MEMBERS, &f.conn, "1001", t_done, &f) == 0 && s->work);
    f.authorized = 0; merkle_sync_recheck_peer(&f.conn);
    assert(s->state == MS_FAILED && !s->work && f.callbacks == 2);
    t_destroy(&f);
    puts("PASS: model sleep policy allows admitted peers; suspension stops work and restoration reuses the live connection");
}

/* Отзыв закрывает даже заблокированный ERROR; возврат начинает новый раунд той же подписки. */
static void test_permission_rounds(void) {
    for (int failing = 0; failing < 2; failing++) {
        struct fixture f; t_init(&f);
        struct merkle_sync_data_ops ops = t_ops; ops.validate_peer = t_authorized;
        f.inst.msync->datasets[MT_DATA_MEMBERS].ops = &ops;
        struct ms_session* s = t_session(&f);
        if (failing) ms_fail(s, MT_ERR_DATA, "injected queued error", 1);
        else assert(ms_send(s, MS_QUERY, 1, NULL, 0) == 0);
        assert(s->tx && s->deadline && !f.callbacks);
        merkle_sync_changed(&f.inst, MT_DATA_MEMBERS, "1001");
        merkle_sync_revalidate_peers(&f.inst, MT_DATA_MEMBERS, "1001");
        assert(s->state == MS_FAILED && !s->tx && !s->deadline && !s->work && !s->waiting);
        assert(f.callbacks == 1 && f.result == MT_ERR_DATA && f.conn.ref_count == 1);
        assert(!f.conn.send_input_q->waiter_head);
        f.authorized = 1;
        merkle_sync_changed(&f.inst, MT_DATA_MEMBERS, "1001");
        merkle_sync_revalidate_peers(&f.inst, MT_DATA_MEMBERS, "1001");
        assert(s->state == MS_IDLE && s->dirty && s->work && f.callbacks == 1);
        uasync_call_soon_cancel(f.inst.ua, s->work); s->work = NULL;
        ms_work(s);
        assert(s->state == MS_BEGIN_WAIT && s->round > 7 && s->tx && s->deadline);
        t_destroy(&f);
    }
    struct fixture f; t_init(&f);
    struct merkle_sync_data_ops ops = t_ops; ops.validate_peer = t_authorized;
    f.inst.msync->datasets[MT_DATA_MEMBERS].ops = &ops;
    struct ms_session* s = t_session(&f); s->requests->cb = t_cancel_done;
    merkle_sync_revalidate_peers(&f.inst, MT_DATA_MEMBERS, "1001");
    assert(!f.inst.msync->sessions && !f.conn.ref_count && f.callbacks == 1 && f.result == MT_ERR_DATA);
    t_destroy(&f);
    puts("PASS: permission revocation releases blocked rounds; restoration restarts watches; callback cancellation is safe");
}

/* Два разных полных ключа с одинаковыми первыми 64 битами проходят пагинацию. */
static int t_full_page(void* ctx, const char* ns, uint64_t prefix, int av, const merkle_key_t* after,
        uint8_t* data, size_t* len, merkle_key_t* next, int* more) {
    (void)ctx; (void)ns; (void)prefix;
    assert(*len >= 1);
    *next = *after; ms_write64(next->bytes, 1);
    next->bytes[MT_KEY_SIZE - 1] = av ? 2 : 1;
    data[0] = next->bytes[MT_KEY_SIZE - 1]; *len = 1; *more = !av;
    return 0;
}

static void test_full_cursor(void) {
    struct fixture f; t_init(&f);
    struct merkle_sync_data_ops ops = t_ops; ops.get_page = t_full_page;
    assert(merkle_sync_register(&f.inst, MT_DATA_FEEDBACK, &ops, &f) == 0);
    struct ms_session* s = ms_create(f.inst.msync, &f.conn, NULL, MT_DATA_FEEDBACK, 1001, MS_EXCHANGE); assert(s);
    s->state = MS_SERVE;
    uint8_t query[MS_QUERY_SIZE] = { MT_MAX_LEVEL };
    assert(ms_serve_query(s, 1, query, sizeof(query)) == 0);
    assert(s->tx->dgram[MS_DATASET_OFF] == MT_DATA_FEEDBACK && s->tx->dgram[MS_HEADER] == 1);
    memcpy(query + 10, s->tx->dgram + MS_HEADER + 1, MT_KEY_SIZE); query[9] = 1;
    ms_discard_tx(s);
    assert(ms_serve_query(s, 2, query, sizeof(query)) == 0);
    assert(s->tx->dgram[MS_HEADER] == 0 && s->tx->dgram[MS_HEADER + MT_KEY_SIZE] == 2);
    uint8_t page[MS_PAGE_HEADER + 1]; memcpy(page, s->tx->dgram + MS_HEADER, sizeof(page));
    ms_discard_tx(s);
    s->state = MS_PULL; s->stack[0].level = MT_MAX_LEVEL; s->waiting = 1; s->request_id = 1;
    memcpy(s->after.bytes, query + 10, MT_KEY_SIZE); s->after_valid = 1;
    page[0] = 1;
    assert(ms_receive_page(s, 1, page, sizeof(page)) == 0);
    assert(s->after.bytes[MT_KEY_SIZE - 1] == 2 && s->request_id == 2);
    ms_discard_tx(s);
    assert(ms_receive_page(s, 2, page, sizeof(page)) == MT_ERR_PROTOCOL);
    t_destroy(&f);
    puts("PASS: full cursors preserve colliding 64-bit prefixes and reject backwards progress");
}

/* Ветви отказа JOIN MEMBERS логируют конечный ключ без прямого ETCP. */
static void test_join_rejection_diagnostics(void) {
    struct fixture f; t_init(&f);
    struct merkle_sync_data_ops ops = t_ops;
    ops.validate_request = t_can_run;
    f.inst.msync->datasets[MT_DATA_MEMBERS].ops = &ops;
    struct chat_join_path path = {.attempt = 42};
    memcpy(path.inviter, f.conn.peer_ed25519_pubkey, 32);
    memcpy(path.guest, f.inst.my_ed25519_pubkey, 32);
    debug_set_category_level(DEBUG_CATEGORY_MEMBER_SYNC, DEBUG_LEVEL_WARN);
    assert(merkle_sync_join_pull(&f.inst, &path, "1001", t_done, &f) == -1);
    assert(!f.inst.msync->sessions && !f.callbacks);

    memcpy(path.inviter, f.inst.my_ed25519_pubkey, 32);
    memcpy(path.guest, f.conn.peer_ed25519_pubkey, 32);
    join_allowed = 1;
    uint8_t control[MS_HEADER] = {0};
    control[0] = f.inst.msync->svc_id;
    control[MS_DATASET_OFF] = MT_DATA_MEMBERS;
    control[MS_TYPE_OFF] = MS_BEGIN | MS_READ_ONLY;
    topo_group_utun_wire_id(control + MS_SCOPE_OFF, 1001);
    merkle_sync_join_receive(&f.inst, &path, control, sizeof(control));
    assert(!f.inst.msync->sessions && !f.callbacks);
    join_allowed = 0;
    debug_set_category_level(DEBUG_CATEGORY_MEMBER_SYNC, DEBUG_LEVEL_NONE);
    t_destroy(&f);
    puts("PASS: JOIN MEMBERS rejection diagnostics with no direct ETCP connection");
}

/* Проверяем реальный handshake отдельного запроса: ни обычного допуска, ни обратной выдачи данных. */
static void test_receive_only(void) {
    struct merkle_sync_data_ops ops = t_ops;
    ops.validate_peer = t_authorized; ops.validate_request = t_can_run;
    ops.begin_pull = t_begin_pull; ops.commit_pull = t_commit_pull; ops.abort_pull = t_abort_pull; ops.stage_page = t_stage_page;
    ops.publish_pull = t_publish_pull;
    for (int rejected = 0; rejected < 2; rejected++) {
        struct fixture f; t_init(&f); f.inst.my_ed25519_pubkey[31] = 3; f.running = 1;
        f.inst.msync->datasets[MT_DATA_MEMBERS].ops = &ops;
        int committed = pull_committed, released = pull_released, published = pull_published;
        uint8_t id[MS_TOKEN_SIZE] = {8}, accepted[MS_TOKEN_SIZE] = {9};
        t_receive_tokens(&f, MS_BEGIN | MS_READ_ONLY, 8, 0, id, NULL, NULL, 0);
        assert(!f.inst.msync->sessions); /* Право запросить не разрешает выдавать данные. */
        t_receive_tokens(&f, MS_READY | MS_READ_ONLY | MS_RESPONSE, 8, 0, id, accepted, NULL, 0);
        assert(!f.inst.msync->sessions); /* Ответ без локального запроса не создаёт контекст. */
        assert(merkle_sync_start(&f.inst, MT_DATA_MEMBERS, &f.conn, "1001", t_cancel_done, &f) == 0);
        struct ms_session* s = ms_find(f.inst.msync, &f.conn, NULL, MT_DATA_MEMBERS, 1001, MS_READER); assert(s && s->leader);
        uasync_call_soon_cancel(f.inst.ua, s->work); s->work = NULL; ms_work(s);
        assert(s->state == MS_BEGIN_WAIT && s->tx->dgram[MS_TYPE_OFF] == (MS_BEGIN | MS_READ_ONLY));
        ms_discard_tx(s);
        t_receive_tokens(&f, MS_READY | MS_READ_ONLY | MS_RESPONSE, s->round, 0, s->round_id, accepted, NULL, 0);
        assert(s->state == MS_PULL && s->pull && s->tx->dgram[MS_TYPE_OFF] == (MS_QUERY | MS_READ_ONLY));
        ms_discard_tx(s);
        uint8_t hashes[MT_BUCKETS * MT_HASH_SIZE] = {0};
        t_receive_tokens(&f, MS_HASHES | MS_READ_ONLY | MS_RESPONSE, s->round, s->request_id,
            s->round_id, s->accept_id, hashes, sizeof(hashes));
        assert(s->state == MS_DONE_WAIT && s->pull && pull_committed == committed && !f.callbacks);
        assert(s->tx && s->tx->dgram[MS_TYPE_OFF] == (MS_CHECK | MS_READ_ONLY) && s->tx->len == MS_HEADER);
        ms_discard_tx(s); f.apply_error = rejected ? MT_ERR_DATA : 0;
        t_receive_tokens(&f, MS_DONE | MS_READ_ONLY | MS_RESPONSE, s->round, 0, s->round_id, s->accept_id, NULL, 0);
        if (rejected) uasync_poll(f.inst.ua, 0);
        assert(pull_committed == committed + 1 && pull_released == released + 1);
        assert(pull_published == published + !rejected);
        assert(f.callbacks == 1 && f.result == (rejected ? MT_ERR_DATA : MT_OK) && !f.inst.msync->sessions);
        t_destroy(&f);
    }
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "PASS receive-only: no unsolicited state or disclosure; commit waits for DONE and can reject data");
}

/* Изменение источника до CHECK либо перед выдачей DONE отменяет весь полученный снимок. */
static void test_receive_only_source_change(void) {
    struct merkle_sync_data_ops ops = t_ops;
    ops.validate_peer = t_authorized; ops.validate_request = t_can_run;
    ops.begin_pull = t_begin_pull; ops.commit_pull = t_commit_pull; ops.abort_pull = t_abort_pull; ops.stage_page = t_stage_page;
    for (int queued = 0; queued < 2; queued++) {
        struct fixture f; t_init(&f); f.authorized = 1;
        f.inst.msync->datasets[MT_DATA_MEMBERS].ops = &ops;
        uint8_t id[MS_TOKEN_SIZE] = {8};
        t_receive_tokens(&f, MS_BEGIN | MS_READ_ONLY, 8, 0, id, NULL, NULL, 0);
        struct ms_session* s = ms_find(f.inst.msync, &f.conn, NULL, MT_DATA_MEMBERS, 1001, MS_WRITER); assert(s);
        assert(s->state == MS_SERVE && !s->leader && s->tx->dgram[MS_TYPE_OFF] == (MS_READY | MS_READ_ONLY | MS_RESPONSE));
        ms_discard_tx(s);
        if (!queued) merkle_sync_changed(&f.inst, MT_DATA_MEMBERS, "1001");
        t_receive_tokens(&f, MS_CHECK | MS_READ_ONLY, 8, 0, s->round_id, s->accept_id, NULL, 0);
        if (queued) {
            assert(s->tx && s->tx->dgram[MS_TYPE_OFF] == (MS_DONE | MS_READ_ONLY | MS_RESPONSE));
            merkle_sync_changed(&f.inst, MT_DATA_MEMBERS, "1001");
        }
        for (int i = 0; i < 4; i++) uasync_poll(f.inst.ua, 0);
        assert(s->state == MS_FAILED && !s->tx && !s->work && !f.callbacks);
        struct ll_entry* packet = queue_data_get(f.conn.send_input_q); assert(packet);
        assert(packet->dgram[MS_TYPE_OFF] == (MS_ERROR | MS_READ_ONLY | MS_RESPONSE));
        assert(ms_read32(packet->dgram + MS_HEADER) == -MT_ERR_CONFLICT);
        queue_dgram_free(packet); queue_entry_free(packet);
        assert(!f.conn.send_input_q->head);
        t_destroy(&f);
    }
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "PASS receive-only: source changes invalidate both immediate and queued confirmation");
}

/* Запрос/обычный обмен/встречная выдача не должны смешиваться даже при одинаковых идентификаторах. */
static void test_receive_only_boundaries(void) {
    struct fixture f; t_init(&f); f.running = 1;
    struct merkle_sync_data_ops ops = t_ops;
    ops.publish_pull = t_publish_pull;
    assert(merkle_sync_register(&f.inst, MT_DATA_FEEDBACK, &ops, &f) < 0);
    ops.publish_pull = NULL;
    ops.validate_peer = t_authorized; ops.validate_request = t_can_run;
    assert(merkle_sync_register(&f.inst, MT_DATA_FEEDBACK, &ops, &f) < 0); /* Нужна изолированная загрузка. */
    ops.begin_pull = t_begin_pull; ops.commit_pull = t_commit_pull; ops.abort_pull = t_abort_pull; ops.stage_page = t_stage_page;
    f.inst.msync->datasets[MT_DATA_MEMBERS].ops = &ops;
    assert(merkle_sync_start(&f.inst, MT_DATA_MEMBERS, &f.conn, "1001", t_done, &f) == 0);
    struct ms_session* reader = ms_find(f.inst.msync, &f.conn, NULL, MT_DATA_MEMBERS, 1001, MS_READER); assert(reader);
    f.authorized = 1;
    struct ms_session* exchange = ms_create(f.inst.msync, &f.conn, NULL, MT_DATA_MEMBERS, 1001, MS_EXCHANGE); assert(exchange);
    struct ms_session* writer = ms_create(f.inst.msync, &f.conn, NULL, MT_DATA_MEMBERS, 1001, MS_WRITER); assert(writer);
    uint8_t query[MS_QUERY_SIZE] = {0}; reader->state = MS_SERVE;
    assert(ms_serve_query(reader, 1, query, sizeof(query)) == MT_ERR_PROTOCOL && !reader->tx);
    merkle_sync_changed(&f.inst, MT_DATA_MEMBERS, "1001");
    assert(reader->revision == 1 && writer->revision == 1 && exchange->revision == 1);
    assert(!writer->work && !writer->dirty && exchange->dirty);
    f.running = 0; merkle_sync_revalidate_peers(&f.inst, MT_DATA_MEMBERS, "1001");
    assert(reader->state == MS_FAILED && !reader->work && f.callbacks == 1 && f.result == MT_ERR_DATA);
    assert(writer->state != MS_FAILED && exchange->state != MS_FAILED);
    merkle_sync_cancel(&f.inst, MT_DATA_MEMBERS, f.conn.peer_ed25519_pubkey, "1001");
    assert(!f.inst.msync->sessions && !f.conn.ref_count);
    t_destroy(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "PASS receive-only: no reverse queries; modes and permissions are independent; cancellation frees all modes");
}

static void test_initial_time_wait(void) {
    struct fixture f; t_init(&f);
    f.inst.ntp.startup_wait = 1;
    assert(merkle_sync_start(&f.inst, MT_DATA_MEMBERS, &f.conn, "1001", t_done, &f) == 0);
    struct ms_session* s = f.inst.msync->sessions;
    assert(s && s->dirty && !s->work && s->requests);
    t_receive(&f, MS_WAKE, 0, 0, NULL, 0);
    uasync_poll(f.inst.ua, 0);
    assert(s->state == MS_IDLE && !s->tx && !queue_entry_count(f.conn.send_input_q) && !f.callbacks);
    f.inst.ntp.startup_wait = 0;
    merkle_sync_time_ready(&f.inst); assert(s->work);
    for (int i = 0; i < 4; i++) uasync_poll(f.inst.ua, 0);
    assert(queue_entry_count(f.conn.send_input_q) && s->requests && !f.callbacks);
    t_destroy(&f);
    puts("PASS: initial time wait preserves Merkle requests and resumes without a new connection");
}

/* Головы уже получены, а модель отдельно догружает хвосты цепочек.
 * Ожидание не подтверждает корень, не освобождает pull и возобновляется один раз. */
static void test_async_model_application(void) {
    for (int mode = 0; mode < 3; mode++) {
        struct fixture f; t_init(&f); f.authorized = 1; f.running = 1;
        struct merkle_sync_data_ops ops = t_ops;
        ops.validate_peer = t_authorized; ops.validate_request = t_can_run;
        ops.begin_pull = t_begin_pull; ops.commit_pull = t_commit_pull;
        ops.abort_pull = t_abort_pull; ops.stage_page = t_stage_page;
        f.inst.msync->datasets[MT_DATA_MEMBERS].ops = &ops;
        struct ms_session* s = t_session(&f);
        if (mode == 1) s->leader = 0;
        if (mode == 2) { s->mode = MS_READER; s->leader = 1; }
        int released = pull_released;
        assert(!ms_pull_start(s)); ms_discard_tx(s); s->depth = -1;
        f.apply_error = MT_APPLY_PENDING;
        assert(!ms_walk_next(s));
        if (mode == 2) {
            assert(s->state == MS_DONE_WAIT); ms_discard_tx(s);
            t_receive_tokens(&f, MS_DONE | MS_READ_ONLY | MS_RESPONSE, s->round, 0, s->round_id, s->accept_id, NULL, 0);
            assert(s->state == MS_READER_APPLY_WAIT);
        } else assert(s->state == MS_APPLY_WAIT);
        assert(s->pull && !s->tx && pull_released == released && !f.callbacks);
        merkle_sync_resume_pull(&f.inst, MT_DATA_MEMBERS, "1001"); assert(s->work);
        void* work = s->work; merkle_sync_resume_pull(&f.inst, MT_DATA_MEMBERS, "1001"); assert(s->work == work);
        uasync_call_soon_cancel(f.inst.ua, s->work); s->work = NULL; ms_work(s);
        assert(s->pull && pull_released == released && !f.callbacks && !s->tx);
        f.apply_error = 0;
        merkle_sync_resume_pull(&f.inst, MT_DATA_MEMBERS, "1001"); assert(s->work);
        uasync_call_soon_cancel(f.inst.ua, s->work); s->work = NULL; ms_work(s);
        assert(!s->pull && pull_released == released + 1);
        if (mode == 0) assert(s->state == MS_SERVE && s->tx && !f.callbacks);
        else if (mode == 1) assert(s->state == MS_DONE_WAIT && s->tx && !f.callbacks);
        else assert(s->state == MS_COMPLETE && f.callbacks == 1 && f.result == MT_OK);
        t_destroy(&f);
    }
    /* Отмена во время ожидания освобождает единственный context и отложенную работу. */
    struct fixture f; t_init(&f); f.authorized = 1;
    struct merkle_sync_data_ops ops = t_ops;
    ops.validate_peer = t_authorized; ops.begin_pull = t_begin_pull; ops.commit_pull = t_commit_pull;
    ops.abort_pull = t_abort_pull; ops.stage_page = t_stage_page;
    f.inst.msync->datasets[MT_DATA_MEMBERS].ops = &ops;
    struct ms_session* s = t_session(&f); assert(!ms_pull_start(s)); ms_discard_tx(s); s->depth = -1;
    f.apply_error = MT_APPLY_PENDING; int released = pull_released;
    assert(!ms_walk_next(s)); merkle_sync_resume_pull(&f.inst, MT_DATA_MEMBERS, "1001"); assert(s->work);
    merkle_sync_cancel_ns(&f.inst, "1001"); assert(!f.inst.msync->sessions && pull_released == released + 1);
    t_destroy(&f);
    puts("PASS async model application: normal and receive-only rounds wait for tails, resume once, cancel safely");
}

static int t_dependency(const void* pull, struct merkle_sync_dependency* fetch) {
    assert(pull);
    *fetch = (struct merkle_sync_dependency){.length=1, .data={0xa1}};
    return MT_OK;
}
static int t_dependency_work(void* ctx, struct merkle_sync_sql_request* request) {
    struct fixture* f = ctx;
    assert(request->action == MT_SQL_APPLY_DEPENDENCY && request->pull && request->fetch.length == 1);
    assert(request->fetch.data[0] == 0xa1 && request->length == 1 && request->data[0] == 0xb2);
    if (++f->applied == 2) f->apply_error = MT_OK;
    return 0;
}
static void test_dependency_requests(void) {
    struct fixture f; t_init(&f);
    struct merkle_sync_data_ops ops = t_ops;
    ops.begin_pull = t_begin_pull; ops.commit_pull = t_commit_pull;
    ops.abort_pull = t_abort_pull; ops.stage_page = t_stage_page;
    ops.dependency = t_dependency; ops.dependency_work = t_dependency_work;
    f.inst.msync->datasets[MT_DATA_MEMBERS].ops = &ops;
    struct ms_session* s = t_session(&f);
    assert(!ms_pull_start(s)); ms_discard_tx(s); s->depth = -1; s->waiting = 0;
    f.apply_error = MT_APPLY_PENDING;
    assert(!ms_walk_next(s) && s->state == MS_APPLY_WAIT && s->waiting);
    uint32_t first = s->request_id;
    uint8_t reply[] = {0xb2}, old_accept[MS_TOKEN_SIZE] = {8};
    ms_discard_tx(s);
    t_receive_tokens(&f, MS_DEP_REPLY, s->round, first, s->round_id, old_accept, reply, sizeof(reply));
    t_receive(&f, MS_DEP_REPLY, s->round, first - 1, reply, sizeof(reply));
    assert(!f.applied && s->request_id == first);
    t_receive(&f, MS_DEP_REPLY, s->round, first, reply, sizeof(reply));
    assert(f.applied == 1 && s->request_id == first + 1 && s->waiting && s->tx->dgram[MS_TYPE_OFF] == MS_DEP_QUERY);
    ms_discard_tx(s);
    t_receive(&f, MS_DEP_REPLY, s->round, first, reply, sizeof(reply));
    assert(f.applied == 1);
    t_receive(&f, MS_DEP_REPLY, s->round, first + 1, reply, sizeof(reply));
    assert(f.applied == 2 && !s->pull && !s->waiting && s->state == MS_SERVE);
    t_receive(&f, MS_DEP_REPLY, s->round, first + 1, reply, sizeof(reply));
    assert(f.applied == 2 && !f.callbacks);
    uint8_t error[4], old_round[MS_TOKEN_SIZE];
    uint64_t round = s->round;
    memcpy(old_round, s->round_id, sizeof(old_round));
    ms_write32(error, (uint32_t)-MT_ERR_STALE);
    t_receive(&f, MS_ERROR, round, 0, error, sizeof(error));
    assert(f.callbacks == 1 && f.result == MT_ERR_STALE && s->state == MS_FAILED && s->restart);
    ms_cancel_restart(s); ms_restart(s);
    uasync_poll(f.inst.ua, 0);
    assert(s->state == MS_BEGIN_WAIT && s->round > round && memcmp(s->round_id, old_round, sizeof(old_round)));
    t_receive_tokens(&f, MS_DEP_REPLY, round, first + 1, old_round, old_accept, reply, sizeof(reply));
    assert(f.applied == 2 && s->state == MS_BEGIN_WAIT);
    t_destroy(&f);
    puts("PASS: dependency replies bind both tokens and request ID; stale dependency starts a fresh round");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    size_t baseline = u_get_allocated_count();
    test_malformed_and_old_round(); test_control_admission(); test_join_rejection_diagnostics(); test_absolute_deadline();
    test_rejected_data(); test_sibling_walk(); test_cancel_waiters();
    test_page_size_and_cursor(); test_readonly_tree(); test_backpressure_and_disconnect(); test_peer_restart();
    test_send_rejection(); test_transport_refill(); test_checkpoint_revision();
    test_error_handoff(); test_blocked_error_timeout(); test_pull_lifecycle();
    test_dataset_isolation(); test_full_cursor(); test_permission_rounds(); test_model_policy(); test_reattach_tokens(); test_handshake_failures();
    test_receive_only(); test_receive_only_source_change(); test_receive_only_boundaries(); test_initial_time_wait();
    test_async_model_application(); test_dependency_requests();
    assert(u_get_allocated_count() == baseline);
    puts("ALL PASS: protocol regressions, no tracked allocations leaked");
    return 0;
}
