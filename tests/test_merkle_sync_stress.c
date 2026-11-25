/* Долгий сетевой тест Merkle sync, запускается только вручную.
 * Два узла: force-close -> 1..100 изменений -> reconnect -> полная сверка SQLite.
 * Три узла: треугольник, записи во время обмена, периодические проверки сходимости.
 * Модель: key BE64, version BE64, value[32]; побеждает большая версия.
 * Эталон меняется только генератором локальных записей, никогда при сетевом приёме. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/platform_compat.h"
#include "../lib/socket_compat.h"
#include "../src/utun_instance.h"
#include "../src/chat/merkle_sync.h"
#include "../src/routing_layer/topo_node.h"
#include "../src/transport_layer/etcp.h"
#include "../src/transport_layer/etcp_connections.h"
#include "../src/transport_layer/node_conn_direct.h"

#define RECORDS 10000
#define MAX_BATCH 100
#define RACE_BATCHES 1000
#define RACE_BURST 20
#define WIRE_SIZE 48
#define TEST_NS "1001"
#define TEST_SVC 0x72
#define SEED UINT64_C(0x4d65726b6c653130)
#define SECOND_TB UINT64_C(10000)

struct record { uint8_t wire[WIRE_SIZE]; };

struct node {
    struct UTUN_INSTANCE* inst;
    sqlite3_stmt* scan;
    sqlite3_stmt* write;
    unsigned port;
    uint64_t pages, continued, received, applied, stale;
};

static struct {
    struct UASYNC* ua;
    struct node nodes[3];
    struct NODE_CONN_DIRECT* links[3][3];
    struct record expected[RECORDS];
    uint64_t rng, version, started, disconnect_max;
    int count, inserted, phase, batch, reconnects, callbacks;
    int race_left, race_writes, race_overlap, page_interleaves;
    uint64_t last_pages;
    void* producer;
} test;

/* На ошибке сохраняем контекст, корни, объёмы и статистику каждого узла. */
static void fail(const char* expression, int line) {
    DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "stress: FAIL seed=%016" PRIx64 " phase=%d batch=%d inserted=%d version=%" PRIu64
        " line=%d: %s", SEED, test.phase, test.batch, test.inserted, test.version, line, expression);
    for (int i = 0; i < test.count; i++) {
        struct node* n = &test.nodes[i];
        if (!n->inst) continue;
        DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "stress: node=%d pages=%" PRIu64 " continued=%" PRIu64 " received=%" PRIu64
            " applied=%" PRIu64 " stale=%" PRIu64 " SQL=%s", i, n->pages, n->continued, n->received, n->applied,
            n->stale, sqlite3_errmsg(n->inst->topo_sqlite_db));
        uint8_t root[MT_HASH_SIZE];
        if (merkle_sync_read_hash(n->inst, MT_DATA_MEMBERS, TEST_NS, 0, 0, root) == 0)
            log_dump(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_DEBUG, "root", root, sizeof(root));
    }
    exit(EXIT_FAILURE);
}

#define CHECK(expr) do { if (!(expr)) fail(#expr, __LINE__); } while (0)

static uint64_t read64(const uint8_t* p) {
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) value = (value << 8) | p[i];
    return value;
}

static void write64(uint8_t* p, uint64_t value) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(value >> (56 - 8 * i));
}

