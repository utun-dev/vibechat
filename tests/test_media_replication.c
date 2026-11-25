/* Репликация: настоящие обработчики media/SQLite, управляемый CM/транспорт и ошибки. */
#include "media_delivery_fixture.h"
#include "../src/routing_layer/topo_group.h"
#include "../src/routing_layer/conn_mgr.h"

static int cm_opened, cm_closed;
struct fake_handle {
    struct UTUN_INSTANCE* inst;
    uint8_t ed[32]; uint64_t group;
    conn_mgr_cb_t cb;
    void* arg;
    void* soon;
};

static void cm_up(void* arg) {
    struct fake_handle* h = arg; h->soon = NULL;
    h->cb((struct CONN_MGR_HANDLE*)h, h->ed, h->group, CONN_EVENT_UP, h->arg);
}

int __wrap_conn_mgr_open(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32],
                         conn_mgr_cb_t cb, void* arg, struct CONN_MGR_HANDLE** out) {
    struct fake_handle* h = u_calloc(1, sizeof(*h)); REQUIRE(h);
    h->inst = inst; h->group = group; memcpy(h->ed, ed, 32); h->cb = cb; h->arg = arg;
    *out = (struct CONN_MGR_HANDLE*)h; cm_opened++;
    h->soon = uasync_call_soon(ua, h, cm_up); REQUIRE(h->soon); return 0;
}

void __wrap_conn_mgr_close(struct CONN_MGR_HANDLE* handle) {
    struct fake_handle* h = (struct fake_handle*)handle; REQUIRE(h);
    if (h->soon) uasync_call_soon_cancel(ua, h->soon);
    cm_closed++; u_free(h);
}

static void sql(sqlite3* db, const char* command) {
    int rc = sqlite3_exec(db, command, NULL, NULL, NULL);
    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "test SQL=%s rc=%d error=%s", command, rc, sqlite3_errmsg(db));
    REQUIRE(rc == SQLITE_OK);
}

static sqlite3_int64 scalar(sqlite3* db, const char* command) {
    sqlite3_stmt* st = NULL; REQUIRE(sqlite3_prepare_v2(db, command, -1, &st, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW);
    sqlite3_int64 value = sqlite3_column_int64(st, 0);
    REQUIRE(sqlite3_finalize(st) == SQLITE_OK); return value;
}

static uint64_t read_le(const void* ptr, unsigned n) {
    const uint8_t* p = ptr; uint64_t v = 0;
    for (unsigned i = 0; i < n; i++) v |= (uint64_t)p[i] << (i * 8);
    return v;
}

static void write_le(void* ptr, uint64_t v, unsigned n) {
    uint8_t* p = ptr;
    for (unsigned i = 0; i < n; i++) p[i] = (uint8_t)(v >> (i * 8));
}

/* Только транспорт управляемый: каталог, роли, router-очереди и обработчики — настоящие.
 * Таблица peers отражает результат уже проверенного member_sync; криптография членства проверяется его тестами. */
static void add_group(struct UTUN_INSTANCE* inst, uint64_t group) {
    media_test_group(inst, group, NULL, 0);
    char command[512];
    const unsigned slots[] = {256,512,768,1000,1001,1002,1003,1004,1005,1006,1007,1008,1009,1010,1011,1012};
    uint64_t known[sizeof(slots) / sizeof(slots[0])];
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) known[i] = media_fixture_id(slots[i]);
    media_test_group(inst, group, known, sizeof(known) / sizeof(*known));
    snprintf(command, sizeof(command), "UPDATE peers_%s SET adm_tags='{}';"
        "UPDATE peers_%s SET adm_tags='{\"supernode\":\"yes\"}' WHERE node_id IN(fixture_node(256),fixture_node(512),fixture_node(768))",
        media_test_channel(group, (char[65]){0}), media_test_channel(group, (char[65]){0}));
    sql(inst->md.db, command);
    media_test_member_keys(inst, group);
}

static struct UTUN_INSTANCE* node(uint64_t id) {
    struct UTUN_INSTANCE* inst = instance(id);
    add_group(inst, GROUP); return inst;
}

static void free_node(struct UTUN_INSTANCE* inst) {
    destroy(inst);
}

