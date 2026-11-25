/* Настоящие UDP/INIT/AES-CCM: односторонний разрыв, повторный handshake и события живости. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "etcp.h"
#include "etcp_api.h"
#include "etcp_connections.h"
#include "etcp_keepalive.h"
#include "node_conn_direct.h"
#include "packet_transport.h"
#include "routing_layer/topo_node.h"
#include "routing_layer/topo_group.h"
#include "utun_instance.h"
#include "test_clock.h"
#include "test_net.h"
#include "../lib/mem.h"

#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "FAIL %s:%d: %s", __func__, __LINE__, #x); exit(1); } } while (0)
struct endpoint {
    struct UTUN_INSTANCE* inst;
    struct ETCP_SOCKET* socket;
    struct ETCP_CONN* conn;
    unsigned block_id, init_zero, response_one, up, down, reinit, received;
};
static struct endpoint ep[2];
static struct UASYNC* ua;
static unsigned close_action, close_fired, late_handshakes;
static int reuse_connection;
static const char* pubkeys[] = {
    "7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f",
    "7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d"
};
static const char* privkeys[] = {
    "704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f",
    "38240cb82199e504686507f11f6eaa4f740fde6f0c425c495e49a523019a5d68"
};

/* Наблюдаем уже зашифрованный wire, отдельная копия crypto не меняет счётчики рабочего соединения. */
static ssize_t send_observer(socket_t fd, const void* bytes, size_t len, const struct sockaddr* addr,
                             socklen_t addr_len, struct ETCP_LINK* link, void* arg) {
    struct endpoint* e = arg;
    uint8_t plain[PACKET_DATA_SIZE]; size_t size = 0;
    sc_context_t copy = link->etcp->crypto_ctx;
    int decoded = sc_decrypt(&copy, bytes, len, plain, &size) == SC_OK;
    if (!decoded && len > SC_PUBKEY_ENC_SIZE) {
        copy = link->etcp->crypto_ctx;
        decoded = sc_decrypt(&copy, bytes, len - SC_PUBKEY_ENC_SIZE, plain, &size) == SC_OK;
    }
    CHECK(decoded && size >= 4);
    if (link->etcp->close_requested && plain[3] >= ETCP_INIT_REQUEST && plain[3] <= ETCP_INIT_RESPONSE_NOINIT) {
        late_handshakes++;
        DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "Handshake after close request: code=%u path=%u", plain[3], link->local_link_id);
    }
    if ((plain[3] == ETCP_INIT_REQUEST || plain[3] == ETCP_INIT_REQUEST_NOINIT) && !(plain[2] & 1)) e->init_zero++;
    if ((plain[3] == ETCP_INIT_RESPONSE || plain[3] == ETCP_INIT_RESPONSE_NOINIT) && (plain[2] & 1)) e->response_one++;
    if (e->block_id && (e->block_id == 256 || e->block_id == link->local_link_id)) return len;
    return socket_sendto(fd, bytes, len, addr, addr_len);
}

/* Подписка действует и на новые линки; close проверяется именно в цепочке уведомления DOWN. */
static void link_event(struct ETCP_CONN* conn, struct ETCP_LINK* link, int old_state, int old_status, void* arg) {
    (void)old_state;
    struct endpoint* e = arg;
    link->send_hook = send_observer; link->send_hook_ctx = e;
    if (e == &ep[1] && old_status && !link->link_status && close_action == 1) {
        CHECK(!close_fired++); etcp_connection_close(conn);
    }
    if (e == &ep[1] && old_status && !link->link_status && close_action == 3) {
        CHECK(!close_fired++); etcp_link_close(link);
    }
}

static void conn_event(struct ETCP_CONN* conn, int event, void* arg) {
    struct endpoint* e = arg;
    if (event == ETCP_CBK_EVENT_UP) e->up++;
    if (event == ETCP_CBK_EVENT_DOWN) e->down++;
    if (event == ETCP_CBK_EVENT_REINIT) e->reinit++;
    if (e == &ep[1] && event == ETCP_CBK_EVENT_DOWN && close_action == 2) {
        CHECK(!close_fired++); etcp_connection_close(conn);
    }
}

static void receive(struct ETCP_CONN* conn, struct ll_entry* entry) {
    unsigned side = conn->instance == ep[1].inst;
    CHECK(entry->len == 2 && entry->dgram[0] == 253 && entry->dgram[1] == 17);
    ep[side].received++; queue_dgram_free(entry); queue_entry_free(entry);
}

static struct ETCP_LINK* first_link(unsigned side) {
    return ep[side].conn->links;
}

static void run_for(unsigned tb) { test_clock_run_for(ua, tb); }

/* Пир мог удалить путь, на котором ещё не измерен RTT: первый RTO равен 1 s.
 * Ждём саму доставку по оставшемуся пути, а не проверяем ровно на границе первого RTO. */
static void wait_alternate_delivery(unsigned side) {
    uint64_t start = get_time_tb(), end = start + 30000;
    while (!ep[side].received && get_time_tb() < end) test_clock_poll(ua, 10);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Alternate delivery: side=%u received=%u waited_tb=%llu links_up=%u/%u",
        side, ep[side].received, (unsigned long long)(get_time_tb() - start), ep[0].conn->links_up, ep[1].conn->links_up);
    CHECK(ep[side].received == 1);
}

