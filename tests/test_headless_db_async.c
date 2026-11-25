#define GROUP_KEY "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951"

#define _GNU_SOURCE
/* Настоящие TCP/worker: JSON без SQL в loop, backpressure, FULL, disconnect/stop и поздние callbacks. */
#include "../src/dm/dm_core.c"
#include "../src/chat/chat_headless_control.c"
#include "../src/db_schema.h"
#include "../src/routing_layer/devgroup.h"
#include <assert.h>
#include <pthread.h>

static pthread_t loop_thread;
static int forbid_sql, prepare_fault, queries, query_fault, list_queries, send_fault;
static __thread int request_fault, query_request_fault, post_fault;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int pause_parse, pause_query, pause_start, entered, released;
static int parser_sql(const char* sql) { return sql && strstr(sql, "END,key,type,value FROM json_each(?1)"); }
static int listing_sql(const char* sql) {
    return sql && (!strncmp(sql, "SELECT c.channel_id", 19) || !strncmp(sql, "SELECT n.x25519_pubkey,n.name", sizeof("SELECT n.x25519_pubkey,n.name") - 1) ||
        !strncmp(sql, "SELECT json_object('id',m.id", 27));
}
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self()));
    if (sql && !strncmp(sql, "INSERT INTO dm_sequence", sizeof("INSERT INTO dm_sequence") - 1)) {
        int fault = __atomic_exchange_n(&send_fault, 0, __ATOMIC_SEQ_CST);
        if (fault) { *st = NULL; return fault; }
    }
    int parser = parser_sql(sql), listing = listing_sql(sql);
    int starting = sql && !strncmp(sql, "SELECT peer_x25519, peer_ed25519", sizeof("SELECT peer_x25519, peer_ed25519") - 1);
    if (parser || listing || starting) {
        if (parser || listing) {
            __atomic_add_fetch(parser ? &queries : &list_queries, 1, __ATOMIC_SEQ_CST);
            int fault = __atomic_exchange_n(parser ? &prepare_fault : &query_fault, 0, __ATOMIC_SEQ_CST);
            if (fault) { *st = NULL; return fault; }
        }
        pthread_mutex_lock(&lock);
        if ((parser && pause_parse) || (listing && pause_query) || (starting && pause_start)) {
            entered = 1; pthread_cond_broadcast(&cond);
            while (!released) pthread_cond_wait(&cond, &lock);
            if (parser) pause_parse = 0; else if (listing) pause_query = 0; else pause_start = 0;
        }
        pthread_mutex_unlock(&lock);
    }
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*fn)(void*, int, char**, char**), void* arg, char** error) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self())); return __real_sqlite3_exec(db, sql, fn, arg, error);
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t bytes, const char* where) {
    if (request_fault && pthread_equal(loop_thread, pthread_self()) && strstr(where, "chat_headless_control.c:") &&
        count * bytes == sizeof(struct hc_command_job)) { request_fault = 0; return NULL; }
    if (query_request_fault && pthread_equal(loop_thread, pthread_self()) && strstr(where, "chat_headless_control.c:") &&
        count * bytes == sizeof(struct hc_query_job)) { query_request_fault = 0; return NULL; }
    return __real_u_calloc_impl(count, bytes, where);
}
void* __real_uasync_call_soon(struct UASYNC*, void*, timeout_callback_t);
void* __wrap_uasync_call_soon(struct UASYNC* ua, void* arg, timeout_callback_t fn) {
    if (post_fault && fn == hc_process_input) { post_fault = 0; return NULL; }
    return __real_uasync_call_soon(ua, arg, fn);
}
static void ready(void* arg, const struct db_async_result* result) {
    assert(pthread_equal(loop_thread, pthread_self()) && !result->status); assert(!(*(int*)arg)++);
}
static void idle(struct UTUN_INSTANCE* inst) {
    uint64_t end = get_time_us() + 5000000; unsigned quiet = 0;
    while (quiet < 2 && get_time_us() < end) {
        uasync_poll(inst->ua, 1); quiet = db_async_pending(inst->db_worker) ? 0 : quiet + 1;
    }
    assert(quiet == 2);
}
static socket_t peer(struct UTUN_INSTANCE* inst) {
    socket_t fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd != SOCKET_INVALID);
    struct sockaddr_in address = {.sin_family=AF_INET, .sin_port=htons(chat_headless_control_get_port(inst))};
    assert(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    assert(!connect(fd, (struct sockaddr*)&address, sizeof(address)) && !socket_set_nonblocking(fd));
    uasync_poll(inst->ua, 1); return fd;
}
static void send_line(socket_t fd, const char* line) { assert(socket_send(fd, line, strlen(line)) == (ssize_t)strlen(line)); }
static void receive_lines(struct UASYNC* ua, socket_t fd, unsigned expected, char* out, size_t capacity) {
    size_t used = 0; unsigned lines = 0; uint64_t end = get_time_us() + 5000000; out[0] = 0;
    while (lines < expected && get_time_us() < end) {
        uasync_poll(ua, 1); assert(used + 1 < capacity);
        ssize_t bytes = recv(fd, out + used, capacity - used - 1, 0);
        if (bytes < 0) { assert(socket_get_error() == ERR_AGAIN || socket_get_error() == ERR_WOULDBLOCK); continue; }
        assert(bytes > 0);
        for (ssize_t i = 0; i < bytes; i++) lines += out[used + (size_t)i] == '\n';
        used += (size_t)bytes; out[used] = 0;
    }
    assert(lines == expected);
}
static void closed(struct UASYNC* ua, socket_t fd) {
    uint64_t end = get_time_us() + 5000000;
    while (get_time_us() < end) {
        uasync_poll(ua, 1); char byte; ssize_t bytes = recv(fd, &byte, 1, 0);
        if (!bytes) return;
        assert(bytes < 0 && (socket_get_error() == ERR_AGAIN || socket_get_error() == ERR_WOULDBLOCK));
    }
    assert(!"headless close timeout");
}
static void arm_pause(void) { pthread_mutex_lock(&lock); pause_parse = 1; entered = released = 0; pthread_mutex_unlock(&lock); }
static void arm_query_pause(void) { pthread_mutex_lock(&lock); pause_query = 1; entered = released = 0; pthread_mutex_unlock(&lock); }
static void wait_paused(struct UTUN_INSTANCE* inst) {
    int seen = 0; uint64_t end = get_time_us() + 5000000;
    while (!seen && get_time_us() < end) {
        uasync_poll(inst->ua, 1); pthread_mutex_lock(&lock); seen = entered; pthread_mutex_unlock(&lock);
    }
    assert(seen);
}
static void unpause(void) { pthread_mutex_lock(&lock); released = 1; pthread_cond_broadcast(&cond); pthread_mutex_unlock(&lock); }
static void tick(void* arg) { *(int*)arg = 1; }
static void collect(struct headless_control* hc) {
    uasync_cancel_timeout(hc->ua, hc->cleanup_timer); hc->cleanup_timer = NULL; hc_cleanup(hc);
}