/* SplitMix64 не зависит от платформенной реализации rand(). */
static uint64_t next_random(void) {
    uint64_t z = (test.rng += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

static int compare_records(const void* a, const void* b) { return memcmp(a, b, 8); }

static void shuffled_order(int order[RECORDS]) {
    for (int i = 0; i < RECORDS; i++) order[i] = i;
    for (int i = RECORDS - 1; i > 0; i--) {
        int j = (int)(next_random() % (uint64_t)(i + 1));
        int tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }
}

/* BLOB-ключи дают unsigned-порядок SQLite, включая старший бит node_id. */
static void scan_leaf(struct node* n, uint64_t prefix, int after_valid, const merkle_key_t* after) {
    uint8_t first[8], last[8], zero[8] = {0};
    write64(first, prefix);
    write64(last, prefix | ((UINT64_C(1) << (64 - MT_MAX_LEVEL * 5)) - 1));
    CHECK(sqlite3_bind_blob(n->scan, 1, first, 8, SQLITE_TRANSIENT) == SQLITE_OK);
    CHECK(sqlite3_bind_blob(n->scan, 2, last, 8, SQLITE_TRANSIENT) == SQLITE_OK);
    CHECK(sqlite3_bind_int(n->scan, 3, after_valid) == SQLITE_OK);
    CHECK(sqlite3_bind_blob(n->scan, 4, after_valid ? after->bytes : zero, 8, SQLITE_TRANSIENT) == SQLITE_OK);
}

/* Каноническое представление записи без padding и платформенного byte order. */
static void read_record(sqlite3_stmt* st, uint8_t wire[WIRE_SIZE]) {
    CHECK(sqlite3_column_bytes(st, 0) == 8 && sqlite3_column_bytes(st, 2) == 32);
    CHECK(sqlite3_column_int64(st, 1) > 0);
    memcpy(wire, sqlite3_column_blob(st, 0), 8);
    write64(wire + 8, (uint64_t)sqlite3_column_int64(st, 1));
    memcpy(wire + 16, sqlite3_column_blob(st, 2), 32);
}

/* Хеш модели вычисляется исключительно из локальной базы узла. */
static int leaf_hash(void* ctx, const char* ns, uint8_t level, uint64_t prefix, EVP_MD_CTX* sha) {
    struct node* n = ctx;
    CHECK(!strcmp(ns, TEST_NS) && level == MT_MAX_LEVEL);
    scan_leaf(n, prefix, 0, NULL);
    int rc, count = 0;
    while ((rc = sqlite3_step(n->scan)) == SQLITE_ROW) {
        uint8_t wire[WIRE_SIZE];
        read_record(n->scan, wire);
        CHECK(EVP_DigestUpdate(sha, wire, sizeof(wire)) == 1);
        count++;
    }
    CHECK(rc == SQLITE_DONE);
    CHECK(sqlite3_reset(n->scan) == SQLITE_OK);
    return count;
}

/* Страница ограничена ёмкостью движка; дополнительная строка определяет more. */
static int get_page(void* ctx, const char* ns, uint64_t prefix, int after_valid, const merkle_key_t* after,
        uint8_t* data, size_t* len, merkle_key_t* next, int* more) {
    struct node* n = ctx;
    CHECK(!strcmp(ns, TEST_NS) && *len >= WIRE_SIZE);
    scan_leaf(n, prefix, after_valid, after);
    size_t used = 0;
    int rc;
    memset(next, 0, sizeof(*next));
    while ((rc = sqlite3_step(n->scan)) == SQLITE_ROW && used + WIRE_SIZE <= *len) {
        read_record(n->scan, data + used);
        memcpy(next->bytes, data + used, 8);
        used += WIRE_SIZE;
    }
    CHECK(rc == SQLITE_ROW || rc == SQLITE_DONE);
    *more = rc == SQLITE_ROW;
    *len = used;
    CHECK(sqlite3_reset(n->scan) == SQLITE_OK);
    n->pages++;
    n->continued += *more;
    return 0;
}

/* Старая или повторно доставленная версия не должна перезаписывать новую. */
static int put_record(struct node* n, const uint8_t wire[WIRE_SIZE]) {
    CHECK(read64(wire + 8) > 0 && read64(wire + 8) <= INT64_MAX);
    CHECK(sqlite3_bind_blob(n->write, 1, wire, 8, SQLITE_STATIC) == SQLITE_OK);
    CHECK(sqlite3_bind_int64(n->write, 2, (sqlite3_int64)read64(wire + 8)) == SQLITE_OK);
    CHECK(sqlite3_bind_blob(n->write, 3, wire + 16, 32, SQLITE_STATIC) == SQLITE_OK);
    CHECK(sqlite3_step(n->write) == SQLITE_DONE);
    int changed = sqlite3_changes(n->inst->topo_sqlite_db);
    CHECK(sqlite3_reset(n->write) == SQLITE_OK);
    return changed;
}

/* Принятые записи и индекс публикуются одной транзакцией; changed будит другие сессии. */
static int apply_page(void* ctx, const char* ns, const uint8_t peer[32], const uint8_t* data, size_t len) {
    struct node* n = ctx;
    CHECK(!strcmp(ns, TEST_NS) && memcmp(peer, n->inst->my_ed25519_pubkey, 32) && len % WIRE_SIZE == 0);
    if (!len) return 0;
    uint64_t prefix = merkle_sync_level_prefix(read64(data), MT_MAX_LEVEL);
    for (size_t off = 0; off < len; off += WIRE_SIZE) {
        CHECK(merkle_sync_level_prefix(read64(data + off), MT_MAX_LEVEL) == prefix);
        CHECK(off == 0 || memcmp(data + off - WIRE_SIZE, data + off, 8) < 0);
        CHECK(read64(data + off + 8) > 0 && read64(data + off + 8) <= INT64_MAX);
    }
    sqlite3* db = n->inst->topo_sqlite_db;
    CHECK(sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK);
    int changed = 0;
    for (size_t off = 0; off < len; off += WIRE_SIZE) changed += put_record(n, data + off);
    if (changed) CHECK(merkle_sync_recompute_path(n->inst, MT_DATA_MEMBERS, ns, prefix) == 1);
    CHECK(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    if (changed) merkle_sync_changed(n->inst, MT_DATA_MEMBERS, ns);
    n->received += len / WIRE_SIZE;
    n->applied += changed;
    n->stale += len / WIRE_SIZE - changed;
    return 0;
}

static const struct merkle_sync_data_ops ops = {
    .update_bucket_hash = leaf_hash, .get_page = get_page, .apply_items = apply_page
};

/* ОС выбирает порт; ядро создаётся сразу после освобождения пробного сокета. */
static unsigned free_port(void) {
    socket_t fd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(fd != SOCKET_INVALID);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    CHECK(getsockname(fd, (struct sockaddr*)&addr, &len) == 0);
    unsigned port = ntohs(addr.sin_port);
    socket_close_wrapper(fd);
    return port;
}

/* Отдельное ядро и SQLite :memory:, без чата, TUN и фонового автоподключения групп. */
static void create_node(int index) {
    struct node* n = &test.nodes[index];
    struct SC_MYKEYS keys;
    /* Фиксированные тестовые ключи сохраняют роли leader/follower между прогонами. */
    for (int i = 0; i < 32; i++) keys.private_key[i] = (uint8_t)(49 + index * 47 + i * 13);
    CHECK(sc_compute_public_key_from_private(keys.private_key, keys.public_key) == SC_OK);
    char private_key[65], public_key[65], config[1024];
    for (int i = 0; i < 32; i++) {
        snprintf(private_key + 2 * i, 3, "%02x", keys.private_key[i]);
        snprintf(public_key + 2 * i, 3, "%02x", keys.public_key[i]);
    }
    n->port = free_port();
    snprintf(config, sizeof(config), "[global]\nmy_private_key=%s\nmy_public_key=%s"
        "\nkeepalive_adaptive=0\n[server: udp]\naddr=127.0.0.1:%u\ntype=public\n[allowed_keys]\nallow_all=1\n",
        private_key, public_key, n->port);
    n->inst = utun_instance_create_from_str(test.ua, config);
    CHECK(n->inst && utun_core_start(n->inst) == 0);
    sqlite3* db = n->inst->topo_sqlite_db;
    CHECK(sqlite3_exec(db, "CREATE TABLE stress_records(key BLOB PRIMARY KEY CHECK(length(key)=8),"
        "version INTEGER NOT NULL,value BLOB NOT NULL CHECK(length(value)=32)) WITHOUT ROWID", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_prepare_v2(db, "SELECT key,version,value FROM stress_records WHERE key>=?1 AND key<=?2 "
        "AND (?3=0 OR key>?4) ORDER BY key", -1, &n->scan, NULL) == SQLITE_OK);
    CHECK(sqlite3_prepare_v2(db, "INSERT INTO stress_records VALUES(?1,?2,?3) ON CONFLICT(key) DO UPDATE SET "
        "version=excluded.version,value=excluded.value WHERE excluded.version>stress_records.version",
        -1, &n->write, NULL) == SQLITE_OK);
    CHECK(merkle_sync_init(n->inst, TEST_SVC, &ops, n) == 0);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "stress: node=%d ready id=%016" PRIx64 " port=%u", index, n->inst->node_id, n->port);
}

/* Обе стороны держат ровно по одному handle: force_close не оставляет чужих владельцев. */
static void open_link(int a, int b) {
    CHECK(!test.links[a][b]);
    struct node* dest = &test.nodes[b];
    struct TOPO_ADDR4 address = { .addr = {127, 0, 0, 1}, .port = dest->port, .protocol = TOPO_PROTO_UDP };
    struct TOPO_NODE node = { .node_id = dest->inst->node_id, .v4_addrs = &address };
    memcpy(node.public_key, dest->inst->my_keys.public_key, 32);
    CHECK(node_conn_direct_open_node(test.nodes[a].inst, node.node_id, NULL, NULL, &test.links[a][b], &node, NULL) != NCD_ERR);
}

static int transport_ready(int a, int b) {
    struct ETCP_CONN* c = instance_find_conn(test.nodes[a].inst, test.nodes[b].inst->node_id);
    return c && c->initialized && c->links_up && !c->close_requested && c->state != 2;
}

/* Инициируем одну сторону, а входящий транспорт берём во владение после UP. */
static void connect_pair(int a, int b) {
    open_link(a, b);
    uint64_t deadline = get_time_tb() + 10 * SECOND_TB;
    while (!transport_ready(a, b) || !transport_ready(b, a)) {
        CHECK(get_time_tb() < deadline);
        uasync_poll(test.ua, 1);
    }
    open_link(b, a);
}

/* Только обработка отложенного close, без ожидания keepalive или fin_wait. */
static void disconnect_pair(int a, int b) {
    uint64_t started = get_time_tb();
    CHECK(test.links[a][b] && test.links[b][a]);
    node_conn_direct_force_close(test.links[a][b]);
    node_conn_direct_force_close(test.links[b][a]);
    test.links[a][b] = test.links[b][a] = NULL;
    while (instance_find_conn(test.nodes[a].inst, test.nodes[b].inst->node_id)
        || instance_find_conn(test.nodes[b].inst, test.nodes[a].inst->node_id)) {
        CHECK(get_time_tb() - started < SECOND_TB);
        uasync_poll(test.ua, 0);
    }
    uint64_t elapsed = get_time_tb() - started;
    if (elapsed > test.disconnect_max) test.disconnect_max = elapsed;
    test.reconnects++;
}

static void sync_done(const uint8_t peer[32], const char* ns, int result, void* arg) {
    (void)arg;
    if (result != MT_OK) DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "stress: sync peer=%s ns=%s result=%d", sc_public_key_hex(peer, (char[65]){0}), ns, result);
    CHECK(result == MT_OK && !strcmp(ns, TEST_NS));
    test.callbacks++;
}

/* Запрос контрольной точки только у leader; follower создаёт сессию по входящему BEGIN. */
static void start_pair(int a, int b) {
    if (memcmp(test.nodes[a].inst->my_ed25519_pubkey, test.nodes[b].inst->my_ed25519_pubkey, 32) > 0) { int tmp = a; a = b; b = tmp; }
    CHECK(merkle_sync_start(test.nodes[a].inst, MT_DATA_MEMBERS, instance_find_conn_by_key(test.nodes[a].inst, test.nodes[b].inst->my_keys.public_key), TEST_NS, sync_done, NULL) == 0);
}

static int roots_equal(void) {
    uint8_t first[MT_HASH_SIZE], current[MT_HASH_SIZE];
    CHECK(merkle_sync_read_hash(test.nodes[0].inst, MT_DATA_MEMBERS, TEST_NS, 0, 0, first) == 0);
    for (int i = 1; i < test.count; i++) {
        CHECK(merkle_sync_read_hash(test.nodes[i].inst, MT_DATA_MEMBERS, TEST_NS, 0, 0, current) == 0);
        if (memcmp(first, current, sizeof(first))) return 0;
    }
    return 1;
}

/* Сравнение с независимым эталоном обнаружит одинаково неверные базы и лишние записи. */
static void verify_databases(void) {
    for (int i = 0; i < test.count; i++) {
        sqlite3_stmt* st = NULL;
        CHECK(sqlite3_prepare_v2(test.nodes[i].inst->topo_sqlite_db,
            "SELECT key,version,value FROM stress_records ORDER BY key", -1, &st, NULL) == SQLITE_OK);
        int count = 0;
        for (int r = 0; r < RECORDS; r++) {
            const uint8_t* expected = test.expected[r].wire;
            if (!read64(expected + 8)) continue;
            CHECK(sqlite3_step(st) == SQLITE_ROW);
            uint8_t actual[WIRE_SIZE];
            read_record(st, actual);
            if (memcmp(actual, expected, WIRE_SIZE)) {
                DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "stress: data mismatch node=%d record=%d key=%016" PRIx64
                    " expected_version=%" PRIu64 " actual_key=%016" PRIx64 " actual_version=%" PRIu64,
                    i, r, read64(expected), read64(expected + 8), read64(actual), read64(actual + 8));
                log_dump(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_DEBUG, "expected", expected, WIRE_SIZE);
                log_dump(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_DEBUG, "actual", actual, WIRE_SIZE);
                CHECK(0);
            }
            count++;
        }
        CHECK(count == test.inserted && sqlite3_step(st) == SQLITE_DONE);
        CHECK(sqlite3_finalize(st) == SQLITE_OK);
    }
}

/* Timeout — страховка зависания теста, разрыв связи никогда не ждёт сетевого timeout. */
static void wait_converged(int callbacks) {
    uint64_t deadline = get_time_tb() + 120 * SECOND_TB;
    while (test.callbacks < callbacks || !roots_equal()) {
        CHECK(get_time_tb() < deadline);
        uasync_poll(test.ua, 1);
    }
    CHECK(test.callbacks == callbacks);
    verify_databases();
}

/* Меняем память эталона и одну SQLite; листья пачки пересчитываются по одному разу. */
static void mutate(int writer, const int* indices, int size, int inserting) {
    struct node* n = &test.nodes[writer];
    sqlite3* db = n->inst->topo_sqlite_db;
    uint64_t prefixes[MAX_BATCH];
    int leaves = 0;
    CHECK(size >= 1 && size <= MAX_BATCH);
    CHECK(sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK);
    for (int i = 0; i < size; i++) {
        uint8_t* wire = test.expected[indices[i]].wire;
        CHECK((read64(wire + 8) == 0) == inserting);
        if (inserting) test.inserted++;
        write64(wire + 8, ++test.version);
        for (int j = 16; j < WIRE_SIZE; j++) wire[j] = (uint8_t)next_random();
        wire[16] = (uint8_t)writer;
        CHECK(put_record(n, wire) == 1);
        uint64_t prefix = merkle_sync_level_prefix(read64(wire), MT_MAX_LEVEL);
        int p = 0;
        while (p < leaves && prefixes[p] != prefix) p++;
        if (p == leaves) prefixes[leaves++] = prefix;
    }
    for (int i = 0; i < leaves; i++)
        CHECK(merkle_sync_recompute_path(n->inst, MT_DATA_MEMBERS, TEST_NS, prefixes[i]) == 1);
    CHECK(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    merkle_sync_changed(n->inst, MT_DATA_MEMBERS, TEST_NS);
    DEBUG_DEBUG(DEBUG_CATEGORY_DEBUG, "stress: mutate phase=%d batch=%d writer=%d size=%d leaves=%d version=%" PRIu64,
        test.phase, test.batch, writer, size, leaves, test.version);
}

/* После каждого разрыва меняем случайную пачку, затем ждём новый обмен и сверяем обе базы. */
static void reconnect_phase(int inserting) {
    int order[RECORDS];
    shuffled_order(order);
    test.phase = inserting ? 1 : 2;
    test.batch = 0;
    for (int done = 0; done < RECORDS;) {
        disconnect_pair(0, 1);
        int size = 1 + (int)(next_random() % MAX_BATCH);
        if (size > RECORDS - done) size = RECORDS - done;
        test.batch++;
        mutate(test.batch % 2, order + done, size, inserting);
        connect_pair(0, 1);
        int callbacks = test.callbacks + 1;
        start_pair(0, 1);
        wait_converged(callbacks);
        done += size;
        if (test.batch == 1 || test.batch % 20 == 0 || done == RECORDS)
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "stress: reconnect verified phase=%d batch=%d size=%d done=%d/%d records=%d"
                " closes=%d close_max=%.1fms elapsed=%.1fs", test.phase, test.batch, size, done, RECORDS, test.inserted,
                test.reconnects, test.disconnect_max / 10.0, (get_time_tb() - test.started) / (double)SECOND_TB);
    }
}

/* Короткий таймер перемежает локальные записи с обработкой настоящих сетевых страниц. */
static void race_write(void* arg) {
    (void)arg;
    test.producer = NULL;
    if (!roots_equal()) test.race_overlap++;
    uint64_t pages = 0;
    for (int i = 0; i < 3; i++) pages += test.nodes[i].pages;
    if (test.race_left < RACE_BURST && pages > test.last_pages) test.page_interleaves++;
    test.last_pages = pages;
    int size = 1 + (int)(next_random() % MAX_BATCH), indices[MAX_BATCH];
    for (int i = 0; i < size; i++) {
        /* Общий небольшой набор горячих ключей создаёт повторные изменения и старые версии. */
        indices[i] = (int)(next_random() % (i % 4 == 0 ? 32 : RECORDS));
    }
    test.batch++;
    mutate(test.race_writes % 3, indices, size, 0);
    test.race_writes++;
    if (--test.race_left) {
        test.producer = uasync_set_timeout(test.ua, 10 + next_random() % 40, NULL, race_write, "merkle stress writer");
        CHECK(test.producer != NULL);
    }
}

/* Треугольник остаётся подключённым; после подписки дальнейшие обмены запускает только changed. */
static void race_phase(void) {
    test.phase = 3;
    test.batch = 0;
    test.count = 3;
    create_node(2);
    connect_pair(0, 2);
    connect_pair(1, 2);
    int callbacks = test.callbacks + 2;
    start_pair(0, 2);
    start_pair(1, 2);
    wait_converged(callbacks);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "stress: triangle ready, all three databases contain %d records", test.inserted);
    for (int burst = 0; burst < RACE_BATCHES / RACE_BURST; burst++) {
        test.race_left = RACE_BURST;
        test.producer = uasync_set_timeout(test.ua, 1, NULL, race_write, "merkle stress writer");
        CHECK(test.producer != NULL);
        uint64_t deadline = get_time_tb() + 120 * SECOND_TB;
        while (test.race_left) {
            CHECK(get_time_tb() < deadline);
            uasync_poll(test.ua, 1);
        }
        wait_converged(test.callbacks);
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "stress: triangle verified batches=%d/%d mutations=%" PRIu64
            " overlapping=%d page_interleaves=%d elapsed=%.1fs", test.race_writes, RACE_BATCHES, test.version,
            test.race_overlap, test.page_interleaves, (get_time_tb() - test.started) / (double)SECOND_TB);
    }
    CHECK(test.race_overlap > RACE_BATCHES / 2 && test.page_interleaves > RACE_BATCHES / 4);
    uint64_t continued = 0, stale = 0;
    for (int i = 0; i < 3; i++) { continued += test.nodes[i].continued; stale += test.nodes[i].stale; }
    CHECK(continued > 0 && stale > 0);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "stress: race coverage continued_pages=%" PRIu64 " stale_or_duplicate_records=%" PRIu64,
        continued, stale);
}

