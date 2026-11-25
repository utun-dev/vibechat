/* Проверяем restart каталога и отказ старой схемы без удаления media_files/курсоров. */
#include "media_delivery_fixture.h"
#include "../src/routing_layer/topo_group.h"

#ifdef __linux__
enum sql_fault { SQL_NONE, SQL_PREPARE, SQL_BIND, SQL_STEP_FIRST, SQL_STEP_AFTER, SQL_FINALIZE };
static enum sql_fault sql_fault;
static sqlite3_stmt* fault_statement;
static unsigned fault_steps, fault_injected;
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __real_sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, sqlite3_destructor_type);
int __real_sqlite3_step(sqlite3_stmt*);
int __real_sqlite3_finalize(sqlite3_stmt*);

/* Ошибки внедряются только в внешний обход; вложенные SQL анонса остаются настоящими. */
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* command, int size, sqlite3_stmt** st, const char** tail) {
    static const char prefix[] = "SELECT id,media_id,block_id,chunk,chat_id FROM media_public_files";
    int target = sql_fault && !strncmp(command, prefix, sizeof(prefix) - 1);
    if (target && sql_fault == SQL_PREPARE) { *st = NULL; fault_injected++; return SQLITE_NOMEM; }
    int rc = __real_sqlite3_prepare_v2(db, command, size, st, tail);
    if (target && rc == SQLITE_OK) fault_statement = *st;
    return rc;
}
int __wrap_sqlite3_bind_blob(sqlite3_stmt* st, int column, const void* value, int size, sqlite3_destructor_type destroy) {
    if (st == fault_statement && sql_fault == SQL_BIND) { fault_injected++; return SQLITE_NOMEM; }
    return __real_sqlite3_bind_blob(st, column, value, size, destroy);
}
int __wrap_sqlite3_step(sqlite3_stmt* st) {
    if (st == fault_statement) {
        unsigned step = fault_steps++;
        if (sql_fault == SQL_STEP_FIRST || (sql_fault == SQL_STEP_AFTER && step == 1)) {
            fault_injected++; return SQLITE_IOERR;
        }
    }
    return __real_sqlite3_step(st);
}
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int target = st && st == fault_statement;
    if (target) fault_statement = NULL;
    int rc = __real_sqlite3_finalize(st);
    if (target && sql_fault == SQL_FINALIZE) { fault_injected++; return SQLITE_IOERR; }
    return rc;
}
#endif

static void sql(sqlite3* db, const char* command) {
    REQUIRE(sqlite3_exec(db, command, NULL, NULL, NULL) == SQLITE_OK);
}

