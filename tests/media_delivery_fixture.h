/* Общий однопоточный стенд media QUERY/BLOCK/репликации: реальные SQLite/router handlers и Ed25519, управляемая доставка.
 * Включать в одну единицу трансляции бинарника с --wrap=etcp_route_send/RAND_bytes (цели tests/Makefile.am).
 * Caller создаёт ua и временный directory, затем instance/result; уничтожает nodes/metadata до uasync и каталога.
 * instance здесь — минимальный UTUN_INSTANCE, без core_start/сети; освобождается только парным destroy этого стенда.
 * Пакеты копируются в локальную FIFO и доставляются receive явно: автоматически по p.to ничего не пересылается.
 * Любая проверка REQUIRE завершает процесс с кодом 1. Статические globals не поддерживают параллельные/вложенные receive. */
#ifndef TEST_MEDIA_DELIVERY_FIXTURE_H
#define TEST_MEDIA_DELIVERY_FIXTURE_H
/* Регрессии QUERY: реальные обработчики сервиса и SQLite, управляемая доставка/ошибки транспорта.
 * Стенд сохраняет владение router: пакет потребляется даже при ошибке. Ответы доставляются отдельно,
 * поэтому можно проверить задержки, дубликаты, чужие ответы и состояние между пачками. */
#include "test_utils.h"
#include "media_members_fixture.h"
#include "../src/utun_instance.h"
#include "../src/chat/chat_core_priv.h"
#include "../src/media_delivery/media_index.h"
#include "../src/media_delivery/media_download.h"
#include "../src/routing_layer/etcp_router.h"
#include "../src/transport_layer/etcp.h"
#include "../src/transport_layer/secure_channel.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/platform_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#ifdef __SANITIZE_ADDRESS__
#include <sanitizer/asan_interface.h>
#endif

#define GROUP 111
#define GROUP_KEY "6f00000000000000000000000000000000000000000000000000000000000001"
#define GROUP2_KEY "de00000000000000000000000000000000000000000000000000000000000001"
#define SERVER media_fixture_id(256)
#define CLIENT media_fixture_id(512)
#define MAX_WIRE (MEDIA_QUERY_RESP_HDR_SIZE + MEDIA_QUERY_MAX_BLOCKS * sizeof(struct media_pkt_query_resp_entry) + 1)
#define REQUIRE(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "QUERY test line=%d: %s", __LINE__, #x); exit(1); } } while (0)

/* Полная копия после удаления service ID: from/to/group — ID узлов/группы; data[0] — MEDIA_SUBCMD_*.
 * len — байты payload без router envelope, не больше MAX_WIRE. */
struct packet { uint64_t from, to, group; size_t len; uint8_t data[MAX_WIRE]; };
static struct packet packets[128]; // FIFO захваченных отправок, индекс 0 выдаёт take_packet.
/* Счётчики отправок/завершений и faults; fail_send — subcmd отказа, fail_rng — устойчивый отказ RAND_bytes,
 * capture_blocks включает захват прочих subcmd помимо QUERY/QUERY_RESP. */
static int packet_count, query_count, request_count, done_count, done_err, fail_send, fail_rng, capture_blocks;
static struct UASYNC* ua; // Общий заимствованный loop, который заранее создаёт main теста.
static char directory[256]; // NUL-путь временного каталога main; start пишет directory/result.bin.
static void* injected; // Единственный текущий payload receive, снимается его dgram_free_fn.
static uint8_t* poison_tail; // Начало 64 защитных байтов после логического payload для ASAN.
static uint8_t author_key[32] = {99, 1}; // Фиксированный Ed25519 private key тестового автора SERVER.
static void (*sent_hook)(struct UTUN_INSTANCE*, uint64_t, uint64_t, int); // Синхронный callback успешной отправки (inst/group/dst/subcmd).

int __real_RAND_bytes(unsigned char* out, int count);
// При fail_rng вернуть 0 без заполнения out; иначе вызвать настоящий RAND_bytes.
int __wrap_RAND_bytes(unsigned char* out, int count) {
    if (fail_rng) return 0;
    return __real_RAND_bytes(out, count);
}

/* Вернуть 0/-1 по fail_send, посчитать QUERY/BLOCK_REQ и при необходимости скопировать payload в FIFO. */
static int media_fixture_send(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t dst,
                              const uint8_t* payload, size_t len, int force, int mode) {
    REQUIRE(mode == (ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED));
    int cmd = payload[0], rc = cmd == fail_send ? -1 : 0;
    if (cmd == MEDIA_SUBCMD_SYNC_REQ || cmd == MEDIA_SUBCMD_SYNC_RESP || cmd == MEDIA_SUBCMD_CHANGED) REQUIRE(!force);
    if (cmd == MEDIA_SUBCMD_BLOCK_REQ) request_count++;
    if (cmd == MEDIA_SUBCMD_QUERY) query_count++;
    if (!rc && (cmd == MEDIA_SUBCMD_QUERY || cmd == MEDIA_SUBCMD_QUERY_RESP || capture_blocks)) {
        REQUIRE(packet_count < 128 && len <= MAX_WIRE);
        struct packet* p = &packets[packet_count++];
        p->from = inst->node_id; p->to = dst; p->group = group; p->len = len;
        memcpy(p->data, payload, len);
    }
    if (!rc && sent_hook) sent_hook(inst, group, dst, cmd);
    return rc;
}

