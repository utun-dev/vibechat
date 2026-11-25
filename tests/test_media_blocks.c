/* Приём и relay: реальные подписи/файлы/SQLite/очереди router, ошибки I/O и повторный вход из callbacks. */
#include "media_delivery_fixture.h"
#include "../src/media_async/media_async.h"
#include <errno.h>

enum io_fault { IO_NONE, IO_WRITE, IO_READ, IO_SEEK, IO_CLOSE, IO_FLUSH, IO_RENAME, IO_HASH };
static enum io_fault fault;
static int fault_skip, fault_hits;

static int fail_io(enum io_fault operation) {
    if (fault != operation) return 0;
    if (fault_skip > 0) { fault_skip--; return 0; }
    fault = IO_NONE; fault_hits++; errno = ENOSPC; return 1;
}

size_t __real_fwrite(const void*, size_t, size_t, FILE*);
size_t __wrap_fwrite(const void* bytes, size_t size, size_t count, FILE* file) {
    if (file != stdout && file != stderr && fail_io(IO_WRITE)) return 0;
    return __real_fwrite(bytes, size, count, file);
}
size_t __real_fread(void*, size_t, size_t, FILE*);
size_t __wrap_fread(void* bytes, size_t size, size_t count, FILE* file) {
    if (fail_io(IO_READ)) return 0;
    return __real_fread(bytes, size, count, file);
}
#ifdef __GLIBC__
size_t __real___fread_chk(void*, size_t, size_t, size_t, FILE*);
size_t __wrap___fread_chk(void* bytes, size_t capacity, size_t size, size_t count, FILE* file) {
    if (fail_io(IO_READ)) return 0;
    return __real___fread_chk(bytes, capacity, size, count, file);
}
#endif
int __real_fseeko(FILE*, off_t, int);
int __wrap_fseeko(FILE* file, off_t offset, int origin) {
    return fail_io(IO_SEEK) ? -1 : __real_fseeko(file, offset, origin);
}
int __real_fclose(FILE*);
int __wrap_fclose(FILE* file) {
    int failed = fail_io(IO_CLOSE), rc = __real_fclose(file);
    return failed ? EOF : rc; // Дескриптор закрывается и при моделируемой ошибке.
}
int __real_fflush(FILE*);
int __wrap_fflush(FILE* file) {
    return file && file != stdout && file != stderr && fail_io(IO_FLUSH) ? EOF : __real_fflush(file);
}
int __real_rename(const char*, const char*);
int __wrap_rename(const char* source, const char* destination) {
    return fail_io(IO_RENAME) ? -1 : __real_rename(source, destination);
}
int __real_ma_sha256_file(const char*, uint8_t[32]);
int __wrap_ma_sha256_file(const char* path, uint8_t hash[32]) {
    return fail_io(IO_HASH) ? -1 : __real_ma_sha256_file(path, hash);
}

struct fixture { struct UTUN_INSTANCE *server, *client; struct media_index_result metadata; };

static struct fixture ready(unsigned size) {
    reset(); fault = IO_NONE; fault_skip = fault_hits = 0;
    struct fixture f = {.server = instance(SERVER), .client = instance(CLIENT), .metadata = result(1)};
    f.metadata.file_size = f.metadata.block_size = size;
    uint8_t* bytes = u_malloc(size); REQUIRE(bytes);
    for (unsigned i = 0; i < size; i++) bytes[i] = (uint8_t)i;
    unsigned length;
    REQUIRE(sc_ed25519_sign(author_key, bytes, size, f.metadata.block_sigs) == SC_OK);
    REQUIRE(EVP_Digest(bytes, size, f.metadata.content_hash, &length, EVP_sha256(), NULL) == 1 && length == 32);
    u_free(bytes);
    seed(f.server, &f.metadata, 1, 2, 0); start(f.client, &f.metadata);
    struct packet response = answer(f.server); receive(f.client, &response);
    REQUIRE(download(f.client) && download(f.client)->blocks[0].state == MD_BLK_REQ && !packet_count);
    return f;
}

static void finish(struct fixture* f) {
    fault = IO_NONE; sent_hook = NULL; capture_blocks = 0;
    destroy(f->client); destroy(f->server); media_index_result_free(&f->metadata);
    char path[512]; snprintf(path, sizeof(path), "%s/result.bin", directory); remove(path);
}

static struct packet chunk_packet(struct UTUN_INSTANCE* inst, uint32_t offset, uint16_t length) {
    struct media_download* dl = download(inst); REQUIRE(dl);
    struct media_download_block* b = &dl->blocks[0];
    struct packet p = {.from = fixture_address(b->holders[b->holder_idx]), .to = inst->node_id,
        .group = dl->group_id, .len = MEDIA_BLOCK_CHUNK_HDR_SIZE + length};
    REQUIRE(p.len <= sizeof(p.data));
    struct media_pkt_block_chunk* ch = (struct media_pkt_block_chunk*)p.data;
    ch->subcmd = MEDIA_SUBCMD_BLOCK_CHUNK; ch->offset = offset; ch->data_len = length;
    memcpy(ch->media_id, dl->media_id, 16); memcpy(ch->block_id, b->block_id, 16); memcpy(ch->transfer_id, b->transfer_id, 16);
    for (unsigned i = 0; i < length; i++) p.data[sizeof(*ch) + i] = (uint8_t)(offset + i);
    return p;
}

