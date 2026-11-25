/* Входящий ETCP: реальные очереди, таймеры, NCD и управляемые часы без сетевого I/O. */
#include <stdlib.h>
#include <string.h>
#include "etcp.h"
#include "etcp_api.h"
#include "etcp_keepalive.h"
#include "etcp_link_io.h"
#include "node_conn_direct.h"
#include "utun_instance.h"
#include "config_parser.h"
#include "test_clock.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "FAIL %s:%d: %s", __func__, __LINE__, #x); abort(); } } while (0)

static struct UTUN_INSTANCE inst;
static struct utun_config cfg;
static struct ETCP_SOCKET sock;
static unsigned deleted, closed, down;
static int release_on_down;
static int send_result;

/* Тестируем жизненный цикл без сетевого I/O; результат принятия пакета задаёт фикстура. */
static int fixture_send(struct ETCP_DGRAM* packet) {
    CHECK(packet && packet->data_len);
    DEBUG_TRACE(DEBUG_CATEGORY_DEBUG, "Lifecycle send: link=%u bytes=%u result=%d",
        packet->link->local_link_id, packet->data_len, send_result);
    return send_result < 0 ? send_result : (int)packet->data_len;
}
static const struct etcp_link_codec fixture_codec = {.send = fixture_send};

static void deleted_cb(struct ETCP_CONN* conn, int event, void* arg) {
    (void)conn; (void)event; (void)arg; deleted++;
}

/* Освобождение последнего владельца внутри DOWN не сокращает срок сохранения ETCP. */
static void owner_cb(struct NODE_CONN_DIRECT* handle, enum ncd_event event, void* arg) {
    (void)arg;
    if (event == NCD_EVENT_DOWN) {
        down++;
        if (release_on_down) node_conn_direct_close(handle);
    } else if (event == NCD_EVENT_CLOSED) {
        closed++; CHECK(node_conn_direct_get_conn(handle) == NULL);
    }
}

static void setup(void) {
    memset(&inst, 0, sizeof(inst)); memset(&cfg, 0, sizeof(cfg)); memset(&sock, 0, sizeof(sock));
    deleted = closed = down = 0; release_on_down = send_result = 0;
    inst.node_id = UINT64_C(0x0123456789abcdef);
    memset(inst.my_ed25519_pubkey, 11, 32);
    inst.ua = uasync_create(); CHECK(inst.ua); inst.config = &cfg;
    cfg.global.bbr_max_cwnd = 100000; cfg.global.keepalive_interval = 200; cfg.global.keepalive_timeout = 2000;
    inst.connections = queue_new(inst.ua, 16, offsetof(struct conn_queue_entry, x25519), SC_PUBKEY_SIZE, "server lifecycle"); CHECK(inst.connections);
    inst.pkt_pool = memory_pool_init(sizeof(struct ETCP_DGRAM) + PACKET_DATA_SIZE, "server lifecycle packets"); CHECK(inst.pkt_pool);
    sock.instance = &inst; strcpy(sock.name, "synthetic server");
    CHECK(init_connections(&inst) == 0); // чисто входящий узел тоже принимает NCD CLOSE
    CHECK(inst.ncd_control_bound && !inst.ncd_registry);
}

static void teardown(void) {
    while (inst.connections->head) {
        struct conn_queue_entry* entry = (void*)inst.connections->head->data;
        etcp_connection_close(entry->conn);
    }
    uasync_drain_immediate(inst.ua); CHECK(!inst.ncd_registry);
    while (inst.activity_cbks) utun_remove_activity_cbk(&inst, inst.activity_cbks->fn, inst.activity_cbks->arg);
    queue_free(inst.connections); memory_pool_destroy(inst.pkt_pool); uasync_poll(inst.ua, 0);
    CHECK(inst.ua->timer_alloc_count == inst.ua->timer_free_count);
    uasync_destroy(inst.ua, 0);
}

static struct ETCP_CONN* new_conn(void) {
    struct ETCP_CONN* conn = etcp_connection_create(&inst, "incoming"); CHECK(conn);
    /* Повторное соединение представляет тот же транспортный ключ тестового пира. */
    memset(conn->crypto_ctx.peer_public_key, 42, SC_PUBKEY_SIZE);
    memset(conn->peer_ed25519_pubkey, 7, 32);
    conn->peer_node_id = 42; etcp_conn_add_cbk(conn, deleted_cb, NULL, ETCP_CBK_EVENT_DELETE);
    queue_set_callback(conn->send_input_q, NULL, NULL);
    return conn;
}