static void start_source(unsigned count) {
    ep[0].conn = etcp_connection_create(ep[0].inst, "liveness"); CHECK(ep[0].conn);
    ep[0].conn->peer_node_id = ep[1].inst->node_id;
    CHECK(sc_init_ctx(&ep[0].conn->crypto_ctx, &ep[0].inst->my_keys) == SC_OK);
    CHECK(sc_set_peer_public_key(&ep[0].conn->crypto_ctx, ep[1].inst->my_keys.public_key, SC_PEER_PUBKEY_BIN) == SC_OK);
    etcp_conn_add_cbk(ep[0].conn, conn_event, &ep[0], ETCP_CBK_EVENT_UP | ETCP_CBK_EVENT_DOWN | ETCP_CBK_EVENT_REINIT);
    for (unsigned i = 0; i < count; i++) {
        struct ETCP_SOCKET* socket = i ? test_etcp_bound_socket(ep[0].inst, "extra") : ep[0].socket;
        CHECK(etcp_link_new(ep[0].conn, socket, &ep[1].socket->interface_addr, 0));
    }
}

/* Только ядро и одна явно выбранная UDP-пара: BGP/NCD не создают обходных путей. */
static void setup(unsigned swapped, unsigned count) {
    memset(ep, 0, sizeof(ep)); close_action = close_fired = late_handshakes = 0;
    ua = uasync_create(); CHECK(ua);
    for (unsigned side = 0; side < 2; side++) {
        unsigned key = side ^ swapped;
        char config[1536];
        snprintf(config, sizeof(config), "[global]\nmy_public_key=%s\nmy_private_key=%s\n"
            "keepalive_interval=100\nkeepalive_timeout=1000\nkeepalive_adaptive=0\nauto_sockets=no\n"
            "[server:srv]\naddr=127.0.0.1:0\ntype=public\n[server:extra]\naddr=127.0.0.1:0\ntype=public\n"
            "[allowed_keys]\nallow_all=1\n", pubkeys[key], privkeys[key]);
        ep[side].inst = utun_instance_create_from_str(ua, config); CHECK(ep[side].inst);
        CHECK(utun_core_start(ep[side].inst) == 0);
        ep[side].socket = test_etcp_bound_socket(ep[side].inst, "srv");
        etcp_add_link_status_cbk(ep[side].inst, link_event, &ep[side]);
        CHECK(etcp_bind(ep[side].inst, 253, receive) == 0);
    }
    if (!count) return; /* Отдельная проверка входящего INIT до создания транспорта. */
    if (reuse_connection) {
        /* Входящий UDP переиспользует общий объект, который уже создал другой транспорт. */
        ep[1].conn = etcp_connection_create(ep[1].inst, "shared native"); CHECK(ep[1].conn);
        CHECK(!etcp_connection_set_peer(ep[1].conn, ep[0].inst->my_keys.public_key));
        CHECK(!etcp_conn_index_peer(ep[1].conn));
    }
    start_source(count);
    uint64_t end = get_time_tb() + 30000;
    while (get_time_tb() < end) {
        test_clock_poll(ua, 10);
        ep[1].conn = instance_find_conn(ep[1].inst, ep[0].inst->node_id);
        if (ep[1].conn && ep[1].conn->links_up && ep[0].conn->links_up && ep[1].response_one) break;
    }
    CHECK(ep[1].conn && ep[0].conn->links_up && ep[1].conn->links_up);
    etcp_conn_add_cbk(ep[1].conn, conn_event, &ep[1], ETCP_CBK_EVENT_UP | ETCP_CBK_EVENT_DOWN | ETCP_CBK_EVENT_REINIT);
    CHECK(etcp_conn_ref_take(ep[1].conn) == 0);
    run_for(1000);
    CHECK(ep[0].init_zero && ep[1].response_one && ep[0].conn->initialized && ep[1].conn->initialized);
}

static void teardown(void) {
    close_action = 0;
    etcp_conn_ref_free(ep[1].conn);
    for (unsigned side = 0; side < 2; side++) {
        etcp_remove_link_status_cbk(ep[side].inst, link_event, &ep[side]);
        utun_instance_destroy(ep[side].inst);
    }
    uasync_poll(ua, 0);
    CHECK(ua->timer_alloc_count == ua->timer_free_count);
    uasync_destroy(ua, 0);
}

