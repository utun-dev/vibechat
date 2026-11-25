#include "media_delivery_fixture.h"

static void test_bounds_and_identity(void) {
    reset(); struct UTUN_INSTANCE* s = instance(SERVER); struct UTUN_INSTANCE* c = instance(CLIENT);
    struct media_index_result r = result(2); seed(s, &r, 2, 2, 0); start(c, &r);
    struct packet q = packets[0]; packet_count = 0;
    for (size_t n = 0; n < q.len; n++) { struct packet bad = q; bad.len = n; receive(s, &bad); REQUIRE(!packet_count); }
    struct packet bad = q; ((struct media_pkt_query*)bad.data)->num_blocks = MEDIA_QUERY_MAX_BLOCKS + 1;
    receive(s, &bad); REQUIRE(!packet_count);
    bad = q; bad.len++; receive(s, &bad); REQUIRE(!packet_count);
    receive(s, &q); struct packet good = take_packet();
    struct media_download snapshot = *download(c);
    struct media_download_block blocks[2]; memcpy(blocks, download(c)->blocks, sizeof(blocks));
    // Нулевой payload — локальное уведомление router о закрытии, а не QUERY_RESP.
    for (size_t n = 1; n < good.len; n++) {
        bad = good; bad.len = n; receive(c, &bad);
        REQUIRE(!memcmp(&snapshot, download(c), sizeof(snapshot)) && !memcmp(blocks, download(c)->blocks, sizeof(blocks)));
    }
    for (int mutation = 0; mutation < 12; mutation++) {
        bad = good;
        struct media_pkt_query_resp* header = (struct media_pkt_query_resp*)bad.data;
        struct media_pkt_query_resp_entry* e = (struct media_pkt_query_resp_entry*)(bad.data + MEDIA_QUERY_RESP_HDR_SIZE);
        switch (mutation) {
        case 0: bad.from++; break;
        case 1: bad.group++; break;
        case 2: header->media_id[0]++; break;
        case 3: header->request_id[0]++; break;
        case 4: header->num_blocks = 1; bad.len -= sizeof(*e); break;
        case 5: e[1].block_id[0]++; break; // Ошибка последней записи не меняет первую.
        case 6: memcpy(e[1].holders[1], e[1].holders[0], 32); break;
        case 7: media_test_public(CLIENT, e[1].holders[0]); break;
        case 8: e[1].num_holders = 5; break;
        case 9: header->status = MEDIA_QUERY_DB_ERROR; break;
        case 10: e[1].holders[2][0] = 1; break;
        case 11: bad.len++; break;
        }
        receive(c, &bad);
        REQUIRE(!memcmp(&snapshot, download(c), sizeof(snapshot)) && !memcmp(blocks, download(c)->blocks, sizeof(blocks)));
    }
    receive(c, &good); REQUIRE(download(c)->inflight == 2 && download(c)->blocks[1].num_holders == 2);
    snapshot = *download(c); memcpy(blocks, download(c)->blocks, sizeof(blocks));
    receive(c, &good); REQUIRE(!memcmp(&snapshot, download(c), sizeof(snapshot)) && !memcmp(blocks, download(c)->blocks, sizeof(blocks)));
    destroy(c); destroy(s); media_index_result_free(&r); REQUIRE(done_count == 1 && done_err == -2);
    puts("QUERY bounds, atomic validation, identity and duplicate: PASS");
}

