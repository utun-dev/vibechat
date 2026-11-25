/* Контракт instance: вложенные подписки, конструкторы, selective/full reload и владение ресурсами. */
#include "utun_instance.h"
#include "transport_layer/etcp.h"
#include "transport_layer/etcp_connections.h"
#include "chat/chat_core.h"
#include "chat/chat_headless_control.h"
#include "call/call_headless.h"
#include "radio/radio_headless.h"
#include "../lib/mem.h"
#include "../lib/debug_config.h"
#include "../lib/platform_compat.h"
#include "test_utils.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int fail_subscription, fail_queue, fail_endpoint;
static const char* fail_timer;
extern void* __real_uasync_set_timeout(struct UASYNC*, int, void*, timeout_callback_t, const char*);
void* __wrap_uasync_set_timeout(struct UASYNC* ua, int timeout, void* arg, timeout_callback_t callback, const char* name) {
    if (fail_timer && !strcmp(fail_timer, name)) {
        DEBUG_WARN(DEBUG_CATEGORY_DEBUG, "injected timer failure name=%s", name); fail_timer = NULL; return NULL;
    }
    return __real_uasync_set_timeout(ua, timeout, arg, callback, name);
}
extern void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t size, const char* where) {
    if (fail_subscription && strstr(where, "utun_instance.c")) { fail_subscription = 0; return NULL; }
    if (fail_queue && strstr(where, "ll_queue.c")) { fail_queue = 0; return NULL; }
    return __real_u_calloc_impl(count, size, where);
}
extern int __real_etcp_create_server_socket(struct UTUN_INSTANCE*, struct CFG_SERVER*, struct ETCP_SOCKET**);
int __wrap_etcp_create_server_socket(struct UTUN_INSTANCE* inst, struct CFG_SERVER* cfg, struct ETCP_SOCKET** out) {
    if (fail_endpoint) {
        fail_endpoint = 0; *out = NULL;
        DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "injected listener failure name=%s", cfg->name); return -1;
    }
    return __real_etcp_create_server_socket(inst, cfg, out);
}

static int activity_a, activity_b, activity_c, activity_d, sleep_a, sleep_b, sleep_c, sleep_d;
static void on_a(struct UTUN_INSTANCE* inst, int active, void* arg) { (void)inst; (void)arg; assert(!active); activity_a++; }
static void on_b(struct UTUN_INSTANCE* inst, int active, void* arg) { (void)inst; (void)active; (void)arg; activity_b++; }
static void on_d(struct UTUN_INSTANCE* inst, int active, void* arg) { (void)inst; (void)arg; assert(!active); activity_d++; }
static void on_c(struct UTUN_INSTANCE* inst, int active, void* arg) {
    (void)arg; assert(active); activity_c++;
    utun_remove_activity_cbk(inst, on_b, NULL); utun_remove_activity_cbk(inst, on_c, NULL);
    utun_add_activity_cbk(inst, on_d, NULL); assert(!utun_set_client_activity(inst, 0));
}
static void sleeping_a(struct ETCP_CONN* conn, int sleeping, void* arg) {
    (void)arg; assert((conn->peer_ed25519_pubkey[31] == 1 || conn->peer_ed25519_pubkey[31] == 2) && sleeping); sleep_a++;
}
static void sleeping_b(struct ETCP_CONN* conn, int sleeping, void* arg) {
    (void)conn; (void)sleeping; (void)arg; sleep_b++;
}
static void sleeping_d(struct ETCP_CONN* conn, int sleeping, void* arg) {
    (void)arg; assert(conn->peer_ed25519_pubkey[31] == 2 && sleeping); sleep_d++;
}
static void sleeping_c(struct ETCP_CONN* conn, int sleeping, void* arg) {
    struct UTUN_INSTANCE* inst = conn->instance;
    (void)arg; assert(conn->peer_ed25519_pubkey[31] == 1 && sleeping); sleep_c++;
    utun_remove_peer_sleep_cbk(inst, sleeping_b, NULL); utun_remove_peer_sleep_cbk(inst, sleeping_c, NULL);
    utun_add_peer_sleep_cbk(inst, sleeping_d, NULL);
    struct ETCP_CONN other = {.instance=inst}; other.peer_ed25519_pubkey[31] = 2; utun_fire_peer_sleep_cbk(&other, 1);
}

