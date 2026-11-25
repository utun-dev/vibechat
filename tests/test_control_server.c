/* Регрессии control_server: настоящие очереди/таймеры, управляемые короткие send
 * и ошибки регистрации. Включение реализации даёт доступ к socket callbacks. */
#define nat_detection_request_check_all test_nat_check_all
#define stun_nat_recheck test_stun_recheck
#include "../src/control_server.c"
#undef nat_detection_request_check_all
#undef stun_nat_recheck
#include <assert.h>

static socket_t pending_accept = -1, capture_fd = -1;
static int fail_registration, fail_timer, fail_entry, send_mode;
static size_t send_chunk, wire_size;
static uint8_t wire[ETCPMON_MAX_MSG_SIZE * 8];
static int nat_checks, stun_checks;

void test_nat_check_all(struct NAT_DETECTION* detector) { assert(detector); nat_checks++; }
void test_stun_recheck(struct UTUN_INSTANCE* instance) { assert(instance); stun_checks++; }

socket_t __real_accept(socket_t, struct sockaddr*, socklen_t*);
socket_t __wrap_accept(socket_t fd, struct sockaddr* addr, socklen_t* len) {
    if (pending_accept < 0) return __real_accept(fd, addr, len);
    struct sockaddr_in ip = {.sin_family = AF_INET};
    ip.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(*len >= sizeof(ip)); memcpy(addr, &ip, sizeof(ip)); *len = sizeof(ip);
    socket_t result = pending_accept; pending_accept = -1;
    return result;
}

void* __real_uasync_add_socket_t(struct UASYNC*, socket_t, socket_t_callback_t, socket_t_callback_t,
                                socket_t_callback_t, const char*, void*);
void* __wrap_uasync_add_socket_t(struct UASYNC* ua, socket_t fd, socket_t_callback_t read, socket_t_callback_t write,
                                socket_t_callback_t error, const char* name, void* arg) {
    if (fail_registration && !strcmp(name, "control_client")) { fail_registration = 0; return NULL; }
    return __real_uasync_add_socket_t(ua, fd, read, write, error, name, arg);
}

void* __real_uasync_set_timeout(struct UASYNC*, int, void*, timeout_callback_t, const char*);
void* __wrap_uasync_set_timeout(struct UASYNC* ua, int tb, void* arg, timeout_callback_t callback, const char* name) {
    if (fail_timer && !strcmp(name, "ctrl_idle")) { fail_timer = 0; return NULL; }
    return __real_uasync_set_timeout(ua, tb, arg, callback, name);
}

void* __real_u_malloc_impl(uint32_t, const char*);
void* __wrap_u_malloc_impl(uint32_t size, const char* location) {
    if (fail_entry && strstr(location, "ll_queue.c:")) { fail_entry = 0; return NULL; }
    return __real_u_malloc_impl(size, location);
}

ssize_t __real_send(socket_t, const void*, size_t, int);
ssize_t __wrap_send(socket_t fd, const void* data, size_t len, int flags) {
    if (fd != capture_fd) return __real_send(fd, data, len, flags);
    if (send_mode == 1) { errno = EAGAIN; return -1; }
    if (send_mode == 2) { errno = EPIPE; return -1; }
    if (send_mode == 3) return 0;
    size_t take = len < send_chunk ? len : send_chunk;
    assert(take > 0 && wire_size + take <= sizeof(wire));
    memcpy(wire + wire_size, data, take); wire_size += take;
    return (ssize_t)take;
}

struct fixture {
    struct UTUN_INSTANCE inst;
    struct control_server server;
    struct CFG_CONTROL_ALLOW allow;
    socket_t peer;
    size_t allocations;
};