static sqlite3_int64 scalar(sqlite3* db, const char* command) {
    sqlite3_stmt* st = NULL;
    REQUIRE(sqlite3_prepare_v2(db, command, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    sqlite3_int64 value = sqlite3_column_int64(st, 0); REQUIRE(sqlite3_finalize(st) == SQLITE_OK); return value;
}

#define ANN_MEDIA "x'0102030405060708090a0b0c0d0e0f10'"
#define ANN_BLOCK "x'2122232425262728292a2b2c2d2e2f30'"
#define ANN_BLOCK_SECOND "x'2122232425262728292a2b2c2d2e2f31'"

/* Данные нарочно добавляются в обход media_index writers: проверяем повреждённую, но допустимую схемой БД. */
static void insert_announcement(sqlite3* db, const char* media, const char* block, const char* chunk, const char* channel) {
    char command[1024];
    int n = snprintf(command, sizeof(command), "INSERT INTO media_files"
        "(media_id,block_id,content_hash,chat_id,location,holder,node_sign,timestamp,file_size,chunk_size,chunk,offset)"
        " VALUES(%s,%s,zeroblob(32),%s,'announcement.bin',fixture_holder(256),zeroblob(64),1,1,1,%s,0)",
        media ? media : ANN_MEDIA, block ? block : ANN_BLOCK, channel ? channel : "'" GROUP_KEY "'", chunk ? chunk : "0");
    REQUIRE(n > 0 && (size_t)n < sizeof(command)); sql(db, command);
}

/* Только удалённый CLIENT имеет роль supernode: refresh не открывает CM и не маскирует чтение плохих строк. */
static void add_announcement_group(struct UTUN_INSTANCE* inst, uint64_t id) {
    media_test_group(inst, id, NULL, 0);
    char command[512];
    snprintf(command, sizeof(command), "DELETE FROM peers_%s;"
             "INSERT INTO peers_%s(node_id,adm_tags) VALUES(fixture_node(256),'{}'),(fixture_node(512),'{\"supernode\":\"yes\"}')",
             media_test_channel(id, (char[65]){0}), media_test_channel(id, (char[65]){0}));
    sql(inst->topo_sqlite_db, command);
    media_test_member_keys(inst, id);
}

static void check_announcement(const struct packet* packet, uint64_t group, uint32_t chunk, uint8_t last_block) {
    REQUIRE(packet->from == SERVER && packet->to == CLIENT && packet->group == group && packet->len == MEDIA_HAVE_BLOCK_SIZE);
    const struct media_pkt_have_block* hb = (const struct media_pkt_have_block*)packet->data;
    REQUIRE(hb->subcmd == MEDIA_SUBCMD_HAVE_BLOCK && !memcmp(hb->group_ed, media_test_group_key(group, (uint8_t[32]){0}), 32) && hb->chunk == chunk && hb->timestamp == 1);
    for (unsigned i = 0; i < 16; i++) {
        REQUIRE(hb->media_id[i] == i + 1);
        REQUIRE(hb->block_id[i] == (i == 15 ? last_block : i + 0x21));
    }
}

/* Реальные сетевые обработчики: сырой индекс/READY и rollback не дают HAVE, собственного QUERY или выдачи блока. */
static void publication_visibility(struct UTUN_INSTANCE* inst, const struct media_index_result* r, int published) {
    reset(); capture_blocks = 1;
    media_delivery_announce_local_blocks(inst);
    media_delivery_announce_media(inst, GROUP, r->media_id, r->block_ids, 1);
    media_delivery_send_have_block(inst, GROUP, media_test_public(CLIENT, (uint8_t[32]){0}), r->media_id, r->block_ids, 0);
    media_delivery_send_processing(inst, GROUP, media_test_public(CLIENT, (uint8_t[32]){0}), r->media_id, r->block_ids, 0);
    REQUIRE((packet_count > 0) == published);
    REQUIRE(scalar(inst->md.db, "SELECT count(*) FROM block_availability WHERE holder=fixture_holder(256)") == published);
    reset(); capture_blocks = 1;
    struct packet query = {.from=CLIENT, .to=SERVER, .group=GROUP, .len=MEDIA_QUERY_HDR_SIZE+16};
    struct media_pkt_query* request = (void*)query.data; request->subcmd = MEDIA_SUBCMD_QUERY; request->num_blocks = 1;
    memcpy(request->media_id, r->media_id, 16); request->request_id[0] = 1;
    memcpy(query.data+MEDIA_QUERY_HDR_SIZE, r->block_ids, 16); receive(inst, &query);
    struct packet response = take_packet(); REQUIRE(!packet_count && response.data[0] == MEDIA_SUBCMD_QUERY_RESP);
    const struct media_pkt_query_resp_entry* row = (void*)(response.data+MEDIA_QUERY_RESP_HDR_SIZE);
    REQUIRE(row->num_holders == published && (!published || fixture_address(row->holders[0]) == SERVER));
    struct packet block = {.from=CLIENT, .to=SERVER, .group=GROUP, .len=MEDIA_BLOCK_REQ_SIZE};
    struct media_pkt_block_req* req = (void*)block.data; req->subcmd = MEDIA_SUBCMD_BLOCK_REQ; req->transfer_id[0] = 1;
    memcpy(req->media_id, r->media_id, 16); memcpy(req->block_id, r->block_ids, 16); receive(inst, &block);
    if (published) {
        REQUIRE(inst->md.active_streams == 1);
        for (int i = 0; i < 20 && inst->md.active_streams; i++) uasync_poll(ua, 1);
        REQUIRE(!inst->md.active_streams && packet_count == 2 && packets[0].data[0] == MEDIA_SUBCMD_BLOCK_CHUNK);
        REQUIRE(((const struct media_pkt_block_done*)packets[1].data)->status == MEDIA_BLOCK_OK);
    } else {
        REQUIRE(!inst->md.active_streams && packet_count == 1 && packets[0].data[0] == MEDIA_SUBCMD_BLOCK_DONE);
        REQUIRE(((const struct media_pkt_block_done*)packets[0].data)->status == MEDIA_BLOCK_UNAVAILABLE);
    }
    reset();
}
static int veto_publication(void* arg) { (void)arg; return 1; }
static void test_publication_barrier(void) {
    struct UTUN_INSTANCE* a = instance(SERVER); struct media_index_result r = result(1);
    add_announcement_group(a, GROUP);
    const char* temporary = getenv("TMPDIR"); REQUIRE(temporary);
    char file[1024]; snprintf(file, sizeof(file), "%s/publication.bin", temporary);
    FILE* out = fopen(file, "wb"); REQUIRE(out && fputc(0, out) == 0 && !fclose(out));
    REQUIRE(!media_index_commit(a->md.db, &r, a->my_ed25519_pubkey, author_key, GROUP_KEY, file, temporary));
    publication_visibility(a, &r, 0); /* Зазор между индексом и READY. */
    struct attachment_job job = {.id={1}, .namespace_token={1}, .owner=ATTACHMENT_OWNER_CHAT, .space=GROUP_KEY,
        .source=file, .request="intent", .request_size=6};
    struct attachment_object object = {.scope=ATTACHMENT_SCOPE_GROUP, .space=GROUP_KEY, .size=1,
        .manifest="transport-fixture", .manifest_size=17};
    struct attachment_ref ref = {.owner=ATTACHMENT_OWNER_CHAT, .space=GROUP_KEY, .message={1}};
    memcpy(job.author, author_key, 32); memcpy(object.author, author_key, 32);
    memcpy(object.id, r.media_id, 16); memcpy(object.hash, r.content_hash, 32); memcpy(ref.object, r.media_id, 16);
    sql(a->md.db, "BEGIN IMMEDIATE"); REQUIRE(!attachment_store_prepare_write(a->md.db));
    REQUIRE(!attachment_store_begin_write(a->md.db, &job) && !attachment_store_ready_write(a->md.db, job.id, &object, file));
    sql(a->md.db, "COMMIT"); publication_visibility(a, &r, 0);
    char destination[1024]; snprintf(destination, sizeof(destination), "%s/unpublished-download.bin", temporary);
    REQUIRE(media_download_start(a, GROUP, &r, destination, temporary, media_test_public(SERVER, (uint8_t[32]){0}), completed, NULL, NULL, NULL) == -1);
    REQUIRE(!done_count && !packet_count && !download(a));
    sql(a->md.db, "BEGIN IMMEDIATE"); REQUIRE(!attachment_store_publish_write(a->md.db, job.id, &ref));
    sqlite3_commit_hook(a->md.db, veto_publication, NULL);
    REQUIRE(sqlite3_exec(a->md.db, "COMMIT", NULL, NULL, NULL) == SQLITE_CONSTRAINT);
    sqlite3_commit_hook(a->md.db, NULL, NULL); publication_visibility(a, &r, 0);
    sql(a->md.db, "BEGIN IMMEDIATE"); REQUIRE(!attachment_store_publish_write(a->md.db, job.id, &ref)); sql(a->md.db, "COMMIT");
    publication_visibility(a, &r, 1);
    sql(a->md.db, "BEGIN IMMEDIATE"); REQUIRE(!attachment_store_release_write(a->md.db, &ref)); sql(a->md.db, "COMMIT");
    publication_visibility(a, &r, 1);
    REQUIRE(!remove(file)); destroy(a); media_index_result_free(&r);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "publication barrier: raw index/READY/commit veto hidden; committed/released parent visible to HAVE/QUERY/provider: PASS");
}

/* Проверяем не только отсутствие crash: каждый HAVE_BLOCK обязан точно соответствовать валидной исходной строке. */
static void test_announcements(void) {
    struct UTUN_INSTANCE* a = instance(SERVER);
    add_announcement_group(a, GROUP); add_announcement_group(a, 1); add_announcement_group(a, UINT64_MAX);
    struct media_index_result published = {.file_size=1};
    for (unsigned i = 0; i < 16; i++) published.media_id[i] = i + 1;
    media_test_publish(a->md.db, GROUP, SERVER, &published);
    media_test_publish(a->md.db, 1, SERVER, &published);
    media_test_publish(a->md.db, UINT64_MAX, SERVER, &published);
    char channel_one[68], channel_max[68];
    snprintf(channel_one, sizeof(channel_one), "'%s'", media_test_channel(1, (char[65]){0}));
    snprintf(channel_max, sizeof(channel_max), "'%s'", media_test_channel(UINT64_MAX, (char[65]){0}));
    const struct {
        const char *name, *media, *block, *chunk, *channel;
        uint64_t expected_group;
        uint32_t expected_chunk;
    } cases[] = {
        {"valid", NULL, NULL, NULL, NULL, GROUP, 0},
        {"media0", "zeroblob(0)", NULL}, {"media1", "zeroblob(1)", NULL},
        {"media15", "zeroblob(15)", NULL}, {"media17", "zeroblob(17)", NULL},
        {"media_text", "'1234567890123456'", NULL}, {"media_int", "1", NULL}, {"media_float", "1.5", NULL},
        {"block0", NULL, "zeroblob(0)"}, {"block1", NULL, "zeroblob(1)"},
        {"block15", NULL, "zeroblob(15)"}, {"block17", NULL, "zeroblob(17)"},
        {"block_text", NULL, "'1234567890123456'"}, {"block_int", NULL, "1"}, {"block_float", NULL, "1.5"},
        {"chunk_negative", NULL, NULL, "-1"}, {"chunk_overflow", NULL, NULL, "4294967296"},
        {"chunk_text", NULL, NULL, "'invalid'"}, {"chunk_float", NULL, NULL, "1.5"}, {"chunk_blob", NULL, NULL, "x'00'"},
        {"chunk_max", NULL, NULL, "4294967295", NULL, GROUP, UINT32_MAX},
        {"channel_empty", NULL, NULL, NULL, "''"}, {"channel_zero", NULL, NULL, NULL, "'0'"},
        {"channel_leading_zero", NULL, NULL, NULL, "'0111'"}, {"channel_suffix", NULL, NULL, NULL, "'111junk'"},
        {"channel_plus", NULL, NULL, NULL, "'+111'"}, {"channel_negative", NULL, NULL, NULL, "'-111'"},
        {"channel_left_space", NULL, NULL, NULL, "' 111'"}, {"channel_right_space", NULL, NULL, NULL, "'111 '"},
        {"channel_blob", NULL, NULL, NULL, "x'313131'"}, {"channel_float", NULL, NULL, NULL, "1.5"},
        {"channel_nul", NULL, NULL, NULL, "CAST(x'313131006a756e6b' AS TEXT)"},
        {"channel_overflow", NULL, NULL, NULL, "'18446744073709551616'"},
        {"channel_too_long", NULL, NULL, NULL, "'184467440737095516150'"},
        {"channel_one", NULL, NULL, NULL, channel_one, 1, 0},
        {"channel_max", NULL, NULL, NULL, channel_max, UINT64_MAX, 0},
    };
    unsigned valid = 0;
    debug_set_category_level(DEBUG_CATEGORY_MEDIA, DEBUG_LEVEL_WARN);
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        sql(a->md.db, "DELETE FROM media_files");
        insert_announcement(a->md.db, cases[i].media, cases[i].block, cases[i].chunk, cases[i].channel);
        reset(); capture_blocks = 1;
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "announcement case=%s expected_packets=%d", cases[i].name, cases[i].expected_group != 0);
        media_delivery_announce_local_blocks(a);
        REQUIRE(packet_count == (cases[i].expected_group ? 1 : 0));
        if (cases[i].expected_group) { check_announcement(&packets[0], cases[i].expected_group, cases[i].expected_chunk, 0x30); valid++; }
        REQUIRE(sqlite3_next_stmt(a->md.db, NULL) == NULL);
    }
    sql(a->md.db, "DELETE FROM media_files");
    insert_announcement(a->md.db, NULL, NULL, NULL, NULL);
    insert_announcement(a->md.db, "x'01'", "x'ff'", "1", NULL);
    insert_announcement(a->md.db, NULL, ANN_BLOCK_SECOND, "4294967295", channel_max);
    reset(); capture_blocks = 1;
    media_delivery_announce_local_blocks(a); REQUIRE(packet_count == 2 && packets[0].group != packets[1].group);
    for (int i = 0; i < packet_count; i++) {
        const struct media_pkt_have_block* hb = (const struct media_pkt_have_block*)packets[i].data;
        check_announcement(&packets[i], hb->chunk ? UINT64_MAX : GROUP, hb->chunk ? UINT32_MAX : 0, hb->chunk ? 0x31 : 0x30);
    }
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "announcement validation cases=%zu valid=%u mixed=2/3 PASS", sizeof(cases) / sizeof(cases[0]), valid);