/* При работающем B->A повтор INIT(0) не сохраняет ложный UP на A; восстановление не меняет эпоху. */
static void one_way(unsigned swapped, unsigned blocked_side) {
    setup(swapped, 1);
    uint64_t epoch[] = {ep[0].conn->reset_id, ep[1].conn->reset_id};
    unsigned before = ep[0].init_zero;
    ep[blocked_side].block_id = 256;
    run_for(25000);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "One-way swapped=%u blocked_side=%u INIT0=%u up=%u/%u remote=%u/%u DOWN=%u/%u",
        swapped, blocked_side, ep[0].init_zero-before, ep[0].conn->links_up, ep[1].conn->links_up,
        first_link(0)->remote_keepalive, first_link(1)->remote_keepalive, ep[0].down, ep[1].down);
    CHECK(!ep[0].conn->links_up && !ep[1].conn->links_up && ep[0].down == 1 && ep[1].down == 1);
    if (blocked_side == 1) CHECK(ep[0].init_zero >= before + 2 && !first_link(1)->remote_keepalive && first_link(1)->recv_keepalive);
    ep[blocked_side].block_id = 0; run_for(10000);
    CHECK(ep[0].conn->links_up && ep[1].conn->links_up && ep[1].up == 1);
    for (unsigned side = 0; side < 2; side++) {
        CHECK(ep[side].conn->reset_id == epoch[side] && !ep[side].reinit);
        struct ll_entry* entry = ll_alloc_lldgram(2); CHECK(entry);
        entry->len = 2; entry->dgram[0] = 253; entry->dgram[1] = 17;
        CHECK(etcp_send(ep[side].conn, entry) == 0);
    }
    run_for(1000); CHECK(ep[0].received == 1 && ep[1].received == 1);
    teardown();
}

static void close_in_callback(unsigned action, unsigned keepalive) {
    setup(0, 1); close_action = action;
    if (keepalive) {
        /* Настоящий зашифрованный KA(0) вызывает DOWN внутри обычного decrypted RX. */
        first_link(0)->recv_keepalive = 0;
        etcp_link_send_keepalive(first_link(0));
        first_link(0)->recv_keepalive = 1;
        run_for(1000);
    } else {
        ep[1].block_id = 256;
        run_for(15000);
    }
    CHECK(close_fired == 1 && ep[1].conn->state == 2 && !ep[1].conn->normalizer && !ep[1].conn->callbacks_running);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Callback close: action=%u RX=%s PASS", action, keepalive ? "keepalive" : "INIT");
    teardown();
}

/* INIT на неработающем пути не меняет доступность соединения с другим исправным линком. */
static void healthy_alternative(void) {
    setup(0, 2);
    struct ETCP_LINK* broken = first_link(0);
    CHECK(broken->next && broken->link_status && broken->next->link_status);
    ep[1].block_id = broken->remote_link_id;
    run_for(15000); CHECK(!broken->link_status && ep[0].conn->links_up && ep[1].conn->links_up);
    unsigned before = ep[0].init_zero;
    etcp_link_enter_reinit(broken); run_for(5000);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Alternative: INIT0=%u broken=%u role=%u state=%u up=%u/%u DOWN=%u/%u block_id=%u",
        ep[0].init_zero - before, broken->link_status, broken->is_server, broken->link_state,
        ep[0].conn->links_up, ep[1].conn->links_up, ep[0].down, ep[1].down, ep[1].block_id);
    /* При исправном обходном пути таймер подавляет повторы INIT; первый запрос всё равно принимается. */
    CHECK(ep[0].init_zero == before + 1 && !broken->link_status && !ep[0].down && !ep[1].down);
    struct ll_entry* entry = ll_alloc_lldgram(2); CHECK(entry);
    entry->len = 2; entry->dgram[0] = 253; entry->dgram[1] = 17;
    CHECK(etcp_send(ep[0].conn, entry) == 0); run_for(1000); CHECK(ep[1].received == 1);
    ep[1].block_id = 0; run_for(10000);
    CHECK(broken->link_status && !ep[0].down && !ep[1].down && !ep[0].reinit && !ep[1].reinit);
    teardown();
}

/* Настоящий wire INIT; caller останавливает фоновый RX перед синхронным чтением. */
static size_t send_init_request_body(const struct ETCP_INIT_REQUEST_PKT *request, unsigned length, unsigned flag) {
    uint8_t plain[ETCP_ENCRYPTED_HDR_SIZE + ETCP_INIT_REQ_SIZE + 32] = {0};
    CHECK(length <= sizeof(plain) - ETCP_ENCRYPTED_HDR_SIZE);
    plain[2] = flag;
    unsigned copied = length < sizeof(*request) ? length : sizeof(*request);
    if (copied) memcpy(plain + ETCP_ENCRYPTED_HDR_SIZE, request, copied);
    sc_context_t crypto;
    CHECK(sc_init_ctx(&crypto, &ep[0].inst->my_keys) == SC_OK);
    CHECK(sc_set_peer_public_key(&crypto, ep[1].inst->my_keys.public_key, SC_PEER_PUBKEY_BIN) == SC_OK);
    uint8_t wire[sizeof(plain) + SC_ENCRYPT_OVERHEAD + SC_PUBKEY_ENC_SIZE]; size_t encrypted = 0;
    CHECK(sc_encrypt(&crypto, plain, ETCP_ENCRYPTED_HDR_SIZE + length, wire, &encrypted) == SC_OK);
    uint8_t salt[SC_PUBKEY_ENC_SALT_SIZE] = {1,2,3,4,5,6,7,8};
    memcpy(wire + encrypted, salt, sizeof(salt));
    CHECK(sc_obfuscate_pubkey(salt, ep[1].inst->my_keys.public_key, ep[0].inst->my_keys.public_key,
        wire + encrypted + sizeof(salt)) == SC_OK);
    size_t size = encrypted + SC_PUBKEY_ENC_SIZE;
    CHECK(socket_sendto(ep[0].socket->fd, wire, size, (const struct sockaddr*)&ep[1].socket->interface_addr,
        sizeof(struct sockaddr_in)) == (ssize_t)size);
    etcp_connections_read_callback_socket(ep[1].socket->fd, ep[1].socket);
    return size;
}