static struct packet done_packet(struct UTUN_INSTANCE* inst) {
    struct media_download* dl = download(inst); REQUIRE(dl);
    struct media_download_block* b = &dl->blocks[0];
    struct packet p = {.from = fixture_address(b->holders[b->holder_idx]), .to = inst->node_id, .group = dl->group_id, .len = MEDIA_BLOCK_DONE_SIZE};
    struct media_pkt_block_done* done = (struct media_pkt_block_done*)p.data;
    done->subcmd = MEDIA_SUBCMD_BLOCK_DONE; done->total_size = b->expected_size;
    memcpy(done->media_id, dl->media_id, 16); memcpy(done->block_id, b->block_id, 16); memcpy(done->transfer_id, b->transfer_id, 16);
    return p;
}

static void unchanged(struct UTUN_INSTANCE* inst, const struct packet* packet) {
    struct media_download* dl = download(inst), snapshot = *dl;
    struct media_download_block block = dl->blocks[0];
    receive(inst, packet);
    REQUIRE(download(inst) == dl && !memcmp(&snapshot, dl, sizeof(snapshot)) && !memcmp(&block, dl->blocks, sizeof(block)));
}

static void test_identity_and_bounds(void) {
    struct fixture f = ready(4);
    struct packet good = chunk_packet(f.client, 0, 2);
    for (int variant = 0; variant < 6; variant++) {
        struct packet bad = good;
        struct media_pkt_block_chunk* ch = (struct media_pkt_block_chunk*)bad.data;
        switch (variant) {
        case 0: bad.group++; break;
        case 1: bad.from++; break;
        case 2: ch->media_id[0]++; break;
        case 3: ch->block_id[0]++; break;
        case 4: ch->transfer_id[0]++; break;
        case 5: ch->chunk = UINT32_MAX; break;
        }
        unchanged(f.client, &bad);
    }
    // Нулевой payload зарезервирован router для уведомления о закрытии.
    for (size_t length = 1; length < MEDIA_BLOCK_CHUNK_HDR_SIZE; length++) {
        struct packet bad = good; bad.len = length; unchanged(f.client, &bad);
    }
    receive(f.client, &good); unchanged(f.client, &good);
    REQUIRE(download(f.client)->blocks[0].source->available == 2);
    struct packet done = done_packet(f.client);
    for (size_t length = 1; length < done.len; length++) {
        struct packet bad = done; bad.len = length; unchanged(f.client, &bad);
    }
    for (int variant = 0; variant < 6; variant++) {
        struct packet bad = done;
        struct media_pkt_block_done* d = (struct media_pkt_block_done*)bad.data;
        switch (variant) {
        case 0: bad.group++; break;
        case 1: bad.from++; break;
        case 2: d->media_id[0]++; break;
        case 3: d->block_id[0]++; break;
        case 4: d->transfer_id[0]++; break;
        case 5: d->chunk++; break;
        }
        unchanged(f.client, &bad);
    }
    struct packet stale = {.from = good.from, .group = GROUP, .len = sizeof(struct media_pkt_block_overloaded)};
    struct media_pkt_block_overloaded* ov = (struct media_pkt_block_overloaded*)stale.data;
    ov->subcmd = MEDIA_SUBCMD_BLOCK_OVERLOADED; memcpy(ov->media_id, f.metadata.media_id, 16);
    memcpy(ov->block_id, f.metadata.block_ids, 16); memset(ov->transfer_id, 0xFF, 16); unchanged(f.client, &stale);
    struct packet redirect = {.from = good.from, .group = GROUP, .len = MEDIA_RELAY_FULL_HDR_SIZE + 32};
    struct media_pkt_block_relay_full* full = (struct media_pkt_block_relay_full*)redirect.data;
    full->subcmd = MEDIA_SUBCMD_BLOCK_RELAY_FULL; full->num_relay_nodes = 1;
    memcpy(full->media_id, f.metadata.media_id, 16); memcpy(full->block_id, f.metadata.block_ids, 16);
    memcpy(full->transfer_id, download(f.client)->blocks[0].transfer_id, 16);
    media_test_public(media_fixture_id(0x300), redirect.data + sizeof(*full));
    for (size_t length = 1; length < redirect.len; length++) {
        struct packet bad = redirect; bad.len = length; unchanged(f.client, &bad);
    }
    stale = redirect; stale.len++; unchanged(f.client, &stale);
    stale = redirect; ((struct media_pkt_block_relay_full*)stale.data)->num_relay_nodes = UINT16_MAX;
    unchanged(f.client, &stale);
    stale = redirect; ((struct media_pkt_block_relay_full*)stale.data)->transfer_id[0]++; unchanged(f.client, &stale);
    finish(&f); REQUIRE(done_count == 1 && done_err == -2);
    puts("block identity, exact input bounds and duplicate without progress: PASS");
}