static void fixture_init(struct fixture* f) {
    memset(f, 0, sizeof(*f));
    f->allocations = u_get_allocated_count();
    f->inst.ua = uasync_create(); assert(f->inst.ua);
    f->inst.config = u_calloc(1, sizeof(*f->inst.config)); assert(f->inst.config);
    f->inst.config->global.control_allows = &f->allow;
    f->inst.config->global.control_allow_count = 1;
    f->inst.config->global.control_allows[0].network = INADDR_LOOPBACK;
    f->inst.config->global.control_allows[0].netmask = UINT32_MAX;
    f->server.ua = f->inst.ua; f->server.instance = &f->inst;
    f->server.max_clients = 8; f->server.listen_fd = -1; f->peer = -1;
}

static struct control_client* fixture_accept(struct fixture* f) {
    socket_t pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(f->peer == -1); f->peer = pair[1]; pending_accept = pair[0];
    accept_callback(-1, &f->server);
    return f->server.clients;
}

static void fixture_destroy(struct fixture* f) {
    capture_fd = -1; send_mode = 0;
    control_server_shutdown(&f->server);
    if (f->peer >= 0) close(f->peer);
    DEBUG_DEBUG(DEBUG_CATEGORY_CONTROL, "Fixture drain after shutdown: timers_alloc=%zu timers_free=%zu immediate=%p",
                f->inst.ua->timer_alloc_count, f->inst.ua->timer_free_count, (void*)f->inst.ua->immediate_queue_head);
    uasync_drain_immediate(f->inst.ua);
    TimeoutEntry pending;
    assert(timeout_heap_peek(f->inst.ua->timeout_heap, &pending) != 0);
    assert(timeout_heap_get_size(f->inst.ua->timeout_heap) == 0);
    assert(f->inst.ua->timer_alloc_count == f->inst.ua->timer_free_count);
    uasync_destroy(f->inst.ua, 0); u_free(f->inst.config);
    assert(u_get_allocated_count() == f->allocations);
}

static void enqueue(struct fixture* f, struct control_client* client, const char* text) {
    uint16_t len = (uint16_t)strlen(text);
    uint8_t* buffer = u_malloc(len); assert(buffer); memcpy(buffer, text, len);
    assert(control_client_send(&f->server, client, buffer, len) == 0);
}

/* Проверяем wire-байты и учёт памяти после нескольких partial send и EAGAIN. */
static void test_partial_send(void) {
    struct fixture f; fixture_init(&f);
    struct control_client* client = fixture_accept(&f); assert(client);
    capture_fd = client->fd; send_chunk = 3; wire_size = 0;
    enqueue(&f, client, "0123456789");
    uint8_t* base = client->output_queue->head->dgram;
    client_write_callback(client->fd, client);
    assert(client->output_offset == 3 && client->output_queue->head->dgram == base);
    assert(queue_total_bytes(client->output_queue) == 10);
    send_mode = 1; client_write_callback(client->fd, client);
    assert(client->output_offset == 3 && wire_size == 3);
    send_mode = 0;
    while (client->output_queue->head) client_write_callback(client->fd, client);
    enqueue(&f, client, "abcdefgh");
    while (client->output_queue->head) client_write_callback(client->fd, client);
    assert(wire_size == 18 && !memcmp(wire, "0123456789abcdefgh", 18));
    assert(!client->output_offset && !client->write_registered && !queue_total_bytes(client->output_queue));
    uasync_drain_immediate(f.inst.ua);
    assert(!client->read_paused && !client->output_waiter.internal && !client->output_waiter.call_soon_id);
    fixture_destroy(&f);
    puts("PASS partial send: exact wire bytes, EAGAIN, stable buffer and queue accounting");
}