/* Настоящая общая схема и history-view; создание также выполняется только worker. */
static int fixture(sqlite3* db, void* unused) {
    (void)unused;
    if (db_schema_init(db) || db_schema_table(db, "peers_" GROUP_KEY "", "peers") || db_schema_history(db, "msg_" GROUP_KEY "")) return SQLITE_ERROR;
    return sqlite3_exec(db,
        "INSERT INTO nodes(node_id,name,x25519_pubkey,ed25519_pubkey) VALUES"
        "(1,'Имя'||char(10)||'\"',zeroblob(32),CAST(zeroblob(31)||x'01' AS BLOB)),(2,'Другой',x'0000000000000000000000000000000000000000000000000000000000000002',CAST(zeroblob(31)||x'02' AS BLOB));"
        "INSERT INTO channels(channel_id,description_json,owner_node_id,x25519_pubkey,ed25519_pubkey,signature)"
        " VALUES('" GROUP_KEY "','{\"name\":\"Канал\",\"group_type\":\"chat\"}',CAST(zeroblob(31)||x'01' AS BLOB),zeroblob(32),x'" GROUP_KEY "',zeroblob(64));"
        "INSERT INTO peers_" GROUP_KEY "(node_id,x25519_pubkey,ed25519_pubkey,join_sig,join_ts,admitted,adm_tags)"
        " VALUES(1,zeroblob(32),CAST(zeroblob(31)||x'01' AS BLOB),zeroblob(64),1,1,'{}'),(2,zeroblob(32),CAST(zeroblob(31)||x'02' AS BLOB),zeroblob(64),1,1,'{}');"
        "INSERT INTO node_addresses(node_x,family,protocol,address,port,rtt) VALUES(zeroblob(32),4,1,x'7f000001',4321,5);"
        "INSERT INTO msg_" GROUP_KEY "(timestamp,id,data,author_signature,message_id,bucket,nonce,ver,edited_at,generation,accepted_key)"
        " VALUES(1,1,'{\"ct\":\"text\",\"text\":\"Привет\"}',zeroblob(64),zeroblob(32),0,zeroblob(16),1,0,zeroblob(32),CAST(zeroblob(31)||x'01' AS BLOB))",
        NULL, NULL, NULL);
}