static struct media_super_peer* peer(struct UTUN_INSTANCE* inst, uint64_t remote, uint64_t group) {
    uint8_t key[40]; memcpy(key, &group, sizeof(group)); media_test_public(remote, key + sizeof(group));
    struct ll_entry* e = inst->md.super_peers ? queue_find_data_by_index(inst->md.super_peers, key) : NULL;
    return e ? (struct media_super_peer*)e->data : NULL;
}

static void poll_ready(void) { for (int i = 0; i < 8; i++) uasync_poll(ua, 0); }

static void expire(struct media_super_peer* p) {
    REQUIRE(p); p->deadline_tb = 0;
    uint64_t until = get_time_tb() + 10000;
    while (get_time_tb() < until) {
        uasync_poll(ua, 100);
        for (int i = 0; i < packet_count; i++) if (packets[i].data[0] == MEDIA_SUBCMD_SYNC_REQ
            && packets[i].from == p->md->inst->node_id && packets[i].to == fixture_address(p->peer_ed) && packets[i].group == p->group_id) return;
    }
    REQUIRE(0);
}

/* Вручную запросить настоящую страницу без подмены реализации SELECT/сериализации. */
static struct packet page(struct UTUN_INSTANCE* source, uint64_t receiver, uint64_t group, uint64_t after,
                           const uint8_t epoch[16], uint8_t token) {
    struct packet p = {.from = receiver, .to = source->node_id, .group = group, .len = MEDIA_SYNC_REQ_SIZE};
    struct media_pkt_sync_req* q = (struct media_pkt_sync_req*)p.data;
    q->subcmd = MEDIA_SUBCMD_SYNC_REQ; memset(q->request_id, token, 16);
    if (epoch) memcpy(q->source_epoch, epoch, 16);
    write_le(&q->after_id, after, 8);
    receive(source, &p); poll_ready();
    struct packet response = {0};
    while (packet_count) {
        struct packet out = take_packet();
        if (out.data[0] == MEDIA_SUBCMD_SYNC_RESP && out.to == receiver && out.group == group
            && ((struct media_pkt_sync_resp*)out.data)->request_id[0] == token) response = out;
    }
    REQUIRE(response.len); return response;
}

static void synchronize(struct UTUN_INSTANCE** nodes, int count, int noisy) {
    int delivered = 0, idle = 0;
    while (idle < 8) {
        uasync_poll(ua, 0);
        if (!packet_count) { idle++; continue; }
        idle = 0; REQUIRE(delivered++ < 10000);
        int at = noisy ? rand() % packet_count : 0;
        struct packet p = packets[at];
        memmove(packets + at, packets + at + 1, (size_t)(--packet_count - at) * sizeof(*packets));
        for (int i = 0; i < count; i++) if (nodes[i]->node_id == p.to) {
            receive(nodes[i], &p);
            if (noisy && delivered % 7 == 0) receive(nodes[i], &p);
        }
    }
}

static void begin_pull(struct UTUN_INSTANCE* a, uint64_t remote, uint64_t group) {
    struct packet p = {.from = remote, .to = a->node_id, .group = group, .len = 1, .data = {MEDIA_SUBCMD_CHANGED}};
    receive(a, &p); poll_ready();
    struct media_super_peer* sp = peer(a, remote, group); REQUIRE(sp);
    if (!sp->request_pending) expire(sp);
}

static struct packet request_from(struct UTUN_INSTANCE* a, uint64_t remote) {
    struct packet q = {0};
    poll_ready();
    while (packet_count) {
        struct packet p = take_packet();
        if (p.from == a->node_id && p.to == remote && p.data[0] == MEDIA_SUBCMD_SYNC_REQ) q = p;
    }
    REQUIRE(q.len); return q;
}