static void test_paging(void) {
    reset(); struct UTUN_INSTANCE* s = instance(SERVER); struct UTUN_INSTANCE* c = instance(CLIENT);
    struct media_index_result r = result(65); seed(s, &r, 65, 6, 0); start(c, &r);
    int covered = 0, replies = 0;
    while (packet_count) {
        struct packet p = answer(s);
        struct media_pkt_query_resp* header = (struct media_pkt_query_resp*)p.data;
        REQUIRE(header->status == MEDIA_QUERY_OK && p.len == MEDIA_QUERY_RESP_HDR_SIZE + header->num_blocks * sizeof(struct media_pkt_query_resp_entry) && header->num_blocks > 0);
        covered += header->num_blocks; replies++;
        receive(c, &p);
    }
    REQUIRE(covered == 65 && replies == 3 && query_count == 3 && !done_count);
    struct media_download* dl = download(c); REQUIRE(dl && dl->inflight == 65);
    for (int i = 0; i < 65; i++) REQUIRE(dl->blocks[i].num_holders == 4 && dl->blocks[i].state == MD_BLK_REQ);
    destroy(c); destroy(s); media_index_result_free(&r); REQUIRE(done_count == 1);
    puts("65 blocks, 260 holder records, complete bounded batches: PASS");
}

static void local_cancelled(void* arg, int status) {
    REQUIRE(status == -2); (*(int*)arg)++;
}

static void test_local_catalog(void) {
    reset();
    struct UTUN_INSTANCE* client = instance(CLIENT);
    REQUIRE(!sqlite3_exec(client->topo_sqlite_db, "UPDATE peers_" GROUP_KEY
        " SET adm_tags='{\"supernode\":\"yes\"}' WHERE node_id=fixture_node(512)", NULL, NULL, NULL));
    struct media_index_result file = result(2);
    seed(client, &file, 2, 2, 0);
    start(client, &file);
    struct media_download* dl = download(client);
    REQUIRE(dl && !query_count && dl->blocks[0].state == MD_BLK_REQ && dl->blocks[1].state == MD_BLK_REQ);
    REQUIRE(dl->blocks[0].num_holders == 2 && dl->blocks[1].num_holders == 2);
    REQUIRE(fixture_address(dl->blocks[0].holders[dl->blocks[0].holder_idx]) != SERVER);
    int cancelled = 0;
    REQUIRE(!media_download_start(client, GROUP, &file, dl->dest_path, directory, dl->author_pubkey, local_cancelled, &cancelled, NULL, NULL));
    REQUIRE(!media_download_unsubscribe(client, GROUP, file.media_id, &cancelled) && cancelled == 1 && download(client) == dl);
    deliver_block(client, dl, 0);
    deliver_block(client, dl, 1);
    REQUIRE(done_count == 1 && !done_err && !query_count);
    destroy(client); media_index_result_free(&file);
    char path[512]; snprintf(path, sizeof(path), "%s/result.bin", directory); REQUIRE(!remove(path));
    puts("local supernode catalog supplies blocks after one shared request cancels, without remote QUERY or author: PASS");
}