static void test_rejected_attempts(void) {
    for (int variant = 0; variant < 12; variant++) {
        struct fixture f = ready(4);
        struct media_download* dl = download(f.client);
        char previous[FILE_IO_PATH_MAX]; snprintf(previous, sizeof(previous), "%s", dl->blocks[0].source->storage.temporary);
        struct packet old_chunk = chunk_packet(f.client, 0, 2), old_done = done_packet(f.client);
        struct packet bad = chunk_packet(f.client, 0, 1);
        struct media_pkt_block_chunk* ch = (struct media_pkt_block_chunk*)bad.data;
        switch (variant) {
        case 0: bad.len--; break;
        case 1: bad.len++; break;
        case 2: ch->data_len = 0; bad.len = sizeof(*ch); break;
        case 3: ch->data_len = MEDIA_BLOCK_DATA_MAX + 1; bad.len = sizeof(*ch) + ch->data_len; break;
        case 4: ch->offset = 4; break;
        case 5: ch->offset = UINT32_MAX; break;
        case 6: ch->offset = 1; break;
        case 7: receive(f.client, &old_chunk); bad = chunk_packet(f.client, 1, 2); break;
        default: {
            if (variant != 8) {
                struct packet full = chunk_packet(f.client, 0, 4);
                if (variant == 11) full.data[MEDIA_BLOCK_CHUNK_HDR_SIZE] ^= 1;
                receive(f.client, &full);
            }
            bad = done_packet(f.client);
            struct media_pkt_block_done* d = (struct media_pkt_block_done*)bad.data;
            if (variant == 9) d->total_size--;
            if (variant == 10) d->status = 17;
        }
        }
        receive(f.client, &bad);
        dl = download(f.client); REQUIRE(dl && !done_count);
        struct media_download_block* b = &dl->blocks[0];
        REQUIRE(b->attempts == 1 && b->holder_idx == 1 && b->state == MD_BLK_REQ && !b->bytes_received);
        REQUIRE(strcmp(previous, b->source->storage.temporary) && access(previous, F_OK));
        unchanged(f.client, &old_chunk); unchanged(f.client, &old_done);
        old_chunk.from = fixture_address(b->holders[b->holder_idx]); unchanged(f.client, &old_chunk); // Старый UUID от текущего узла также запрещён.
        finish(&f);
    }
    puts("malformed payload/ranges/DONE/signature fail over to an isolated attempt; late packets ignored: PASS");
}

static void old_destination(struct UTUN_INSTANCE* inst) {
    FILE* file = fopen(download(inst)->dest_path, "wb");
    REQUIRE(file && fwrite("old", 1, 3, file) == 3 && !fclose(file));
}

static void require_old_destination(void) {
    char path[512]; snprintf(path, sizeof(path), "%s/result.bin", directory);
    FILE* file = fopen(path, "rb"); REQUIRE(file);
    REQUIRE(fgetc(file) == 'o' && fgetc(file) == 'l' && fgetc(file) == 'd' && fgetc(file) == EOF && !fclose(file));
}

static void test_io_failures(void) {
    for (int variant = 0; variant < 11; variant++) {
        struct fixture f = ready(4); old_destination(f.client);
        struct packet chunk = chunk_packet(f.client, 0, 4);
        struct packet done = done_packet(f.client);
        if (variant < 2) {
            fault = variant ? IO_SEEK : IO_WRITE; receive(f.client, &chunk);
        } else {
            receive(f.client, &chunk);
            const enum io_fault operations[] = {IO_READ, IO_CLOSE, IO_WRITE, IO_READ, IO_CLOSE, IO_CLOSE, IO_FLUSH, IO_RENAME, IO_HASH};
            fault = operations[variant - 2];
            if (variant == 5 || variant == 6) fault_skip = 1;
            if (variant == 7) fault_skip = 2;
            receive(f.client, &done);
        }
        if (fault_hits != 1 || download(f.client) || done_count != 1 || done_err != -1)
            DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "IO regression variant=%d fault=%d hits=%d remaining_skip=%d active=%p done=%d error=%d",
                        variant, fault, fault_hits, fault_skip, (void*)download(f.client), done_count, done_err);
        REQUIRE(fault_hits == 1 && !download(f.client) && done_count == 1 && done_err == -1);
        REQUIRE(!f.client->md.destinations && !f.client->md.block_sources->head);
        require_old_destination(); finish(&f);
    }
    for (int zero = 0; zero < 2; zero++) {
        struct fixture f = ready(4); old_destination(f.client);
        memset(download(f.client)->content_hash, zero ? 0 : 0x77, 32);
        struct packet chunk = chunk_packet(f.client, 0, 4); receive(f.client, &chunk);
        struct packet done = done_packet(f.client); receive(f.client, &done);
        REQUIRE(!download(f.client) && done_count == 1 && done_err == -1);
        require_old_destination(); finish(&f);
    }
    puts("seek/read/write/close/flush/hash/rename failures terminate once and preserve destination; zero hash checked: PASS");
}

struct cancel_context { struct fixture* fixture; int inside, callbacks, restart; };
static void cancelled(void* arg, int error) {
    struct cancel_context* ctx = arg;
    REQUIRE(!ctx->inside && error == -2 && !ctx->callbacks++);
    completed(NULL, error);
}
static void cancel_progress(void* arg, int done, int total) {
    struct cancel_context* ctx = arg;
    struct UTUN_INSTANCE* inst = ctx->fixture->client;
    ctx->inside = 1; REQUIRE(done == 1 && total == 1);
    REQUIRE(!media_download_cancel(inst, ctx->fixture->metadata.media_id, NULL, 0));
    REQUIRE(!download(inst) && !ctx->callbacks && !done_count);
    REQUIRE(media_download_cancel(inst, ctx->fixture->metadata.media_id, NULL, 0) == -1);
    if (ctx->restart) start(inst, &ctx->fixture->metadata);
    ctx->inside = 0;
}