int __wrap_etcp_route_send(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t dst,
                          struct ll_entry* entry, int force, int mode) {
    int rc = media_fixture_send(inst, group, dst, entry->dgram + 1, entry->len - 1, force, mode);
    queue_dgram_free(entry); queue_entry_free(entry); return rc;
}

int __wrap_etcp_route_send_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t dst[32],
                              struct ll_entry* entry, int force, int mode) {
    uint8_t x[32]; REQUIRE(sc_identity_public_x("media fixture", dst, x) == SC_OK);
    return __wrap_etcp_route_send(inst, group, sc_derive_node_id_from_pubkey(x), entry, force, mode);
}

int __wrap_etcp_router_conn_send(struct ETCP_ROUTER_CONN* conn, const uint8_t* data, size_t len, int mode) {
    return media_fixture_send(conn->inst, conn->group_id, conn->remote_node_id, data, len, 0, mode);
}

/* u_malloc округляет выделение: ASAN защищает именно логическую границу входного payload. */
static void release_injected(uint8_t* data) {
#ifdef __SANITIZE_ADDRESS__
    if (data == injected) __asan_unpoison_memory_region(poison_tail, 64);
#endif
    injected = NULL; poison_tail = NULL; u_free(data);
}

/* Скопировать p в router envelope с src/group и вызвать настоящий media callback выбранного inst.
 * p заимствован на вызов; callback обязан потребить payload синхронно, что проверяется через injected. */
static void receive(struct UTUN_INSTANCE* inst, const struct packet* p) {
    struct ll_entry* entry = queue_entry_new(0); REQUIRE(entry);
    size_t bytes = ROUTER_SVC_PAYLOAD_OFF + p->len;
    entry->dgram = u_calloc(1, bytes + 64); REQUIRE(entry->dgram);
    entry->len = (uint16_t)bytes; entry->dgram_free_fn = release_injected;
    entry->dgram[0] = ETCP_RT_ID_MEDIA_DELIVERY;
    media_test_public(p->from, entry->dgram + ROUTER_SVC_SRC_ED_OFF);
    memcpy(entry->dgram + ROUTER_SVC_DST_ED_OFF, inst->my_ed25519_pubkey, 32);
    entry->dgram[ROUTER_SVC_FLAGS_OFF] = ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED;
    memcpy(entry->dgram + ROUTER_SVC_GROUP_OFF, &p->group, 8);
    memcpy(entry->dgram + ROUTER_SVC_PAYLOAD_OFF, p->data, p->len);
    injected = entry->dgram; poison_tail = entry->dgram + bytes;
#ifdef __SANITIZE_ADDRESS__
    __asan_poison_memory_region(poison_tail, 64);
#endif
    struct ETCP_CONN conn = {.instance = inst};
    inst->router_bindings.callbacks[ETCP_RT_ID_MEDIA_DELIVERY](&conn, entry);
    REQUIRE(!injected);
}

// Извлечь первую копию FIFO по значению; пустая очередь завершает тест. Данные результата не требуют free.
static struct packet take_packet(void) {
    REQUIRE(packet_count > 0);
    struct packet result = packets[0];
    memmove(packets, packets + 1, (size_t)--packet_count * sizeof(*packets));
    return result;
}

/* SQLite-запросы используют те же читаемые fixture_slot, что C-код сценария. */
static void fixture_node_sql(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    (void)argc; sqlite3_result_int64(ctx, (sqlite3_int64)media_fixture_id((unsigned)sqlite3_value_int(argv[0])));
}

static uint64_t fixture_address(const uint8_t ed[32]) {
    uint8_t x[32]; REQUIRE(sc_identity_public_x("media fixture", ed, x) == SC_OK);
    return sc_derive_node_id_from_pubkey(x);
}

static void fixture_holder_sql(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    (void)argc; uint8_t ed[32]; media_test_public(media_fixture_id((unsigned)sqlite3_value_int(argv[0])), ed);
    sqlite3_result_blob(ctx, ed, sizeof(ed), SQLITE_TRANSIENT);
}

/* Создать минимальный собственный узел node на общем ua: :memory: SQLite, router и media delivery.
 * БД содержит публичный ключ автора SERVER; полноценные core/chat/topology сервисы здесь не запускаются. */