static void test_callbacks(void) {
    size_t baseline = u_get_allocated_count();
    struct UTUN_INSTANCE inst = {.client_type = CLIENT_TYPE_DESKTOP};
    utun_add_activity_cbk(&inst, on_a, NULL); utun_add_activity_cbk(&inst, on_a, NULL);
    utun_add_activity_cbk(&inst, on_b, NULL); utun_add_activity_cbk(&inst, on_c, NULL);
    assert(!utun_set_client_activity(&inst, 1));
    assert(activity_a == 1 && !activity_b && activity_c == 1 && activity_d == 1 && !inst.lifecycle_callbacks_depth);
    utun_remove_activity_cbk(&inst, on_a, NULL); utun_remove_activity_cbk(&inst, on_d, NULL);
    utun_add_peer_sleep_cbk(&inst, sleeping_a, NULL); utun_add_peer_sleep_cbk(&inst, sleeping_a, NULL);
    utun_add_peer_sleep_cbk(&inst, sleeping_b, NULL); utun_add_peer_sleep_cbk(&inst, sleeping_c, NULL);
    struct ETCP_CONN peer = {.instance=&inst}; peer.peer_ed25519_pubkey[31] = 1; utun_fire_peer_sleep_cbk(&peer, 1);
    assert(sleep_a == 2 && !sleep_b && sleep_c == 1 && sleep_d == 1 && !inst.lifecycle_callbacks_depth);
    utun_remove_peer_sleep_cbk(&inst, sleeping_a, NULL); utun_remove_peer_sleep_cbk(&inst, sleeping_d, NULL);
    fail_subscription = 1; utun_add_activity_cbk(&inst, on_a, NULL); assert(!fail_subscription && !inst.activity_cbks);
    fail_subscription = 1; utun_add_peer_sleep_cbk(&inst, sleeping_a, NULL); assert(!fail_subscription && !inst.peer_sleep_cbks);
    assert(u_get_allocated_count() == baseline);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "PASS nested callbacks: remove self/next, duplicate add, newer activity and subscription faults");
}

static const char* private_key = "704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f";
static const char* public_key = "7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f";

static void write_config(const char* path, const char* name, const char* servers, const char* extras) {
    FILE* file = fopen(path, "wb"); assert(file);
    assert(fprintf(file, "[global]\nmy_node_name=%s\nmy_private_key=%s\nmy_public_key=%s\n%s%s",
        name, private_key, public_key, servers, extras) > 0 && !fclose(file));
}
static struct ETCP_SOCKET* endpoint(struct UTUN_INSTANCE* inst, const char* name) {
    for (struct ETCP_SOCKET* s = inst->etcp_sockets; s; s = s->next) if (!strcmp(s->name, name)) return s;
    return NULL;
}
static unsigned endpoint_count(struct UTUN_INSTANCE* inst) {
    unsigned count = 0; for (struct ETCP_SOCKET* s = inst->etcp_sockets; s; s = s->next) count++; return count;
}
static void connection_created(struct ETCP_CONN* conn, void* arg) { (void)conn; (void)arg; }
static void connection_status(struct ETCP_CONN* conn, int event, void* arg) { (void)conn; (void)event; (void)arg; }
static void link_status(struct ETCP_CONN* conn, struct ETCP_LINK* link, int state, int status, void* arg) {
    (void)conn; (void)link; (void)state; (void)status; (void)arg;
}
static void socket_status(struct ETCP_SOCKET* sock, int event, void* arg) { (void)sock; (void)event; (void)arg; }

static void test_creation(struct UASYNC* ua) {
    size_t baseline = u_get_allocated_count();
    assert(!utun_instance_create(NULL, "/tmp/utun-must-not-create-invalid-owner"));
    assert(!utun_instance_create_from_str(NULL, "[global]\n"));
    struct utun_config* config = parse_config_from_buf("", 0, "caller-config"); assert(config);
    assert(!utun_instance_create_from_config(NULL, config));
    assert(!utun_instance_create_from_config(ua, config)); // не задана идентичность, но config остаётся caller
    assert(config->global.mtu == 1280); free_config(config);
    assert(u_get_allocated_count() == baseline);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "PASS constructor invalid owner, partial teardown and retained caller config");
}