struct accept_case {
    struct NODE_CONN_DIRECT *owner;
    struct ETCP_LINK *replacement;
    unsigned with_owner, with_link, fired, indexed, closed;
    int open_result;
};
static void accept_owner_event(struct NODE_CONN_DIRECT *h, enum ncd_event event, void *arg) {
    (void)h;
    struct accept_case *test = arg;
    if (event == NCD_EVENT_CLOSED) test->closed++;
}
static void cancel_accept(struct ETCP_CONN *conn, struct ETCP_LINK *link, int old_state, int old_status, void *arg) {
    (void)old_status;
    struct accept_case *test = arg;
    if (test->fired || old_state != -1 || !link->is_server) return;
    test->fired++;
    ep[1].conn = conn; CHECK(etcp_conn_ref_take(conn) == 0);
    test->indexed = instance_find_conn(conn->instance, conn->peer_node_id) == conn;
    struct ETCP_SOCKET *socket = link->conn;
    struct sockaddr_storage remote = link->remote_addr;
    etcp_link_close(link);
    if (test->with_link) { test->replacement = etcp_link_new(conn, socket, &remote, 0); CHECK(test->replacement); }
    if (test->with_owner) {
        /* Адресов пока нет: владелец удерживает входящий транспорт и ждёт будущего линка. */
        struct TOPO_NODE peer = {.node_id = ep[0].inst->node_id};
        memcpy(peer.public_key, ep[0].inst->my_keys.public_key, SC_PUBKEY_SIZE);
        test->open_result = node_conn_direct_open_node(conn->instance, peer.node_id, accept_owner_event, test,
            &test->owner, &peer, socket);
        CHECK(test->open_result != NCD_ERR && test->owner);
    }
}
static void cancelled_accept(unsigned swapped, unsigned with_owner, unsigned with_link) {
    setup(swapped, 0);
    packet_transport_receive(ep[1].socket->transport, 0);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "INIT cancelled accept: RX owned by manual socket reader");
    struct accept_case test = {.with_owner = with_owner, .with_link = with_link};
    etcp_add_link_status_cbk(ep[1].inst, cancel_accept, &test);
    struct ETCP_INIT_REQUEST_PKT request = {.code = ETCP_INIT_REQUEST, .link_id = 1};
    memcpy(request.ed25519_pubkey, ep[0].inst->my_ed25519_pubkey, SC_PUBKEY_SIZE);
    request.mtu[0] = ep[0].socket->mtu >> 8; request.mtu[1] = ep[0].socket->mtu;
    send_init_request_body(&request, sizeof(request), 0);
    etcp_remove_link_status_cbk(ep[1].inst, cancel_accept, &test);
    CHECK(test.fired == 1 && ep[1].conn);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "UDP cancelled accept swapped=%u other_link=%u owner=%u: indexed=%u closing=%u owner_conn=%p conn=%p",
        swapped, with_link, with_owner, test.indexed, ep[1].conn->close_requested,
        (void*)node_conn_direct_get_conn(test.owner), (void*)ep[1].conn);
    CHECK(test.indexed && !late_handshakes);
    if (with_owner || with_link) {
        CHECK(!ep[1].conn->close_requested && instance_find_conn(ep[1].inst, ep[0].inst->node_id) == ep[1].conn);
        CHECK(ep[1].inst->connections->count == 1 && !test.closed);
        if (with_owner) CHECK(test.open_result == NCD_REUSED && node_conn_direct_get_conn(test.owner) == ep[1].conn);
        if (with_link) CHECK(ep[1].conn->links == test.replacement && !test.replacement->closing);
        packet_transport_receive(ep[1].socket->transport, 1);
        start_source(1); run_for(10000);
        CHECK(ep[0].conn->links_up && ep[1].conn->links_up && !test.closed);
        for (unsigned side = 0; side < 2; side++) {
            struct ll_entry *entry = ll_alloc_lldgram(2); CHECK(entry);
            entry->len = 2; entry->dgram[0] = 253; entry->dgram[1] = 17;
            CHECK(etcp_send(ep[side].conn, entry) == 0);
        }
        run_for(10000); CHECK(ep[0].received == 1 && ep[1].received == 1);
    } else CHECK(ep[1].conn->close_requested);
    node_conn_direct_force_close(test.owner);
    teardown();
}