static void test_group_boundary(void) {
    struct UTUN_INSTANCE* a = node(SERVER);
    struct packet p = {.from = CLIENT, .to = SERVER, .group = GROUP, .len = MEDIA_HAVE_BLOCK_SIZE};
    struct media_pkt_have_block* hb = (struct media_pkt_have_block*)p.data;
    hb->subcmd = MEDIA_SUBCMD_HAVE_BLOCK; media_test_group_key(GROUP + 1, hb->group_ed); hb->block_id[0] = 1; hb->timestamp = 1;
    struct media_pkt_sync_entry row = { .timestamp = 1, .status = 1, .version = 1}; row.block_id[0] = 1; media_test_public(CLIENT, row.holder);
    uint8_t private[32]; media_test_private(CLIENT, private);
    REQUIRE(!media_delivery_sign_holder(media_test_group_key(GROUP, (uint8_t[32]){0}), &row, private));
    hb->version = 1; memcpy(hb->signature, row.signature, 64);
    receive(a, &p);
    REQUIRE(!scalar(a->md.db, "SELECT COUNT(*) FROM block_availability"));
    media_test_group_key(GROUP, hb->group_ed); receive(a, &p);
    REQUIRE(scalar(a->md.db, "SELECT COUNT(*) FROM block_availability") == 1);
    sqlite3_int64 high = scalar(a->md.db, "SELECT seq FROM sqlite_sequence WHERE name='block_availability'");
    receive(a, &p);
    REQUIRE(scalar(a->md.db, "SELECT seq FROM sqlite_sequence WHERE name='block_availability'") == high);
    hb->timestamp = 2; hb->version = 2; row.timestamp = 2; row.version = 2;
    REQUIRE(!media_delivery_sign_holder(media_test_group_key(GROUP, (uint8_t[32]){0}), &row, private)); memcpy(hb->signature, row.signature, 64); receive(a, &p);
    REQUIRE(scalar(a->md.db, "SELECT id FROM block_availability") > high);
    hb->subcmd = MEDIA_SUBCMD_BLOCK_PROCESSING; hb->timestamp = 3; hb->version = 3; row.status = 0; row.timestamp = 3; row.version = 3;
    REQUIRE(!media_delivery_sign_holder(media_test_group_key(GROUP, (uint8_t[32]){0}), &row, private)); memcpy(hb->signature, row.signature, 64); receive(a, &p);
    REQUIRE(scalar(a->md.db, "SELECT status FROM block_availability") == 1);
    free_node(a); reset(); capture_blocks = 1;
}

static void test_pages(void) {
    const int counts[] = {0, 1, 32, 33, 65, 10000};
    for (unsigned t = 0; t < sizeof(counts) / sizeof(*counts); t++) {
        struct UTUN_INSTANCE* a = node(SERVER);
        struct media_index_result r = result(counts[t] ? counts[t] : 1);
        if (counts[t]) seed(a, &r, counts[t], 1, 0);
        /* Реальный 64-bit AUTOINCREMENT и пропуски после DELETE/чужой группы. */
        sql(a->md.db, "UPDATE block_availability SET id=id+4294967296;"
            "INSERT INTO block_availability(block_uuid,group_id,holder,chunk,timestamp,status,version,signature)"
            " VALUES(zeroblob(16),'" GROUP2_KEY "',fixture_holder(1001),0,1,1,1,zeroblob(64));"
            "DELETE FROM block_availability WHERE group_id='" GROUP2_KEY "';");
        uint64_t cursor = 0; int seen = 0, pages = 0;
        for (;;) {
            struct packet p = page(a, CLIENT, GROUP, cursor, a->md.source_epoch, 1);
            const struct media_pkt_sync_resp* h = (const struct media_pkt_sync_resp*)p.data;
            REQUIRE(h->status == MEDIA_SYNC_OK && read_le(&h->from_id, 8) == cursor);
            unsigned n = (unsigned)read_le(&h->num_entries, 2);
            REQUIRE(n <= 32 && p.len == sizeof(*h) + n * sizeof(struct media_pkt_sync_entry));
            const struct media_pkt_sync_entry* rows = (const struct media_pkt_sync_entry*)(p.data + sizeof(*h));
            for (unsigned i = 0; i < n; i++) {
                REQUIRE(read_le(&rows[i].id, 8) > cursor);
                REQUIRE(fixture_address(rows[i].holder) == media_fixture_id(1000));
                cursor = read_le(&rows[i].id, 8);
            }
            seen += n; pages++;
            REQUIRE(read_le(&h->next_id, 8) >= cursor);
            cursor = read_le(&h->next_id, 8);
            if (!h->more) break;
        }
        REQUIRE(seen == counts[t] && pages == (counts[t] ? (counts[t] + MEDIA_SYNC_MAX_ENTRIES - 1) / MEDIA_SYNC_MAX_ENTRIES : 1));
        REQUIRE(cursor == (uint64_t)scalar(a->md.db, "SELECT seq FROM sqlite_sequence WHERE name='block_availability'"));
        media_index_result_free(&r); free_node(a); reset(); capture_blocks = 1;
    }
}