/* Фикстура владеет только сокетом; линки выделены штатно освобождаемой памятью ETCP. */
static struct ETCP_LINK* add_link(struct ETCP_CONN* conn, int tcp) {
    struct ETCP_LINK* link = u_calloc(1, sizeof(*link)); CHECK(link);
    link->etcp = conn; link->conn = &sock; link->is_server = 1; link->is_tcp = tcp;
    link->codec = &fixture_codec;
    link->initialized = link->link_status = link->recv_keepalive = link->remote_keepalive = 1;
    link->link_state = LINK_STATE_CONNECTED; link->keepalive_interval = link->ka_period_ms = 200;
    link->keepalive_timeout = 2000; link->last_recv_local_time = get_time_tb();
    link->local_link_id = conn->links ? conn->links->local_link_id + 1 : 1;
    link->next = conn->links; conn->links = link;
    return link;
}

static struct ETCP_CONN* ready_conn(int tcp) {
    struct ETCP_CONN* conn = new_conn(); add_link(conn, tcp); etcp_conn_ready(conn);
    CHECK(conn->links_up && !conn->down_timer);
    return conn;
}

static void receive_control(struct ETCP_CONN* conn, uint8_t subcmd) {
    struct ncd_control_msg msg = { ETCP_ID_NCD_CONTROL, subcmd, {0} };
    memcpy(msg.ed25519, conn->peer_ed25519_pubkey, 32);
    struct ll_entry* entry = ll_alloc_lldgram(sizeof(msg)); CHECK(entry);
    memcpy(entry->dgram, &msg, sizeof(msg)); entry->len = sizeof(msg);
    inst.api_bindings.callbacks[ETCP_ID_NCD_CONTROL](conn, entry);
    uasync_drain_immediate(inst.ua);
}

/* Pending имеет конечный срок; внешняя ссылка задерживает только освобождение памяти. */
static void check_pending(void) {
    setup(); struct ETCP_CONN* conn = new_conn(); CHECK(etcp_conn_ref_take(conn) == 0);
    test_clock_run_for(inst.ua, ETCP_DOWN_TIMEOUT_TB - 10000); CHECK(!deleted && inst.connections->count == 1);
    test_clock_run_for(inst.ua, 20000); CHECK(deleted == 1 && conn->state == 2 && !conn->down_timer);
    CHECK(!inst.connections->count && !conn->free_scheduled);
    etcp_conn_ref_free(conn); teardown();
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS initial DOWN timeout and external reference");
}

/* Два пути, потеря последнего, неизменный deadline, восстановление и новая потеря. */
static void check_reconnect(void) {
    setup(); struct ETCP_CONN* conn = ready_conn(0); struct ETCP_LINK* second = add_link(conn, 1);
    etcp_link_close(second); CHECK(conn->links_up && !conn->down_timer);
    conn->links->link_status = 0; etcp_on_link_down(conn, conn->links); CHECK(conn->down_timer);
    void* timer = conn->down_timer;
    test_clock_run_for(inst.ua, 200000); etcp_on_link_down(conn, conn->links); CHECK(conn->down_timer == timer);
    conn->links->link_status = 1; conn->link_ready_for_send_fn(conn); CHECK(conn->links_up && !conn->down_timer);
    test_clock_run_for(inst.ua, ETCP_DOWN_TIMEOUT_TB + 10000); CHECK(!deleted && inst.connections->count == 1);
    etcp_link_close(conn->links); CHECK(!conn->links && !conn->links_up && conn->down_timer);
    test_clock_run_for(inst.ua, ETCP_DOWN_TIMEOUT_TB + 10000); CHECK(deleted == 1 && !inst.connections->count);
    teardown(); DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS multiple links, reconnect cancellation and fresh DOWN deadline");
}