/* SQL списков не обращается к runtime; порядок TCP, живые статусы и lifetime проверяются отдельно от parser. */
static void query_cases(struct UTUN_INSTANCE* inst, socket_t* current) {
    struct UASYNC* ua = inst->ua; char response[16384]; socket_t fd = *current;
    send_line(fd, "{\"id\":30,\"cmd\":\"channels\"}\n{\"id\":31,\"cmd\":\"members\",\"ch\":\"" GROUP_KEY "\"}\n"
        "{\"id\":32,\"cmd\":\"messages\",\"ch\":\"" GROUP_KEY "\"}\n");
    receive_lines(ua, fd, 3, response, sizeof(response)); idle(inst);
    const char *a = strstr(response, "\"id\":30,"), *b = strstr(response, "\"id\":31,"), *c = strstr(response, "\"id\":32,");
    assert(a && b && c && a < b && b < c && !strstr(response, "\"ok\":false"));
    DEBUG_DEBUG(DEBUG_CATEGORY_HEADLESS, "headless list fixture response=%s", response);
    assert(strstr(response, "\"peers\":2,\"msgs\":1,\"online\":1") && strstr(response, "Имя\\n\\\"") &&
        strstr(response, "\"ip\":\"127.0.0.1\"") && strstr(response, "\"content_type\":\"text\""));
    arm_query_pause(); send_line(fd, "{\"id\":33,\"cmd\":\"members\",\"ch\":\"" GROUP_KEY "\"}\n{\"id\":34,\"cmd\":\"ping\"}\n");
    wait_paused(inst); struct headless_control* hc = hc_of(inst); struct headless_client* cli = hc->clients;
    assert(cli->query && !cli->parser && cli->recv_len && cli->refs == 2);
    int prepared = __atomic_load_n(&queries, __ATOMIC_SEQ_CST), alive = 0;
    assert(uasync_set_timeout(ua, 10, &alive, tick, "headless query live")); uint64_t end = get_time_us() + 5000000;
    while (!alive && get_time_us() < end) uasync_poll(ua, 1);
    assert(alive && __atomic_load_n(&queries, __ATOMIC_SEQ_CST) == prepared);
    inst->node_id = 2; inst->my_ed25519_pubkey[31] = 2;
    /* Уже разобранная команда другого клиента ждёт SQL; disconnect отменяет её до prepare. */
    socket_t queued_fd = peer(inst); struct headless_client* queued = hc->clients; struct hc_request empty = {0};
    queued->request = &empty; hc_handle_query(queued, 35, HC_CHANNELS); queued->request = NULL;
    assert(queued->query && db_async_pending(inst->db_worker) == 2);
    int lists = __atomic_load_n(&list_queries, __ATOMIC_SEQ_CST);
    hc_close_client(queued); collect(hc); assert(queued->refs == 1); socket_close_wrapper(queued_fd);
    unpause(); receive_lines(ua, fd, 2, response, sizeof(response)); idle(inst);
    a = strstr(response, "\"node_id\":\"0000000000000000000000000000000000000000000000000000000000000001\""); b = strstr(response, "\"node_id\":\"0000000000000000000000000000000000000000000000000000000000000002\"");
    const char* offline = a ? strstr(a, "\"online\":0") : NULL;
    assert(a && b && a < b && offline && offline < b && strstr(b, "\"online\":1"));
    assert(__atomic_load_n(&list_queries, __ATOMIC_SEQ_CST) == lists); inst->node_id = 1; inst->my_ed25519_pubkey[31] = 1;
    __atomic_store_n(&query_fault, SQLITE_BUSY, __ATOMIC_SEQ_CST); send_line(fd, "{\"id\":36,\"cmd\":\"channels\"}\n");
    receive_lines(ua, fd, 1, response, sizeof(response)); assert(strstr(response, "\"id\":36,") && strstr(response, "\"ok\":false"));
    query_request_fault = 1; send_line(fd, "{\"id\":37,\"cmd\":\"channels\"}\n"); receive_lines(ua, fd, 1, response, sizeof(response));
    assert(!query_request_fault && strstr(response, "out of memory"));
    send_line(fd, "{\"id\":38,\"cmd\":\"channels\"}\n"); receive_lines(ua, fd, 1, response, sizeof(response));
    assert(strstr(response, "\"ok\":true"));
    arm_query_pause(); send_line(fd, "{\"id\":39,\"cmd\":\"members\",\"ch\":\"" GROUP_KEY "\"}\n"); wait_paused(inst);
    chat_headless_control_destroy(inst); socket_close_wrapper(fd);
    assert(!chat_headless_control_init(ua, inst, "127.0.0.1", 0) && hc_of(inst) != hc); *current = peer(inst);
    send_line(*current, "{\"id\":40,\"cmd\":\"ping\"}\n"); unpause(); receive_lines(ua, *current, 1, response, sizeof(response)); idle(inst);
    assert(strstr(response, "\"id\":40,") && !strstr(response, "\"id\":39,"));
}

