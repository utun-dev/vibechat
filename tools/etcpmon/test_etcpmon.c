/* Регрессии опроса и выбора connections на настоящих Winsock/WinAPI. */
#include <assert.h>
#include <winsock2.h>

static int WSAAPI test_send(SOCKET fd, const char* bytes, int size, int flags);

/* Проверяем также внутренние callbacks без расширения публичного API. */
#define send test_send
#include "etcpmon_client.c"
#undef send
#include "etcpmon_gui.c"

static SOCKET listener;
static SOCKET server_sock;
static struct etcpmon_client client;
static struct etcpmon_app app;
static int callbacks;
static SOCKET limited_socket = INVALID_SOCKET;
static int send_allowance, send_chunk;

/* Настоящий TCP с управляемой границей short send и WSAEWOULDBLOCK. */
static int WSAAPI test_send(SOCKET fd, const char* bytes, int size, int flags) {
    if (fd == limited_socket) {
        if (!send_allowance) { WSASetLastError(WSAEWOULDBLOCK); return SOCKET_ERROR; }
        if (send_allowance > 0) send_allowance--;
        if (size > send_chunk) size = send_chunk;
    }
    return send(fd, bytes, size, flags);
}

static void request_callback(void* arg, uint8_t type, const uint8_t* payload, uint16_t size) {
    (void)arg; (void)type; (void)payload; (void)size;
    callbacks++;
}

/* Соединяем клиент с локальным сервером, не обращаясь к реальному uTun. */
static void connect_client(void) {
    struct sockaddr_in addr;
    int len = sizeof(addr);
    assert(getsockname(listener, (struct sockaddr*)&addr, &len) == 0);
    assert(etcpmon_client_connect(&client, "127.0.0.1", ntohs(addr.sin_port)) == 0);
    for (int i = 0; i < 1000 && !client.fully_connected; i++) {
        assert(etcpmon_client_process(&client) >= 0);
        Sleep(1);
    }
    assert(client.fully_connected);
    server_sock = accept(listener, NULL, NULL);
    assert(server_sock != INVALID_SOCKET);
    u_long nonblock = 1;
    assert(ioctlsocket(server_sock, FIONBIO, &nonblock) == 0);
    app.need_initial_request = 1;
}

/* Разбираем реальные команды в TCP-потоке и считаем запросы нужного типа. */
static int drain_commands(uint8_t type) {
    static uint8_t buffer[ETCPMON_MAX_MSG_SIZE];
    static int used;
    int count = 0;
    for (int i = 0; i < 100; i++) {
        int got = recv(server_sock, (char*)buffer + used, sizeof(buffer) - used, 0);
        if (got == SOCKET_ERROR) {
            assert(WSAGetLastError() == WSAEWOULDBLOCK);
            /* Nagle может доставить следующую целую команду чуть позже. */
            Sleep(1);
            continue;
        }
        assert(got > 0);
        used += got;
        while (used >= (int)sizeof(struct etcpmon_msg_header)) {
            struct etcpmon_msg_header* hdr = (struct etcpmon_msg_header*)buffer;
            assert(etcpmon_validate_header(hdr) == 0);
            if (used < hdr->size) break;
            if (hdr->type == type) count++;
            int size = hdr->size;
            memmove(buffer, buffer + size, used - size);
            used -= size;
        }
    }
    assert(used == 0);
    return count;
}

/* Доставляем снимок списка через TCP, в том числе двумя отдельными фрагментами. */
static void deliver_list(struct etcpmon_conn_info* list, uint8_t count) {
    uint8_t buffer[ETCPMON_CONN_LIST_SIZE(3)];
    assert(count <= 3);
    uint16_t size = (uint16_t)ETCPMON_CONN_LIST_SIZE(count);
    etcpmon_build_header((struct etcpmon_msg_header*)buffer, size - sizeof(struct etcpmon_msg_header), ETCPMON_RSP_CONN_LIST, 0);
    buffer[sizeof(struct etcpmon_msg_header)] = count;
    if (count) memcpy(buffer + sizeof(struct etcpmon_msg_header) + 1, list, count * sizeof(*list));
    assert(send(server_sock, (const char*)buffer, 2, 0) == 2);
    assert(etcpmon_client_process(&client) >= 0);
    assert(send(server_sock, (const char*)buffer + 2, size - 2, 0) == size - 2);
    int received = 0;
    for (int i = 0; i < 1000 && !received; i++) {
        int result = etcpmon_client_process(&client);
        assert(result >= 0);
        received += result;
        Sleep(1);
    }
    assert(received == 1);
    assert(client.conn_count == count);
    assert(SendMessage(app.hListConnections, LB_GETCOUNT, 0, 0) == count);
}