static void test_cleanup(void) {
    for (int mode = 0; mode <= 4; mode++) {
        struct fixture f; fixture_init(&f);
        struct control_client* client = fixture_accept(&f); assert(client);
        enqueue(&f, client, "first message");
        capture_fd = client->fd; send_chunk = 2; wire_size = 0;
        client_write_callback(client->fd, client); assert(client->output_offset == 2);
        if (mode == 4) {
            send_chunk = 100;
            client_write_callback(client->fd, client);
            assert(client->output_waiter.call_soon_id);
            close_client(&f.server, client);
            uasync_drain_immediate(f.inst.ua);
        } else if (mode >= 2) { send_mode = mode; client_write_callback(client->fd, client); }
        else if (mode == 1) {
            uasync_cancel_timeout(f.inst.ua, client->idle_timer);
            control_client_idle_cb(client);
        } else close_client(&f.server, client);
        assert(!f.server.clients && !f.server.client_count);
        fixture_destroy(&f);
    }
    for (int fault = 0; fault < 2; fault++) {
        struct fixture f; fixture_init(&f);
        fail_registration = fault == 0; fail_timer = fault == 1;
        assert(!fixture_accept(&f));
        assert(!f.server.client_count && !fail_registration && !fail_timer);
        fixture_destroy(&f);
    }
    struct fixture f; fixture_init(&f);
    struct control_client* client = fixture_accept(&f); assert(client);
    uint8_t* buffer = u_malloc(8); assert(buffer);
    size_t before = u_get_allocated_count(); fail_entry = 1;
    assert(control_client_send(&f.server, client, buffer, 8) == -1 && !fail_entry);
    assert(u_get_allocated_count() == before - 1 && !client->output_queue->head);
    fixture_destroy(&f);
    puts("PASS cleanup: pending/partial sends, idle, send failure, registration/timer failure, allocation failure; no leaks");
}

/* Байты после объявленного SET_DEBUG_CONFIG не должны менять настройки. */
static void test_debug_payload(void) {
    for (int variant = 0; variant < 6; variant++) {
        struct fixture f; fixture_init(&f);
        struct control_client* client = fixture_accept(&f); assert(client);
        debug_config_t saved = g_debug_config;
        struct etcpmon_msg_header* hdr = (struct etcpmon_msg_header*)client->recv_buffer;
        uint8_t* payload = client->recv_buffer + sizeof(*hdr);
        memset(payload, DEBUG_LEVEL_TRACE, 100);
        payload[0] = DEBUG_LEVEL_ERROR; payload[1] = 3;
        uint16_t len = variant == 0 ? 1 : variant == 1 ? 2 : variant == 2 ? 4 : variant == 3 ? 6 : 5;
        if (variant == 4) { payload[1] = ETCPMON_MAX_DEBUG_CATEGORIES + 1; len = 2 + payload[1]; }
        etcpmon_build_header(hdr, len, ETCPMON_CMD_SET_DEBUG_CONFIG, 17);
        client->recv_len = hdr->size;
        if (variant == 5) {
            client->recv_len--; handle_client_data(&f.server, client);
            assert(!memcmp(&saved, &g_debug_config, sizeof(saved)) && f.server.clients == client);
            client->recv_len++; handle_client_data(&f.server, client);
            assert(g_debug_config.level == DEBUG_LEVEL_ERROR);
            for (int i = 1; i <= 3; i++) assert(g_debug_config.category_levels[i] == DEBUG_LEVEL_TRACE);
        } else {
            handle_client_data(&f.server, client);
            assert(!f.server.clients && !memcmp(&saved, &g_debug_config, sizeof(saved)));
        }
        g_debug_config = saved;
        fixture_destroy(&f);
    }
    puts("PASS SET_DEBUG_CONFIG: short, truncated, extra and excessive payloads rejected; fragmented valid command applied");
}