#ifdef __linux__
    sql(a->md.db, "DELETE FROM media_files");
    insert_announcement(a->md.db, NULL, NULL, NULL, NULL);
    insert_announcement(a->md.db, NULL, ANN_BLOCK_SECOND, "1", NULL);
    for (enum sql_fault fault = SQL_PREPARE; fault <= SQL_FINALIZE; fault++) {
        const char* logfile = "test_media_announcement_fault.log";
        reset(); capture_blocks = 1; sql_fault = fault; fault_steps = fault_injected = 0;
        debug_set_category_level(DEBUG_CATEGORY_MEDIA, DEBUG_LEVEL_INFO);
        debug_enable_file_output(logfile, 1);
        media_delivery_announce_local_blocks(a);
        debug_disable_file_output(); debug_enable_console(1);
        REQUIRE(fault_injected == 1 && fault_statement == NULL);
        int expected = fault == SQL_FINALIZE ? 2 : fault == SQL_STEP_AFTER ? 1 : 0;
        REQUIRE(packet_count == expected && sqlite3_next_stmt(a->md.db, NULL) == NULL);
        FILE* file = fopen(logfile, "rb"); REQUIRE(file);
        char log[8192]; size_t n = fread(log, 1, sizeof(log) - 1, file); log[n] = 0;
        REQUIRE(!ferror(file) && fgetc(file) == EOF && !fclose(file) && !remove(logfile));
        REQUIRE(strstr(log, "[ERROR]") && !strstr(log, "local block scan complete"));
        REQUIRE(strstr(log, fault == SQL_PREPARE ? "announce_local_blocks prepare failed rc=" : "SQL phase=enumerate local blocks"));
        if (fault != SQL_PREPARE) REQUIRE(strstr(log, "local block scan incomplete"));
        sql_fault = SQL_NONE; reset(); capture_blocks = 1;
        media_delivery_announce_local_blocks(a); REQUIRE(packet_count == 2 && sqlite3_next_stmt(a->md.db, NULL) == NULL);
        unsigned seen = 0;
        for (int i = 0; i < packet_count; i++) {
            uint32_t chunk = ((const struct media_pkt_have_block*)packets[i].data)->chunk;
            REQUIRE(chunk < 2 && !(seen & (1u << chunk))); seen |= 1u << chunk;
            check_announcement(&packets[i], GROUP, chunk, chunk ? 0x31 : 0x30);
        }
        REQUIRE(seen == 3);
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "announcement SQL fault=%d injected=1 packets_before_error=%d recovery=2 PASS", fault, expected);
    }