/* Повторный вывод тех же величин после очистки и отсутствие старого сэмпла при исчезновении линка. */
static void test_graph_values(void) {
    struct etcpmon_client graph_client;
    etcpmon_client_init(&graph_client);
    struct etcpmon_client* previous = app.client;
    app.client = &graph_client;
    struct etcpmon_link_metrics link = {.inflight_lim_bytes = 64000, .bbr_bw_hi = 900000, .bbr_min_rtt_us = 12300};
    struct etcpmon_rsp_metrics metrics = {0};
    graph_client.last_links = &link; graph_client.last_links_count = 1;
    etcpmon_client_add_to_history(&graph_client, &metrics);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < GRAPH_METRIC_COUNT; i++) SetWindowTextA(app.hChannelValue[i], "");
        UpdateChannelValues(&app, 0);
        char text[64];
        GetWindowTextA(app.hChannelValue[GRAPH_METRIC_INFLIGHT_LIM], text, sizeof(text));
        assert(!strcmp(text, "62.5 KiB"));
        GetWindowTextA(app.hChannelValue[GRAPH_METRIC_BW_HI], text, sizeof(text));
        assert(!strcmp(text, "878.9 KiB/s"));
        GetWindowTextA(app.hChannelValue[GRAPH_METRIC_MIN_RTT], text, sizeof(text));
        assert(!strcmp(text, "12.3 ms"));
    }
    graph_client.history.head = 0; graph_client.last_links_count = 0;
    etcpmon_client_add_to_history(&graph_client, &metrics);
    for (int i = 0; i < GRAPH_METRIC_COUNT; i++) assert(graph_client.history.values[i][0] == 0);
    app.client = previous;
    puts("PASS graph: bytes/rates, same values after clear and no stale values without links");
}