static void test_reentrant_cancel(void) {
    for (int restart = 0; restart < 2; restart++) {
        struct fixture f = ready(1); old_destination(f.client);
        struct cancel_context ctx = {.fixture = &f, .restart = restart};
        struct media_download* dl = download(f.client);
        dl->listeners->progress = cancel_progress; dl->listeners->progress_arg = &ctx;
        dl->listeners->done = cancelled; dl->listeners->arg = &ctx;
        struct packet chunk = chunk_packet(f.client, 0, 1), done = done_packet(f.client);
        receive(f.client, &chunk); receive(f.client, &done);
        REQUIRE(ctx.callbacks == 1 && done_count == 1 && done_err == -2);
        require_old_destination();
        if (restart) {
            unchanged(f.client, &chunk); unchanged(f.client, &done);
            struct packet response = answer(f.server); receive(f.client, &response);
            unchanged(f.client, &chunk); unchanged(f.client, &done);
            deliver_block(f.client, download(f.client), 0);
            REQUIRE(done_count == 2 && !done_err && !download(f.client));
        }
        finish(&f);
    }
    puts("cancel/repeated cancel/restart at final progress: callback ownership, old packets and reservation release: PASS");
}

static struct packet request_packet(struct media_download* dl, uint64_t peer) {
    struct packet p = {.from = peer, .group = dl->group_id, .len = MEDIA_BLOCK_REQ_SIZE};
    struct media_pkt_block_req* req = (struct media_pkt_block_req*)p.data;
    req->subcmd = MEDIA_SUBCMD_BLOCK_REQ;
    memcpy(req->media_id, dl->media_id, 16); memcpy(req->block_id, dl->blocks[0].block_id, 16);
    REQUIRE(!media_index_generate_uuid(req->transfer_id)); return p;
}

static struct packet cancellation(const struct packet* request) {
    const struct media_pkt_block_req* req = (const struct media_pkt_block_req*)request->data;
    struct packet p = {.from = request->from, .group = request->group, .len = sizeof(struct media_pkt_cancel)};
    struct media_pkt_cancel* cancel = (struct media_pkt_cancel*)p.data;
    cancel->subcmd = MEDIA_SUBCMD_CANCEL; cancel->chunk = req->chunk;
    memcpy(cancel->media_id, req->media_id, 16); memcpy(cancel->block_id, req->block_id, 16);
    memcpy(cancel->transfer_id, req->transfer_id, 16); return p;
}

static void occupy_slow_queue(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t peer, int cmd) {
    if (peer != media_fixture_id(0x300) || cmd != MEDIA_SUBCMD_BLOCK_CHUNK) return;
    struct ETCP_ROUTER_CONN* conn = etcp_router_conn_get(inst, group, peer, ETCP_RT_ID_MEDIA_DELIVERY); REQUIRE(conn);
    struct ll_entry* entry = queue_entry_new(0); REQUIRE(entry);
    REQUIRE(!queue_data_put(conn->send_q, entry));
}

static void poll_ready(void) { for (int i = 0; i < 12; i++) uasync_poll(ua, 0); }

static void check_relay_packets(uint32_t bytes[2], int done[2]) {
    while (packet_count) {
        struct packet p = take_packet();
        if (p.to != media_fixture_id(0x300) && p.to != media_fixture_id(0x400)) continue;
        int peer = p.to == media_fixture_id(0x300) ? 0 : 1;
        if (p.data[0] == MEDIA_SUBCMD_BLOCK_CHUNK) {
            const struct media_pkt_block_chunk* ch = (const struct media_pkt_block_chunk*)p.data;
            REQUIRE(!done[peer] && ch->offset == bytes[peer] && p.len == sizeof(*ch) + ch->data_len);
            for (unsigned i = 0; i < ch->data_len; i++) REQUIRE(p.data[sizeof(*ch) + i] == (uint8_t)(bytes[peer] + i));
            bytes[peer] += ch->data_len;
        } else if (p.data[0] == MEDIA_SUBCMD_BLOCK_DONE) {
            const struct media_pkt_block_done* d = (const struct media_pkt_block_done*)p.data;
            REQUIRE(!done[peer]++ && d->status == MEDIA_BLOCK_OK && d->total_size == 3000 && bytes[peer] == 3000);
        } else REQUIRE(0);
    }
}

static void test_growing_relay(void) {
    struct fixture f = ready(3000); capture_blocks = 1; sent_hook = occupy_slow_queue;
    struct media_download* dl = download(f.client);
    struct media_block_source* source = dl->blocks[0].source;
    char temporary[FILE_IO_PATH_MAX]; snprintf(temporary, sizeof(temporary), "%s", source->storage.temporary);
    struct packet requests[2] = {request_packet(dl, media_fixture_id(0x300)), request_packet(dl, media_fixture_id(0x400))};
    receive(f.client, &requests[0]); receive(f.client, &requests[1]);
    REQUIRE(source->refs == 3); poll_ready(); REQUIRE(!packet_count);
    struct ETCP_ROUTER_CONN* slow = etcp_router_conn_get(f.client, GROUP, media_fixture_id(0x300), ETCP_RT_ID_MEDIA_DELIVERY);
    queue_set_threshold(slow->send_q, 0, 0);
    struct packet first = chunk_packet(f.client, 0, 1024); receive(f.client, &first);
    struct packet second = chunk_packet(f.client, 1024, 1024); receive(f.client, &second);
    poll_ready(); uint32_t bytes[2] = {0}; int done[2] = {0}; check_relay_packets(bytes, done);
    REQUIRE(bytes[0] == 1024 && bytes[1] == 2048 && !done[0] && !done[1]);
    unchanged(f.client, &first); poll_ready(); REQUIRE(!packet_count && source->available == 2048);
    struct packet tail = chunk_packet(f.client, 2048, 952); receive(f.client, &tail);
    struct packet end = done_packet(f.client); receive(f.client, &end);
    REQUIRE(!download(f.client) && done_count == 1 && !done_err && !access(temporary, F_OK));
    poll_ready(); check_relay_packets(bytes, done); REQUIRE(done[1] == 1 && !done[0]);
    for (int i = 0; i < 2; i++) {
        struct ll_entry* entry = queue_data_get(slow->send_q); REQUIRE(entry); queue_entry_free(entry);
        poll_ready(); check_relay_packets(bytes, done); REQUIRE(!done[0] && !access(temporary, F_OK));
    }
    REQUIRE(bytes[0] == 3000);
    struct ll_entry* entry = queue_data_get(slow->send_q); REQUIRE(entry); queue_entry_free(entry);
    poll_ready(); check_relay_packets(bytes, done);
    REQUIRE(done[0] == 1 && access(temporary, F_OK) && !f.client->md.streams && !f.client->md.block_sources->head);
    finish(&f);
    puts("growing relay: independent offsets, real queue backpressure, duplicate, retained temp, DATA tail before DONE: PASS");
}