static void test_metrics(void) {
    struct fixture f; fixture_init(&f);
    struct control_client* client = fixture_accept(&f); assert(client);
    struct ll_queue empty = {0};
    struct ETCP_CONN conn = {.peer_node_id = 42, .input_queue = &empty, .input_send_q = &empty,
        .input_wait_ack = &empty, .recv_q = &empty, .output_queue = &empty};
    struct ll_queue ack_queues[2] = {{.count = 1, .total_bytes = 16}, {.count = 2, .total_bytes = 32}};
    struct ETCP_LINK links[2] = {{.local_link_id = 7, .ack_q = &ack_queues[0]},
        {.local_link_id = 8, .ack_q = &ack_queues[1], .retrans_timer = &empty}};
    links[0].next = &links[1]; conn.links = links;
    f.inst.connections = queue_new(f.inst.ua, 16, offsetof(struct conn_queue_entry, x25519), SC_PUBKEY_SIZE, "test connections"); assert(f.inst.connections);
    struct ll_entry* entry = queue_entry_new(sizeof(struct conn_queue_entry)); assert(entry);
    struct conn_queue_entry* ce = (struct conn_queue_entry*)entry->data;
    ce->peer_node_id = conn.peer_node_id; ce->conn = &conn;
    assert(queue_data_put_with_index(f.inst.connections, entry) == 0);
    client->selected_peer_id = conn.peer_node_id;
    for (int count = 1; count <= 2; count++) {
        links[0].next = count == 1 ? NULL : &links[1];
        send_metrics(&f.server, client, 23);
        struct ll_entry* response = queue_data_get(client->output_queue); assert(response);
        u_check(response->dgram, "metrics bounds", LOCATION);
        struct etcpmon_msg_header* hdr = (struct etcpmon_msg_header*)response->dgram;
        assert(hdr->type == ETCPMON_RSP_METRICS && hdr->seq_id == 23 && hdr->size == ETCPMON_METRICS_SIZE(count));
        struct etcpmon_rsp_metrics* rsp = (void*)(response->dgram + sizeof(*hdr));
        assert(rsp->etcp.ack_q_packets == (count == 1 ? 1 : 3) && rsp->etcp.ack_q_bytes == (count == 1 ? 16 : 48));
        assert(rsp->etcp.retrans_timer_active == (count == 2));
        struct etcpmon_link_metrics* metrics = (void*)(response->dgram + sizeof(*hdr) + sizeof(struct etcpmon_rsp_metrics));
        for (int i = 0; i < count; i++) {
            assert(metrics[i].local_link_id == 7 + i);
            const uint8_t* start = &metrics[i].bbr_mode;
            for (size_t j = 0; j < sizeof(*metrics) - offsetof(struct etcpmon_link_metrics, bbr_mode); j++) assert(start[j] == 0);
        }
        queue_dgram_free(response); queue_entry_free(response);
    }
    queue_entry_free(queue_data_get(f.inst.connections)); queue_free(f.inst.connections);
    fixture_destroy(&f);
    puts("PASS metrics: one/two links without BBR, zeroed BBR fields and intact allocation guards");
}

static void test_socket_limit(void) {
    struct fixture f; fixture_init(&f);
    struct control_client* client = fixture_accept(&f); assert(client);
    size_t limit = (ETCPMON_MAX_MSG_SIZE - ETCPMON_SOCKET_LIST_SIZE(0)) / sizeof(struct etcpmon_socket_info);
    struct ETCP_SOCKET* sockets = u_calloc(251, sizeof(*sockets)); assert(sockets);
    for (size_t i = 0; i < 251; i++) {
        snprintf(sockets[i].name, sizeof(sockets[i].name), "socket-%zu", i);
        sockets[i].interface_addr.ss_family = AF_INET;
        if (i < 250) sockets[i].next = &sockets[i + 1];
    }
    f.inst.etcp_sockets = sockets;
    const size_t counts[] = {0, limit, limit + 1, 250};
    for (size_t n = 0; n < sizeof(counts) / sizeof(counts[0]); n++) {
        size_t count = counts[n];
        for (size_t i = 0; i < 251; i++) sockets[i].probe_only = i >= count;
        send_socket_list(&f.server, client, 31);
        struct ll_entry* response = queue_data_get(client->output_queue); assert(response);
        struct etcpmon_msg_header* hdr = (void*)response->dgram;
        assert(etcpmon_validate_header(hdr) == 0 && hdr->size == response->len && hdr->seq_id == 31);
        if (count <= limit) {
            assert(hdr->type == ETCPMON_RSP_SOCKET_LIST && hdr->size == ETCPMON_SOCKET_LIST_SIZE(count));
            assert(response->dgram[sizeof(*hdr)] == count);
        } else {
            assert(hdr->type == ETCPMON_RSP_ERROR && response->dgram[sizeof(*hdr)] == ETCPMON_ERR_SERVER_BUSY);
        }
        queue_dgram_free(response); queue_entry_free(response);
    }
    u_free(sockets); f.inst.etcp_sockets = NULL;
    fixture_destroy(&f);
    puts("PASS socket list: zero, maximum fitting, first excessive and 250 sockets; probe-only sockets excluded");
}