static void test_reload(struct UASYNC* ua) {
    char path[] = "/tmp/utun-instance-contract-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0); close(fd);
    const char* first = "[server:udp]\naddr=127.0.0.1:0\ntype=private\n"
        "[server:tcp]\naddr=127.0.0.1:0\ntype=private\ntransport=tcp\n";
    write_config(path, "same", first, "");
    struct UTUN_INSTANCE* inst = utun_instance_create(ua, path); assert(inst && !utun_core_start(inst));
    struct ETCP_CONN marker = {0};
    struct ll_entry* entry = queue_entry_new(sizeof(struct conn_queue_entry)); assert(entry);
    struct conn_queue_entry* tcp = (struct conn_queue_entry*)entry->data; tcp->x25519[31] = 123; tcp->conn = &marker;
    assert(!queue_data_put_with_index(inst->connections, entry) && instance_find_conn_by_key(inst, tcp->x25519) == &marker);
    queue_remove_data(inst->connections, entry); queue_entry_free(entry);
    uint64_t udp_generation = endpoint(inst, "udp")->generation, tcp_generation = endpoint(inst, "tcp")->generation;
    uint16_t udp_port = ss_get_port(&endpoint(inst, "udp")->local_addr);
    struct UTUN_INSTANCE* original = inst;
    assert(utun_instance_reload(inst, ua, path) == original);
    assert(endpoint(inst, "udp")->generation == udp_generation && endpoint(inst, "tcp")->generation == tcp_generation);
    assert(ss_get_port(&endpoint(inst, "udp")->local_addr) == udp_port && endpoint_count(inst) == 2);
    write_config(path, "same", first, "[global]\nmtu=bad\n");
    assert(utun_instance_reload(inst, ua, path) == inst && endpoint(inst, "udp")->generation == udp_generation);
    write_config(path, "same", first, "");
    fail_queue = 1;
    assert(utun_instance_reload(inst, ua, path) == inst && !fail_queue && endpoint(inst, "udp")->generation == udp_generation);
    const char* changed = "[server:udp]\naddr=127.0.0.2:0\ntype=private\n"
        "[server:tcp]\naddr=127.0.0.1:0\ntype=private\ntransport=tcp\n";
    write_config(path, "same", changed, ""); fail_endpoint = 1;
    assert(utun_instance_reload(inst, ua, path) == inst && !fail_endpoint && !endpoint(inst, "udp"));
    assert(endpoint(inst, "tcp")->generation == tcp_generation && stcp_server_list_count(inst) == 1);
    assert(utun_instance_reload(inst, ua, path) == inst && endpoint_count(inst) == 2);
    assert(endpoint(inst, "udp")->generation != udp_generation && endpoint(inst, "tcp")->generation == tcp_generation);
    const char* one = "[server:tcp]\naddr=127.0.0.1:0\ntype=private\ntransport=udp\n";
    write_config(path, "same", one, "");
    assert(utun_instance_reload(inst, ua, path) == inst && endpoint_count(inst) == 1);
    assert(!endpoint(inst, "tcp")->is_tcp && !stcp_server_list_count(inst));
    const char* duplicates = "[server:shared]\naddr=127.0.0.1:0\ntype=private\n"
        "[server:shared]\naddr=127.0.0.1:0\ntype=private\n";
    write_config(path, "same", duplicates, "");
    inst = utun_instance_reload(inst, ua, path); assert(inst && inst->core_started && endpoint_count(inst) == 2);
    write_config(path, "same", one, "");
    inst = utun_instance_reload(inst, ua, path); assert(inst && inst->core_started && endpoint_count(inst) == 1);
    sqlite3_stmt* statement = NULL;
    assert(sqlite3_prepare_v2(inst->topo_sqlite_db, "SELECT 1", -1, &statement, NULL) == SQLITE_OK);
    utun_instance_stop(inst); assert(!inst->running);
    write_config(path, "new name", first, "");
    inst = utun_instance_reload(inst, ua, path); assert(inst && inst->core_started && !inst->utun_started && !inst->chat_started);
    assert(!strcmp(inst->name, "new name") && !inst->running && endpoint_count(inst) == 2 && stcp_server_list_count(inst) == 1);
    assert(sqlite3_finalize(statement) == SQLITE_OK);
    write_config(path, "new name", first, "[tcp_proxy_server]\nallowed_group=utun\nallowed_group=dd9d33df43dfd02381612a908ff3e1296e4862b7121d590a12edefbae7d73d28\n");
    inst = utun_instance_reload(inst, ua, path); assert(inst && inst->tcp_proxy_server.enabled);
    uint16_t proxy_test_port = socket_get_bound_port(endpoint(inst, "udp")->fd);
    assert(proxy_test_port && proxy_exit_group_allowed(inst, TOPO_GROUP_UTUN, inst->my_ed25519_pubkey));
    write_config(path, "new name", first, "[tcp_proxy_server]\nallowed_group=dd9d33df43dfd02381612a908ff3e1296e4862b7121d590a12edefbae7d73d28\nallowed_group=utun\nallowed_group=dd9d33df43dfd02381612a908ff3e1296e4862b7121d590a12edefbae7d73d28\n");
    assert(utun_instance_reload(inst, ua, path) == inst && socket_get_bound_port(endpoint(inst, "udp")->fd) == proxy_test_port);
    write_config(path, "new name", first, "[tcp_proxy_server]\nallowed_group=dd9d33df43dfd02381612a908ff3e1296e4862b7121d590a12edefbae7d73d28\n");
    inst = utun_instance_reload(inst, ua, path); assert(inst && !proxy_exit_group_allowed(inst, TOPO_GROUP_UTUN, inst->my_ed25519_pubkey));
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "PASS proxy reload preserves endpoint for reordered groups and applies a changed set");
    struct UASYNC wrong_owner = {0}; assert(utun_instance_reload(inst, &wrong_owner, path) == inst);
    etcp_add_new_conn_cbk(inst, connection_created, NULL); etcp_add_conn_status_cbk(inst, connection_status, NULL);
    etcp_add_link_status_cbk(inst, link_status, NULL); etcp_add_socket_cbk(inst, socket_status, NULL, 3);
    utun_instance_destroy(inst); uasync_drain_immediate(ua); unlink(path);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "PASS reload: ephemeral stability, malformed config, queue/create faults, TCP change/delete and restart state");
}