static void test_atomic_and_validation(void) {
    struct UTUN_INSTANCE* a = node(SERVER), *b = node(CLIENT);
    struct media_index_result r = result(2); seed(b, &r, 2, 1, 0);
    begin_pull(a, CLIENT, GROUP);
    struct packet q = request_from(a, CLIENT);
    const struct media_pkt_sync_req* query = (const struct media_pkt_sync_req*)q.data;
    struct packet response = page(b, SERVER, GROUP, read_le(&query->after_id, 8), query->source_epoch, query->request_id[0]);
    memcpy(((struct media_pkt_sync_resp*)response.data)->request_id, query->request_id, 16);
    struct media_super_peer* sp = peer(a, CLIENT, GROUP); REQUIRE(sp && sp->request_pending);
    uint64_t deadline = sp->deadline_tb;
    struct packet bad = response;
    for (size_t len = 1; len < response.len; len++) { bad.len = len; receive(a, &bad); }
    bad = response; bad.group++; receive(a, &bad);
    bad = response; bad.from = media_fixture_id(768); receive(a, &bad);
    bad = response; ((struct media_pkt_sync_resp*)bad.data)->request_id[1] ^= 1; receive(a, &bad);
    bad = response; ((struct media_pkt_sync_resp*)bad.data)->more = 1; receive(a, &bad);
    bad = response;
    struct media_pkt_sync_entry* rows = (struct media_pkt_sync_entry*)(bad.data + MEDIA_SYNC_RESP_HDR_SIZE);
    rows[1].status = 3; receive(a, &bad);
    for (int fault = 0; fault < 14; fault++) {
        bad = response;
        struct media_pkt_sync_resp* h = (struct media_pkt_sync_resp*)bad.data;
        rows = (struct media_pkt_sync_entry*)(bad.data + sizeof(*h));
        if (fault == 0) bad.len++;
        if (fault == 1) write_le(&h->num_entries, 33, 2);
        if (fault == 2) h->status = 3;
        if (fault == 3) write_le(&h->next_id, UINT64_MAX, 8);
        if (fault == 4) rows[1].id = rows[0].id;
        if (fault == 5) memset(rows[0].holder, 0, 32);
        if (fault == 6) write_le(&rows[1].timestamp, UINT64_MAX, 8);
        if (fault == 7) memcpy(rows[1].block_id, rows[0].block_id, 16);
        if (fault == 8) media_test_public(media_fixture_id(768), rows[0].holder);
        if (fault == 9) write_le(&rows[0].version, INT64_MAX, 8);
        if (fault == 10) write_le(&rows[0].chunk, 99, 4);
        if (fault == 11) write_le(&rows[0].timestamp, INT64_MAX, 8);
        if (fault == 12) rows[0].signature[0] ^= 1;
        if (fault == 13) REQUIRE(!media_delivery_sign_holder(media_test_group_key(GROUP, (uint8_t[32]){0}), &rows[0], b->my_ed25519_privkey));
        receive(a, &bad);
    }
    REQUIRE(sp->request_pending && sp->deadline_tb == deadline && !scalar(a->md.db, "SELECT COUNT(*) FROM super_sync"));
    REQUIRE(!scalar(a->md.db, "SELECT COUNT(*) FROM block_availability"));
    const char* failures[] = {
        "CREATE TRIGGER fail BEFORE INSERT ON block_availability WHEN NEW.chunk=1 BEGIN SELECT RAISE(ABORT,'row fault'); END",
        "CREATE TRIGGER fail BEFORE INSERT ON super_sync BEGIN SELECT RAISE(ABORT,'cursor fault'); END"
    };
    for (unsigned i = 0; i < 2; i++) {
        sql(a->md.db, failures[i]); receive(a, &response);
        REQUIRE(sp->request_pending && !scalar(a->md.db, "SELECT COUNT(*) FROM block_availability")
            && !scalar(a->md.db, "SELECT COUNT(*) FROM super_sync"));
        sql(a->md.db, "DROP TRIGGER fail");
    }
    sql(a->md.db, "BEGIN"); receive(a, &response); sql(a->md.db, "ROLLBACK");
    REQUIRE(sp->request_pending && !scalar(a->md.db, "SELECT COUNT(*) FROM super_sync"));
    receive(a, &response);
    REQUIRE(!sp->request_pending && scalar(a->md.db, "SELECT COUNT(*) FROM block_availability") == 2);
    sqlite3_int64 high = scalar(a->md.db, "SELECT seq FROM sqlite_sequence WHERE name='block_availability'");
    deadline = sp->deadline_tb; receive(a, &response);
    REQUIRE(deadline == sp->deadline_tb && scalar(a->md.db, "SELECT seq FROM sqlite_sequence WHERE name='block_availability'") == high);
    free_node(a); free_node(b); media_index_result_free(&r); reset(); capture_blocks = 1;
}