/* Очередь сохраняет wire-порядок и не запускает response timeout до полной отправки. */
static void test_client_transport(void) {
    closesocket(server_sock); connect_client();
    limited_socket = client.sock; send_chunk = 2; send_allowance = 1;
    struct etcpmon_cmd_action action = {"nat"};
    assert(etcpmon_client_send_request(&client, ETCPMON_CMD_ACTION, &action, sizeof(action), request_callback, NULL) == 0);
    assert(client.tx_count == 1 && client.tx_head->offset == 2 && !client.pending_head->sent);
    uint8_t seq = client.pending_head->seq_id;
    client.pending_head->sent_at = GetTickCount() - REQUEST_TIMEOUT_MS;
    assert(etcpmon_client_process(&client) == 0 && client.connected);
    assert(etcpmon_client_request_list(&client) == 0 && client.tx_count == 2);
    limited_socket = INVALID_SOCKET;
    assert(etcpmon_client_process(&client) >= 0 && !client.tx_head && client.pending_head->sent);
    uint8_t expected[sizeof(struct etcpmon_msg_header) * 2 + sizeof(action)], actual[sizeof(expected)];
    etcpmon_build_header((void*)expected, sizeof(action), ETCPMON_CMD_ACTION, seq);
    memcpy(expected + sizeof(struct etcpmon_msg_header), &action, sizeof(action));
    etcpmon_build_header((void*)(expected + sizeof(struct etcpmon_msg_header) + sizeof(action)), 0, ETCPMON_CMD_LIST_CONN, 0);
    size_t used = 0;
    for (int i = 0; i < 1000 && used < sizeof(actual); i++) {
        int got = recv(server_sock, (char*)actual + used, sizeof(actual) - used, 0);
        if (got == SOCKET_ERROR) assert(WSAGetLastError() == WSAEWOULDBLOCK);
        else { assert(got > 0); used += got; }
        Sleep(1);
    }
    assert(used == sizeof(actual) && !memcmp(expected, actual, used));
    uint8_t error[] = {ETCPMON_ERR_INVALID_CMD, 'n','o',0};
    client.last_response_seq_id = seq;
    int before = callbacks;
    handle_response_with_callback(&client, ETCPMON_RSP_ERROR, error, sizeof(error));
    assert(client.connected && !client.pending_head && callbacks == before + 1);

    limited_socket = client.sock; send_allowance = 0;
    uint8_t payload[ETCPMON_MAX_MSG_SIZE - sizeof(struct etcpmon_msg_header)] = {0};
    for (int i = 0; i < 2; i++)
        assert(etcpmon_client_send_request(&client, ETCPMON_CMD_ACTION, payload, sizeof(payload), request_callback, NULL) == 0);
    assert(client.tx_bytes == ETCPMON_TX_MAX_BYTES && client.tx_count == 2);
    assert(etcpmon_client_send_request(&client, ETCPMON_CMD_ACTION, NULL, 0, request_callback, NULL) == -1);
    assert(client.connected && client.tx_count == 2);
    client.tx_progress_at = GetTickCount() - REQUEST_TIMEOUT_MS;
    assert(etcpmon_client_process(&client) == -1 && !client.connected && !client.tx_head && !client.pending_head);
    limited_socket = INVALID_SOCKET;
    for (int malformed = 0; malformed < 2; malformed++) {
        closesocket(server_sock); connect_client();
        assert(etcpmon_client_send_request(&client, ETCPMON_CMD_ACTION, &action, sizeof(action), request_callback, NULL) == 0);
        client.last_response_seq_id = client.pending_head->seq_id;
        before = callbacks;
        const uint8_t bad[] = {0, 'x'};
        handle_response_with_callback(&client, malformed ? ETCPMON_RSP_ACTION_RESULT : ETCPMON_RSP_CONN_LIST,
                                      bad, malformed ? sizeof(bad) : 1);
        assert(!client.connected && !client.pending_head && callbacks == before);
    }
    puts("PASS client TX: short send/EWOULDBLOCK, exact FIFO, queue bound, progress deadline and response validation");
}