static void test_relay_cancel(void) {
    struct fixture f = ready(1); capture_blocks = 1;
    struct media_download* dl = download(f.client); struct media_block_source* source = dl->blocks[0].source;
    char temporary[FILE_IO_PATH_MAX]; snprintf(temporary, sizeof(temporary), "%s", source->storage.temporary);
    struct packet a = request_packet(dl, media_fixture_id(0x300)), b = request_packet(dl, media_fixture_id(0x400)), cancel = cancellation(&a);
    receive(f.client, &a); receive(f.client, &b); REQUIRE(source->refs == 3);
    receive(f.client, &a); REQUIRE(source->refs == 3); // Повтор запроса не создаёт новую выдачу.
    for (size_t length = 1; length < cancel.len; length++) {
        struct packet bad = cancel; bad.len = length; receive(f.client, &bad); REQUIRE(source->refs == 3);
    }
    struct packet bad = cancel; ((struct media_pkt_cancel*)bad.data)->transfer_id[0]++;
    receive(f.client, &bad); REQUIRE(source->refs == 3);
    bad = cancel; bad.group++; receive(f.client, &bad); REQUIRE(source->refs == 3);
    bad = cancel; bad.from++; receive(f.client, &bad); REQUIRE(source->refs == 3);
    receive(f.client, &cancel); REQUIRE(source->refs == 2);
    REQUIRE(!media_download_cancel(f.client, dl->media_id, NULL, 0)); REQUIRE(!access(temporary, F_OK));
    poll_ready(); int errors = 0;
    while (packet_count) {
        struct packet p = take_packet();
        if (p.to != media_fixture_id(0x400)) continue;
        REQUIRE(p.data[0] == MEDIA_SUBCMD_BLOCK_DONE && ((struct media_pkt_block_done*)p.data)->status == MEDIA_BLOCK_UNAVAILABLE);
        errors++;
    }
    REQUIRE(errors == 1 && access(temporary, F_OK) && !f.client->md.streams && done_count == 1 && done_err == -2);
    finish(&f);
    puts("relay cancellation: exact attempt, idempotent request, sibling isolation and producer failure cleanup: PASS");
}

static void test_relay_generation(void) {
    for (int cancel_new = 0; cancel_new < 2; cancel_new++) {
        struct fixture f = ready(1);
        struct media_download* dl = download(f.client);
        struct media_block_source* old_source = dl->blocks[0].source;
        char old_path[FILE_IO_PATH_MAX]; snprintf(old_path, sizeof(old_path), "%s", old_source->storage.temporary);
        struct packet request = request_packet(dl, media_fixture_id(0x300)); receive(f.client, &request);
        struct ETCP_ROUTER_CONN* slow = etcp_router_conn_get(f.client, GROUP, media_fixture_id(0x300), ETCP_RT_ID_MEDIA_DELIVERY);
        queue_set_threshold(slow->send_q, 0, 0);
        struct ll_entry* pending = queue_entry_new(0); REQUIRE(pending && !queue_data_put(slow->send_q, pending));
        struct packet old_chunk = chunk_packet(f.client, 0, 1), old_done = done_packet(f.client);
        receive(f.client, &old_chunk); receive(f.client, &old_done); poll_ready();
        REQUIRE(!download(f.client) && old_source->refs == 1 && old_source->verified && !access(old_path, F_OK));
        start(f.client, &f.metadata); struct packet response = answer(f.server); receive(f.client, &response);
        dl = download(f.client); struct media_block_source* new_source = dl->blocks[0].source;
        REQUIRE(new_source != old_source && !old_source->indexed && new_source->indexed && new_source->refs == 1);
        unchanged(f.client, &old_chunk); unchanged(f.client, &old_done);
        if (cancel_new) REQUIRE(!media_download_cancel(f.client, dl->media_id, NULL, 0));
        capture_blocks = 1;
        pending = queue_data_get(slow->send_q); REQUIRE(pending); queue_entry_free(pending); poll_ready();
        REQUIRE(access(old_path, F_OK) && !f.client->md.streams && packet_count == 2);
        REQUIRE(packets[0].data[0] == MEDIA_SUBCMD_BLOCK_CHUNK && packets[1].data[0] == MEDIA_SUBCMD_BLOCK_DONE);
        REQUIRE(!memcmp(((struct media_pkt_block_chunk*)packets[0].data)->transfer_id,
            ((struct media_pkt_block_req*)request.data)->transfer_id, 16));
        REQUIRE(((struct media_pkt_block_done*)packets[1].data)->status == MEDIA_BLOCK_OK);
        if (!cancel_new) {
            REQUIRE(download(f.client) == dl && dl->blocks[0].source == new_source && new_source->indexed && !new_source->available);
            struct packet chunk = chunk_packet(f.client, 0, 1), done = done_packet(f.client);
            receive(f.client, &chunk); receive(f.client, &done);
            REQUIRE(done_count == 2 && !done_err && !download(f.client));
        } else REQUIRE(done_count == 2 && done_err == -2);
        finish(&f);
    }
    puts("relay across restart: old verified file drains independently of a new producer's success or cancellation: PASS");
}