#endif
    destroy(a);
    debug_set_category_level(DEBUG_CATEGORY_MEDIA, DEBUG_LEVEL_ERROR);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
    ua = uasync_create(); REQUIRE(ua);
    struct UTUN_INSTANCE* a = instance(SERVER);
    sql(a->md.db, "UPDATE peers_" GROUP_KEY " SET adm_tags='{\"supernode\":\"yes\"}' WHERE node_id=fixture_node(256)");
    uint8_t epoch[16]; memcpy(epoch, a->md.source_epoch, 16);
    sql(a->md.db, "INSERT INTO block_availability(block_uuid,group_id,holder,chunk,timestamp,status,version,signature)"
        " VALUES(zeroblob(16),'" GROUP_KEY "',fixture_holder(256),0,1,1,1,zeroblob(64)),(zeroblob(16),'" GROUP2_KEY "',fixture_holder(256),0,1,1,1,zeroblob(64));"
        "INSERT INTO super_sync VALUES('" GROUP_KEY "',fixture_holder(512),zeroblob(16),4294967297),('" GROUP2_KEY "',fixture_holder(512),zeroblob(16),8)");
    REQUIRE(scalar(a->md.db, "SELECT COUNT(*) FROM block_availability") == 2);
    REQUIRE(scalar(a->md.db, "SELECT COUNT(*) FROM super_sync") == 2);
    media_delivery_destroy(a);
    REQUIRE(!media_delivery_init(a) && !memcmp(epoch, a->md.source_epoch, 16));
    REQUIRE(scalar(a->md.db, "SELECT last_recv_id FROM super_sync WHERE group_id='" GROUP_KEY "'") == 4294967297LL);
    REQUIRE(scalar(a->md.db, "SELECT COUNT(*) FROM block_availability") == 2);

    /* Некорректная версия каталога останавливает сервис без удаления записей. */
    REQUIRE(!media_index_set_base(a->md.db, "/tmp"));
    uint8_t media[16] = {1}, block[16] = {2}, hash[32] = {0};
    REQUIRE(!media_index_register_downloaded(a->md.db, media, block, hash, GROUP_KEY, "media-repl-preserved.bin",
        a->my_ed25519_pubkey, 1, 1, 0, 0));
    REQUIRE(scalar(a->md.db, "SELECT COUNT(*) FROM media_files") == 1);
    media_delivery_destroy(a);
    sql(a->topo_sqlite_db, "UPDATE media_replication_meta SET version=1");
    REQUIRE(media_delivery_init(a) < 0);
    REQUIRE(scalar(a->md.db, "SELECT COUNT(*) FROM super_sync") == 2);
    REQUIRE(scalar(a->md.db, "SELECT COUNT(*) FROM block_availability") == 2);
    REQUIRE(scalar(a->md.db, "SELECT COUNT(*) FROM media_files") == 1);
    sql(a->topo_sqlite_db, "UPDATE media_replication_meta SET version=3");
    REQUIRE(!media_delivery_init(a) && !memcmp(epoch, a->md.source_epoch, 16));
    struct media_index_result published = {.file_size=1}; memcpy(published.media_id, media, 16);
    media_test_publish(a->md.db, GROUP, SERVER, &published);
    media_delivery_announce_media(a, GROUP, media, block, 1);
    REQUIRE(scalar(a->md.db, "SELECT COUNT(*) FROM block_availability") == 3);
    sqlite3_int64 high = scalar(a->md.db, "SELECT seq FROM sqlite_sequence WHERE name='block_availability'");
    media_delivery_announce_media(a, GROUP, media, block, 1);
    REQUIRE(scalar(a->md.db, "SELECT seq FROM sqlite_sequence WHERE name='block_availability'") == high);

    /* Старая схема и её курсоры сохраняются при отказе; новая meta не остаётся. */
    media_delivery_destroy(a);
    sql(a->topo_sqlite_db, "DROP TABLE media_replication_meta; DROP TABLE block_availability; DROP TABLE super_sync;"
        "CREATE TABLE block_availability(id INTEGER PRIMARY KEY,block_uuid BLOB);"
        "CREATE TABLE super_sync(peer_node_id INTEGER PRIMARY KEY,last_recv_id INTEGER);"
        "INSERT INTO super_sync VALUES(fixture_node(512),99)");
    REQUIRE(media_delivery_init(a) < 0);
    REQUIRE(scalar(a->md.db, "SELECT last_recv_id FROM super_sync WHERE peer_node_id=fixture_node(512)") == 99);
    REQUIRE(scalar(a->md.db, "SELECT COUNT(*) FROM media_files") == 1);
    REQUIRE(!scalar(a->md.db, "SELECT COUNT(*) FROM sqlite_master WHERE name='media_replication_meta'"));
    destroy(a); test_announcements(); test_publication_barrier(); uasync_destroy(ua, 0);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "media catalog SQL restart/rejection/preservation: PASS");
    return 0;
}