/* Action подтверждает запуск NAT, возвращает ошибки без разрыва и не принимает префиксы/повреждённые строки. */
static void test_actions(void) {
    for (int variant = 0; variant < 7; variant++) {
        struct fixture f; fixture_init(&f);
        struct control_client* client = fixture_accept(&f); assert(client);
        struct etcpmon_msg_header* header = (void*)client->recv_buffer;
        struct etcpmon_cmd_action action = {0};
        strcpy(action.action, variant == 2 ? "nat123" : variant == 3 ? "unknown" : variant == 6 ? "nodes" : "nat");
        if (variant == 0) f.inst.nat_det = (void*)&f;
        if (variant == 5) memset(&action, 'x', sizeof(action));
        etcpmon_build_header(header, sizeof(action) - (variant == 4), ETCPMON_CMD_ACTION, 41 + variant);
        memcpy(client->recv_buffer + sizeof(*header), &action, sizeof(action));
        client->recv_len = header->size;
        handle_client_data(&f.server, client);
        if (variant == 4 || variant == 5) assert(!f.server.clients);
        else {
            assert(f.server.clients == client);
            struct ll_entry* entry = queue_data_get(client->output_queue); assert(entry);
            struct etcpmon_msg_header* response = (void*)entry->dgram;
            assert(response->seq_id == 41 + variant && response->size == entry->len);
            assert(response->type == (variant == 0 ? ETCPMON_RSP_ACTION_RESULT : ETCPMON_RSP_ERROR));
            if (variant == 0) assert(strstr((char*)entry->dgram + sizeof(*response) + 1, "requested"));
            queue_dgram_free(entry); queue_entry_free(entry);
        }
        fixture_destroy(&f);
    }
    assert(nat_checks == 1 && stun_checks == 1);
    puts("PASS Action: NAT acknowledgement, unavailable services, unknown/exact names and malformed payloads");
}

/* Пакет запросов остаётся в bounded RX до отправки предыдущего ответа; команды не продлевают TX deadline. */
static void test_backpressure(void) {
    struct fixture f; fixture_init(&f);
    struct control_client* client = fixture_accept(&f); assert(client);
    for (int i = 0; i < 8; i++)
        etcpmon_build_header((void*)(client->recv_buffer + i * sizeof(struct etcpmon_msg_header)), 0,
                             ETCPMON_CMD_GET_DEBUG_CONFIG, i + 1);
    client->recv_len = 8 * sizeof(struct etcpmon_msg_header);
    assert(handle_client_data(&f.server, client) == 0);
    assert(client->output_queue->count == 1 && client->read_paused && client->timer_is_tx);
    assert(client->recv_len == 7 * sizeof(struct etcpmon_msg_header));
    void* deadline = client->idle_timer;
    capture_fd = client->fd; send_mode = 1; wire_size = 0;
    for (int i = 0; i < 10; i++) {
        assert(handle_client_data(&f.server, client) == 0);
        client_write_callback(client->fd, client);
        assert(client->output_queue->count == 1 && client->idle_timer == deadline);
    }
    send_mode = 0; send_chunk = 3;
    for (int step = 0; step < 5000 && (client->read_paused || client->output_queue->head); step++) {
        if (client->output_queue->head) client_write_callback(client->fd, client);
        uasync_drain_immediate(f.inst.ua);
        assert(client->output_queue->count <= 1 && queue_total_bytes(client->output_queue) <= ETCPMON_MAX_MSG_SIZE);
    }
    assert(!client->read_paused && !client->recv_len && !client->timer_is_tx);
    size_t offset = 0;
    for (int i = 0; i < 8; i++) {
        struct etcpmon_msg_header* header = (void*)(wire + offset);
        assert(header->type == ETCPMON_RSP_DEBUG_CONFIG && header->seq_id == i + 1);
        offset += header->size;
    }
    assert(offset == wire_size);
    fixture_destroy(&f);
    puts("PASS backpressure: one TX frame, paused buffered requests, exact ordered replies and independent TX deadline");
}