struct dm_seed { uint64_t peer; uint8_t x[32], ed[32]; int done; };
static int dm_seed_work(sqlite3* db, void* arg) {
    struct dm_seed* seed = arg; sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(db, "INSERT INTO nodes(node_id,x25519_pubkey,ed25519_pubkey,name) VALUES(?,?,?,'Async peer')", -1, &st, NULL));
    assert(!sqlite3_bind_int64(st, 1, (sqlite3_int64)seed->peer) && !sqlite3_bind_blob(st, 2, seed->x, 32, SQLITE_STATIC));
    assert(!sqlite3_bind_blob(st, 3, seed->ed, 32, SQLITE_STATIC) && sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st)); return SQLITE_OK;
}
static void dm_seed_done(void* arg, const struct db_async_result* result) { assert(!result->status); ((struct dm_seed*)arg)->done++; }
static int dm_sent_work(sqlite3* db, void* arg) {
    (void)arg; sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(db, "SELECT (SELECT count(*) FROM dm_messages),(SELECT count(*) FROM dm_outbox),"
        "(SELECT count(*) FROM dm_delivery),(SELECT value FROM dm_sequence)", -1, &st, NULL));
    assert(sqlite3_step(st) == SQLITE_ROW);
    for (int i = 0; i < 4; i++) assert(sqlite3_column_int(st, i) == 2);
    assert(!sqlite3_finalize(st)); return SQLITE_OK;
}
/* DM WRITE удерживает pipeline до commit; disconnect не оставляет ни сокет, ни ссылку на клиента. */
static void dm_start_cases(struct UTUN_INSTANCE* inst, socket_t* fd) {
    uint64_t saved_node = inst->node_id; struct dm_seed seed = {0};
    assert(!sc_identity_generate(inst->my_keys.public_key, inst->my_keys.private_key, inst->my_ed25519_pubkey, inst->my_ed25519_privkey));
    inst->node_id = sc_derive_node_id_from_pubkey(inst->my_keys.public_key);
    uint8_t peer_secret[32], peer_seed[32];
    assert(!sc_identity_generate(seed.x, peer_secret, seed.ed, peer_seed)); seed.peer = sc_derive_node_id_from_pubkey(seed.x);
    inst->dm = u_calloc(1, sizeof(*inst->dm)); assert(inst->dm); inst->dm->inst = inst; inst->dm->initialized = 1;
    db_async_run(inst->db_worker, &(struct db_async_task){.name="headless-dm-fixture", .mode=DB_ASYNC_WRITE,
        .work=dm_seed_work, .data=&seed, .done=dm_seed_done}); idle(inst); assert(seed.done);
    char pipeline[1024], response[4096], peer_ed[65]; sc_public_key_hex(seed.ed, peer_ed);
    send_line(*fd, "{\"id\":28,\"cmd\":\"dm_start\",\"node_id\":\"1234\"}\n"
                   "{\"id\":29,\"cmd\":\"dm_start\",\"ed25519\":\"1234\"}\n");
    receive_lines(inst->ua, *fd, 2, response, sizeof(response));
    assert(strstr(response, "missing 'ed25519' param") && strstr(response, "invalid Ed25519 key"));
    snprintf(pipeline, sizeof(pipeline),
        "{\"id\":30,\"cmd\":\"dm_start\",\"ed25519\":\"%s\",\"ch\":\"" GROUP_KEY "\",\"mode\":\"device\"}\n"
        "{\"id\":31,\"cmd\":\"dm_start\",\"ed25519\":\"%s\",\"mode\":\"groups\"}\n{\"id\":32,\"cmd\":\"ping\"}\n",
        peer_ed, peer_ed);
    pthread_mutex_lock(&lock); pause_start = 1; entered = released = 0; pthread_mutex_unlock(&lock);
    send_line(*fd, pipeline); wait_paused(inst);
    struct headless_control* hc = hc_of(inst); struct headless_client* cli = hc->clients;
    while (cli && !cli->query) cli = cli->next;
    assert(cli && cli->query->sql.kind == HC_DM_START && !cli->parser && cli->recv_len);
    assert(recv(*fd, response, sizeof(response), 0) < 0 && (socket_get_error() == ERR_AGAIN || socket_get_error() == ERR_WOULDBLOCK));
    int alive = 0; assert(uasync_set_timeout(inst->ua, 10, &alive, tick, "headless DM WRITE alive"));
    uint64_t end = get_time_us() + 5000000; while (!alive && get_time_us() < end) uasync_poll(inst->ua, 1); assert(alive);
    unpause(); receive_lines(inst->ua, *fd, 3, response, sizeof(response)); idle(inst);
    const char *a = strstr(response, "\"id\":30,"), *b = strstr(response, "\"id\":31,"), *c = strstr(response, "\"id\":32,");
    assert(a && b && c && a < b && b < c && strstr(response, "\"started\":true") && !strstr(response, "\"ok\":false"));
    pthread_mutex_lock(&lock); pause_start = 1; entered = released = 0; pthread_mutex_unlock(&lock);
    send_line(*fd, pipeline); wait_paused(inst); hc_close_client(cli); collect(hc); socket_close_wrapper(*fd);
    unpause(); idle(inst); *fd = peer(inst); send_line(*fd, "{\"id\":34,\"cmd\":\"ping\"}\n");
    receive_lines(inst->ua, *fd, 1, response, sizeof(response)); assert(strstr(response, "\"id\":34,"));
    char conv[64]; snprintf(conv, sizeof(conv), "%llu", (unsigned long long)dm_derive_conv_id(inst->my_ed25519_pubkey, seed.ed));
    snprintf(pipeline, sizeof(pipeline), "{\"id\":50,\"cmd\":\"dm_send\",\"conv_id\":\"%s\",\"data\":\"worker text\"}\n"
        "{\"id\":51,\"cmd\":\"ping\"}\n", conv);
    pthread_mutex_lock(&lock); pause_start = 1; entered = released = 0; pthread_mutex_unlock(&lock);
    send_line(*fd, pipeline); wait_paused(inst); cli = hc_of(inst)->clients;
    while (cli && !cli->query) cli = cli->next;
    assert(cli && cli->query->sql.kind == HC_DM_SEND && cli->recv_len && cli->refs == 2);
    assert(recv(*fd, response, sizeof(response), 0) < 0 && (socket_get_error() == ERR_AGAIN || socket_get_error() == ERR_WOULDBLOCK));
    alive = 0; assert(uasync_set_timeout(inst->ua, 10, &alive, tick, "headless send WRITE alive"));
    end = get_time_us() + 5000000; while (!alive && get_time_us() < end) uasync_poll(inst->ua, 1); assert(alive);
    unpause(); receive_lines(inst->ua, *fd, 2, response, sizeof(response)); idle(inst);
    a = strstr(response, "\"id\":50,"); b = strstr(response, "\"id\":51,");
    assert(a && b && a < b && strstr(response, "\"sent\":true") && !strstr(response, "\"ok\":false"));
    __atomic_store_n(&send_fault, SQLITE_BUSY, __ATOMIC_SEQ_CST);
    send_line(*fd, pipeline); receive_lines(inst->ua, *fd, 2, response, sizeof(response)); idle(inst);
    a = strstr(response, "\"id\":50,"); b = strstr(response, "\"id\":51,");
    assert(a && b && a < b && strstr(response, "failed to send DM") && strstr(b, "\"pong\":true"));
    pthread_mutex_lock(&lock); pause_start = 1; entered = released = 0; pthread_mutex_unlock(&lock);
    send_line(*fd, pipeline); wait_paused(inst); hc_close_client(cli); collect(hc_of(inst)); socket_close_wrapper(*fd);
    unpause(); idle(inst); *fd = peer(inst); send_line(*fd, "{\"id\":52,\"cmd\":\"ping\"}\n");
    receive_lines(inst->ua, *fd, 1, response, sizeof(response)); assert(strstr(response, "\"id\":52,"));
    int checked = 0; db_async_run(inst->db_worker, &(struct db_async_task){.name="headless-send-check", .mode=DB_ASYNC_READ,
        .work=dm_sent_work, .data=&checked, .done=ready}); idle(inst); assert(checked);
    dm_core_destroy(inst); inst->node_id = saved_node; OPENSSL_cleanse(&inst->my_keys, sizeof(inst->my_keys));
    OPENSSL_cleanse(inst->my_ed25519_privkey, 32);
    memset(inst->my_ed25519_pubkey, 0, 32);
    puts("PASS headless DM start: FILE/RW without loop SQL, ordered DEVICE/GROUPS/ping, live timer and disconnect during WRITE");
    puts("PASS headless DM send: reply after commit, ordered pipeline after SQL failure, live timer and disconnect during WRITE");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); loop_thread = pthread_self(); size_t before = u_get_allocated_count();
    struct UASYNC* ua = uasync_create(); assert(ua); struct UTUN_INSTANCE inst = {.ua=ua};
    struct chat_core_ctx chat = {.inst=&inst, .initialized=1}; inst.chat_core = &chat; inst.node_id = 1; inst.my_ed25519_pubkey[31] = 1;
    char path[] = "/tmp/utun-headless-parser-XXXXXX"; int fd = mkstemp(path); assert(fd >= 0); close(fd);
    forbid_sql = 1; int opened = 0; inst.db_worker = db_async_create(ua, path, 2, ready, &opened); assert(inst.db_worker);
    idle(&inst); assert(opened && !inst.topo_sqlite_db && !chat.db);
    int initialized = 0;
    db_async_run(inst.db_worker, &(struct db_async_task){.name="headless-fixture", .mode=DB_ASYNC_DIRECT,
        .work=fixture, .data=&initialized, .done=ready}); idle(&inst); assert(initialized);
    assert(!chat_headless_control_init(ua, &inst, "127.0.0.1", 0)); socket_t first = peer(&inst); char response[16384];
    const char* pipeline = "{\"id\":1,\"cmd\":\"p\\u0069ng\"}\n{\"id\":2,\"cmd\":\"ping\"}\n";
    assert(socket_send(first, pipeline, 5) == 5); uasync_poll(ua, 1); assert(!hc_of(&inst)->clients->parser);
    arm_pause(); send_line(first, pipeline + 5); wait_paused(&inst);
    struct headless_control* hc = hc_of(&inst); struct headless_client* first_client = hc->clients;
    assert(first_client->parser && first_client->recv_len && first_client->refs == 2 && hc->refs == 2);
    int alive = 0; assert(uasync_set_timeout(ua, 10, &alive, tick, "headless parser live")); uint64_t end = get_time_us() + 5000000;
    while (!alive && get_time_us() < end) uasync_poll(ua, 1); assert(alive);
    unpause(); receive_lines(ua, first, 2, response, sizeof(response)); idle(&inst);
    const char *one = strstr(response, "\"id\":1,"), *two = strstr(response, "\"id\":2,");
    assert(one && two && one < two && strstr(response, "\"pong\":true") && !first_client->parser);
    send_line(first, "{\"id\":3,\"cmd\":\"ping\",\"cmd\":\"status\"}\n"); receive_lines(ua, first, 1, response, sizeof(response));
    assert(strstr(response, "\"id\":0,") && strstr(response, "\"ok\":false"));
    __atomic_store_n(&prepare_fault, SQLITE_BUSY, __ATOMIC_SEQ_CST); send_line(first, "{\"id\":4,\"cmd\":\"ping\"}\n");
    receive_lines(ua, first, 1, response, sizeof(response)); assert(strstr(response, "\"ok\":false"));
    send_line(first, "{\"id\":5,\"cmd\":\"ping\"}\n"); receive_lines(ua, first, 1, response, sizeof(response)); assert(strstr(response, "\"id\":5,"));
    /* FULL завершается отложенным продолжением строки, без рекурсии; queued disconnect отменяет SQL до prepare. */
    arm_pause(); send_line(first, "{\"id\":6,\"cmd\":\"ping\"}\n"); wait_paused(&inst);
    unsigned prepared = (unsigned)__atomic_load_n(&queries, __ATOMIC_SEQ_CST);
    socket_t second = peer(&inst); struct headless_client* second_client = hc->clients;
    send_line(second, "{\"id\":7,\"cmd\":\"create_channel\",\"name\":\"cancelled\"}\n");
    end = get_time_us() + 5000000; while (db_async_pending(inst.db_worker) < 2 && get_time_us() < end) uasync_poll(ua, 1);
    assert(db_async_pending(inst.db_worker) == 2 && second_client->parser);
    socket_t third = peer(&inst); char batch[8192] = {0};
    for (unsigned i = 0; i < 96; i++) strcat(batch, "{\"id\":8,\"cmd\":\"ping\"}\n");
    send_line(third, batch); receive_lines(ua, third, 96, response, sizeof(response)); assert(strstr(response, "\"ok\":false"));
    hc_close_client(second_client); collect(hc); assert(second_client->closing && second_client->refs == 1);
    socket_close_wrapper(second); socket_close_wrapper(third); unpause(); receive_lines(ua, first, 1, response, sizeof(response)); idle(&inst);
    assert((unsigned)__atomic_load_n(&queries, __ATOMIC_SEQ_CST) == prepared);
    /* Контекст listener живёт до старого READ; новый запуск сразу принимает соединения. */
    arm_pause(); send_line(first, "{\"id\":9,\"cmd\":\"create_channel\",\"name\":\"stopped\"}\n"); wait_paused(&inst);
    chat_headless_control_destroy(&inst); assert(!inst.headless && first_client->closing && !hc->running);
    socket_close_wrapper(first); assert(!chat_headless_control_init(ua, &inst, "127.0.0.1", 0)); assert(hc_of(&inst) != hc);
    socket_t replacement = peer(&inst); send_line(replacement, "{\"id\":10,\"cmd\":\"ping\"}\n");
    unpause(); receive_lines(ua, replacement, 1, response, sizeof(response)); idle(&inst); assert(strstr(response, "\"id\":10,"));
    query_cases(&inst, &replacement);
    dm_start_cases(&inst, &replacement);
    /* OOM и ошибка posted callback закрывают только клиента, без зависшего read-disabled socket. */
    socket_t rejected = peer(&inst); request_fault = 1; send_line(rejected, "{\"id\":11,\"cmd\":\"ping\"}\n");
    closed(ua, rejected); assert(!request_fault); socket_close_wrapper(rejected);
    rejected = peer(&inst); post_fault = 1; send_line(rejected, "{\"id\":12,\"cmd\":\"ping\"}\n");
    receive_lines(ua, rejected, 1, response, sizeof(response)); closed(ua, rejected); assert(!post_fault); socket_close_wrapper(rejected);
    __atomic_store_n(&query_fault, SQLITE_IOERR, __ATOMIC_SEQ_CST); send_line(replacement, "{\"id\":13,\"cmd\":\"channels\"}\n");
    receive_lines(ua, replacement, 1, response, sizeof(response)); assert(strstr(response, "\"ok\":false"));
    send_line(replacement, "{\"id\":14,\"cmd\":\"ping\"}\n"); receive_lines(ua, replacement, 1, response, sizeof(response));
    assert(strstr(response, "\"ok\":false")); socket_close_wrapper(replacement); chat_headless_control_destroy(&inst);
    assert(!db_async_destroy(inst.db_worker)); inst.db_worker = NULL; uasync_destroy(ua, 0); forbid_sql = 0;
    assert(!remove(path) && u_get_allocated_count() == before);
    puts("PASS headless parser/lists: no loop SQL/legacy, pipeline/UTF/live status/timer, FULL/cancel/stop/restart, OOM/post and fatal close");
    return 0;
}