int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_WARN);
    /* Полная сверка 10k строк между poll намеренно занимает >10ms; ошибки SYS сохраняем. */
    debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_ERROR);
    debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
    if (getenv("UTUN_TEST_DEBUG")) {
        debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_MEMBER_SYNC, DEBUG_LEVEL_DEBUG);
    }
    test.rng = SEED;
    test.started = get_time_tb();
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "stress: start seed=%016" PRIx64 " records=%d batch=1..100 race_batches=%d",
        SEED, RECORDS, RACE_BATCHES);
    /* 1024 записей одного листа требуют нескольких страниц; остальные распределены по дереву. */
    for (uint32_t i = 0; i < RECORDS; i++) {
        uint64_t key = i < 1024 ? i : ((uint64_t)(i * UINT32_C(2654435761)) << 32) | i;
        write64(test.expected[i].wire, key);
    }
    qsort(test.expected, RECORDS, sizeof(test.expected[0]), compare_records);
    for (int i = 1; i < RECORDS; i++) CHECK(memcmp(test.expected[i - 1].wire, test.expected[i].wire, 8) < 0);
    test.ua = uasync_create();
    CHECK(test.ua != NULL);
    test.count = 2;
    create_node(0);
    create_node(1);
    connect_pair(0, 1);
    reconnect_phase(1);
    reconnect_phase(0);
    race_phase();
    CHECK(test.producer == NULL);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "stress: cleanup nodes=%d reconnects=%d callbacks=%d", test.count, test.reconnects, test.callbacks);
    /* Освобождаем владельцев без нового poll: destroy ниже сразу закрывает весь транспорт. */
    for (int a = 0; a < test.count; a++) for (int b = 0; b < test.count; b++) {
        if (test.links[a][b]) node_conn_direct_close(test.links[a][b]);
        test.links[a][b] = NULL;
    }
    for (int i = 0; i < test.count; i++) {
        struct node* n = &test.nodes[i];
        merkle_sync_destroy(n->inst);
        CHECK(sqlite3_finalize(n->scan) == SQLITE_OK && sqlite3_finalize(n->write) == SQLITE_OK);
        utun_instance_destroy(n->inst);
        n->inst = NULL;
    }
    uasync_poll(test.ua, 0);
    CHECK(test.ua->timer_alloc_count == test.ua->timer_free_count);
    CHECK(test.ua->socket_alloc_count == test.ua->socket_free_count);
    uasync_destroy(test.ua, 0);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "stress: PASS 10000 inserts + 10000 updates with reconnect; 1000 triangle race batches, elapsed=%.1fs",
        (get_time_tb() - test.started) / (double)SECOND_TB);
    return EXIT_SUCCESS;
}