/* Локальные handles защищают DOWN; release последнего учитывает уже прошедший срок. */
static void check_owner(int release) {
    setup(); struct ETCP_CONN* conn = ready_conn(0); struct NODE_CONN_DIRECT* handle = NULL;
    CHECK(node_conn_direct_open(&inst, 42, owner_cb, NULL, &handle, NULL) == NCD_REUSED);
    uasync_poll(inst.ua, 0); release_on_down = release;
    conn->links->link_status = 0; etcp_on_link_down(conn, conn->links);
    CHECK(down == 1 && !deleted && conn->down_timer && !conn->close_requested);
    test_clock_run_for(inst.ua, ETCP_DOWN_TIMEOUT_TB + 10000);
    if (!release) {
        CHECK(!deleted && !closed && !conn->down_timer && conn->down_deadline_tb && inst.connections->count == 1);
        CHECK(node_conn_direct_handle_count(conn) == 1);
        struct NODE_CONN_DIRECT* second = NULL;
        CHECK(node_conn_direct_open(&inst, 42, owner_cb, NULL, &second, NULL) == NCD_REUSED);
        CHECK(node_conn_direct_handle_count(conn) == 2);
        node_conn_direct_close(handle); CHECK(node_conn_direct_handle_count(conn) == 1 && !conn->close_requested);
        node_conn_direct_close(second); test_clock_next_timer(inst.ua);
    }
    CHECK(deleted == 1 && !closed && !inst.connections->count);
    teardown(); DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS DOWN timeout with local owner release_on_down=%d", release);
}

/* Закрытие handle до deadline не начинает новые 30 секунд ожидания. */
static void check_release_before_deadline(void) {
    setup(); struct ETCP_CONN* conn = ready_conn(0); struct NODE_CONN_DIRECT* handle = NULL;
    CHECK(node_conn_direct_open(&inst, 42, NULL, NULL, &handle, NULL) == NCD_REUSED);
    conn->links->link_status = 0; etcp_on_link_down(conn, conn->links);
    uint64_t deadline = conn->down_deadline_tb;
    test_clock_run_for(inst.ua, 100000); node_conn_direct_close(handle);
    CHECK(!deleted && node_conn_direct_handle_count(conn) == 0 && conn->down_deadline_tb == deadline);
    test_clock_run_for(inst.ua, 100000); CHECK(!deleted);
    test_clock_run_for(inst.ua, 110000); CHECK(deleted == 1 && !inst.connections->count);
    teardown(); DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS last handle release preserves original DOWN deadline");
}

/* RX продлевает watchdog, затем молчание даёт DOWN и ещё 30 секунд до DELETE. */
static void check_silent_peer(int tcp) {
    setup(); struct ETCP_CONN* conn = ready_conn(tcp); struct ETCP_LINK* link = conn->links;
    etcp_keepalive_watch_rx(link); CHECK(link->rx_timeout_timer);
    test_clock_run_for(inst.ua, 15000); link->last_recv_local_time = get_time_tb(); etcp_keepalive_watch_rx(link);
    test_clock_run_for(inst.ua, 10000); CHECK(conn->links_up && !conn->down_timer);
    test_clock_run_for(inst.ua, 15000); CHECK(!conn->links_up && conn->down_timer && !deleted);
    test_clock_run_for(inst.ua, ETCP_DOWN_TIMEOUT_TB + 10000); CHECK(deleted == 1 && !inst.connections->count);
    teardown(); DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS silent incoming peer tcp=%d", tcp);
}

/* Исходящий TCP без listen-сокета: keepalive с padding проходит общий RX и продлевает deadline. */
static void check_sockless_tcp_keepalive(void) {
    setup(); struct ETCP_CONN* conn = ready_conn(1); struct ETCP_LINK* link = conn->links;
    link->conn = NULL; link->is_server = 0;
    etcp_keepalive_watch_rx(link);
    const unsigned padding[] = { 0, 10, 11, 13, ETCP_PADDING_MAX };
    for (unsigned i = 0; i < sizeof(padding) / sizeof(padding[0]); i++) {
        test_clock_run_for(inst.ua, 10000); CHECK(conn->links_up && !conn->down_timer);
        struct ETCP_DGRAM* pkt = memory_pool_alloc(inst.pkt_pool); CHECK(pkt);
        memset(pkt, 0, sizeof(*pkt) + 4 + padding[i]); pkt->flag_up = 1;
        pkt->data[0] = ETCP_KEEPALIVE; pkt->data[2] = 200;
        CHECK(etcp_packet_decrypted(NULL, pkt, link, 7 + padding[i]) == 0);
        CHECK(link->keepalive_recv_count == i + 1 && link->rx_deadline_tb > get_time_tb());
        CHECK(!link->burst_recv_received && !conn->recv_q->count && !conn->down_timer);
    }
    test_clock_run_for(inst.ua, 10000); CHECK(conn->links_up && !conn->down_timer);
    test_clock_run_for(inst.ua, 15000); CHECK(!conn->links_up && conn->down_timer);
    teardown(); DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS TCP without listen socket: padded RX refreshes deadline; silence closes link");
}