static void test_headless(struct UASYNC* ua) {
    char text[1024];
    snprintf(text, sizeof(text), "[global]\nmy_private_key=%s\nmy_public_key=%s\nclient_type=0\n"
        "[chatserver]\nheadless_control_bind=127.0.0.1:0\ncall_audio_bind=127.0.0.1:0\nradio_audio_bind=127.0.0.1:0\n",
        private_key, public_key);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, text); assert(inst && !utun_core_start(inst));
    assert(!chat_service_start(inst) && !inst->utun_started);
    assert(chat_headless_control_get_port(inst) && call_headless_get_port(inst) && radio_headless_get_port(inst));
    uint16_t ports[] = {chat_headless_control_get_port(inst), call_headless_get_port(inst), radio_headless_get_port(inst)};
    socket_t clients[3];
    for (unsigned i = 0; i < 3; i++) {
        clients[i] = socket(AF_INET, SOCK_STREAM, 0); assert(clients[i] != SOCKET_INVALID);
        struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(ports[i])};
        assert(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
        assert(!connect(clients[i], (struct sockaddr*)&address, sizeof(address)));
    }
    for (unsigned i = 0; i < 5; i++) uasync_poll(ua, 1);
    sqlite3* db = inst->topo_sqlite_db;
    chat_service_stop(inst);
    assert(inst->core_started && inst->topo_sqlite_db == db && !inst->chat_started);
    assert(!chat_headless_control_get_port(inst) && !call_headless_get_port(inst) && !radio_headless_get_port(inst));
    assert(!inst->call_audio && !inst->radio_headless && !inst->headless);
    for (unsigned i = 0; i < 3; i++) socket_close_wrapper(clients[i]);
    const char* timer_names[] = {"ca_cleanup", "rh_cleanup", "rh_mix"};
    for (unsigned i = 0; i < 3; i++) {
        fail_timer = timer_names[i]; assert(!chat_service_start(inst) && !fail_timer);
        if (!i) assert(!inst->call_audio && !call_headless_get_port(inst));
        else assert(!inst->radio_headless && !radio_headless_get_port(inst));
        chat_service_stop(inst);
    }
    strcpy(inst->config->global.headless_control_bind, "127.0.0.1:garbage");
    strcpy(inst->config->global.call_audio_bind, "127.0.0.1:65536");
    strcpy(inst->config->global.radio_audio_bind, "127.0.0.1");
    assert(!chat_service_start(inst));
    assert(!chat_headless_control_get_port(inst) && !call_headless_get_port(inst) && !radio_headless_get_port(inst));
    utun_instance_destroy(inst);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "PASS chat-only lifecycle: three headless ports/clients, timer faults, retained SQLite and malformed binds");
}