static void request_boundaries(unsigned established) {
    setup(0, established);
    packet_transport_receive(ep[1].socket->transport, 0);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "INIT boundaries: RX owned by manual socket reader");
    unsigned drained = 0; uint8_t pending;
    while (recv(ep[1].socket->fd, (char*)&pending, 1, MSG_PEEK) >= 0) {
        etcp_connections_read_callback_socket(ep[1].socket->fd, ep[1].socket); drained++;
    }
    CHECK(socket_get_error() == ERR_WOULDBLOCK || socket_get_error() == ERR_AGAIN);
    DEBUG_DEBUG(DEBUG_CATEGORY_DEBUG, "INIT boundary fixture: established=%u drained_previous_packets=%u", established, drained);
    struct ETCP_INIT_REQUEST_PKT request = {0};
    for (unsigned i = 0; i < 8; i++) {
        request.reset_id[i] = (ep[0].conn ? ep[0].conn->reset_id : UINT64_C(0x1020304050607080)) >> (56 - i * 8);
    }
    request.mtu[0] = ep[0].socket->mtu >> 8; request.mtu[1] = ep[0].socket->mtu;
    request.keepalive[0] = ep[0].inst->keepalive_interval >> 8;
    request.keepalive[1] = ep[0].inst->keepalive_interval;
    request.link_id = established ? first_link(0)->local_link_id : 1;
    request.socket_id = ep[0].socket->sock_id; request.type = ep[0].socket->type;
    request.device_type = ep[0].inst->client_type;
    memcpy(request.ed25519_pubkey, ep[0].inst->my_ed25519_pubkey, SC_PUBKEY_SIZE);
    /* Прогреть пул, чтобы отказ первого пакета не менял размер пула. */
    void *warm = memory_pool_alloc(ep[1].inst->pkt_pool); CHECK(warm); memory_pool_free(ep[1].inst->pkt_pool, warm);
    unsigned rejected = 0, accepted = 0;
    for (unsigned variant = 0; variant < 2; variant++) {
        request.code = variant ? ETCP_INIT_REQUEST_NOINIT : ETCP_INIT_REQUEST;
        for (unsigned invalid = 0; invalid < sizeof(request) + sizeof(request.ed25519_pubkey) + 5; invalid++) {
            unsigned length = invalid < sizeof(request) ? invalid : sizeof(request);
            unsigned fault = invalid < sizeof(request) ? 0 : invalid - sizeof(request);
            uint8_t saved_mtu[2]; memcpy(saved_mtu, request.mtu, 2);
            uint8_t saved_id = request.link_id;
            if (invalid >= sizeof(request) && fault < SC_PUBKEY_SIZE) request.ed25519_pubkey[fault] ^= 1;
            if (invalid >= sizeof(request) && fault >= SC_PUBKEY_SIZE && fault < SC_PUBKEY_SIZE + 4) {
                const uint16_t values[] = {0, 1, 100, ETCP_MIN_UDP_MTU(AF_INET) - 1};
                request.mtu[0] = values[fault - SC_PUBKEY_SIZE] >> 8; request.mtu[1] = values[fault - SC_PUBKEY_SIZE];
            }
            if (invalid >= sizeof(request) && fault == SC_PUBKEY_SIZE + 4) request.link_id = 0;
            struct ETCP_CONN before_conn; struct ETCP_LINK before_link;
            if (established) { memcpy(&before_conn, ep[1].conn, sizeof(before_conn)); memcpy(&before_link, first_link(1), sizeof(before_link)); }
            int connections = ep[1].inst->connections->count, pool_free = ep[1].inst->pkt_pool->free_count;
            size_t errors = ep[1].socket->pkt_format_errors;
            size_t received_size = send_init_request_body(&request, length, 0);
            if (invalid >= sizeof(request) && fault < SC_PUBKEY_SIZE) request.ed25519_pubkey[fault] ^= 1;
            memcpy(request.mtu, saved_mtu, 2); request.link_id = saved_id;
            DEBUG_DEBUG(DEBUG_CATEGORY_DEBUG, "INIT_REQUEST boundary: established=%u code=%u body=%u errors=%zu->%zu",
                established, request.code, length, errors, ep[1].socket->pkt_format_errors);
            CHECK(ep[1].socket->pkt_format_errors == errors + 1 && ep[1].socket->errorcode != 0);
            CHECK(ep[1].inst->connections->count == connections && ep[1].inst->pkt_pool->free_count == pool_free);
            if (established) {
                /* Граница драйвера учитывает также отклонённый wire; протокольное состояние остаётся прежним. */
                before_conn.diagnostic_wire_rx_bytes += received_size;
                before_link.diagnostic_wire_rx_bytes += received_size;
                CHECK(!memcmp(&before_conn, ep[1].conn, sizeof(before_conn)));
                CHECK(!memcmp(&before_link, first_link(1), sizeof(before_link)));
            }
            rejected++;
        }
    }
    for (unsigned variant = 0; variant < 2; variant++) {
        request.code = variant ? ETCP_INIT_REQUEST_NOINIT : ETCP_INIT_REQUEST;
        const unsigned lengths[] = {sizeof(request), sizeof(request) + 1, sizeof(request) + 32};
        for (unsigned i = 0; i < 3; i++) for (unsigned flag = 0; flag < 2; flag++) {
            size_t errors = ep[1].socket->pkt_format_errors;
            send_init_request_body(&request, lengths[i], flag);
            CHECK(ep[1].socket->pkt_format_errors == errors);
            if (!ep[1].conn) {
                ep[1].conn = instance_find_conn(ep[1].inst, ep[0].inst->node_id); CHECK(ep[1].conn);
                CHECK(etcp_conn_ref_take(ep[1].conn) == 0);
            }
            struct ETCP_LINK *link = first_link(1); CHECK(link);
            CHECK(link->initialized && link->remote_keepalive == flag && link->recv_keepalive);
            accepted++;
        }
        const uint16_t mtus[] = {ETCP_MIN_UDP_MTU(AF_INET), PACKET_DATA_MAX_MTU, UINT16_MAX};
        for (unsigned i = 0; i < 3; i++) {
            request.mtu[0] = mtus[i] >> 8; request.mtu[1] = mtus[i];
            size_t errors = ep[1].socket->pkt_format_errors;
            send_init_request_body(&request, sizeof(request), 1);
            CHECK(ep[1].socket->pkt_format_errors == errors);
            CHECK(first_link(1)->mtu_remote == (mtus[i] > PACKET_DATA_MAX_MTU ? PACKET_DATA_MAX_MTU : mtus[i]));
            CHECK(ep[1].conn->normalizer->frag_size >= 3 && ep[1].conn->normalizer->frag_size < PACKET_DATA_SIZE);
            accepted++;
        }
    }
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "INIT_REQUEST boundaries established=%u: rejected=%u accepted=%u", established, rejected, accepted);
    teardown();
}