static void test_destinations(void) {
    struct fixture f = ready(1); struct media_download* dl = download(f.client);
    char alias[1024]; snprintf(alias, sizeof(alias), "%s/./result.bin", directory);
    REQUIRE(!media_delivery_reserve(f.client, alias));
    struct media_index_result other = result(1); other.media_id[0]++;
    REQUIRE(media_download_start(f.client, GROUP, &other, alias, directory, media_test_public(SERVER, (uint8_t[32]){0}), completed, NULL, NULL, NULL) == -1);
    REQUIRE(!done_count && download(f.client) == dl);
    REQUIRE(!media_download_cancel(f.client, dl->media_id, NULL, 0));
    struct media_destination* writer = media_delivery_reserve(f.client, alias); REQUIRE(writer);
    REQUIRE(media_download_start(f.client, GROUP, &other, alias, directory, media_test_public(SERVER, (uint8_t[32]){0}), completed, NULL, NULL, NULL) == -1);
    media_delivery_release(f.client, writer); start(f.client, &other);
    REQUIRE(download(f.client)); media_index_result_free(&other); finish(&f);
    puts("normalized destination reservation is shared by incoming and outgoing writers and released on cancel: PASS");
}

static int deny_registration(void* arg, int action, const char* table, const char* column, const char* db, const char* trigger) {
    (void)arg; (void)column; (void)db; (void)trigger;
    return action == SQLITE_INSERT && !strcmp(table, "media_files") ? SQLITE_DENY : SQLITE_OK;
}

static void test_registration_failure_and_keys(void) {
    struct fixture f = ready(4); old_destination(f.client);
    struct packet chunk = chunk_packet(f.client, 0, 4); receive(f.client, &chunk);
    REQUIRE(!sqlite3_set_authorizer(f.client->topo_sqlite_db, deny_registration, NULL));
    struct packet done = done_packet(f.client); receive(f.client, &done);
    REQUIRE(!sqlite3_set_authorizer(f.client->topo_sqlite_db, NULL, NULL));
    REQUIRE(!download(f.client) && done_count == 1 && done_err == -1 && !f.client->md.destinations);
    char path[512]; snprintf(path, sizeof(path), "%s/result.bin", directory);
    uint8_t hash[32]; REQUIRE(!ma_sha256_file(path, hash) && !memcmp(hash, f.metadata.content_hash, 32));
    sqlite3_stmt* statement = NULL;
    REQUIRE(!sqlite3_prepare_v2(f.client->topo_sqlite_db,
        "SELECT (SELECT COUNT(*) FROM media_files),(SELECT COUNT(*) FROM media_preparations)", -1, &statement, NULL));
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW && !sqlite3_column_int(statement, 0) && sqlite3_column_int(statement, 1) == 1);
    REQUIRE(!sqlite3_finalize(statement));
    for (int variant = 0; variant < 4; variant++) {
        const char* values[] = {"NULL", "zeroblob(31)", "zeroblob(33)", "x''"};
        char sql[256]; snprintf(sql, sizeof(sql), "UPDATE peers_" GROUP_KEY " SET ed25519_pubkey=%s WHERE node_id=fixture_node(256)", values[variant]);
        REQUIRE(!sqlite3_exec(f.client->topo_sqlite_db, sql, NULL, NULL, NULL));
        REQUIRE(media_download_start(f.client, GROUP, &f.metadata, path, directory, media_test_public(SERVER, (uint8_t[32]){0}), completed, NULL, NULL, NULL) == -1);
        REQUIRE(done_count == 1 && !f.client->md.destinations);
    }
    finish(&f);
    puts("registration failure retains published/prepared file; missing and malformed author keys reject before callbacks: PASS");
}

static void test_relay_limit_and_idle(void) {
    struct fixture f = ready(1); capture_blocks = 1;
    struct media_download* dl = download(f.client); struct media_block_source* source = dl->blocks[0].source;
    for (int i = 0; i < MD_MAX_RELAY_DOWNSTREAM; i++) {
        struct packet request = request_packet(dl, media_fixture_id(0x300 + i)); receive(f.client, &request);
    }
    REQUIRE(source->refs == 1 + MD_MAX_RELAY_DOWNSTREAM && !f.client->md.active_streams);
    struct packet overflow = request_packet(dl, media_fixture_id(0x900)); receive(f.client, &overflow);
    REQUIRE(packet_count == 1 && packets[0].data[0] == MEDIA_SUBCMD_BLOCK_RELAY_FULL);
    const struct media_pkt_block_relay_full* full = (const struct media_pkt_block_relay_full*)packets[0].data;
    REQUIRE(full->num_relay_nodes == MD_MAX_RELAY_DOWNSTREAM && packets[0].len == sizeof(*full) + MD_MAX_RELAY_DOWNSTREAM * 32);
    REQUIRE(!memcmp(full->transfer_id, ((struct media_pkt_block_req*)overflow.data)->transfer_id, 16));
    packet_count = 0;
    f.client->md.dl_stall_timeout_tb = 1000;
    uint64_t deadline = get_time_tb() + 5000;
    while (f.client->md.streams && get_time_tb() < deadline) uasync_poll(ua, 5);
    REQUIRE(!f.client->md.streams && source->refs == 1 && download(f.client) == dl && !done_count);
    finish(&f);
    puts("relay admission permits five readers, echoes redirect attempt and releases idle readers: PASS");
}