/* Подпись holder не даёт ему права задавать общий chunk остальных держателей блока. */
static void test_independent_holders(void) {
    struct UTUN_INSTANCE* a = node(SERVER), *b = node(CLIENT);
    struct media_index_result r = result(2); seed(b, &r, 2, 1, 0);
    begin_pull(a, CLIENT, GROUP); struct packet q = request_from(a, CLIENT);
    const struct media_pkt_sync_req* query = (const void*)q.data;
    struct packet response = page(b, SERVER, GROUP, read_le(&query->after_id, 8), query->source_epoch, query->request_id[0]);
    memcpy(((struct media_pkt_sync_resp*)response.data)->request_id, query->request_id, 16);
    struct media_pkt_sync_entry* rows = (void*)(response.data + MEDIA_SYNC_RESP_HDR_SIZE);
    memcpy(rows[1].block_id, rows[0].block_id, 16); media_test_public(media_fixture_id(1001), rows[1].holder); write_le(&rows[1].chunk, 99, 4);
    uint8_t private[32]; media_test_private(media_fixture_id(1001), private);
    REQUIRE(!media_delivery_sign_holder(media_test_group_key(GROUP, (uint8_t[32]){0}), &rows[1], private)); receive(a, &response);
    REQUIRE(!peer(a, CLIENT, GROUP)->request_pending);
    REQUIRE(scalar(a->md.db, "SELECT COUNT(*) FROM block_availability") == 2);
    REQUIRE(scalar(a->md.db, "SELECT chunk FROM block_availability WHERE holder=fixture_holder(1000)") == 0);
    REQUIRE(scalar(a->md.db, "SELECT chunk FROM block_availability WHERE holder=fixture_holder(1001)") == 99);
    free_node(a); free_node(b); media_index_result_free(&r); reset(); capture_blocks = 1;
    puts("PASS independently signed holders cannot block or rewrite each other's chunk");
}