int main(void) {
    WSADATA data;
    assert(WSAStartup(MAKEWORD(2, 2), &data) == 0);
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(listener != INVALID_SOCKET);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(listener, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    assert(listen(listener, 1) == 0);
    etcpmon_client_init(&client);
    client.log_file = fopen("test_etcpmon.log", "w");
    assert(client.log_file);
    app.client = &client;
    app.hWndMain = CreateWindowExA(0, "STATIC", "etcpmon regression", WS_OVERLAPPEDWINDOW,
                                  0, 0, 500, 300, NULL, NULL, GetModuleHandle(NULL), NULL);
    assert(app.hWndMain);
    app.hListConnections = CreateWindowExA(0, "LISTBOX", "", WS_CHILD | LBS_NOTIFY,
        0, 0, 400, 100, app.hWndMain, (HMENU)IDC_LIST_CONNECTIONS, GetModuleHandle(NULL), NULL);
    app.hListSockets = CreateWindowExA(0, "LISTBOX", "", WS_CHILD,
        0, 100, 400, 100, app.hWndMain, (HMENU)IDC_LIST_SOCKETS, GetModuleHandle(NULL), NULL);
    assert(app.hListConnections && app.hListSockets);
    for (int i = 0; i < GRAPH_METRIC_COUNT; i++) {
        app.hChannelValue[i] = CreateWindowExA(0, "EDIT", "", WS_CHILD, 0, 0, 100, 20,
            app.hWndMain, (HMENU)(UINT_PTR)(IDC_CH_VALUE_0 + i), GetModuleHandle(NULL), NULL);
        assert(app.hChannelValue[i]);
    }
    etcpmon_client_set_callbacks(&client, on_connected, on_disconnected, on_conn_list,
        on_socket_list, on_metrics, on_error, NULL, &app);
    connect_client();

    puts("Initial and periodic polling while connections are empty");
    OnTimer(&app);
    assert(drain_commands(ETCPMON_CMD_LIST_CONN) == 1);
    deliver_list(NULL, 0);
    Sleep(LIST_UPDATE_INTERVAL + 10);
    OnTimer(&app);
    assert(drain_commands(ETCPMON_CMD_LIST_CONN) == 1);

    puts("Appearance, manual selection, reorder and rename preserve the full peer ID");
    struct etcpmon_conn_info list[3] = {
        {0x123456789ABCDEF0ULL, "first"}, {0xFEDCBA9876543210ULL, "second"}, {3, "third"}
    };
    deliver_list(list, 2);
    assert(client.selected_peer_id == list[0].peer_node_id);
    SendMessage(app.hListConnections, LB_SETCURSEL, 1, 0);
    OnConnectionSelect(&app);
    assert(client.selected_peer_id == list[1].peer_node_id);
    assert(drain_commands(ETCPMON_CMD_SELECT_CONN) == 2);
    struct etcpmon_conn_info reordered[3] = {list[1], list[2], list[0]};
    strcpy(reordered[0].name, "renamed");
    deliver_list(reordered, 3);
    assert(client.selected_peer_id == list[1].peer_node_id);
    assert(SendMessage(app.hListConnections, LB_GETCURSEL, 0, 0) == 0);
    deliver_list(reordered, 3);
    assert(client.selected_peer_id == list[1].peer_node_id);

    puts("Removal selects a surviving peer; stale metrics are ignored; each sample is stored once");
    deliver_list(&list[2], 1);
    assert(client.selected_peer_id == 3);
    struct etcpmon_rsp_metrics metrics = {0};
    metrics.etcp.peer_node_id = list[1].peer_node_id;
    handle_response(&client, ETCPMON_RSP_METRICS, (const uint8_t*)&metrics, sizeof(metrics));
    assert(client.history.count == 0);
    metrics.etcp.peer_node_id = 3;
    handle_response(&client, ETCPMON_RSP_METRICS, (const uint8_t*)&metrics, sizeof(metrics));
    assert(client.history.count == 1 && client.last_links_count == 0);

    puts("Empty snapshots clear connections, metrics, graph and sockets");
    deliver_list(NULL, 0);
    assert(client.selected_peer_id == 0 && app.last_selected_peer_id == 0);
    assert(client.history.count == 0 && client.last_metrics == NULL && client.conn_list == NULL);
    SendMessageA(app.hListSockets, LB_ADDSTRING, 0, (LPARAM)"old socket");
    const uint8_t empty[] = {0};
    handle_response(&client, ETCPMON_RSP_SOCKET_LIST, empty, sizeof(empty));
    assert(SendMessage(app.hListSockets, LB_GETCOUNT, 0, 0) == 0);

    puts("Removing the queue tail keeps earlier requests linked; seq IDs cannot collide");
    struct etcpmon_pending_request* first = client.pending_head;
    assert(first); /* Начальный запрос debug config пока без ответа. */
    assert(etcpmon_client_send_request(&client, ETCPMON_CMD_ACTION, NULL, 0, request_callback, NULL) == 0);
    uint8_t last_seq = client.pending_tail->seq_id;
    uint8_t error_payload[] = {ETCPMON_ERR_INVALID_CMD, 't', 'e', 's', 't', 0};
    client.last_response_seq_id = last_seq;
    handle_response_with_callback(&client, ETCPMON_RSP_ERROR, error_payload, sizeof(error_payload));
    assert(client.pending_tail != NULL && client.pending_tail->next == NULL && callbacks == 1);
    client.next_seq_id = first->seq_id;
    assert(etcpmon_client_send_request(&client, ETCPMON_CMD_ACTION, NULL, 0, request_callback, NULL) == 0);
    assert(first->next && client.pending_tail->seq_id != first->seq_id);
    struct etcpmon_pending_request* old_tail = client.pending_tail;
    assert(etcpmon_client_send_request(&client, ETCPMON_CMD_ACTION, NULL, ETCPMON_MAX_MSG_SIZE, request_callback, NULL) == -1);
    assert(client.pending_tail == old_tail && old_tail->next == NULL);

    puts("Disconnect and reconnect reset selection and request the initial snapshots again");
    OnDisconnect(&app);
    assert(client.conn_list == NULL && client.pending_head == NULL && app.need_initial_request);
    closesocket(server_sock);
    connect_client();
    OnTimer(&app);
    assert(drain_commands(ETCPMON_CMD_LIST_CONN) == 1);
    deliver_list(&list[0], 1);
    assert(client.selected_peer_id == list[0].peer_node_id);

    puts("Slow metrics responses cannot accumulate requests; response timeout clears state");
    assert(etcpmon_client_process(&client) >= 0);
    int pending_metrics = 0;
    for (struct etcpmon_pending_request* req = client.pending_head; req; req = req->next) {
        if (req->type == ETCPMON_CMD_GET_METRICS) pending_metrics++;
    }
    assert(pending_metrics == 1);
    Sleep(ETCPMON_UPDATE_INTERVAL_MS + 10);
    assert(etcpmon_client_process(&client) >= 0);
    pending_metrics = 0;
    for (struct etcpmon_pending_request* req = client.pending_head; req; req = req->next) {
        if (req->type == ETCPMON_CMD_GET_METRICS) pending_metrics++;
    }
    assert(pending_metrics == 1);
    client.pending_head->sent_at = GetTickCount() - REQUEST_TIMEOUT_MS;
    assert(etcpmon_client_process(&client) == -1);
    assert(client.pending_head == NULL && !client.connected && app.need_initial_request);
    assert(SendMessage(app.hListConnections, LB_GETCOUNT, 0, 0) == 0);

    puts("Malformed debug responses are rejected and socket snapshots are not capped at 16");
    client.debug_config_dirty = 0;
    const uint8_t invalid_names[] = {1, 0, 'x'};
    handle_response(&client, ETCPMON_RSP_DEBUG_CONFIG, invalid_names, sizeof(invalid_names));
    assert(!client.debug_config_dirty);
    const uint8_t missing_levels[] = {1, 2, 'a', ',', 'b', 0, 1};
    handle_response(&client, ETCPMON_RSP_DEBUG_CONFIG, missing_levels, sizeof(missing_levels));
    assert(!client.debug_config_dirty);
    const uint8_t valid_debug[] = {1, 2, 'a', ',', 'b', 0, 1, 2};
    handle_response(&client, ETCPMON_RSP_DEBUG_CONFIG, valid_debug, sizeof(valid_debug));
    assert(client.debug_config_dirty && client.debug_category_levels[1] == 2);
    uint8_t socket_payload[1 + 17 * sizeof(struct etcpmon_socket_info)] = {17};
    struct etcpmon_socket_info* sockets = (struct etcpmon_socket_info*)(socket_payload + 1);
    for (int i = 0; i < 17; i++) {
        strcpy(sockets[i].name, "socket");
        strcpy(sockets[i].ip, "127.0.0.1");
    }
    handle_response(&client, ETCPMON_RSP_SOCKET_LIST, socket_payload, sizeof(socket_payload));
    assert(client.socket_count == 17 && SendMessage(app.hListSockets, LB_GETCOUNT, 0, 0) == 17);

    puts("A send failure clears pending requests and allows another connection");
    closesocket(server_sock);
    connect_client();
    assert(etcpmon_client_send_request(&client, ETCPMON_CMD_ACTION, NULL, 0, request_callback, NULL) == 0);
    assert(shutdown(client.sock, SD_SEND) == 0);
    assert(etcpmon_client_send_request(&client, ETCPMON_CMD_ACTION, NULL, 0, request_callback, NULL) == -1);
    assert(!client.connected && client.pending_head == NULL && client.pending_tail == NULL && app.need_initial_request);

    test_graph_values();
    test_client_transport();
    etcpmon_client_cleanup(&client);
    closesocket(server_sock);
    closesocket(listener);
    DestroyWindow(app.hWndMain);
    WSACleanup();
    puts("PASS: etcpmon polling, selection, snapshots, queue and reconnect regressions");
    return 0;
}