/* Сон не даёт преждевременный DOWN, но локальные фазовые таймеры не продлевают жизнь пира. */
static void check_sleep(void) {
    setup(); struct ETCP_CONN* conn = ready_conn(1); struct ETCP_LINK* link = conn->links;
    struct ETCP_DGRAM* pkt = memory_pool_alloc(inst.pkt_pool); CHECK(pkt);
    memset(pkt, 0, sizeof(*pkt) + 8); pkt->link = link; pkt->data_len = 8;
    pkt->data[0] = ETCP_KEEPALIVE; pkt->data[1] = KA_FLAG_SLEEP;
    pkt->data[2] = 0xe8; pkt->data[3] = 3; pkt->data[4] = 5; pkt->data[6] = 200; // 1 s period, 0.5 s ACTIVE, 20 s SLEEP
    CHECK(etcp_keepalive_on_recv(pkt) == 1);
    CHECK(link->ka_peer_sleeping && !link->keepalive_timer && link->rx_timeout_timer);
    test_clock_run_for(inst.ua, 60000); CHECK(conn->links_up && !conn->down_timer);
    struct NODE_CONN_DIRECT* handle = NULL;
    CHECK(node_conn_direct_open(&inst, 42, NULL, NULL, &handle, NULL) == NCD_REUSED);
    node_conn_direct_close(handle); CHECK(conn->fin_wait);
    test_clock_run_for(inst.ua, 60000); CHECK(!deleted && conn->links_up && conn->fin_wait);
    receive_control(conn, NCD_SUBCMD_KEEP_ALIVE); CHECK(!conn->fin_wait);
    test_clock_run_for(inst.ua, 120000); CHECK(!conn->links_up && conn->down_timer && !deleted);
    test_clock_run_for(inst.ua, ETCP_DOWN_TIMEOUT_TB + 10000); CHECK(deleted == 1 && !inst.connections->count);
    teardown(); DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS sleeping peer deadline, FIN_WAIT and disappearance");
}

/* UDP flag_up=0 переводит весь транспорт в DOWN, обратный переход отменяет удаление. */
static void check_remote_down(void) {
    setup(); struct ETCP_CONN* conn = ready_conn(0); struct ETCP_LINK* link = conn->links;
    for (int up = 0; up <= 1; up++) {
        struct ETCP_DGRAM* pkt = memory_pool_alloc(inst.pkt_pool); CHECK(pkt);
        memset(pkt, 0, sizeof(*pkt) + 4); pkt->flag_up = up;
        pkt->data[0] = ETCP_KEEPALIVE; pkt->data[2] = 200;
        CHECK(etcp_packet_decrypted(&sock, pkt, link, 7) == 0);
        CHECK(conn->links_up == up && !!conn->down_timer == !up);
    }
    teardown(); DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS remote DOWN flag and recovered UDP transport");
}

/* Пробуждение сокращает RX timeout сразу, не оставляя таймер прежнего длинного сна. */
static void check_wake_deadline(void) {
    setup(); struct ETCP_CONN* conn = ready_conn(1); struct ETCP_LINK* link = conn->links;
    struct ETCP_DGRAM* pkt = memory_pool_alloc(inst.pkt_pool); CHECK(pkt);
    memset(pkt, 0, sizeof(*pkt) + 8); pkt->data_len = 8; pkt->data[0] = ETCP_KEEPALIVE; pkt->data[1] = KA_FLAG_SLEEP;
    pkt->data[2] = 0xe8; pkt->data[3] = 3; pkt->data[6] = 200;
    pkt->link = link; CHECK(etcp_keepalive_on_recv(pkt) == 1);
    test_clock_run_for(inst.ua, 10000); CHECK(conn->links_up && link->ka_peer_sleeping);
    pkt = memory_pool_alloc(inst.pkt_pool); CHECK(pkt); memset(pkt, 0, sizeof(*pkt) + 4);
    pkt->data_len = 4; pkt->data[0] = ETCP_KEEPALIVE; pkt->data[2] = 200;
    link->last_recv_local_time = get_time_tb();
    pkt->link = link; CHECK(etcp_keepalive_on_recv(pkt) == 1); CHECK(!link->ka_peer_sleeping);
    test_clock_run_for(inst.ua, 10000); CHECK(conn->links_up);
    test_clock_run_for(inst.ua, 20000); CHECK(!conn->links_up && conn->down_timer && !deleted);
    teardown(); DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS wake replaces long sleep deadline with normal timeout");
}