/* Минимальная настоящая топология для сериализации большого списка без сетевых служб/БД. */
struct node_fixture {
    struct TOPO_GROUPS groups;
    struct TOPO_GROUP* group;
    struct TOPO_NODE* records;
};

static void node_fixture_init(struct fixture* f, struct node_fixture* nodes, int count) {
    memset(nodes, 0, sizeof(*nodes));
    nodes->groups.instance = &f->inst; f->inst.topo_groups = &nodes->groups;
    nodes->groups.group_list = queue_new(f->inst.ua, 16, 0, 8, "test groups");
    nodes->groups.node_registry = queue_new(f->inst.ua, 16, 0, 8, "test registry");
    struct ll_entry* entry = queue_entry_new(sizeof(struct TOPO_GROUP)); assert(entry);
    memset(entry->data, 0, entry->size);
    nodes->group = (void*)entry; nodes->group->group_id = TOPO_GROUP_UTUN; nodes->group->instance = &f->inst;
    nodes->group->nodes = queue_new(f->inst.ua, 16, offsetof(struct TOPO_GROUP_NODE, node_id) - sizeof(struct ll_entry), 8, "test nodes");
    assert(nodes->groups.group_list && nodes->groups.node_registry && nodes->group->nodes);
    assert(queue_data_put_with_index(nodes->groups.group_list, entry) == 0);
    nodes->records = u_calloc(count, sizeof(*nodes->records)); assert(nodes->records);
    for (int i = 0; i < count; i++) {
        struct TOPO_NODE* record = &nodes->records[i];
        record->node_id = 100 + i; record->timestamp = 1; record->node_name = "monitor-test";
        memcpy(record->ed25519_public_key, &record->node_id, sizeof(record->node_id));
        entry = queue_entry_new(8 + sizeof(record)); assert(entry);
        memcpy(entry->data, &record->node_id, 8); memcpy(entry->data + 8, &record, sizeof(record));
        assert(queue_data_put_with_index(nodes->groups.node_registry, entry) == 0);
        entry = queue_entry_new(sizeof(struct TOPO_GROUP_NODE)); assert(entry);
        memset(entry->data, 0, entry->size);
        ((struct TOPO_GROUP_NODE*)entry)->node_id = record->node_id;
        assert(queue_data_put_with_index(nodes->group->nodes, entry) == 0);
    }
}

static void node_fixture_destroy(struct fixture* f, struct node_fixture* nodes) {
    if (f->server.clients) close_client(&f->server, f->server.clients);
    struct ll_entry* entry;
    while ((entry = queue_data_get(nodes->group->nodes))) queue_entry_free(entry);
    queue_free(nodes->group->nodes);
    queue_entry_free(queue_data_get(nodes->groups.group_list)); queue_free(nodes->groups.group_list);
    while ((entry = queue_data_get(nodes->groups.node_registry))) queue_entry_free(entry);
    queue_free(nodes->groups.node_registry); u_free(nodes->records); f->inst.topo_groups = NULL;
}