static void test_database_and_logger(struct UASYNC* ua) {
    char root[512] = "/tmp/utun-instance-owners-XXXXXX"; assert(!test_mkdtemp(root));
    char path[256], db_a[256], db_b[256], log_a[256], log_b[256], extras[1024], file[512];
    snprintf(path, sizeof(path), "%s/config", root); snprintf(db_a, sizeof(db_a), "%s/one/deep", root);
    snprintf(db_b, sizeof(db_b), "%s/two", root); snprintf(log_a, sizeof(log_a), "%s/first.log", root);
    snprintf(log_b, sizeof(log_b), "%s/second.log", root);
    snprintf(extras, sizeof(extras), "[global]\ndb_path=%s\nlog_file=%s\n", db_a, log_a);
    write_config(path, "same owner", "", extras);
    struct UTUN_INSTANCE* inst = utun_instance_create(ua, path); assert(inst && !inst->core_started);
    snprintf(file, sizeof(file), "%s/chats.db", db_a);
    assert(!strcmp(sqlite3_db_filename(inst->topo_sqlite_db, "main"), file));
    assert(sqlite3_exec(inst->topo_sqlite_db, "CREATE TABLE instance_contract(value); INSERT INTO instance_contract VALUES(7)",
        NULL, NULL, NULL) == SQLITE_OK);
    snprintf(extras, sizeof(extras), "[global]\ndb_path=%s\nlog_file=%s\n", db_b, log_b);
    write_config(path, "same owner", "", extras);
    inst = utun_instance_reload(inst, ua, path); assert(inst && !inst->core_started && !inst->running);
    snprintf(file, sizeof(file), "%s/chats.db", db_b);
    assert(!strcmp(sqlite3_db_filename(inst->topo_sqlite_db, "main"), file));
    assert(!strcmp(g_debug_config.output_file, log_a)); // уже открытый общий logger сохраняет первый путь
    utun_instance_destroy(inst);
    assert(!strcmp(g_debug_config.output_file, log_a) && !debug_reopen_log()); // config освобождён, путь остаётся жив
    debug_disable_file_output();
    snprintf(file, sizeof(file), "%s/chats.db", db_a); assert(!unlink(file));
    snprintf(file, sizeof(file), "%s/chats.db", db_b); assert(!unlink(file));
    assert(!unlink(path) && !unlink(log_a) && !rmdir(db_a) && !rmdir(db_b));
    snprintf(file, sizeof(file), "%s/one", root); assert(!rmdir(file) && !rmdir(root));
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "PASS db_path restart, recursive directory, stopped state and logger path after config destruction");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_INFO);
    debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
    test_callbacks();
    size_t baseline = u_get_allocated_count();
    struct UASYNC* ua = uasync_create(); assert(ua);
    test_creation(ua); test_reload(ua); test_headless(ua); test_database_and_logger(ua);
    uasync_poll(ua, 0); // удалённые heap-таймеры освобождаются при следующем poll
    debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_INFO);
    uasync_print_resources(ua, "instance contract before owner destroy");
    assert(ua->timer_alloc_count == ua->timer_free_count && ua->socket_alloc_count == ua->socket_free_count);
    uasync_destroy(ua, 0); // кэш таймеров принадлежит UASYNC, освобождается вместе с ним
    if (u_get_allocated_count() != baseline) u_report_unfreed_blocks();
    assert(u_get_allocated_count() == baseline); return 0;
}