static void peer_phase_changed(struct ETCP_CONN* conn, int sleeping, void* arg) {
    unsigned* events = arg;
    CHECK(conn->instance == &inst && conn->peer_ed25519_pubkey[0] == 7);
    events[sleeping ? 1 : 0]++;
}

static void receive_keepalive(struct ETCP_LINK* link, uint8_t flags, uint16_t active) {
    struct ETCP_DGRAM* pkt = memory_pool_alloc(inst.pkt_pool); CHECK(pkt);
    memset(pkt, 0, sizeof(*pkt) + 8); pkt->link = link;
    pkt->data_len = flags & KA_FLAG_SLEEP ? 8 : 4;
    pkt->data[0] = ETCP_KEEPALIVE; pkt->data[1] = flags;
    pkt->data[2] = 200; pkt->data[4] = active & 0xff; pkt->data[5] = active >> 8;
    pkt->data[6] = 200; // 20 секунд сна
    link->last_recv_local_time = get_time_tb();
    CHECK(etcp_keepalive_on_recv(pkt) == 1);
}

/* Новый линк снимает сон всего соединения, включая ещё не наступившую SLEEP-фазу. */
static void check_peer_wake_new_link(int tcp, int active_window) {
    setup(); struct ETCP_CONN* conn = ready_conn(!tcp); struct ETCP_LINK* old = conn->links;
    unsigned events[2] = {0}; utun_add_peer_sleep_cbk(&inst, peer_phase_changed, events);
    receive_keepalive(old, KA_FLAG_SLEEP, active_window ? 5 : 0);
    CHECK(conn->peer_phase_timer && conn->peer_sleep_phase == !active_window);
    CHECK(events[1] == (unsigned)!active_window && !events[0]);
    /* REINIT потока сам по себе не означает пробуждение приложения. */
    etcp_conn_reinit_id(conn, "peer restart before wake", conn->reset_id);
    CHECK(conn->peer_phase_timer && conn->peer_sleep_phase == !active_window);
    struct ETCP_LINK* fresh = add_link(conn, tcp); start_keepalive_timer(fresh);
    CHECK(!fresh->ka_peer_sleeping && fresh->keepalive_timer);
    receive_keepalive(fresh, KA_FLAG_SLEEP_ACK, 0);
    CHECK(conn->peer_phase_timer && conn->peer_sleep_phase == !active_window);
    void* timer = fresh->keepalive_timer;
    receive_keepalive(fresh, 0, 0);
    CHECK(!conn->peer_sleep_phase && !conn->peer_phase_timer && fresh->keepalive_timer == timer);
    CHECK(events[0] == (unsigned)!active_window && old->ka_peer_sleeping);
    uint64_t allocations = inst.ua->timer_alloc_count;
    receive_keepalive(fresh, 0, 0);
    CHECK(inst.ua->timer_alloc_count == allocations && events[0] == (unsigned)!active_window);
    /* Частый RX не отодвигает TX; отменённый фазовый таймер не усыпляет снова. */
    unsigned packets = fresh->keepalive_sent_count;
    for (unsigned i = 0; i < 6; i++) {
        test_clock_run_for(inst.ua, 1000); receive_keepalive(fresh, 0, 0);
        CHECK(!conn->peer_sleep_phase && !conn->peer_phase_timer);
    }
    CHECK(fresh->keepalive_sent_count > packets && events[0] == (unsigned)!active_window);
    /* Пробуждение пира не выключает собственный standby на прежнем линке. */
    old->ka_my_sleeping = 1; receive_keepalive(old, 0, 0);
    CHECK(old->ka_my_sleeping && !old->ka_peer_sleeping && !old->keepalive_timer);
    CHECK(!conn->peer_phase_timer && events[0] == (unsigned)!active_window);
    utun_remove_peer_sleep_cbk(&inst, peer_phase_changed, events);
    teardown();
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS peer wake via new link tcp=%d active_window=%d: phase cancelled, TX preserved", tcp, active_window);
}

/* Потеря первого normal и запоздалый SLEEP исправляются следующим normal без ожидания конца сна. */
static void check_peer_wake_reordered(void) {
    setup(); struct ETCP_CONN* conn = ready_conn(0); struct ETCP_LINK* link = conn->links;
    receive_keepalive(link, KA_FLAG_SLEEP, 0);
    test_clock_run_for(inst.ua, 2000); // первый normal потерян в сети
    CHECK(conn->peer_sleep_phase && conn->peer_phase_timer);
    receive_keepalive(link, 0, 0);
    CHECK(!conn->peer_sleep_phase && !conn->peer_phase_timer && link->keepalive_timer);
    receive_keepalive(link, KA_FLAG_SLEEP, 0); // старый анонс пришёл после пробуждения
    CHECK(conn->peer_sleep_phase && conn->peer_phase_timer);
    test_clock_run_for(inst.ua, 2000);
    receive_keepalive(link, 0, 0);
    CHECK(!conn->peer_sleep_phase && !conn->peer_phase_timer && link->keepalive_timer);
    teardown(); DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS normal keepalive restores awake state after loss and reordered sleep");
}

