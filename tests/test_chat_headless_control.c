/* Настоящий TCP/uasync/SQLite/media;управляем только короткий send/EAGAIN и отложенный media completion. */
#define _GNU_SOURCE
#include "../lib/socket_compat.h"
static ssize_t hc_test_send(socket_t fd, const void* data, size_t size);
#define socket_send hc_test_send
#include "../src/chat/chat_headless_control.c"
#include "test_utils.h"
#undef socket_send
#include <assert.h>
#include <pthread.h>

static int blocked;
static size_t short_write;
static unsigned observed, fail_timer, fail_context, parser_fault;
static chat_media_submitted_fn deferred;
static void* deferred_arg;
static int delay_media;
static pthread_t test_thread;

static ssize_t hc_test_send(socket_t fd, const void* data, size_t size) {
    if (blocked) {
#ifdef _WIN32
        WSASetLastError(WSAEWOULDBLOCK);
#else
        errno = EAGAIN;
#endif
        return -1;
    }
    return socket_send(fd, data, short_write && size > short_write ? short_write : size);
}
extern void __real_chat_core_submit_media_message(struct UTUN_INSTANCE*, struct chat_msg_submit*, chat_media_submitted_fn, void*);
void __wrap_chat_core_submit_media_message(struct UTUN_INSTANCE* inst, struct chat_msg_submit* req, chat_media_submitted_fn fn, void* arg) {
    if (delay_media) { assert(!deferred); deferred = fn; deferred_arg = arg; return; }
    __real_chat_core_submit_media_message(inst, req, fn, arg);
}
extern void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t bytes, const char* site) {
    if (pthread_equal(test_thread, pthread_self()) && fail_context && strstr(site, "chat_headless_control.c")) { fail_context = 0; return NULL; }
    return __real_u_calloc_impl(count, bytes, site);
}
extern void* __real_uasync_set_timeout(struct UASYNC*, int, void*, void (*)(void*), const char*);
void* __wrap_uasync_set_timeout(struct UASYNC* ua, int tb, void* arg, void (*fn)(void*), const char* name) {
    if (fail_timer && !strcmp(name, "hc_cleanup")) { fail_timer = 0; return NULL; }
    return __real_uasync_set_timeout(ua, tb, arg, fn, name);
}
extern int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int fail = st && strstr(sqlite3_sql(st), "END,key,type,value FROM json_each(?1)") &&
        __atomic_exchange_n(&parser_fault, 0, __ATOMIC_SEQ_CST);
    int result = __real_sqlite3_finalize(st); return fail ? SQLITE_BUSY : result;
}

static void hc_observe(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    (void)inst; (void)type; (void)data; (void)len; observed++;
}
static socket_t hc_peer(struct UTUN_INSTANCE* inst) {
    socket_t fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd != SOCKET_INVALID);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(chat_headless_control_get_port(inst))};
    assert(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    assert(!connect(fd, (struct sockaddr*)&address, sizeof(address)) && !socket_set_nonblocking(fd));
    uasync_poll(inst->ua, 0); return fd;
}
static size_t hc_receive(struct UASYNC* ua, socket_t fd, char* out, size_t capacity, size_t minimum) {
    size_t used = 0;
    for (unsigned i = 0; i < 1000; i++) {
        uasync_poll(ua, 5);
        ssize_t count = recv(fd, out + used, (int)(capacity - used - 1), 0);
        if (count > 0) used += (size_t)count;
        else if (count == 0) break;
        else assert(socket_get_error() == ERR_AGAIN || socket_get_error() == ERR_WOULDBLOCK);
        out[used] = 0;
        if (minimum ? used >= minimum : strchr(out, '\n') != NULL) return used;
        assert(used + 1 < capacity);
    }
    assert(!"headless response timeout/early EOF"); return 0;
}
static void hc_request_line(struct UTUN_INSTANCE* inst, socket_t fd, const char* request, char response[65536]) {
    assert(socket_send(fd, request, strlen(request)) == (ssize_t)strlen(request));
    response[0] = 0; hc_receive(inst->ua, fd, response, 65536, 0);
}
static int hc_response_ok(sqlite3* db, const char* response, int id) {
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT json_valid(?1),json_extract(?1,'$.id'),json_extract(?1,'$.ok')", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, response, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_int(st, 0) && sqlite3_column_int(st, 1) == id);
    int ok = sqlite3_column_int(st, 2); assert(sqlite3_finalize(st) == SQLITE_OK); return ok;
}