static void test_convergence_and_lifecycle(void) {
    struct UTUN_INSTANCE* nodes[3] = {node(SERVER), node(CLIENT), node(media_fixture_id(768))};
    for (int n = 0; n < 3; n++) {
        struct media_index_result r = result(65);
        for (int i = 0; i < 65; i++) r.block_ids[i * 16 + 15] = (uint8_t)n;
        seed(nodes[n], &r, 65, 1, 0); media_index_result_free(&r);
        media_delivery_refresh_roles(nodes[n]);
    }
    synchronize(nodes, 3, 1);
    for (int i = 0; i < 3; i++) {
        REQUIRE(scalar(nodes[i]->md.db, "SELECT COUNT(*) FROM block_availability") == 195);
        REQUIRE(scalar(nodes[i]->md.db, "SELECT COUNT(*) FROM super_sync") == 2);
    }
    sqlite3_int64 highs[3];
    for (int i = 0; i < 3; i++) {
        highs[i] = scalar(nodes[i]->md.db, "SELECT seq FROM sqlite_sequence WHERE name='block_availability'");
        for (struct ll_entry* e = nodes[i]->md.super_peers->head; e; e = e->next)
            ((struct media_super_peer*)e->data)->deadline_tb = 0;
    }
    /* Настоящий watchdog запускает периодическую сверку, а не тестовая копия алгоритма. */
    uint64_t until = get_time_tb() + 3000;
    while (get_time_tb() < until) uasync_poll(ua, 100);
    synchronize(nodes, 3, 1);
    for (int i = 0; i < 3; i++) REQUIRE(highs[i] == scalar(nodes[i]->md.db,
        "SELECT seq FROM sqlite_sequence WHERE name='block_availability'"));
    add_group(nodes[0], 222); media_delivery_refresh_roles(nodes[0]); poll_ready();
    REQUIRE(peer(nodes[0], CLIENT, GROUP) && peer(nodes[0], CLIENT, 222));
    sql(nodes[0]->md.db, "UPDATE peers_" GROUP_KEY " SET adm_tags=NULL WHERE node_id=fixture_node(256)");
    media_delivery_refresh_roles(nodes[0]);
    REQUIRE(!peer(nodes[0], CLIENT, GROUP) && peer(nodes[0], CLIENT, 222));
    REQUIRE(!scalar(nodes[0]->md.db, "SELECT COUNT(*) FROM super_sync WHERE group_id='" GROUP_KEY "'"));
    REQUIRE(!scalar(nodes[0]->md.db, "SELECT COUNT(*) FROM block_availability WHERE group_id='" GROUP_KEY "' AND holder!=fixture_holder(256)"));
    sql(nodes[0]->md.db, "UPDATE peers_" GROUP_KEY " SET adm_tags='{\"supernode\":\"yes\"}' WHERE node_id=fixture_node(256)");
    media_delivery_refresh_roles(nodes[0]);
    synchronize(nodes, 3, 0);
    REQUIRE(scalar(nodes[0]->md.db, "SELECT COUNT(*) FROM block_availability WHERE group_id='" GROUP_KEY "'") == 195);
    for (int i = 0; i < 3; i++) free_node(nodes[i]);
    reset(); capture_blocks = 1;
}

static struct packet respond_to(struct UTUN_INSTANCE* source, const struct packet* q) {
    receive(source, q); poll_ready();
    struct packet answer = {0};
    while (packet_count) {
        struct packet p = take_packet();
        if (p.from == source->node_id && p.to == q->from && p.group == q->group && p.data[0] == MEDIA_SUBCMD_SYNC_RESP
            && !memcmp(((struct media_pkt_sync_resp*)p.data)->request_id,
                       ((const struct media_pkt_sync_req*)q->data)->request_id, 16)) answer = p;
    }
    REQUIRE(answer.len); return answer;
}

static int reject_commit(void* arg) { (void)arg; return 1; }

static int deny_rows(void* arg, int action, const char* table, const char* column, const char* db, const char* trigger) {
    (void)arg; (void)column; (void)db; (void)trigger;
    return action == SQLITE_READ && table && !strcmp(table, "block_availability") ? SQLITE_DENY : SQLITE_OK;
}

/* Обновление после последнего snapshot: обе перестановки CHANGED/RESP и потеря уведомления. */
static void test_hints_and_read_error(void) {
    for (int order = 0; order < 3; order++) {
        struct UTUN_INSTANCE* a = node(SERVER), *b = node(CLIENT);
        begin_pull(a, CLIENT, GROUP);
        struct packet q = request_from(a, CLIENT);
        struct media_super_peer* sp = peer(a, CLIENT, GROUP);
        uint64_t deadline = sp->deadline_tb;
        REQUIRE(sqlite3_set_authorizer(b->md.db, deny_rows, NULL) == SQLITE_OK);
        struct packet error = respond_to(b, &q);
        REQUIRE(((struct media_pkt_sync_resp*)error.data)->status == MEDIA_SYNC_DB_ERROR && error.len == MEDIA_SYNC_RESP_HDR_SIZE);
        receive(a, &error); REQUIRE(sp->request_pending && sp->deadline_tb == deadline);
        REQUIRE(sqlite3_set_authorizer(b->md.db, NULL, NULL) == SQLITE_OK);
        struct packet empty = respond_to(b, &q);
        struct media_index_result r = result(1); seed(b, &r, 1, 1, 0);
        struct packet hint = {.from = CLIENT, .to = SERVER, .group = GROUP, .len = 1, .data = {MEDIA_SUBCMD_CHANGED}};
        if (order == 0) receive(a, &hint);
        receive(a, &empty);
        if (order == 1) receive(a, &hint);
        if (order != 0) { packet_count = 0; expire(sp); } /* order=2: потерянный hint, истечение idle-сверки. */
        q = request_from(a, CLIENT);
        struct packet filled = respond_to(b, &q); receive(a, &filled);
        REQUIRE(scalar(a->md.db, "SELECT COUNT(*) FROM block_availability") == 1 && !sp->request_pending);
        free_node(a); free_node(b); media_index_result_free(&r); reset(); capture_blocks = 1;
    }
}