/* Счётчик keepalive отражает только принятые транспортом пакеты, включая busy и ошибку. */
static void check_keepalive_send_result(void) {
    setup(); struct ETCP_CONN* conn = ready_conn(0); struct ETCP_LINK* link = conn->links;
    unsigned sent = link->keepalive_sent_count;
    for (send_result = -2; send_result <= 0; send_result++) {
        etcp_link_send_keepalive(link);
        CHECK(link->keepalive_sent_count == sent + (send_result == 0));
    }
    teardown(); DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS keepalive counts accepted send only; busy/error preserve counter");
}

/* Мягкое закрытие работает без NCD entry; KEEP_ALIVE сохраняет транспорт дольше прежних 5 секунд. */
static void check_graceful(void) {
    setup(); struct ETCP_CONN* conn = ready_conn(0);
    conn->peer_node_id = UINT64_C(0xfedcba9876543210);
    receive_control(conn, NCD_SUBCMD_CLOSE); CHECK(deleted == 1 && !inst.connections->count); teardown();
    setup(); conn = ready_conn(0); struct NODE_CONN_DIRECT* handle = NULL;
    CHECK(node_conn_direct_open(&inst, 42, NULL, NULL, &handle, NULL) == NCD_REUSED);
    receive_control(conn, NCD_SUBCMD_CLOSE); CHECK(!deleted && !conn->fin_wait);
    CHECK(conn->send_input_q->count == 1);
    struct ncd_control_msg* response = (void*)conn->send_input_q->head->dgram;
    CHECK(conn->send_input_q->head->len == sizeof(*response) && response->cmd == ETCP_ID_NCD_CONTROL && response->subcmd == NCD_SUBCMD_KEEP_ALIVE);
    CHECK(!memcmp(response->ed25519, inst.my_ed25519_pubkey, 32));
    node_conn_direct_close(handle); CHECK(conn->fin_wait);
    receive_control(conn, NCD_SUBCMD_KEEP_ALIVE); CHECK(!conn->fin_wait);
    test_clock_run_for(inst.ua, 70000); CHECK(!deleted && conn->links_up);
    receive_control(conn, NCD_SUBCMD_CLOSE); CHECK(deleted == 1 && !inst.connections->count); teardown();
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS inbound CLOSE, owned refusal and durable KEEP_ALIVE");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); test_clock_init(10000);
    debug_set_category_level(DEBUG_CATEGORY_CONNECTION, DEBUG_LEVEL_DEBUG);
    debug_set_category_level(DEBUG_CATEGORY_KEEPALIVE, DEBUG_LEVEL_DEBUG);
    debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
    size_t baseline = u_get_allocated_count();
    check_pending(); CHECK(u_get_allocated_count() == baseline);
    check_reconnect(); CHECK(u_get_allocated_count() == baseline);
    check_owner(0); CHECK(u_get_allocated_count() == baseline);
    check_owner(1); CHECK(u_get_allocated_count() == baseline);
    check_release_before_deadline(); CHECK(u_get_allocated_count() == baseline);
    check_silent_peer(0); CHECK(u_get_allocated_count() == baseline);
    check_silent_peer(1); CHECK(u_get_allocated_count() == baseline);
    check_sockless_tcp_keepalive(); CHECK(u_get_allocated_count() == baseline);
    check_sleep(); CHECK(u_get_allocated_count() == baseline);
    check_wake_deadline(); CHECK(u_get_allocated_count() == baseline);
    for (int tcp = 0; tcp <= 1; tcp++) for (int active = 0; active <= 1; active++) {
        check_peer_wake_new_link(tcp, active); CHECK(u_get_allocated_count() == baseline);
    }
    check_peer_wake_reordered(); CHECK(u_get_allocated_count() == baseline);
    check_keepalive_send_result(); CHECK(u_get_allocated_count() == baseline);
    check_remote_down(); CHECK(u_get_allocated_count() == baseline);
    check_graceful(); CHECK(u_get_allocated_count() == baseline);
    test_clock_finish(); return 0;
}