/* Снимок больше wire-лимита, удаление ещё не отправленного узла и объединение live-изменений. */
static void test_node_snapshot(void) {
    struct fixture f; fixture_init(&f);
    struct node_fixture nodes; node_fixture_init(&f, &nodes, 400);
    struct control_client* client = fixture_accept(&f); assert(client);
    control_server_send_node_list(&f.server, client, 77);
    assert(client->snapshot_count == 400 && !client->output_queue->head);
    assert(control_wait_output(client) == 0);
    uasync_drain_immediate(f.inst.ua);
    assert(client->output_queue->count == 1);
    struct TOPO_GROUP_NODE* removed = topo_node_find_by_id(nodes.group, 101); assert(removed);
    control_server_notify_node_removed(&f.server, 101);
    assert(queue_remove_data(nodes.group->nodes, &removed->ll) == 0); queue_entry_free(&removed->ll);
    nodes.records[2].timestamp = 2;
    for (int i = 0; i < 100; i++) control_server_notify_node_change(&f.server, topo_node_find_by_id(nodes.group, 102));
    assert(client->dirty_count == 2);
    capture_fd = client->fd; send_chunk = ETCPMON_MAX_MSG_SIZE; wire_size = 0;
    for (int step = 0; step < 2000 && client->read_paused; step++) {
        if (client->output_queue->head) client_write_callback(client->fd, client);
        uasync_drain_immediate(f.inst.ua);
        assert(client->output_queue->count <= 1 && queue_total_bytes(client->output_queue) <= ETCPMON_MAX_MSG_SIZE);
    }
    assert(!client->read_paused && !client->snapshot_ids && !client->dirty_count && wire_size > ETCPMON_MAX_MSG_SIZE);
    size_t offset = 0; int infos = 0, ends = 0, removals = 0, updated = 0;
    while (offset < wire_size) {
        struct etcpmon_msg_header* header = (void*)(wire + offset);
        assert(etcpmon_validate_header(header) == 0 && offset + header->size <= wire_size);
        if (header->type == ETCPMON_RSP_NODE_INFO) {
            struct TOPOMSG_NODE* node = (void*)(wire + offset + sizeof(*header) + 2);
            assert(memcmp(node->ed25519_public_key, nodes.records[1].ed25519_public_key, 32));
            if (!memcmp(node->ed25519_public_key, nodes.records[2].ed25519_public_key, 32)) { assert(node->timestamp == 2); updated++; }
            infos++;
        } else if (header->type == ETCPMON_RSP_NODE_INFO_END) {
            assert(header->seq_id == 77 && infos == 399); ends++;
        } else {
            uint64_t id; memcpy(&id, wire + offset + sizeof(*header), sizeof(id));
            assert(header->type == ETCPMON_RSP_NODE_REMOVED && id == 101 && ends == 1); removals++;
        }
        offset += header->size;
    }
    assert(infos == 400 && ends == 1 && removals == 1 && updated == 2);
    for (int i = 0; i < CONTROL_NODE_DIRTY_MAX; i++) control_server_notify_node_removed(&f.server, 1000 + i);
    assert(client->dirty_count == CONTROL_NODE_DIRTY_MAX);
    control_server_notify_node_removed(&f.server, 1000 + CONTROL_NODE_DIRTY_MAX);
    assert(!f.server.clients);
    uasync_drain_immediate(f.inst.ua);
    node_fixture_destroy(&f, &nodes); fixture_destroy(&f);
    puts("PASS node snapshot: 400 nodes with one frame, removed/updated IDs, ordered end, coalescing and bounded overload cleanup");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_CONTROL, DEBUG_LEVEL_DEBUG);
    test_partial_send(); test_cleanup(); test_debug_payload(); test_metrics(); test_socket_limit(); test_actions();
    test_backpressure(); test_node_snapshot();
    return 0;
}