/* Потери запроса/ответа, backoff с тем же UUID, ошибка commit и пересоздание БД источника. */
static void test_retry_and_epoch(void) {
    struct UTUN_INSTANCE* a = node(SERVER), *b = node(CLIENT);
    struct media_index_result r = result(2); seed(b, &r, 2, 1, 0);
    begin_pull(a, CLIENT, GROUP);
    struct packet q = request_from(a, CLIENT);
    struct media_super_peer* sp = peer(a, CLIENT, GROUP);
    expire(sp);
    struct packet retry = request_from(a, CLIENT);
    REQUIRE(q.len == retry.len && !memcmp(q.data, retry.data, q.len) && sp->timeout_tb > MEDIA_REPL_TIMEOUT_TB);
    struct packet old = respond_to(b, &retry);
    /* Ответ потерян, повторный запрос не меняет UUID и допускает новый снимок источника. */
    sql(b->md.db, "UPDATE block_availability SET id=id+100;"
        "UPDATE sqlite_sequence SET seq=102 WHERE name='block_availability'");
    expire(sp); retry = request_from(a, CLIENT);
    REQUIRE(!memcmp(q.data, retry.data, q.len));
    struct packet newer = respond_to(b, &retry);
    sqlite3_commit_hook(a->md.db, reject_commit, NULL);
    receive(a, &newer);
    REQUIRE(sp->request_pending && !scalar(a->md.db, "SELECT COUNT(*) FROM block_availability")
        && !scalar(a->md.db, "SELECT COUNT(*) FROM super_sync") && sqlite3_get_autocommit(a->md.db));
    sqlite3_commit_hook(a->md.db, NULL, NULL);
    receive(a, &newer); receive(a, &old);
    REQUIRE(!sp->request_pending && scalar(a->md.db, "SELECT last_recv_id FROM super_sync WHERE peer=fixture_holder(512)") == 102);

    /* DOWN/UP отменяет RPC; старый ответ не завершает запрос нового подключения. */
    struct fake_handle* h = (struct fake_handle*)sp->connection; REQUIRE(h);
    h->cb(sp->connection, h->ed, GROUP, CONN_EVENT_DOWN, h->arg);
    h->cb(sp->connection, h->ed, GROUP, CONN_EVENT_UP, h->arg);
    struct packet current = request_from(a, CLIENT);
    REQUIRE(memcmp(((struct media_pkt_sync_req*)q.data)->request_id,
                   ((struct media_pkt_sync_req*)current.data)->request_id, 16));
    receive(a, &old); REQUIRE(sp->request_pending);
    uint8_t epoch[16]; memcpy(epoch, b->md.source_epoch, 16);
    /* Новый пустой каталог источника: очистка здесь — явная подготовка теста, в приложении сбросов нет. */
    media_delivery_destroy(b);
    sql(b->topo_sqlite_db, "DELETE FROM block_availability;DELETE FROM super_sync;DELETE FROM media_replication_meta;"
        "DELETE FROM sqlite_sequence WHERE name='block_availability'");
    REQUIRE(!media_delivery_init(b) && memcmp(epoch, b->md.source_epoch, 16));
    r.block_ids[0] = 0xEE; seed(b, &r, 1, 1, 0);
    struct packet fresh = respond_to(b, &current);
    REQUIRE(read_le(&((struct media_pkt_sync_resp*)fresh.data)->from_id, 8) == 0);
    receive(a, &fresh); REQUIRE(!sp->request_pending);
    REQUIRE(scalar(a->md.db, "SELECT COUNT(*) FROM block_availability WHERE substr(block_uuid,1,1)=x'ee'") == 1);
    sqlite3_stmt* st = NULL;
    REQUIRE(sqlite3_prepare_v2(a->md.db, "SELECT source_epoch FROM super_sync WHERE peer=fixture_holder(512)", -1, &st, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW && !memcmp(sqlite3_column_blob(st, 0), b->md.source_epoch, 16));
    REQUIRE(sqlite3_finalize(st) == SQLITE_OK);
    /* Источник восстановлен из более старого снимка с прежней эпохой: отдельный reset только RX-курсора. */
    sql(a->md.db, "UPDATE super_sync SET last_recv_id=999 WHERE peer=fixture_holder(512)");
    packet_count = 0; expire(sp); current = request_from(a, CLIENT);
    fresh = respond_to(b, &current);
    REQUIRE(((struct media_pkt_sync_resp*)fresh.data)->status == MEDIA_SYNC_CURSOR_INVALID);
    receive(a, &fresh);
    REQUIRE(!sp->request_pending && !scalar(a->md.db, "SELECT COUNT(*) FROM super_sync WHERE peer=fixture_holder(512)"));
    REQUIRE(scalar(a->md.db, "SELECT COUNT(*) FROM block_availability") == 3);
    free_node(a); free_node(b); media_index_result_free(&r); reset(); capture_blocks = 1;
}

/* Реальные router send_q/waiter: заполнение, синхронное пробуждение и reset очереди. */
static void test_backpressure(void) {
    for (int restart = 0; restart < 2; restart++) {
        struct UTUN_INSTANCE* a = node(SERVER);
        struct ETCP_ROUTER_CONN* channel = etcp_router_conn_get(a, GROUP, CLIENT, ETCP_RT_ID_MEDIA_DELIVERY); REQUIRE(channel);
        queue_set_threshold(channel->send_q, 0, 0);
        struct ll_entry* occupied = queue_entry_new(0); REQUIRE(occupied && !queue_data_put(channel->send_q, occupied));
        begin_pull(a, CLIENT, GROUP);
        struct media_super_peer* sp = peer(a, CLIENT, GROUP); REQUIRE(sp && sp->request_pending && !packet_count);
        for (int i = 1; i <= 100; i++) {
            struct packet p = {.from = CLIENT, .to = SERVER, .group = GROUP, .len = MEDIA_SYNC_REQ_SIZE};
            struct media_pkt_sync_req* req = (struct media_pkt_sync_req*)p.data;
            req->subcmd = MEDIA_SUBCMD_SYNC_REQ; req->request_id[0] = (uint8_t)i;
            receive(a, &p); poll_ready();
        }
        REQUIRE(!packet_count && sp->response_pending && sp->response.request_id[0] == 100);
        if (restart) {
            etcp_router_conn_restart(a, GROUP, CLIENT, ETCP_RT_ID_MEDIA_DELIVERY);
            uint64_t until = get_time_tb() + 10000;
            while (!packet_count && get_time_tb() < until) uasync_poll(ua, 100);
        } else {
            occupied = queue_data_get(channel->send_q); REQUIRE(occupied); queue_entry_free(occupied); poll_ready();
        }
        int responses = 0, requests = 0;
        while (packet_count) {
            struct packet p = take_packet();
            if (p.data[0] == MEDIA_SUBCMD_SYNC_REQ) requests++;
            if (p.data[0] == MEDIA_SUBCMD_SYNC_RESP) {
                REQUIRE(((struct media_pkt_sync_resp*)p.data)->request_id[0] == 100); responses++;
            }
        }
        REQUIRE(responses == 1 && requests == 1);
        free_node(a); reset(); capture_blocks = 1;
    }
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); srand(17);
    debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
    ua = uasync_create(); REQUIRE(ua); capture_blocks = 1;
    test_group_boundary();
    test_pages();
    test_atomic_and_validation();
    test_independent_holders();
    test_convergence_and_lifecycle();
    test_retry_and_epoch();
    test_hints_and_read_error();
    test_backpressure();
    REQUIRE(cm_opened == cm_closed);
    uasync_destroy(ua, 0);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "media replication regressions: PASS CM=%d/%d", cm_opened, cm_closed);
    return 0;
}