static void test_indexed_source(void) {
    for (int variant = 0; variant < 4; variant++) {
        struct fixture f = ready(4);
        struct media_download* dl = download(f.client);
        struct packet requests[4];
        for (int i = 0; i < 4; i++) requests[i] = request_packet(dl, media_fixture_id(0x300 + i));
        struct packet chunk = chunk_packet(f.client, 0, 4), done = done_packet(f.client);
        receive(f.client, &chunk); receive(f.client, &done); REQUIRE(done_count == 1 && !done_err);
        capture_blocks = 1;
        receive(f.client, &requests[0]); REQUIRE(f.client->md.active_streams == 1);
        if (variant) {
            const enum io_fault operations[] = {IO_NONE, IO_READ, IO_SEEK, IO_CLOSE};
            fault = operations[variant]; poll_ready(); REQUIRE(fault_hits == 1 && !f.client->md.active_streams);
            REQUIRE(!f.client->md.streams && !f.client->md.file_loads->head && packet_count == 1);
            REQUIRE(packets[0].data[0] == MEDIA_SUBCMD_BLOCK_DONE
                && ((struct media_pkt_block_done*)packets[0].data)->status == MEDIA_BLOCK_UNAVAILABLE);
        } else {
            receive(f.client, &requests[0]); REQUIRE(f.client->md.active_streams == 1);
            receive(f.client, &requests[1]); receive(f.client, &requests[2]); receive(f.client, &requests[3]);
            REQUIRE(f.client->md.active_streams == 3 && packet_count == 1 && packets[0].data[0] == MEDIA_SUBCMD_BLOCK_RELAY_FULL);
            const struct media_pkt_block_relay_full* full = (const struct media_pkt_block_relay_full*)packets[0].data;
            REQUIRE(full->num_relay_nodes == 3 && !memcmp(full->transfer_id,
                ((struct media_pkt_block_req*)requests[3].data)->transfer_id, 16));
            struct packet cancel = cancellation(&requests[2]); receive(f.client, &cancel);
            struct md_file_load* load = md_file_load_find(&f.client->md, f.metadata.media_id);
            REQUIRE(f.client->md.active_streams == 2 && load->active_downloads == 2
                && fixture_address(load->downloader_keys[0]) == media_fixture_id(0x300) && fixture_address(load->downloader_keys[1]) == media_fixture_id(0x301));
            cancel = cancellation(&requests[0]); receive(f.client, &cancel);
            REQUIRE(load->active_downloads == 1 && fixture_address(load->downloader_keys[0]) == media_fixture_id(0x301));
            receive(f.client, &requests[3]); REQUIRE(f.client->md.active_streams == 2);
            media_delivery_cancel_channel(f.client, GROUP);
            REQUIRE(!f.client->md.streams && !f.client->md.active_streams && !f.client->md.file_loads->head);
            poll_ready(); REQUIRE(packet_count == 1); // Отменённые pump не отправили DATA/DONE.
        }
        char path[512]; snprintf(path, sizeof(path), "%s/result.bin", directory);
        uint8_t hash[32]; REQUIRE(!ma_sha256_file(path, hash) && !memcmp(hash, f.metadata.content_hash, 32));
        finish(&f);
    }
    puts("indexed source: idempotency, source admission/redirect, exact cancellation counters and TX I/O failures: PASS");
}