static void test_holder_selection(void) {
    reset(); struct UTUN_INSTANCE* s = instance(SERVER); struct UTUN_INSTANCE* c = instance(CLIENT);
    struct media_index_result r = result(2); seed(s, &r, 2, 6, 0);
    /* Нулевой держатель запрещён схемой; для защиты QUERY от повреждённой БД отключаем CHECK только в стенде. */
    REQUIRE(sqlite3_exec(s->topo_sqlite_db, "PRAGMA ignore_check_constraints=ON", NULL, NULL, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_exec(s->topo_sqlite_db,
        "UPDATE block_availability SET group_id='7000000000000000000000000000000000000000000000000000000000000001' WHERE holder=fixture_holder(1000);"
        "INSERT INTO block_availability(block_uuid,group_id,holder,chunk,timestamp,status,version,signature)"
        " SELECT block_uuid,'" GROUP_KEY "',fixture_holder(512),chunk,100,1,version,signature FROM block_availability WHERE holder=fixture_holder(1001);"
        "INSERT INTO block_availability(block_uuid,group_id,holder,chunk,timestamp,status,version,signature)"
        " SELECT block_uuid,'" GROUP_KEY "',zeroblob(32),chunk,100,1,version,signature FROM block_availability WHERE holder=fixture_holder(1001);",
        NULL, NULL, NULL) == SQLITE_OK);
    REQUIRE(!media_index_register_downloaded(s->topo_sqlite_db, r.media_id, r.block_ids, r.content_hash,
        GROUP_KEY, "own.bin", s->my_ed25519_pubkey, 2, 1, 0, 0));
    media_test_publish(s->topo_sqlite_db, GROUP, SERVER, &r);
    start(c, &r); struct packet p = answer(s);
    struct media_pkt_query_resp_entry* entries = (struct media_pkt_query_resp_entry*)(p.data + MEDIA_QUERY_RESP_HDR_SIZE);
    REQUIRE(entries[0].num_holders == 4 && fixture_address(entries[0].holders[0]) == SERVER && entries[1].num_holders == 4);
    for (int i = 0; i < 4; i++) {
        uint64_t holder = fixture_address(entries[1].holders[i]); int known = 0;
        for (int slot = 1001; slot <= 1005; slot++) if (holder == media_fixture_id(slot)) known = 1;
        REQUIRE(known);
        if (i) REQUIRE(memcmp(entries[1].holders[i - 1], entries[1].holders[i], 32) < 0);
        if (i < 3) REQUIRE(!memcmp(entries[0].holders[i + 1], entries[1].holders[i], 32));
    }
    destroy(c); destroy(s); media_index_result_free(&r); REQUIRE(done_count == 1);
    puts("holder selection: own file, group scope, requester/zero exclusions before LIMIT: PASS");
}

/* Нулевые результаты ищутся дальше, в то время как найденные блоки уже скачиваются. */
static void test_partial_and_timeout(void) {
    reset(); struct UTUN_INSTANCE* s = instance(SERVER); struct UTUN_INSTANCE* c = instance(CLIENT);
    struct media_index_result r = result(2); seed(s, &r, 1, 1, 0); start(c, &r);
    struct media_download* dl = download(c);
    media_test_public(media_fixture_id(257), dl->super_nodes[1]); media_test_public(media_fixture_id(258), dl->super_nodes[2]); dl->super_count = 3;
    struct packet p = answer(s); receive(c, &p);
    REQUIRE(dl->blocks[0].state == MD_BLK_REQ && dl->blocks[1].num_holders == 0);
    REQUIRE(fixture_address(dl->query_peer) == media_fixture_id(257) && dl->query_count == 1 && dl->query_blocks[0] == 1);
    REQUIRE(fixture_address(dl->announce_peer) == SERVER && dl->inflight == 1 && !dl->blocks[1].author_tried);
    uint64_t deadline = dl->query_deadline_tb;
    dl->blocks[0].last_progress_tb = deadline - 1;
    REQUIRE(!md_dl_check_stall(dl, deadline));
    REQUIRE(fixture_address(dl->query_peer) == media_fixture_id(258) && dl->query_deadline_tb > deadline && dl->inflight == 1);
    REQUIRE(dl->blocks[0].attempts == 0 && fixture_address(dl->announce_peer) == SERVER);
    destroy(c); destroy(s); media_index_result_free(&r); REQUIRE(done_count == 1);
    puts("partial results and QUERY timeout during another block's progress: PASS");
}


static void test_peer_reuse_and_completion(void) {
    reset(); struct UTUN_INSTANCE* s = instance(SERVER); struct UTUN_INSTANCE* c = instance(CLIENT);
    struct media_index_result r = result(65); seed(s, &r, 65, 1, 13); start(c, &r);
    while (packet_count) { struct packet p = answer(s); receive(c, &p); }
    REQUIRE(download(c)->num_peers == MD_MAX_PEERS && download(c)->blocks[10].wait_reason == MD_WAIT_SLOT);
    int received = 0;
    while (download(c)) {
        struct media_download* dl = download(c); int bi = -1, inflight = 0;
        REQUIRE(dl->num_peers <= MD_MAX_PEERS && !done_count);
        for (int i = 0; i < dl->num_blocks; i++) {
            if (dl->blocks[i].state == MD_BLK_REQ) { bi = i; inflight++; }
            REQUIRE(dl->blocks[i].attempts == 0);
        }
        REQUIRE(bi >= 0 && inflight == dl->inflight && received < 65);
        deliver_block(c, dl, bi); received++;
    }
    REQUIRE(received == 65 && done_count == 1 && !done_err);
    char path[512]; snprintf(path, sizeof(path), "%s/result.bin", directory);
    FILE* f = fopen(path, "rb"); REQUIRE(f);
    for (int i = 0; i < 65; i++) REQUIRE(fgetc(f) == i);
    REQUIRE(fgetc(f) == EOF && !fclose(f));
    sqlite3_stmt* st = NULL;
    REQUIRE(sqlite3_prepare_v2(c->topo_sqlite_db, "SELECT COUNT(*) FROM media_files", -1, &st, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 65); sqlite3_finalize(st);
    destroy(c); destroy(s); media_index_result_free(&r); REQUIRE(done_count == 1); remove(path);
    puts("65 blocks from 13 holders, peer slot reuse, file contents and registration: PASS");
}

/* Размер файла больше прежнего предела одного QUERY; в транспорт всё равно уходит только 32 ID. */
static void test_large_file_and_live_deadline(void) {
    reset(); struct UTUN_INSTANCE* c = instance(CLIENT);
    struct media_index_result large = result(4100); start(c, &large);
    REQUIRE(download(c) && download(c)->num_blocks == 4100 && download(c)->query_count == 32);
    REQUIRE(packet_count == 1 && packets[0].len == MEDIA_QUERY_HDR_SIZE + 32 * 16);
    destroy(c); media_index_result_free(&large); REQUIRE(done_count == 1);

    reset(); struct UTUN_INSTANCE* s = instance(SERVER); c = instance(CLIENT);
    c->md.query_timeout_tb = 1000; c->md.dl_stall_timeout_tb = 10000;
    struct media_index_result r = result(2); seed(s, &r, 1, 1, 0); start(c, &r);
    struct media_download* dl = download(c); media_test_public(media_fixture_id(257), dl->super_nodes[1]); dl->super_count = 2;
    struct packet p = answer(s); receive(c, &p);
    REQUIRE(fixture_address(dl->query_peer) == media_fixture_id(257) && dl->query_deadline_tb);
    struct packet chunk = {.from = media_fixture_id(1000), .to = CLIENT, .group = GROUP, .len = MEDIA_BLOCK_CHUNK_HDR_SIZE + 1};
    struct media_pkt_block_chunk* ch = (struct media_pkt_block_chunk*)chunk.data;
    ch->subcmd = MEDIA_SUBCMD_BLOCK_CHUNK; ch->data_len = 1;
    memcpy(ch->media_id, r.media_id, 16); memcpy(ch->block_id, r.block_ids, 16);
    memcpy(ch->transfer_id, dl->blocks[0].transfer_id, 16);
    uint64_t stop = get_time_tb() + 5000;
    while (dl->query_deadline_tb && get_time_tb() < stop) {
        receive(c, &chunk); uasync_poll(ua, 5); REQUIRE(download(c) == dl && !done_count);
    }
    REQUIRE(!dl->query_deadline_tb && dl->blocks[0].state == MD_BLK_RECV && !dl->blocks[0].attempts);
    REQUIRE(dl->blocks[1].state == MD_BLK_REQ && fixture_address(dl->blocks[1].holders[0]) == SERVER);
    destroy(c); destroy(s); media_index_result_free(&r); uasync_poll(ua, 20); REQUIRE(done_count == 1);
    puts("4100-block start and real QUERY watchdog during incoming chunks: PASS");
}

static int denied_reads, started_selects;
static int trace_query(unsigned event, void* arg, void* statement, void* sql) {
    (void)arg; (void)sql;
    if (event == SQLITE_TRACE_STMT && !strncmp(sqlite3_sql(statement), "SELECT holder FROM (", sizeof("SELECT holder FROM (") - 1))
        started_selects++;
    return 0;
}
static int deny_second_block(void* arg, int action, const char* table, const char* column, const char* db, const char* trigger) {
    (void)arg; (void)db; (void)trigger;
    if (action == SQLITE_READ && table && column && !strcmp(table, "block_availability") && !strcmp(column, "block_uuid"))
        if (started_selects) { denied_reads++; return SQLITE_DENY; }
    return SQLITE_OK;
}

static void test_failures_and_restart(void) {
    reset(); struct UTUN_INSTANCE* s = instance(SERVER); struct UTUN_INSTANCE* c = instance(CLIENT);
    struct media_index_result r = result(2); seed(s, &r, 2, 2, 0); start(c, &r);
    denied_reads = started_selects = 0;
    sqlite3_trace_v2(s->topo_sqlite_db, SQLITE_TRACE_STMT, trace_query, NULL);
    sqlite3_set_authorizer(s->topo_sqlite_db, deny_second_block, NULL);
    struct packet error = answer(s);
    struct media_pkt_query_resp* header = (struct media_pkt_query_resp*)error.data;
    REQUIRE(denied_reads == 1 && started_selects == 1 && header->status == MEDIA_QUERY_DB_ERROR
        && !header->num_blocks && error.len == MEDIA_QUERY_RESP_HDR_SIZE);
    sqlite3_set_authorizer(s->topo_sqlite_db, NULL, NULL);
    sqlite3_trace_v2(s->topo_sqlite_db, 0, NULL, NULL);
    struct media_download* dl = download(c);
    REQUIRE(!media_download_cancel(c, dl->media_id, NULL, 0) && done_count == 1);
    start(c, &r); dl = download(c); struct media_download snapshot = *dl;
    receive(c, &error); REQUIRE(!memcmp(&snapshot, dl, sizeof(snapshot)) && done_count == 1);
    fail_send = MEDIA_SUBCMD_QUERY_RESP;
    struct packet q = take_packet(); receive(s, &q); REQUIRE(!packet_count); fail_send = 0;
    REQUIRE(!media_download_cancel(c, dl->media_id, NULL, 0) && done_count == 2);
    fail_rng = 1; start(c, &r); fail_rng = 0;
    REQUIRE(!download(c) && done_count == 3 && done_err == -1 && !packet_count);
    fail_send = MEDIA_SUBCMD_QUERY; start(c, &r); fail_send = 0;
    dl = download(c); REQUIRE(dl && !dl->query_deadline_tb && dl->inflight == 2);
    destroy(c); destroy(s); media_index_result_free(&r); REQUIRE(done_count == 4);
    puts("SQL error after partial lookup, cancel/restart, stale response, RNG and send ownership: PASS");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    if (getenv("UTUN_TEST_DEBUG")) debug_set_category_level(DEBUG_CATEGORY_MEDIA, DEBUG_LEVEL_DEBUG);
    ua = uasync_create(); REQUIRE(ua);
    snprintf(directory, sizeof(directory), "/tmp/utun-query-XXXXXX"); REQUIRE(!test_mkdtemp(directory));
    test_bounds_and_identity(); test_paging(); test_local_catalog(); test_holder_selection(); test_partial_and_timeout();
    test_peer_reuse_and_completion(); test_large_file_and_live_deadline(); test_failures_and_restart();
    char path[512]; snprintf(path, sizeof(path), "%s/result.bin", directory); remove(path);
    for (int i = 0; i < 65; i++) { snprintf(path, sizeof(path), "%s/result.bin.chunk_%d", directory, i); remove(path); }
    REQUIRE(!rmdir(directory)); uasync_destroy(ua, 0);
    puts("media QUERY regressions: PASS"); return 0;
}