/* Граница тела не включает timestamp/flag_up; отказ не меняет линк, соединение и владение pkt. */
static void response_boundaries(void) {
    setup(0, 1);
    struct ETCP_LINK* link = first_link(0);
    struct ETCP_LINK* peer = first_link(1);
    struct ETCP_INIT_RESPONSE_PKT response = {0};
    for (unsigned i = 0; i < 8; i++) {
        response.reset_id[i] = ep[1].conn->reset_id >> (56 - 8 * i);
    }
    response.mtu[0] = peer->mtu_local >> 8; response.mtu[1] = peer->mtu_local;
    response.link_id = peer->local_link_id; response.remote_socket_id = ep[1].socket->sock_id;
    response.device_type = ep[1].inst->client_type;
    response.keepalive[0] = ep[1].inst->keepalive_interval >> 8;
    response.keepalive[1] = ep[1].inst->keepalive_interval;
    memcpy(response.ed25519_pubkey, ep[1].inst->my_ed25519_pubkey, SC_PUBKEY_SIZE);
    unsigned rejected = 0, accepted = 0;
    for (unsigned variant = 0; variant < 2; variant++) {
        response.code = variant ? ETCP_INIT_RESPONSE_NOINIT : ETCP_INIT_RESPONSE;
        for (unsigned invalid = 1; invalid <= sizeof(response) + 1 + 5; invalid++) {
            unsigned len = invalid <= sizeof(response) + 1 ? invalid : sizeof(response);
            struct ETCP_DGRAM* pkt = memory_pool_alloc(ep[0].inst->pkt_pool); CHECK(pkt);
            memcpy(pkt->data, &response, sizeof(response)); pkt->data[sizeof(response)] = 0;
            if (invalid <= sizeof(response) + 1 && len >= sizeof(response)) pkt->data[1] ^= 1;
            if (invalid > sizeof(response) + 1) {
                unsigned fault = invalid - sizeof(response) - 2;
                struct ETCP_INIT_RESPONSE_PKT *bad = (struct ETCP_INIT_RESPONSE_PKT*)pkt->data;
                if (fault == 4) bad->link_id = 0;
                else {
                    const uint16_t values[] = {0, 1, 100, ETCP_MIN_UDP_MTU(AF_INET) - 1};
                    bad->mtu[0] = values[fault] >> 8; bad->mtu[1] = values[fault];
                }
            }
            pkt->timestamp = 123; pkt->flag_up = 0;
            struct ETCP_LINK before_link;
            struct ETCP_CONN before_conn;
            memcpy(&before_link, link, sizeof(before_link));
            memcpy(&before_conn, ep[0].conn, sizeof(before_conn));
            int free_count = ep[0].inst->pkt_pool->free_count;
            int result = etcp_packet_decrypted(ep[0].socket, pkt, link, len + ETCP_ENCRYPTED_HDR_SIZE);
            DEBUG_DEBUG(DEBUG_CATEGORY_DEBUG, "INIT_RESPONSE validation code=%u body=%u result=%d", response.code, len, result);
            CHECK(result != 0 && ep[0].inst->pkt_pool->free_count == free_count);
            CHECK(!memory_pool_is_freed(ep[0].inst->pkt_pool, pkt));
            CHECK(!memcmp(&before_link, link, sizeof(before_link)) && !memcmp(&before_conn, ep[0].conn, sizeof(before_conn)));
            memory_pool_free(ep[0].inst->pkt_pool, pkt); rejected++;
        }
        const unsigned lengths[] = {sizeof(response), sizeof(response) + 1, sizeof(response) + 32};
        for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) for (unsigned flag = 0; flag < 2; flag++) {
            struct ETCP_DGRAM* pkt = memory_pool_alloc(ep[0].inst->pkt_pool); CHECK(pkt);
            memset(pkt->data, 0, lengths[i]); memcpy(pkt->data, &response, sizeof(response));
            pkt->timestamp = 456; pkt->flag_up = flag;
            uint64_t epoch = ep[0].conn->reset_id;
            CHECK(etcp_packet_decrypted(ep[0].socket, pkt, link, lengths[i] + ETCP_ENCRYPTED_HDR_SIZE) == 0);
            CHECK(link->recv_keepalive && link->remote_keepalive == flag && link->link_status == flag);
            CHECK(link->last_recv_timestamp == 456 && !ep[0].conn->callbacks_running);
            CHECK(ep[0].conn->reset_id == epoch && !ep[0].reinit); accepted++;
        }
        const uint16_t mtus[] = {ETCP_MIN_UDP_MTU(AF_INET), PACKET_DATA_MAX_MTU, UINT16_MAX};
        for (unsigned i = 0; i < 3; i++) {
            struct ETCP_DGRAM *pkt = memory_pool_alloc(ep[0].inst->pkt_pool); CHECK(pkt);
            memcpy(pkt->data, &response, sizeof(response)); pkt->timestamp = 456; pkt->flag_up = 1;
            struct ETCP_INIT_RESPONSE_PKT *valid = (struct ETCP_INIT_RESPONSE_PKT*)pkt->data;
            valid->mtu[0] = mtus[i] >> 8; valid->mtu[1] = mtus[i];
            CHECK(etcp_packet_decrypted(ep[0].socket, pkt, link, sizeof(response) + ETCP_ENCRYPTED_HDR_SIZE) == 0);
            CHECK(link->mtu_remote == (mtus[i] > PACKET_DATA_MAX_MTU ? PACKET_DATA_MAX_MTU : mtus[i]));
            CHECK(ep[0].conn->normalizer->frag_size >= 3 && ep[0].conn->normalizer->frag_size < PACKET_DATA_SIZE);
            accepted++;
        }
    }
    CHECK(ep[0].down == 6 && ep[0].up == 7);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "INIT_RESPONSE boundaries: rejected=%u accepted=%u (both codes, padding, flag_up=0/1)",
        rejected, accepted);
    teardown();
}