int main(void) {
    test_thread = pthread_self();
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_CONTROL, DEBUG_LEVEL_DEBUG);
    size_t baseline = u_get_allocated_count(); utun_instance_set_tun_init_enabled(0);
    struct UASYNC* ua = uasync_create(); assert(ua);
    char directory[512] = "/tmp/utun-headless-XXXXXX", config[1024]; assert(!test_mkdtemp(directory));
    snprintf(config, sizeof(config), "[global]\nmy_node_name=headless\ndb_path=%s\n[ntp]\nenabled=no\n"
        "[server: udp]\naddr=127.0.0.1:0\ntype=public\n"
        "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", directory);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config); assert(inst && !utun_core_start(inst) && !chat_service_start(inst));
    chat_event_set_handler(inst, hc_observe); size_t before = u_get_allocated_count();
    fail_context = 1; assert(chat_headless_control_init(ua, inst, "127.0.0.1", 0) < 0 && !fail_context && !inst->headless);
    assert(u_get_allocated_count() == before && inst->chat_event_handler == hc_observe);
    fail_timer = 1; assert(chat_headless_control_init(ua, inst, "127.0.0.1", 0) < 0 && !fail_timer && !inst->headless);
    assert(u_get_allocated_count() == before && inst->chat_event_handler == hc_observe);
    assert(!chat_headless_control_init(ua, inst, "127.0.0.1", 0) && !chat_headless_control_init(ua, inst, "127.0.0.1", 0));
    assert(chat_headless_control_init(ua, inst, "127.0.0.2", 0) < 0);
    socket_t peer = hc_peer(inst); char response[65536];
    const char* ping = "{\"id\":7,\"cmd\":\"p\\u0069ng\"}\n";
    assert(socket_send(peer, ping, 5) == 5); uasync_poll(ua, 0);
    assert(socket_send(peer, ping + 5, strlen(ping) - 5) == (ssize_t)strlen(ping) - 5);
    response[0] = 0; hc_receive(ua, peer, response, sizeof(response), 0); assert(hc_response_ok(inst->topo_sqlite_db, response, 7));
    hc_request_line(inst, peer, "{\"id\":101,\"cmd\":\"dm_list\"}\n", response);
    assert(hc_response_ok(inst->topo_sqlite_db, response, 101) && strstr(response, "\"data\":[]"));
    uint8_t dm_x[32], dm_secret[32], dm_ed[32], dm_seed[32]; uint64_t dm_conv = 0;
    assert(!sc_identity_generate(dm_x, dm_secret, dm_ed, dm_seed));
    assert(!dm_start(inst, sc_derive_node_id_from_pubkey(dm_x), dm_x, dm_ed, "DM reader", NULL, DM_MODE_DEVICE, &dm_conv));
    char dm_request[256]; snprintf(dm_request, sizeof(dm_request), "{\"id\":102,\"cmd\":\"dm_messages\",\"conv_id\":\"%llu\"}\n",
        (unsigned long long)dm_conv);
    hc_request_line(inst, peer, dm_request, response);
    assert(hc_response_ok(inst->topo_sqlite_db, response, 102) && strstr(response, "\"data\":[]"));
    hc_request_line(inst, peer, "{\"id\":103,\"cmd\":\"dm_messages\",\"conv_id\":\"01\"}\n", response);
    assert(!hc_response_ok(inst->topo_sqlite_db, response, 0)); /* Неканонический ID отклоняет общий parser. */
    hc_request_line(inst, peer, "{\"id\":104,\"cmd\":\"dm_messages\",\"conv_id\":\"1\"}\n", response);
    assert(!hc_response_ok(inst->topo_sqlite_db, response, 104));
    hc_request_line(inst, peer, "{\"id\":8,\"cmd\":\"create_channel\",\"name\":\"quote\\\"\\\\\\u0001\"}\n", response);
    assert(hc_response_ok(inst->topo_sqlite_db, response, 8));
    const char* invalid[] = {"{\"id\":9,\"cmd\":\"ping\",\"cmd\":\"status\"}\n", "{\"id\":2147483648,\"cmd\":\"ping\"}\n",
        "{\"id\":9,\"cmd\":\"ping\",}\n", "{\"id\":9,\"cmd\":\"ping\",\"data\":\"\\u0000\"}\n"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        hc_request_line(inst, peer, invalid[i], response); assert(!hc_response_ok(inst->topo_sqlite_db, response, 0));
    }
    __atomic_store_n(&parser_fault, 1, __ATOMIC_SEQ_CST);
    hc_request_line(inst, peer, "{\"id\":9,\"cmd\":\"create_channel\",\"name\":\"not committed\"}\n", response);
    assert(!__atomic_load_n(&parser_fault, __ATOMIC_SEQ_CST) && !hc_response_ok(inst->topo_sqlite_db, response, 0));
    hc_request_line(inst, peer, "{\"id\":10,\"cmd\":\"send\",\"ch\":\"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\",\"data\":\"text\"}\n", response);
    assert(!hc_response_ok(inst->topo_sqlite_db, response, 10));
    unsigned old_observed = observed; chat_event_post(inst, CHAT_EVT_STATUS_REFRESH, NULL, 0); assert(observed == old_observed + 1);
    hc_request_line(inst, peer, "{\"id\":11,\"cmd\":\"status\"}\n", response); assert(hc_response_ok(inst->topo_sqlite_db, response, 11));
    hc_request_line(inst, peer, "{\"id\":30,\"cmd\":\"devgroup_rename\",\"name\":\"My devices\"}\n", response);
    assert(hc_response_ok(inst->topo_sqlite_db, response, 30) && strstr(response, "My devices"));
    hc_request_line(inst, peer, "{\"id\":31,\"cmd\":\"devgroup_status\"}\n", response);
    assert(hc_response_ok(inst->topo_sqlite_db, response, 31) && strstr(response, "My devices") && !strstr(response, "pin"));
    hc_request_line(inst, peer, "{\"id\":32,\"cmd\":\"devgroup_invite\",\"ch\":\"1\",\"peer\":\"invalid\"}\n", response);
    assert(!hc_response_ok(inst->topo_sqlite_db, response, 0));
    hc_request_line(inst, peer, "{\"id\":33,\"cmd\":\"devgroup_leave\"}\n", response);
    assert(hc_response_ok(inst->topo_sqlite_db, response, 33) && !strstr(response, "My devices"));
    uint64_t slot = chat_core_create_channel_auto(inst, "media"); assert(slot);
    const char* channel = topo_groups_channel(inst->topo_groups, slot); assert(channel);
    char rename_request[2048];
    snprintf(rename_request, sizeof(rename_request),
        "{\"id\":40,\"cmd\":\"group_info\",\"ch\":\"%s\",\"description\":\"{\\n\\\"name\\\":\\\"media\\\",\\n\\\"ver\\\":2,\\\"group_type\\\":\\\"chat\\\",\\\"options\\\":{\\\"flag\\\":true}\\n}\"}\n",
        channel);
    hc_request_line(inst, peer, rename_request, response);
    assert(hc_response_ok(inst->topo_sqlite_db, response, 40) && strstr(response, "\"flag\":true"));
    snprintf(rename_request, sizeof(rename_request), "{\"id\":41,\"cmd\":\"rename_channel\",\"ch\":\"%s\",\"name\":\"Новое имя\"}\n",
        channel);
    hc_request_line(inst, peer, rename_request, response);
    assert(hc_response_ok(inst->topo_sqlite_db, response, 41) && strstr(response, "Новое имя") &&
        strstr(response, "\"ver\":3") && strstr(response, "\"flag\":true"));
    snprintf(rename_request, sizeof(rename_request), "{\"id\":42,\"cmd\":\"rename_channel\",\"ch\":\"%s\",\"name\":\"\"}\n",
        channel);
    hc_request_line(inst, peer, rename_request, response); assert(!hc_response_ok(inst->topo_sqlite_db, response, 42));
    hc_request_line(inst, peer, "{\"id\":20,\"cmd\":\"server_link\"}\n", response);
    assert(hc_response_ok(inst->topo_sqlite_db, response, 20));
    sqlite3_stmt* server_link = NULL;
    assert(sqlite3_prepare_v2(inst->topo_sqlite_db, "SELECT json_extract(?1,'$.data.link')", -1, &server_link, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(server_link, 1, response, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(server_link) == SQLITE_ROW);
    const char* link = (const char*)sqlite3_column_text(server_link, 0); struct InviteData server_target;
    assert(link && !invite_link_decode(link, strlen(link), &server_target, NULL, 0));
    assert(server_target.kind == INVITE_KIND_SERVER && !memcmp(server_target.pubkey, inst->my_keys.public_key, 32));
    char add[INVITE_LINK_MAX + 128];
    snprintf(add, sizeof(add), "{\"id\":21,\"cmd\":\"server_add\",\"ch\":\"%s\",\"link\":\"%s\"}\n",
        channel, link);
    assert(sqlite3_finalize(server_link) == SQLITE_OK);
    /* Нельзя добавить свой узел самому себе; команда обязана вернуть отказ запуска. */
    hc_request_line(inst, peer, add, response); assert(!hc_response_ok(inst->topo_sqlite_db, response, 21));
    hc_request_line(inst, peer, "{\"id\":22,\"cmd\":\"server_add\",\"ch\":\"1\"}\n", response);
    assert(!hc_response_ok(inst->topo_sqlite_db, response, 0));
    inst->config->global.chatserver_has_add_key = 0;
    hc_request_line(inst, peer, "{\"id\":23,\"cmd\":\"server_link\"}\n", response);
    assert(!hc_response_ok(inst->topo_sqlite_db, response, 23)); inst->config->global.chatserver_has_add_key = 1;
    char poll[1024]; snprintf(poll, sizeof(poll), "{\"id\":17,\"cmd\":\"poll_create\",\"ch\":\"%s\",\"question\":\"Q?\","
        "\"mode\":\"single\",\"allow_custom_answer\":true,\"options\":[\"yes\",\"no\"]}\n", channel);
    hc_request_line(inst, peer, poll, response); assert(hc_response_ok(inst->topo_sqlite_db, response, 17));
    hc_request_line(inst, peer, "{\"id\":18,\"cmd\":\"subscribe\",\"enable\":true}\n", response);
    assert(hc_response_ok(inst->topo_sqlite_db, response, 18));
    char ch[65]; strcpy(ch, channel); size_t cl = strlen(ch);
    const char invite_name[] = "q\"\\\x01"; uint8_t event[128]; event[0] = (uint8_t)cl; memcpy(event + 1, ch, cl);
    event[cl + 1] = sizeof(invite_name) - 1; memcpy(event + cl + 2, invite_name, sizeof(invite_name) - 1);
    memcpy(event + cl + 1 + sizeof(invite_name), inst->my_keys.public_key, 32); event[cl + 33 + sizeof(invite_name)] = 0;
    chat_event_post(inst, CHAT_EVT_INVITE_RECEIVED, event, (int)(cl + 34 + sizeof(invite_name)));
    response[0] = 0; hc_receive(ua, peer, response, sizeof(response), 0);
    sqlite3_stmt* decoded = NULL;
    assert(sqlite3_prepare_v2(inst->topo_sqlite_db, "SELECT json_valid(?1),json_extract(?1,'$.ch_name')", -1, &decoded, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(decoded, 1, response, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(decoded) == SQLITE_ROW);
    assert(sqlite3_column_int(decoded, 0) && !strcmp((const char*)sqlite3_column_text(decoded, 1), invite_name));
    assert(sqlite3_finalize(decoded) == SQLITE_OK);
    hc_request_line(inst, peer, "{\"id\":19,\"cmd\":\"subscribe\",\"enable\":false}\n", response);
    assert(hc_response_ok(inst->topo_sqlite_db, response, 19));
    char source[600], attach[1000]; snprintf(source, sizeof(source), "%s/input.bin", directory);
    FILE* file = fopen(source, "wb"); assert(file && fwrite("headless fixture", 1, 16, file) == 16 && !fclose(file));
    snprintf(attach, sizeof(attach), "{\"id\":12,\"cmd\":\"attach\",\"ch\":\"%s\",\"path\":\"%s\"}\n", channel, source);
    hc_request_line(inst, peer, attach, response); assert(hc_response_ok(inst->topo_sqlite_db, response, 12));
    struct headless_client* cli = hc_of(inst)->clients;
    char first[20000], second[10000]; memset(first, 'a', sizeof(first)); memset(second, 'b', sizeof(second));
    short_write = 7; cli_send(cli, first, sizeof(first)); cli_send(cli, second, sizeof(second)); short_write = 0;
    client_write_callback(cli->fd, cli); response[0] = 0; size_t count = hc_receive(ua, peer, response, sizeof(response), 30000);
    assert(count == 30000 && !memcmp(response, first, sizeof(first)) && !memcmp(response + sizeof(first), second, sizeof(second)));
    blocked = 1; cli_send(cli, first, sizeof(first)); cli_send(cli, first, sizeof(first));
    assert(cli->closing && cli->fd == SOCKET_INVALID); blocked = 0; socket_close_wrapper(peer);
    peer = hc_peer(inst); delay_media = 1;
    blocked = 1; const char* quit = "{\"id\":20,\"cmd\":\"quit\"}\n";
    assert(socket_send(peer, quit, strlen(quit)) == (ssize_t)strlen(quit));
    for (unsigned i = 0; i < 100 && !hc_of(inst)->clients->draining; i++) uasync_poll(ua, 1);
    assert(hc_of(inst)->clients->draining && !hc_of(inst)->clients->closing); blocked = 0;
    response[0] = 0; hc_receive(ua, peer, response, sizeof(response), 0); assert(hc_response_ok(inst->topo_sqlite_db, response, 20));
    socket_close_wrapper(peer); peer = hc_peer(inst);
    assert(socket_send(peer, attach, strlen(attach)) == (ssize_t)strlen(attach));
    for (unsigned i = 0; i < 100 && !deferred; i++) uasync_poll(ua, 1);
    assert(deferred);
    chat_headless_control_destroy(inst); assert(inst->chat_event_handler == hc_observe); socket_close_wrapper(peer);
    deferred(deferred_arg, 0); deferred = NULL; deferred_arg = NULL; delay_media = 0;
    utun_instance_destroy(inst); uasync_poll(ua, 0); uasync_destroy(ua, 0); assert(u_get_allocated_count() == baseline);
    DEBUG_INFO(DEBUG_CATEGORY_CONTROL, "PASS headless:JSON/Unicode/SQL admission,real TCP fragmentation/short writes/pressure,media commit/disconnect,handler restore");
    return 0;
}