static void test_membership_revocation(void) {
    struct fixture f = ready(4); capture_blocks = 1;
    struct media_download* dl = download(f.client);
    struct media_block_source* source = dl->blocks[0].source;
    struct packet a = request_packet(dl, media_fixture_id(0x300)), b = request_packet(dl, media_fixture_id(0x400));
    receive(f.client, &a); receive(f.client, &b); REQUIRE(source->refs == 3);
    uint64_t peer = media_fixture_id(0x300);
    media_test_group(f.client, 222, &peer, 1);
    struct ETCP_ROUTER_CONN* neighbour = etcp_router_conn_get(f.client, 222, peer, ETCP_RT_ID_MEDIA_DELIVERY);
    REQUIRE(neighbour && etcp_router_conn_get(f.client, GROUP, peer, ETCP_RT_ID_MEDIA_DELIVERY));
    REQUIRE(sqlite3_exec(f.client->md.db, "UPDATE peers_" GROUP_KEY " SET admitted=0 WHERE node_id=fixture_node(768)", NULL, NULL, NULL) == SQLITE_OK);
    etcp_router_revalidate_group(f.client, GROUP);
    REQUIRE(source->refs == 2 && !neighbour->closed && !etcp_router_conn_get(f.client, GROUP, peer, ETCP_RT_ID_MEDIA_DELIVERY));
    REQUIRE(topo_group_chat_allowed(f.client, 222, peer) && !topo_group_chat_allowed(f.client, GROUP, peer));
    // Каждый признак неактивного членства запрещает и исходящий запрос, и новый канал.
    const char* changes[] = {"admitted=1,source=1", "source=0,deleted=1", "deleted=0,admitted=0"};
    for (unsigned i = 0; i < sizeof(changes) / sizeof(changes[0]); i++) {
        char sql[192]; snprintf(sql, sizeof(sql), "UPDATE peers_" GROUP_KEY " SET %s WHERE node_id=fixture_node(768)", changes[i]);
        REQUIRE(sqlite3_exec(f.client->md.db, sql, NULL, NULL, NULL) == SQLITE_OK);
        REQUIRE(!topo_group_chat_allowed(f.client, GROUP, peer));
        REQUIRE(md_send_key(f.client, GROUP, media_test_public(peer, (uint8_t[32]){0}), a.data, a.len, 1) == -1 && !packet_count);
    }
    uint64_t holder = fixture_address(dl->blocks[0].holders[dl->blocks[0].holder_idx]);
    REQUIRE(etcp_router_conn_get(f.client, GROUP, holder, ETCP_RT_ID_MEDIA_DELIVERY));
    char sql[192]; snprintf(sql, sizeof(sql), "UPDATE peers_" GROUP_KEY " SET admitted=0 WHERE node_id=%lld", (long long)holder);
    REQUIRE(sqlite3_exec(f.client->md.db, sql, NULL, NULL, NULL) == SQLITE_OK);
    etcp_router_revalidate_group(f.client, GROUP); poll_ready();
    dl = download(f.client);
    REQUIRE(dl && !done_count && fixture_address(dl->blocks[0].holders[dl->blocks[0].holder_idx]) != holder && !neighbour->closed);
    while (packet_count) { struct packet p = take_packet(); REQUIRE(p.to != holder && p.to != peer); }
    // Стенд перехватывает send до router; моделируем созданный реальной отправкой канал нового держателя.
    REQUIRE(etcp_router_conn_get_key(f.client, GROUP, dl->blocks[0].holders[dl->blocks[0].holder_idx], ETCP_RT_ID_MEDIA_DELIVERY));
    REQUIRE(sqlite3_exec(f.client->md.db, "UPDATE peers_" GROUP_KEY " SET deleted=1 WHERE node_id=fixture_node(512)", NULL, NULL, NULL) == SQLITE_OK);
    REQUIRE(etcp_router_conn_get(f.client, GROUP, media_fixture_id(0x400), ETCP_RT_ID_MEDIA_DELIVERY) == NULL);
    etcp_router_revalidate_group(f.client, GROUP); poll_ready();
    REQUIRE(!download(f.client) && done_count == 1 && done_err == -1 && !f.client->md.streams && !neighbour->closed);
    finish(&f);
    puts("membership revoke: stop relay, reject inactive peers, fail over holder, stop self, preserve neighbour: PASS");
}

static void test_service_group_scope(void) {
    reset(); struct UTUN_INSTANCE* inst = instance(SERVER);
    uint64_t peer = CLIENT, group = 222; uint8_t key[40];
    memcpy(key, &group, sizeof(group)); media_test_public(peer, key + sizeof(group));
    media_test_group(inst, 222, &peer, 1);
    struct packet p = {.from = peer, .group = GROUP, .len = sizeof(struct media_pkt_serve_reg)};
    struct media_pkt_serve_reg* reg = (void*)p.data;
    reg->subcmd = MEDIA_SUBCMD_SERVE_REG; media_test_group_key(222, reg->group_ed);
    receive(inst, &p); REQUIRE(!inst->md.served_nodes);
    media_test_group_key(GROUP, reg->group_ed); receive(inst, &p);
    p.group = 222; media_test_group_key(222, reg->group_ed); receive(inst, &p);
    REQUIRE(inst->md.served_nodes->count == 2 && queue_find_data_by_index(inst->md.served_nodes, key));
    struct media_pkt_serve_leave* leave = (void*)p.data;
    p.group = GROUP; p.len = sizeof(*leave); leave->subcmd = MEDIA_SUBCMD_SERVE_LEAVE; media_test_group_key(222, leave->group_ed);
    receive(inst, &p); REQUIRE(inst->md.served_nodes->count == 2);
    media_test_group_key(GROUP, leave->group_ed); receive(inst, &p);
    REQUIRE(inst->md.served_nodes->count == 1 && queue_find_data_by_index(inst->md.served_nodes, key));
    REQUIRE(etcp_router_conn_get(inst, 222, peer, ETCP_RT_ID_MEDIA_DELIVERY));
    REQUIRE(sqlite3_exec(inst->md.db, "UPDATE peers_" GROUP2_KEY " SET admitted=0 WHERE node_id=fixture_node(512)", NULL, NULL, NULL) == SQLITE_OK);
    etcp_router_revalidate_group(inst, 222); REQUIRE(!inst->md.served_nodes->count);
    destroy(inst);
    puts("SERVE_REG/LEAVE: signed envelope group, independent registrations and revoke cleanup: PASS");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    if (getenv("UTUN_TEST_DEBUG")) debug_set_category_level(DEBUG_CATEGORY_MEDIA, DEBUG_LEVEL_DEBUG);
    /* Настоящий ключ с первым байтом 00: все успешные сценарии обязаны проверить его подпись. */
    uint8_t x[32], secret[32], public[32]; REQUIRE(!sc_identity_from_seed(author_key, x, secret, public) && !public[0]);
    ua = uasync_create(); REQUIRE(ua);
    snprintf(directory, sizeof(directory), "/tmp/utun-blocks-XXXXXX"); REQUIRE(!test_mkdtemp(directory));
    test_identity_and_bounds(); test_rejected_attempts(); test_io_failures(); test_reentrant_cancel();
    test_growing_relay(); test_relay_cancel(); test_relay_generation(); test_destinations();
    test_registration_failure_and_keys(); test_relay_limit_and_idle();
    test_indexed_source(); test_membership_revocation(); test_service_group_scope();
    REQUIRE(!rmdir(directory)); uasync_destroy(ua, 0);
    puts("media block regressions: PASS"); return 0;
}