/* Удаление самого link из уведомления не оставляет RX со ссылкой на освобождённый объект. */
static void delete_in_callback(unsigned keepalive) {
    setup(0, 2); close_action = 3;
    struct ETCP_LINK* link = first_link(0);
    uint64_t epoch = ep[1].conn->reset_id;
    link->recv_keepalive = 0;
    if (keepalive) etcp_link_send_keepalive(link);
    else etcp_link_enter_reinit(link);
    link->recv_keepalive = 1;
    run_for(1000);
    CHECK(close_fired == 1 && ep[1].conn->links && !ep[1].conn->links->next && ep[1].conn->links_up == 1);
    CHECK(ep[1].conn->reset_id == epoch && !ep[1].reinit && !ep[1].down && !ep[1].conn->callbacks_running);
    struct ll_entry* entry = ll_alloc_lldgram(2); CHECK(entry);
    entry->len = 2; entry->dgram[0] = 253; entry->dgram[1] = 17;
    CHECK(etcp_send(ep[0].conn, entry) == 0); wait_alternate_delivery(1);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Callback deletes link: RX=%s, alternate delivery PASS", keepalive ? "keepalive" : "INIT");
    teardown();
}

struct transition_case {
    struct ETCP_CONN *retained;
    struct ETCP_LINK *replacement;
    unsigned stage, action, fired;
};
static void close_transition(struct ETCP_CONN *conn, struct ETCP_LINK *link, int old_state, int old_status, void *arg) {
    struct transition_case *test = arg;
    if (test->fired) return;
    int selected = test->stage == 2 ? old_status && !link->link_status :
        old_state == LINK_STATE_CONNECTED && link->link_state == (test->stage ? LINK_STATE_TRY_RECONNECT : LINK_STATE_HANDSHAKE);
    if (!selected) return;
    test->fired++;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "UDP transition callback stage=%u action=%u path=%u active_users=%u",
        test->stage, test->action, link->local_link_id, link->active_users);
    CHECK(link->active_users > 0 && conn->callbacks_running > 0);
    if (test->action == 1) { etcp_connection_close(conn); return; }
    struct ETCP_SOCKET *socket = link->conn;
    struct sockaddr_storage remote = link->remote_addr;
    etcp_link_close(link);
    CHECK(link->closing && link->active_users > 0); /* Память заимствована на весь callback. */
    if (test->action == 2) {
        test->replacement = etcp_link_new(conn, socket, &remote, 0); CHECK(test->replacement);
        CHECK(test->replacement != link); /* Удалённый объект не переиспользуется внутри активного стека. */
    }
}
static void transition_callbacks(unsigned swapped, unsigned stage, unsigned action) {
    setup(swapped, 2);
    unsigned side = stage == 2 ? 1 : 0;
    struct transition_case test = {.retained = ep[side].conn, .stage = stage, .action = action};
    CHECK(etcp_conn_ref_take(test.retained) == 0);
    etcp_add_link_status_cbk(ep[side].inst, close_transition, &test);
    if (stage == 2) {
        ep[0].block_id = first_link(0)->local_link_id;
        uint64_t end = get_time_tb() + 15000;
        while (!test.fired && get_time_tb() < end) test_clock_poll(ua, 10);
    } else if (stage == 1) etcp_link_enter_reinit(first_link(0));
    else etcp_link_enter_init(first_link(0));
    CHECK(test.fired == 1 && !late_handshakes);
    if (test.replacement) {
        CHECK(test.replacement->link_state == LINK_STATE_HANDSHAKE && !test.replacement->recv_keepalive);
        CHECK(!test.replacement->keepalive_timer && test.replacement->init_timer && !test.replacement->active_users);
    }
    run_for(100);
    CHECK(!late_handshakes && !test.retained->callbacks_running);
    if (action == 1) CHECK(test.retained->state == 2 && !test.retained->links && !test.retained->normalizer);
    else {
        CHECK(test.retained->links_up && !test.retained->close_requested && !ep[side].down && !ep[side].reinit);
        ep[0].block_id = ep[1].block_id = 0;
        struct ll_entry *entry = ll_alloc_lldgram(2); CHECK(entry);
        entry->dgram[0] = 253; entry->dgram[1] = 17; entry->len = 2;
        CHECK(etcp_send(ep[0].conn, entry) == 0); wait_alternate_delivery(1);
        for (struct ETCP_LINK *link = test.retained->links; link; link = link->next) CHECK(!link->active_users);
    }
    etcp_remove_link_status_cbk(ep[side].inst, close_transition, &test);
    etcp_conn_ref_free(test.retained);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS UDP transition swapped=%u stage=%u action=%u: no stale use/handshake, alternate delivery",
        swapped, stage, action);
    teardown();
}