static struct UTUN_INSTANCE* instance(uint64_t node) {
    struct UTUN_INSTANCE* inst = u_calloc(1, sizeof(*inst)); REQUIRE(inst);
    inst->ua = ua; inst->node_id = node;
    inst->chat_core = u_calloc(1, sizeof(struct chat_core_ctx)); REQUIRE(inst->chat_core);
    REQUIRE(sqlite3_open(":memory:", &inst->topo_sqlite_db) == SQLITE_OK);
    REQUIRE(sqlite3_create_function(inst->topo_sqlite_db, "fixture_node", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL,
        fixture_node_sql, NULL, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_create_function(inst->topo_sqlite_db, "fixture_holder", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL,
        fixture_holder_sql, NULL, NULL) == SQLITE_OK);
    uint64_t peers[sizeof(media_devices) / sizeof(media_devices[0])];
    for (size_t i = 0; i < sizeof(peers) / sizeof(peers[0]); i++) peers[i] = media_devices[i].id;
    media_test_group(inst, GROUP, peers, sizeof(peers) / sizeof(*peers));
    REQUIRE(!etcp_router_init(inst) && !media_index_init(inst->topo_sqlite_db) && !media_delivery_init(inst));
    uint8_t pubkey[32]; size_t length = sizeof(pubkey);
    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, author_key, 32); REQUIRE(key);
    REQUIRE(EVP_PKEY_get_raw_public_key(key, pubkey, &length) == 1 && length == 32); EVP_PKEY_free(key);
    REQUIRE(sqlite3_exec(inst->topo_sqlite_db, "CREATE TABLE nodes(node_id INTEGER PRIMARY KEY,ed25519_pubkey BLOB)",
                         NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_stmt* st = NULL;
    REQUIRE(sqlite3_prepare_v2(inst->topo_sqlite_db, "UPDATE peers_" GROUP_KEY " SET ed25519_pubkey=? WHERE node_id=fixture_node(256)", -1, &st, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_bind_blob(st, 1, pubkey, 32, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_DONE);
    REQUIRE(sqlite3_finalize(st) == SQLITE_OK);
    return inst;
}

/* Уничтожить media/router/SQLite/chat_core этого стенда и выполнить poll(0); inst обязателен, повтор не допускается.
 * Дополнительными ресурсами, которые тест сам добавил в inst (например topo_groups), владеет caller. */
static void destroy(struct UTUN_INSTANCE* inst) {
    media_delivery_destroy(inst); etcp_router_destroy(inst); media_test_groups_free(inst); sqlite3_close(inst->topo_sqlite_db);
    u_free(inst->chat_core); u_free(inst); uasync_poll(ua, 0);
}

// Callback загрузки: увеличить done_count и сохранить последнюю ошибку; arg не используется.
static void completed(void* arg, int err) { (void)arg; done_count++; done_err = err; }

/* Собственные метаданные count>0 однобайтовых блоков: payload i-го блока=(uint8_t)i, подписи/общий SHA256 настоящие.
 * media_id фиксирован AA; block_ids/block_sigs освобождаются caller через media_index_result_free. */
static struct media_index_result result(int count) {
    struct media_index_result r = {.file_size = count, .block_size = 1, .num_blocks = count};
    memset(r.media_id, 0xAA, 16);
    r.block_ids = u_calloc(count, 16); r.block_sigs = u_calloc(count, 64); REQUIRE(r.block_ids && r.block_sigs);
    EVP_MD_CTX* hash = EVP_MD_CTX_new(); REQUIRE(hash && EVP_DigestInit_ex(hash, EVP_sha256(), NULL) == 1);
    for (int i = 0; i < count; i++) {
        uint32_t id = (uint32_t)i + 1; uint8_t byte = (uint8_t)i; memcpy(r.block_ids + i * 16, &id, 4);
        REQUIRE(sc_ed25519_sign(author_key, &byte, 1, r.block_sigs + i * 64) == SC_OK);
        REQUIRE(EVP_DigestUpdate(hash, &byte, 1) == 1);
    }
    unsigned length; REQUIRE(EVP_DigestFinal_ex(hash, r.content_hash, &length) == 1 && length == 32); EVP_MD_CTX_free(hash);
    return r;
}

// Заимствованная первая загрузка inst либо NULL; не удерживает её после callback/отмены/destroy.
static struct media_download* download(struct UTUN_INSTANCE* inst) {
    return inst->md.downloads && inst->md.downloads->head ? (struct media_download*)inst->md.downloads->head->data : NULL;
}

// Запустить загрузку r в directory/result.bin для GROUP/SERVER с callback completed; directory уже должен существовать.
static void start(struct UTUN_INSTANCE* inst, struct media_index_result* r) {
    char path[512]; snprintf(path, sizeof(path), "%s/result.bin", directory);
    media_test_publish(inst->topo_sqlite_db, GROUP, SERVER, r);
    REQUIRE(!media_download_start(inst, GROUP, r, path, directory, media_test_public(SERVER, (uint8_t[32]){0}), completed, NULL, NULL, NULL));
}

/* Добавить availability первых count блоков r для GROUP: node_id=1000+h при distinct=0,
 * иначе 1000+i%distinct (в этом режиме задавать holders=1). Данные блоков на диск не записываются. */
static void seed(struct UTUN_INSTANCE* inst, const struct media_index_result* r, int count, int holders, int distinct) {
    sqlite3_stmt* st = NULL;
    REQUIRE(sqlite3_prepare_v2(inst->topo_sqlite_db,
        "INSERT INTO block_availability(block_uuid,group_id,holder,chunk,timestamp,status,version,signature)"
        " VALUES(?,'" GROUP_KEY "',?,?,1,1,1,?)", -1, &st, NULL) == SQLITE_OK);
    for (int i = 0; i < count; i++) for (int h = 0; h < holders; h++) {
        uint64_t holder = media_fixture_id(1000 + (distinct ? i % distinct : h));
        struct media_pkt_sync_entry row = {.chunk = i, .timestamp = 1, .status = 1, .version = 1};
        media_test_public(holder, row.holder);
        uint8_t private[32]; media_test_private(holder, private); memcpy(row.block_id, r->block_ids + i * 16, 16);
        REQUIRE(!media_delivery_sign_holder(media_test_group_key(GROUP, (uint8_t[32]){0}), &row, private));
        REQUIRE(sqlite3_bind_blob(st, 1, r->block_ids + i * 16, 16, SQLITE_STATIC) == SQLITE_OK);
        REQUIRE(sqlite3_bind_blob(st, 2, row.holder, sizeof(row.holder), SQLITE_TRANSIENT) == SQLITE_OK);
        REQUIRE(sqlite3_bind_blob(st, 4, row.signature, 64, SQLITE_TRANSIENT) == SQLITE_OK);
        REQUIRE(sqlite3_bind_int(st, 3, i) == SQLITE_OK && sqlite3_step(st) == SQLITE_DONE);
        REQUIRE(sqlite3_reset(st) == SQLITE_OK);
    }
    REQUIRE(sqlite3_finalize(st) == SQLITE_OK);
}

// Очистить FIFO/счётчики/faults и sent_hook; не освобождает узлы/загрузки/ua и не удаляет файлы.
static void reset(void) {
    packet_count = query_count = request_count = done_count = done_err = fail_send = fail_rng = capture_blocks = 0;
    sent_hook = NULL;
}

// Доставить первый QUERY серверу и извлечь первый QUERY_RESP; ожидается именно такая последовательность FIFO.
static struct packet answer(struct UTUN_INSTANCE* server) {
    struct packet query = take_packet(); REQUIRE(query.data[0] == MEDIA_SUBCMD_QUERY);
    receive(server, &query);
    struct packet response = take_packet(); REQUIRE(response.data[0] == MEDIA_SUBCMD_QUERY_RESP);
    return response;
}

/* Для однобайтового блока bi загрузки dl доставить CHUNK и DONE от выбранного holder.
 * Нужны уже выбранный holder и активная загрузка CLIENT; последний DONE может завершить/освободить загрузку. */
static void deliver_block(struct UTUN_INSTANCE* c, struct media_download* dl, int bi) {
    struct media_download_block* b = &dl->blocks[bi];
    struct packet p = {.from = fixture_address(b->holders[b->holder_idx]), .to = CLIENT, .group = GROUP, .len = MEDIA_BLOCK_CHUNK_HDR_SIZE + 1};
    struct media_pkt_block_chunk* chunk = (struct media_pkt_block_chunk*)p.data;
    chunk->subcmd = MEDIA_SUBCMD_BLOCK_CHUNK; chunk->chunk = bi; chunk->data_len = 1;
    memcpy(chunk->media_id, dl->media_id, 16); memcpy(chunk->block_id, dl->blocks[bi].block_id, 16);
    memcpy(chunk->transfer_id, b->transfer_id, 16);
    p.data[MEDIA_BLOCK_CHUNK_HDR_SIZE] = (uint8_t)bi; receive(c, &p);
    memset(p.data, 0, sizeof(p.data)); p.len = MEDIA_BLOCK_DONE_SIZE;
    struct media_pkt_block_done* done = (struct media_pkt_block_done*)p.data;
    done->subcmd = MEDIA_SUBCMD_BLOCK_DONE; done->chunk = bi; done->total_size = 1;
    memcpy(done->media_id, dl->media_id, 16); memcpy(done->block_id, dl->blocks[bi].block_id, 16);
    memcpy(done->transfer_id, b->transfer_id, 16);
    receive(c, &p);
}


#endif