/* Общий ETCP_CONN не несёт политики BLE; настоящие UDP/AES-CCM работают без CHAT-мемберства. */
static void native_socket_admission(void) {
    reuse_connection = 1; setup(0, 1);
    for (unsigned side = 0; side < 2; ++side) {
        CHECK(ep[side].conn->initialized && ep[side].conn->links_up);
        CHECK(!first_link(side)->require_known_node);
        CHECK(!topo_groups_peer_key_known(ep[side].inst, ep[1-side].inst->my_keys.public_key));
        struct ll_entry* entry = ll_alloc_lldgram(2); CHECK(entry);
        entry->len = 2; entry->dgram[0] = 253; entry->dgram[1] = 17;
        CHECK(!etcp_send(ep[side].conn, entry));
    }
    run_for(1000); CHECK(ep[0].received == 1 && ep[1].received == 1);
    teardown(); reuse_connection = 0;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS native socket policy: unknown CHAT peers, shared conn, encrypted duplex UDP");
}

/* При отказе показать конкретные живые блоки вместо одного несовпавшего счётчика. */
static void check_memory(size_t baseline) {
    if (u_get_allocated_count() == baseline) return;
    DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "Memory imbalance: baseline=%zu actual=%zu", baseline, u_get_allocated_count());
    debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_DEBUG);
    u_report_unfreed_blocks();
    CHECK(u_get_allocated_count() == baseline);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_category_level(DEBUG_CATEGORY_CRYPTO, DEBUG_LEVEL_DISABLED);
    // На loopback после пустого poll достаточно шага до 10мс. Часы останавливаются на ближайшем таймере.
    test_clock_init(100); CHECK(socket_platform_init() == 0);
    if (getenv("UTUN_TEST_DEBUG")) {
        debug_set_category_level(DEBUG_CATEGORY_ETCP, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_DEBUG);
    }
    size_t baseline = u_get_allocated_count();
    native_socket_admission(); check_memory(baseline);
    for (unsigned swapped = 0; swapped < 2; swapped++) for (unsigned owner = 0; owner < 2; owner++)
        for (unsigned link = 0; link < 2; link++) { cancelled_accept(swapped, owner, link); check_memory(baseline); }
    for (unsigned swapped = 0; swapped < 2; swapped++) for (unsigned stage = 0; stage < 3; stage++)
        for (unsigned action = 0; action < 3; action++) { transition_callbacks(swapped, stage, action); check_memory(baseline); }
    for (unsigned established = 0; established < 2; established++) { request_boundaries(established); check_memory(baseline); }
    response_boundaries(); check_memory(baseline);
    for (unsigned swapped = 0; swapped < 2; swapped++) for (unsigned order = 0; order < 2; order++) {
        one_way(swapped, 1 - order); check_memory(baseline);
    }
    for (unsigned keepalive = 0; keepalive < 2; keepalive++) for (unsigned action = 1; action <= 2; action++) {
        close_in_callback(action, keepalive); check_memory(baseline);
    }
    for (unsigned keepalive = 0; keepalive < 2; keepalive++) { delete_in_callback(keepalive); check_memory(baseline); }
    healthy_alternative(); check_memory(baseline);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS encrypted INIT liveness: cold start, one-way loss, repeated INIT, recovery and callback close");
    test_clock_finish(); socket_platform_cleanup(); return 0;
}
