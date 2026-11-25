/* Настоящие очереди/таймеры ETCP; управляемые часы, перехват только передачи и отказа аллокации. */
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "etcp.h"
#include "etcp_api.h"
#include "etcp_bbr.h"
#include "etcp_loadbalancer.h"
#include "etcp_session.h"
#include "config_parser.h"
#include "utun_instance.h"
#include "test_clock.h"
#include "topo_node_rtt.h"
#include "../lib/mem.h"

#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "FAIL %s:%d: %s", __func__, __LINE__, #x); exit(1); } } while (0)
static struct UASYNC* ua;
static struct UTUN_INSTANCE inst;
static struct utun_config cfg;
static struct ETCP_CONN* conn;
static struct ETCP_LINK *a, *b;
static unsigned sends, fail_alloc, fail_send, fail_tx_timer, bbr_calls, saw_cwnd_limited, saw_app_limited;
static unsigned fail_shaper_timer;
static unsigned fail_rto_timer;
static unsigned close_rtt_callback;
static unsigned pacer_clock_active, capture_shaper, shaper_arm_count, pacer_ready_count, pacer_ready_action;
static uint64_t pacer_now_tb;
static int captured_shaper_delay;
static timeout_callback_t captured_shaper_callback;
static void* captured_shaper_arg;
static unsigned fail_rx_data, fail_rx_fragment, fail_ack_entry;
static struct ETCP_LINK* last_sent_link;
static unsigned last_sent_request, last_sent_payload;
static struct bbr_rate_sample last_sample;
static uint32_t last_mss;
static unsigned fail_rx_message_at, fail_tx_inflight, fail_normalizer_timer, fail_normalizer_waiter, fail_session_timer_at;
static unsigned no_alloc, unexpected_alloc, fail_rng, session_sends, received, receive_action;
static uint8_t received_values[8];
static unsigned ack_clock_active, capture_ack_timer, fail_ack_timer;
static uint64_t ack_now_us;
static timeout_callback_t captured_ack_callback;
static void* captured_ack_arg;

/* Фиксируем только TB на время синхронной проверки pacer; uasync при этих часах не опрашивается. */
uint64_t __real_get_time_tb(void);
uint64_t __wrap_get_time_tb(void) {
    if (ack_clock_active) return ack_now_us / 100;
    return pacer_clock_active ? pacer_now_tb : __real_get_time_tb();
}

uint64_t __real_get_time_us(void);
uint64_t __wrap_get_time_us(void) { return ack_clock_active ? ack_now_us : __real_get_time_us(); }

void __real_topo_node_ping_update_rtt(struct TOPO_GROUPS*, const uint8_t*, uint16_t);
void __wrap_topo_node_ping_update_rtt(struct TOPO_GROUPS* groups, const uint8_t x[32], uint16_t rtt) {
    if (close_rtt_callback) { close_rtt_callback = 0; etcp_connection_close(conn); return; }
    __real_topo_node_ping_update_rtt(groups,x,rtt);
}

void* __real_uasync_call_soon(struct UASYNC*, void*, timeout_callback_t);
void* __wrap_uasync_call_soon(struct UASYNC* loop, void* arg, timeout_callback_t callback) {
    if (fail_normalizer_timer && conn && arg == conn->normalizer) { fail_normalizer_timer--; return NULL; }
    if (fail_normalizer_waiter && conn && arg == &conn->normalizer->input_waiter_handle) {
        fail_normalizer_waiter--; return NULL;
    }
    if (fail_tx_timer && arg == conn) { fail_tx_timer--; return NULL; }
    return __real_uasync_call_soon(loop, arg, callback);
}

void* __real_uasync_set_timeout(struct UASYNC*, int, void*, timeout_callback_t, const char*);
void* __wrap_uasync_set_timeout(struct UASYNC* loop, int delay, void* arg, timeout_callback_t callback, const char* name) {
    if (fail_rto_timer && !strcmp(name,"etcp_attempt_rto")) { fail_rto_timer--; return NULL; }
    if (capture_ack_timer && !strcmp(name, "etcp_ack_resp")) { captured_ack_callback = callback; captured_ack_arg = arg; }
    if (fail_ack_timer && !strcmp(name, "etcp_ack_resp")) { fail_ack_timer--; return NULL; }
    if (capture_shaper && name && !strcmp(name, "etcp_shaper")) {
        shaper_arm_count++; captured_shaper_delay = delay;
        captured_shaper_callback = callback; captured_shaper_arg = arg;
    }
    if (fail_session_timer_at && !strcmp(name, "etcp_session") && !--fail_session_timer_at) return NULL;
    if (fail_shaper_timer && !strcmp(name, "etcp_shaper")) { fail_shaper_timer--; return NULL; }
    if (fail_tx_timer && !strcmp(name, "etcp_tx")) { fail_tx_timer--; return NULL; }
    return __real_uasync_set_timeout(loop, delay, arg, callback, name);
}

int __wrap_etcp_encrypt_send(struct ETCP_DGRAM* pkt) {
    if (pkt->data_len == ETCP_SESSION_CONTROL_SIZE && pkt->data[0] >= ETCP_SESSION_HELLO &&
        pkt->data[0] <= ETCP_SESSION_CONFIRM) { session_sends++; return pkt->data_len; }
    CHECK(conn->tx_running && pkt->link && pkt->link->initialized && pkt->link->link_status);
    sends++;
    last_sent_link = pkt->link; last_sent_request = pkt->ack_request; last_sent_payload = pkt->payload_len;
    if (fail_send) { fail_send--; return -1; }
    return pkt->data_len;
}

void* __real_memory_pool_alloc_impl(struct memory_pool* pool, const char* location);
void* __wrap_memory_pool_alloc_impl(struct memory_pool* pool, const char* location) {
    if (no_alloc) { unexpected_alloc++; return NULL; }
    if (fail_tx_inflight && conn && pool == conn->inflight_pool) { fail_tx_inflight--; return NULL; }
    if (pool == inst.data_pool && fail_rx_data) { fail_rx_data--; return NULL; }
    if (fail_rx_fragment && conn && pool == conn->io_pool) { fail_rx_fragment--; return NULL; }
    if (pool == inst.ack_pool && fail_ack_entry) { fail_ack_entry--; return NULL; }
    if (pool == inst.pkt_pool && fail_alloc) { fail_alloc--; return NULL; }
    return __real_memory_pool_alloc_impl(pool, location);
}

struct ll_entry* __real_ll_alloc_lldgram(uint16_t);
struct ll_entry* __wrap_ll_alloc_lldgram(uint16_t len) {
    if (fail_rx_message_at && !--fail_rx_message_at) return NULL;
    return __real_ll_alloc_lldgram(len);
}
void* __real_u_malloc_impl(uint32_t, const char*);
void* __wrap_u_malloc_impl(uint32_t size, const char* where) {
    if (no_alloc) { unexpected_alloc++; return NULL; }
    if (fail_normalizer_waiter && strstr(where, "ll_queue.c:")) { fail_normalizer_waiter--; return NULL; }
    return __real_u_malloc_impl(size, where);
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t size, const char* where) {
    if (no_alloc) { unexpected_alloc++; return NULL; }
    return __real_u_calloc_impl(count, size, where);
}
void* __real_u_realloc_impl(void*, uint32_t, const char*);
void* __wrap_u_realloc_impl(void* ptr, uint32_t size, const char* where) {
    if (no_alloc) { unexpected_alloc++; return NULL; }
    return __real_u_realloc_impl(ptr, size, where);
}
int __real_random_bytes(uint8_t*, size_t);
int __wrap_random_bytes(uint8_t* data, size_t len) {
    if (fail_rng) { fail_rng--; return -1; }
    return __real_random_bytes(data, len);
}

void __real_bbr_main(struct bbr*, const struct bbr_rate_sample*, uint32_t*, uint32_t*, uint32_t, uint32_t, int);
void __wrap_bbr_main(struct bbr* model, const struct bbr_rate_sample* sample, uint32_t* cwnd,
                    uint32_t* pacing, uint32_t mss, uint32_t inflight, int limited) {
    bbr_calls++; saw_cwnd_limited |= !!limited; saw_app_limited |= !!sample->is_app_limited;
    last_sample = *sample;
    last_mss = mss;
    __real_bbr_main(model, sample, cwnd, pacing, mss, inflight, limited);
}

/* Счётчики сверяются с содержимым очередей, а не с ожидаемыми результатами самого алгоритма. */
static void check_accounting(void) {
    uint32_t attempts = 0;
    for (struct ETCP_LINK* l = conn->links; l; l = l->next) {
        uint32_t bytes = 0, packets = 0;
        for (struct ll_entry* e = l->attempts->head; e; e = e->next) { bytes += e->len; packets++; }
        CHECK(l->inflight_bytes == bytes && l->inflight_packets == packets);
        attempts += packets;
    }
    CHECK(conn->active_attempts == attempts);
    struct ll_queue* logical_queues[] = {conn->input_send_q, conn->input_wait_ack};
    for (unsigned i = 0; i < 2; i++) for (struct ll_entry* e = logical_queues[i]->head; e; e = e->next) {
        struct INFLIGHT_PACKET* packet = (struct INFLIGHT_PACKET*)e;
        unsigned active = 0;
        for (struct ETCP_LINK* link = conn->links; link; link = link->next)
            for (struct ll_entry* entry = link->attempts->head; entry; entry = entry->next)
                active += ((struct ETCP_TX_ATTEMPT*)entry)->seq == packet->seq;
        CHECK(packet->active_attempts == active);
        CHECK(packet->state == (i ? INFLIGHT_STATE_WAIT_ACK : INFLIGHT_STATE_WAIT_SEND));
    }
    CHECK(conn->unacked_bytes == conn->input_send_q->total_bytes + conn->input_wait_ack->total_bytes);
}

static void setup(void) {
    memset(&inst, 0, sizeof(inst)); memset(&cfg, 0, sizeof(cfg));
    sends = fail_alloc = fail_send = fail_tx_timer = bbr_calls = saw_cwnd_limited = saw_app_limited = 0;
    fail_shaper_timer = 0;
    fail_rto_timer = 0;
    close_rtt_callback = 0;
    pacer_clock_active = capture_shaper = shaper_arm_count = pacer_ready_count = pacer_ready_action = 0;
    captured_shaper_delay = 0; captured_shaper_callback = NULL; captured_shaper_arg = NULL;
    fail_rx_data = fail_rx_fragment = fail_ack_entry = 0;
    fail_rx_message_at = fail_tx_inflight = fail_normalizer_timer = fail_normalizer_waiter = fail_session_timer_at = 0;
    no_alloc = unexpected_alloc = fail_rng = session_sends = received = receive_action = 0;
    memset(received_values, 0, sizeof(received_values));
    ack_clock_active = capture_ack_timer = fail_ack_timer = 0;
    captured_ack_callback = NULL; captured_ack_arg = NULL;
    last_sent_link = NULL; last_sent_request = last_sent_payload = 0;
    ua = uasync_create(); CHECK(ua);
    inst.ua = ua; inst.node_id = 1; inst.config = &cfg; cfg.global.bbr_max_cwnd = INFLIGHT_LIM_MAX;
    inst.connections = queue_new(ua, 16, offsetof(struct conn_queue_entry, x25519), SC_PUBKEY_SIZE, "windows");
    inst.pkt_pool = memory_pool_init(sizeof(struct ETCP_DGRAM) + PACKET_DATA_SIZE, "window_packets");
    inst.data_pool = memory_pool_init(PACKET_DATA_SIZE, "window_data");
    inst.ack_pool = memory_pool_init(sizeof(struct ACK_PACKET), "window_acks");
    CHECK(inst.connections && inst.pkt_pool && inst.data_pool && inst.ack_pool);
    conn = etcp_connection_create(&inst, "windows"); CHECK(conn);
    conn->crypto_ctx.peer_public_key[0] = 2; conn->peer_node_id = 2; conn->links_up = 1;
    etcp_conn_ready(conn);
    a = u_calloc(1, sizeof(*a)); b = u_calloc(1, sizeof(*b)); CHECK(a && b);
    a->next = b; conn->links = a;
    unsigned id = 0;
    for (struct ETCP_LINK* l = a; l; l = l->next) {
        l->etcp = conn; l->local_link_id = ++id; l->initialized = l->link_status = 1;
        l->mtu = 1280; l->remote_addr.ss_family = AF_INET; l->inflight_lim_bytes = 65536;
        l->rtt_last = 1; CHECK(etcp_tx_link_init(l) == 0);
    }
    etcp_conn_on_inflight_lim_changed(conn);
}

static void teardown(void) {
    ack_clock_active = 0;
    check_accounting();
    topo_node_rtt_stop(&inst);
    etcp_connection_close(conn); conn = NULL;
    uasync_poll(ua, 0);
    queue_free(inst.connections);
    memory_pool_destroy(inst.pkt_pool); memory_pool_destroy(inst.data_pool); memory_pool_destroy(inst.ack_pool);
    uasync_poll(ua, 0);
    CHECK(ua->timer_alloc_count == ua->timer_free_count);
    uasync_destroy(ua, 0);
}

/* Вход отправителя уже после normalizer: фрагмент остаётся в реальной indexed send_q. */
static struct INFLIGHT_PACKET* enqueue(unsigned len) {
    struct INFLIGHT_PACKET* p = (struct INFLIGHT_PACKET*)queue_entry_new_from_pool(conn->inflight_pool); CHECK(p);
    p->seq = conn->next_tx_id++; p->state = INFLIGHT_STATE_WAIT_SEND;
    p->ll.len = len; p->ll.dgram = memory_pool_alloc(inst.data_pool); p->ll.dgram_pool = inst.data_pool;
    CHECK(p->ll.dgram); memset(p->ll.dgram, 0x5a, len);
    conn->unacked_bytes += len;
    CHECK(queue_data_put_with_index(conn->input_send_q, &p->ll) == 0);
    etcp_conn_wake_sender(conn);
    return p;
}

static struct ETCP_DGRAM* request(void) {
    struct ETCP_DGRAM* pkt = etcp_request_pkt(conn);
    if (pkt) etcp_tx_complete(pkt, 1); // ручной стенд принимает подготовленный пакет без сетевого send
    check_accounting();
    return pkt;
}

/* Wire/window-тесты моделируют уже ожидающий ACK, не меняя часы и не запуская sender через uasync. */
static void mature_acks(struct ETCP_LINK* link) {
    for (struct ll_entry* e = link->ack_q->head; e; e = e->next)
        ((struct ACK_PACKET*)e)->received_at_us = get_time_us() - ETCP_ACK_DELAY_US;
}

static void fill(struct ETCP_LINK* target, unsigned count, unsigned len) {
    int up_a = a->link_status, up_b = b->link_status;
    a->link_status = target == a; b->link_status = target == b;
    for (unsigned i = 0; i < count; i++) {
        enqueue(len);
        struct ETCP_DGRAM* pkt = request(); CHECK(pkt && pkt->link == target);
        memory_pool_free(inst.pkt_pool, pkt);
    }
    a->link_status = up_a; b->link_status = up_b;
}

static void ack(uint32_t seq) {
    struct INFLIGHT_PACKET* packet = (struct INFLIGHT_PACKET*)queue_find_data_by_index(conn->input_wait_ack, &seq);
    if (!packet) packet = (struct INFLIGHT_PACKET*)queue_find_data_by_index(conn->input_send_q, &seq);
    if (packet && packet->last_link) etcp_tx_ack(packet->last_link, seq, packet->last_attempt_id, 0, 0, 0);
    else etcp_ack_recv(conn, seq);
    check_accounting();
}
static struct ETCP_TX_ATTEMPT* last_attempt(struct INFLIGHT_PACKET* packet) {
    return (struct ETCP_TX_ATTEMPT*)queue_find_data_by_index(packet->last_link->attempts, &packet->last_attempt_id);
}
static void ack_all(void) {
    while (conn->input_wait_ack->head) ack(((struct INFLIGHT_PACKET*)conn->input_wait_ack->head)->seq);
}

/* Ждём наблюдаемое событие, а не точную границу миллисекундного таймера uasync. */
static void wait_sends(unsigned expected) {
    uint64_t end = get_time_tb() + 20000;
    while (sends < expected && get_time_tb() < end) test_clock_poll(ua, 1);
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "TX observed=%u expected=%u send_q=%d wait_ack=%d", sends, expected,
        conn->input_send_q->count, conn->input_wait_ack->count);
    CHECK(sends == expected); check_accounting();
}

/* В полном пути свободное окно другого линка не разрешает DATA через заполненный. */
static void selection(void) {
    setup(); fill(a, 8, 1024); fill(b, 16, 1024); a->inflight_lim_bytes = 8192;
    enqueue(512);
    struct ETCP_DGRAM* pkt = request(); CHECK(pkt && pkt->link == b);
    CHECK(a->inflight_bytes == 8192 && b->inflight_bytes == 16896);
    memory_pool_free(inst.pkt_pool, pkt);
    teardown();
}

static void boundaries_ack_burst(void) {
    setup(); b->link_status = 0; a->inflight_lim_bytes = 8192;
    fill(a, 7, 1024); enqueue(1024);
    struct ETCP_DGRAM* pkt = request(); CHECK(pkt && a->inflight_bytes == 8192);
    memory_pool_free(inst.pkt_pool, pkt);
    struct INFLIGHT_PACKET* waiting = enqueue(1); CHECK(!request() && a->cwnd_limited);
    struct ACK_PACKET* confirmation = (struct ACK_PACKET*)queue_entry_new_from_pool(inst.ack_pool); CHECK(confirmation);
    confirmation->seq = 777;
    confirmation->attempt_id = etcp_tx_attempt_id(777, 1);
    CHECK(queue_data_put_with_index(a->ack_q, &confirmation->ll) == 0);
    pkt = request(); CHECK(pkt && pkt->data[0] == ETCP_SECTION_ACK && pkt->data[1] == 1);
    CHECK(waiting == (struct INFLIGHT_PACKET*)conn->input_send_q->head && a->inflight_bytes == 8192);
    memory_pool_free(inst.pkt_pool, pkt);
    a->burst_active = 1; a->burst_count = 2;
    for (int i = 0; i < 2; i++) {
        pkt = request(); CHECK(pkt && pkt->data[8] == ETCP_SECTION_MEAS_TS && (pkt->data[11] & MEAS_FLAG_IS_FILLER));
        CHECK(a->inflight_bytes == 8192 && conn->input_send_q->head == &waiting->ll);
        memory_pool_free(inst.pkt_pool, pkt);
    }
    CHECK(!a->burst_active);
    teardown();
}

/* RTO освобождает окно; поздний ACK удаляет ожидающий retry, затем остальные повторы переходят A -> B. */
static void late_ack_and_migration(void) {
    setup(); fill(a, 8, 1024); enqueue(600);
    a->link_status = b->link_status = 0;
    test_clock_run_for(ua, 120);
    check_accounting(); CHECK(!conn->input_wait_ack->count && !a->inflight_bytes);
    ack(1); ack(1); CHECK(!a->inflight_bytes);
    b->link_status = 1; etcp_link_update_inflight_lim(b, 8192);
    wait_sends(8);
    CHECK(!conn->input_send_q->count && b->inflight_bytes == 7768 && sends == 8);
    for (struct ll_entry* e = conn->input_wait_ack->head; e; e = e->next)
        CHECK(((struct INFLIGHT_PACKET*)e)->last_link == b);
    ack_all(); teardown();
}

static void lost_ack_and_shrink(void) {
    setup(); b->link_status = 0; a->inflight_lim_bytes = 16384; fill(a, 16, 1024);
    etcp_link_update_inflight_lim(a, 8192);
    enqueue(1024); CHECK(!request() && a->inflight_bytes == 16384);
    wait_sends(8);
    check_accounting(); CHECK(sends == 8 && a->inflight_bytes == 8192 && conn->input_send_q->count == 9);
    ack_all(); wait_sends(16);
    CHECK(sends == 16 && a->inflight_bytes == 8192 && conn->input_send_q->count == 1);
    ack_all(); wait_sends(17); ack_all();
    CHECK(!conn->unacked_bytes && !conn->input_send_q->count);
    teardown();
}

static void grow_and_session(void) {
    setup(); b->link_status = 0; a->inflight_lim_bytes = 8192; fill(a, 8, 1024); enqueue(100);
    CHECK(!request());
    uint64_t allocated = ua->timer_alloc_count;
    test_clock_run_for(ua, 20); CHECK(!sends && ua->timer_alloc_count - allocated < 4);
    etcp_link_update_inflight_lim(a, 16384); wait_sends(1);
    CHECK(sends == 1 && a->inflight_bytes == 8292); ack_all();
    conn->initialized = 0; conn->session_required = conn->reinit_pending = 1;
    enqueue(100); CHECK(!request()); test_clock_run_for(ua, 10); CHECK(sends == 1);
    conn->session_required = 0; etcp_conn_ready(conn); wait_sends(2);
    CHECK(sends == 2 && conn->input_wait_ack->count == 1);
    ack_all(); teardown();
}

static void allocation_and_send_failure(void) {
    setup(); b->link_status = 0; enqueue(100); fail_alloc = 1;
    test_clock_run_for(ua, 20);
    CHECK(!sends && conn->input_send_q->count == 1 && !a->inflight_bytes && conn->tx_token && conn->tx_delayed);
    fail_send = 1; wait_sends(1);
    CHECK(sends == 1 && conn->input_send_q->count == 1 && !a->inflight_bytes);
    wait_sends(2);
    ack_all(); teardown();
}

static void removed_link_and_bbr(void) {
    setup(); fill(a, 1, 1024);
    etcp_link_close(a); a = NULL;
    ack(1); CHECK(!conn->unacked_bytes);
    b->bbr = u_calloc(1, sizeof(*b->bbr)); CHECK(b->bbr); bbr_init(b->bbr);
    b->inflight_lim_bytes = 8192;
    for (int i = 0; i < 7; i++) enqueue(1024);
    for (int i = 0; i < 7; i++) {
        struct ETCP_DGRAM* pkt = request(); CHECK(pkt); memory_pool_free(inst.pkt_pool, pkt);
    }
    enqueue(1025); CHECK(!request() && b->cwnd_limited && b->inflight_bytes < b->inflight_lim_bytes);
    // Первый ACK относится к отправке при наличии ожидающих данных: она не app_limited.
    struct INFLIGHT_PACKET* first = (struct INFLIGHT_PACKET*)conn->input_wait_ack->head;
    CHECK(!last_attempt(first)->is_app_limited);
    ack(first->seq); CHECK(bbr_calls == 1 && saw_cwnd_limited && !saw_app_limited);
    teardown();
}

static void shaper_ready(void* arg) {
    struct ETCP_LINK* link = arg;
    link->shaper_timer = NULL;
    loadbalancer_link_ready(link);
}

/* Реальный RTO: границы, округление, начальный jitter и выбор DATA/KA RTT; без подмены обработчика таймера. */
static void retransmission_timeout(void) {
    const struct { uint32_t rtt_us, jitter_us; uint16_t ka_rtt_tb; uint32_t expected_us; } cases[] = {
        {0, 0, 0, 1000000}, {0, UINT32_MAX, 0, 1000000}, {2000, 1000, 0, 10000},
        {20000, 0, 0, 27000}, {20000, 7500, 0, 38250}, {200000, 10000, 0, 258000},
        {600000, 100000, 0, 873000}, {1000000, 0, 0, 1203000}, {UINT32_MAX, UINT32_MAX, 0, UINT32_MAX},
        {2000000, 0, 0, 2403000}, {2000000, 200000, 0, 2703000},
        {5833, 0, 0, 10000}, {5834, 0, 0, 10001}, {20001, 1, 0, 27003},
        {20000, 10000, 0, 42000}, {200000, 100000, 0, 393000},
        {0, 0, 200, 27000}, {20000, 0, 6000, 723000}, {0, 0, UINT16_MAX, 7867200},
        {20000, 0, 1000, 123000}, {20000, 7500, 1000, 134250},
        {200000, 0, 200, 243000}, {20000, 0, 1, 27000}, {20000, 0, UINT16_MAX, 7867200},
        {830832, 0, 0, 999999}, {830833, 0, 0, 1000000}, {830834, 0, 0, 1000001}
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        setup(); b->link_status = 0;
        conn->rtt_avg_10 = UINT16_MAX; conn->jitter = UINT16_MAX; // Общие/чужие метрики не участвуют в RTO этого пути.
        a->rtt_last = cases[i].ka_rtt_tb; a->data_rtt_us = cases[i].rtt_us; a->data_jitter_us = cases[i].jitter_us;
        CHECK(etcp_tx_rto_us(a) == cases[i].expected_us);
        if (cases[i].expected_us == UINT32_MAX) { teardown(); continue; } // Проверка насыщения, без ожидания часа.
        fill(a, 1, 100);
        struct INFLIGHT_PACKET* packet = (struct INFLIGHT_PACKET*)conn->input_wait_ack->head;
        uint64_t sent = last_attempt(packet)->sent_at_us, deadline = sent + cases[i].expected_us + 100000;
        unsigned expected_tb = (cases[i].expected_us + 99) / 100;
        // Блокируем только новую передачу: статус UP и настоящий retrans-таймер сохраняются.
        a->shaper_timer = uasync_set_timeout(ua, expected_tb + 10000, a, shaper_ready, "test_rto_shaper");
        CHECK(a->shaper_timer);
        while (packet->state == INFLIGHT_STATE_WAIT_ACK && get_time_us() < deadline) test_clock_poll(ua, 10);
        uint64_t elapsed = get_time_us() - sent;
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "RTO: DATA_RTT_us=%u jitter_us=%u KA_RTT_tb=%u expected_us=%u observed_us=%llu state=%u",
            cases[i].rtt_us, cases[i].jitter_us, cases[i].ka_rtt_tb, cases[i].expected_us, (unsigned long long)elapsed, packet->state);
        CHECK(packet->state == INFLIGHT_STATE_WAIT_SEND && elapsed >= cases[i].expected_us && elapsed <= cases[i].expected_us + 100000);
        CHECK(!sends && !a->inflight_bytes && conn->input_send_q->count == 1 && conn->unacked_bytes == 100);
        check_accounting(); ack(packet->seq); teardown();
    }
}

static void pacing_reset_close(void) {
    setup(); b->link_status = 0;
    a->shaper_timer = uasync_set_timeout(ua, 20, a, shaper_ready, "test_shaper"); CHECK(a->shaper_timer);
    enqueue(100); CHECK(!request());
    wait_sends(1); ack_all();
    enqueue(100); CHECK(conn->tx_token);
    etcp_conn_reinit_id(conn, "queued sender reset", conn->reset_id);
    CHECK(!conn->tx_token && !conn->unacked_bytes && conn->reinit_pending);
    etcp_conn_ready(conn);
    enqueue(100); CHECK(conn->tx_token);
    CHECK(etcp_conn_ref_take(conn) == 0);
    etcp_connection_close(conn); CHECK(conn->state == 2 && !conn->tx_token);
    etcp_conn_ref_free(conn);
    teardown();
}

/* Смена ставки пересчитывает долг, а не оставляет таймер от прежней скорости. */
static void pacing_rate_changes(void) {
    setup(); b->link_status = 0; a->data_rtt_us = 1000000;
    etcp_loadbalancer_set_pacing_rate(a, 100);
    enqueue(1000); wait_sends(1); enqueue(500);
    uint64_t debt = a->shaper_load_time_tb - get_time_tb();
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Pacing below 1Kbit/s: rate=%u debt_tb=%llu", a->bbr_pacing_rate, (unsigned long long)debt);
    CHECK(a->shaper_timer && a->bbr_pacing_rate == 100 && debt >= 99000 && debt <= 103000);
    etcp_loadbalancer_set_pacing_rate(a, 1000000);
    CHECK(a->shaper_load_time_tb <= get_time_tb() + 20);
    wait_sends(2); ack_all(); teardown();

    setup(); b->link_status = 0; a->data_rtt_us = 1000000;
    etcp_loadbalancer_set_pacing_rate(a, 500000); // 1000 DATA-байтов оставляют долг даже после кредита 1мс.
    enqueue(1000); wait_sends(1); enqueue(500);
    etcp_loadbalancer_set_pacing_rate(a, 100);
    CHECK(a->shaper_timer && a->shaper_load_time_tb > get_time_tb() + 1000);
    test_clock_run_for(ua, 200); CHECK(sends == 1 && conn->input_send_q->count == 1);
    etcp_loadbalancer_set_pacing_rate(a, 0);
    CHECK(!a->shaper_timer && !a->shaper_sub_nanotime);
    wait_sends(2); ack_all(); teardown();
}

static void pacing_timer_failure(void) {
    setup(); CHECK(etcp_conn_ref_take(conn) == 0); b->link_status = 0;
    etcp_loadbalancer_set_pacing_rate(a, 100); fail_shaper_timer = 1;
    enqueue(100); uasync_drain_immediate(ua);
    CHECK(!fail_shaper_timer && conn->state == 2 && !conn->links);
    etcp_conn_ref_free(conn); teardown();
}

/* Миллисекундное округление таймеров не должно превращать 1MB/s в 0.5MB/s. */
static void pacing_continuity(void) {
    setup(); b->link_status = 0; a->data_rtt_us = 1000000;
    etcp_loadbalancer_set_pacing_rate(a, 1000000);
    uint64_t started = get_time_tb();
    for (unsigned i = 0; i < 50; i++) enqueue(1000);
    wait_sends(50);
    uint64_t elapsed = get_time_tb() - started;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Pacing continuity: DATA=50000 rate=1000000 elapsed_tb=%llu", (unsigned long long)elapsed);
    CHECK(elapsed >= 450 && elapsed <= 650);
    ack_all(); teardown();
}

/* Ставка BBR измеряется в DATA B/s: большой piggyback ACK не должен уменьшать допустимую скорость DATA. */
static void pacing_data_units(void) {
    setup(); b->link_status = 0;
    etcp_loadbalancer_set_pacing_rate(a, 100000);
    uint64_t started = get_time_tb();
    for (unsigned i = 0; i < 50; i++) {
        while (!loadbalancer_link_can_send(a)) test_clock_poll(ua, 10);
        struct ETCP_DGRAM* pkt = memory_pool_alloc(inst.pkt_pool); CHECK(pkt);
        memset(pkt, 0, sizeof(*pkt)); pkt->link = a; pkt->payload_len = 100; pkt->data_len = 1000;
        conn->tx_running = 1; CHECK(etcp_loadbalancer_send(pkt) >= 0); conn->tx_running = 0;
    }
    uint64_t elapsed = get_time_tb() - started;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Pacing DATA units: DATA=5000 encoded=50000 rate=100000 elapsed_tb=%llu",
        (unsigned long long)elapsed);
    CHECK(elapsed >= 450 && elapsed <= 650);
    teardown();
}

/* Изолируем pacer от cwnd: передача настоящего dgram, без логической попытки и сетевого I/O. */
static void send_pacer_data(unsigned bytes) {
    struct ETCP_DGRAM* pkt = memory_pool_alloc(inst.pkt_pool); CHECK(pkt);
    memset(pkt, 0, sizeof(*pkt)); pkt->link = a; pkt->payload_len = bytes; pkt->data_len = 1000;
    conn->tx_running = 1; CHECK(etcp_loadbalancer_send(pkt) >= 0); conn->tx_running = 0;
}

static void pacer_ready_notify(struct ETCP_CONN* ready) {
    CHECK(ready == conn && !a->shaper_timer);
    pacer_ready_count++;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Pacer ready: callbacks=%u action=%u", pacer_ready_count, pacer_ready_action);
    if (pacer_ready_action == 1) etcp_conn_reinit_id(conn, "pacer readiness reset", conn->reset_id);
    else if (pacer_ready_action == 2) etcp_connection_close(conn);
}

/* Используем настоящий callback, предварительно отменив его реальный таймер: нет фиктивных handles. */
static void invoke_pacer_at(uint64_t now_tb) {
    CHECK(pacer_clock_active && a->shaper_timer && captured_shaper_callback && captured_shaper_arg == a);
    timeout_callback_t callback = captured_shaper_callback;
    void* arg = captured_shaper_arg;
    uasync_cancel_timeout(ua, a->shaper_timer);
    pacer_now_tb = now_tb;
    callback(arg);
}

/* Таймер должен будить в первый разрешённый TB, включая дробь и ограничение длинного ожидания. */
static void pacing_first_ready(void) {
    const struct { uint64_t debt; int delay; } cases[] = {
        {9,0}, {10,1}, {11,2}, {30,21},
        {(uint64_t)INT_MAX + 9,INT_MAX}, {(uint64_t)INT_MAX + 10,INT_MAX}
    };
    const uint64_t fractions[] = {0, 1, SUB_MODULO-1};
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        for (unsigned f = 0; f < sizeof(fractions)/sizeof(fractions[0]); f++) {
            setup(); b->link_status = 0; conn->link_ready_for_send_fn = pacer_ready_notify;
            pacer_now_tb = __real_get_time_tb(); pacer_clock_active = capture_shaper = 1;
            uint64_t start = pacer_now_tb;
            etcp_loadbalancer_set_pacing_rate(a, 10000); // Один DATA-байт занимает ровно 1TB.
            a->shaper_load_time_tb = start + cases[i].debt - 1;
            a->shaper_sub_nanotime = fractions[f];
            send_pacer_data(1);
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Pacer first-ready: debt_tb=%llu fraction=%llu timer_tb=%d expected=%d",
                (unsigned long long)cases[i].debt, (unsigned long long)fractions[f], captured_shaper_delay, cases[i].delay);
            CHECK(a->shaper_load_time_tb == start + cases[i].debt && a->shaper_sub_nanotime == fractions[f]);
            if (cases[i].delay) {
                CHECK(a->shaper_timer && shaper_arm_count == 1 && captured_shaper_delay == cases[i].delay);
                CHECK(!loadbalancer_link_can_send(a));
                invoke_pacer_at(start + cases[i].debt - SHAPER_BURST_DELAY_TB);
                CHECK(a->shaper_timer && captured_shaper_delay == 1 && !pacer_ready_count);
                CHECK(!loadbalancer_link_can_send(a));
                invoke_pacer_at(start + cases[i].debt - SHAPER_BURST_DELAY_TB + 1);
                CHECK(!a->shaper_timer && pacer_ready_count == 1 && loadbalancer_link_can_send(a));
                CHECK(a->shaper_load_time_tb == start + cases[i].debt && a->shaper_sub_nanotime == fractions[f]);
            } else CHECK(!a->shaper_timer && !shaper_arm_count && loadbalancer_link_can_send(a));
            pacer_clock_active = capture_shaper = 0;
            teardown();
        }
    }
}

/* Поздний callback не превращает простой в неограниченный burst; окно не ограничивает эту проверку. */
static void pacing_late_wakeup(void) {
    setup(); b->link_status = 0; conn->link_ready_for_send_fn = pacer_ready_notify;
    pacer_now_tb = __real_get_time_tb(); pacer_clock_active = capture_shaper = 1;
    uint64_t start = pacer_now_tb;
    etcp_loadbalancer_set_pacing_rate(a, 1000000);
    send_pacer_data(1000); CHECK(a->shaper_timer);
    invoke_pacer_at(start + 1000); // Callback опоздал на 100мс, а разрешённый кредит остаётся 1мс.
    CHECK(pacer_ready_count == 1 && loadbalancer_link_can_send(a));
    unsigned before = sends;
    while (loadbalancer_link_can_send(a)) {
        CHECK(sends-before < 10);
        send_pacer_data(1000);
    }
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Pacer late wake: stall_tb=1000 immediate_DATA=%u credit_tb=%llu",
               sends-before, (unsigned long long)SHAPER_BURST_DELAY_TB);
    CHECK(sends-before == 2 && a->shaper_timer);
    pacer_clock_active = capture_shaper = 0;
    teardown();
}

/* Отказ повторной установки таймера закрывает соединение, не уведомляя о фиктивной готовности. */
static void pacing_rearm_failure(void) {
    setup(); b->link_status = 0; CHECK(etcp_conn_ref_take(conn) == 0);
    conn->link_ready_for_send_fn = pacer_ready_notify;
    pacer_now_tb = __real_get_time_tb(); pacer_clock_active = capture_shaper = 1;
    uint64_t start = pacer_now_tb;
    etcp_loadbalancer_set_pacing_rate(a, 10000);
    a->shaper_load_time_tb = start + 99;
    send_pacer_data(1); CHECK(a->shaper_timer);
    fail_shaper_timer = 1;
    invoke_pacer_at(start + 90);
    CHECK(!fail_shaper_timer && !pacer_ready_count && conn->close_requested);
    pacer_clock_active = capture_shaper = 0;
    uasync_drain_immediate(ua);
    CHECK(conn->state == 2 && !conn->links);
    etcp_conn_ref_free(conn); teardown();
}

/* Реальный dispatch таймера: готовность может синхронно сбросить сессию или освободить физический линк. */
static void pacing_ready_lifecycle(void) {
    for (unsigned action = 1; action <= 2; action++) {
        setup(); b->link_status = 0; CHECK(etcp_conn_ref_take(conn) == 0);
        conn->link_ready_for_send_fn = pacer_ready_notify; pacer_ready_action = action;
        etcp_loadbalancer_set_pacing_rate(a, 100000);
        send_pacer_data(1000); CHECK(a->shaper_timer);
        uint64_t end = get_time_us() + 1000000;
        while (!pacer_ready_count && get_time_us() < end) test_clock_poll(ua, 10);
        CHECK(pacer_ready_count == 1);
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Pacer callback result: action=%u state=%u initialized=%u reinit=%u session=%u links=%d",
            action, conn->state, conn->initialized, conn->reinit_pending, conn->session_required, conn->links != NULL);
        if (action == 1) CHECK(conn->reinit_pending && !conn->initialized && conn->reinit_count == 1 && conn->links);
        else CHECK(conn->state == 2 && !conn->links);
        test_clock_run_for(ua, 40); CHECK(pacer_ready_count == 1);
        etcp_conn_ref_free(conn); teardown();
    }
}

/* Физические копии уже доставленных seq также занимают бюджет, пока не придёт собственный ACK. */
static void attempt_memory_limits(void) {
    setup();
    a->inflight_lim_bytes = b->inflight_lim_bytes = MAX_INFLIGHT_SIZE * 4;
    for (unsigned path = 0; path < 2; path++) {
        struct ETCP_LINK* link = path ? b : a;
        for (unsigned i = 0; i < MAX_INFLIGHT_SIZE; i++) {
            struct INFLIGHT_PACKET* packet = enqueue(1);
            struct ETCP_TX_ATTEMPT* attempt = etcp_tx_prepare(link, packet); CHECK(attempt);
            struct ETCP_DGRAM pkt = { .link = link, .attempt = attempt };
            etcp_tx_complete(&pkt, 1); etcp_ack_recv(conn, packet->seq);
        }
        CHECK(link->attempts->count == MAX_INFLIGHT_SIZE);
    }
    CHECK(conn->active_attempts == 2 * MAX_INFLIGHT_SIZE);
    struct INFLIGHT_PACKET* pending = enqueue(1);
    CHECK(!etcp_loadbalancer_select_link(conn, 1, NULL));
    CHECK(!etcp_tx_prepare(a, pending));
    struct ETCP_TX_ATTEMPT old = *(struct ETCP_TX_ATTEMPT*)a->attempts->head;
    etcp_tx_ack(a, old.seq, old.attempt_id, UINT32_MAX, 0, 0);
    CHECK(etcp_loadbalancer_select_link(conn, 1, NULL) == a);
    CHECK(conn->active_attempts == 2 * MAX_INFLIGHT_SIZE - 1);
    for (unsigned i = 0; i < MAX_INFLIGHT_SIZE; i++) CHECK(etcp_tx_queue_ack(a, i+1, etcp_tx_attempt_id(i+1, 1)) == 0);
    CHECK(etcp_tx_queue_ack(a, MAX_INFLIGHT_SIZE+1, etcp_tx_attempt_id(MAX_INFLIGHT_SIZE+1, 1)) < 0);
    CHECK(a->ack_q->count == MAX_INFLIGHT_SIZE);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Attempt limits: physical=%u path=%d ACK=%d ready_seq=%u",
        conn->active_attempts, a->attempts->count, a->ack_q->count, pending->seq);
    teardown();
}

/* Отправитель не превращает pending transport в UP до первого INIT/подтверждения сессии. */
static void initial_readiness(void) {
    setup(); conn->state = 0; conn->initialized = conn->links_up = conn->tx_state = 0;
    conn->session_required = 1; enqueue(100);
    test_clock_run_for(ua, 20);
    CHECK(!conn->tx_state && !sends && !conn->links_up);
    conn->link_ready_for_send_fn(conn); CHECK(!conn->links_up);
    conn->session_required = 0; etcp_conn_ready(conn); wait_sends(1);
    CHECK(conn->state == 1 && conn->links_up && conn->initialized);
    ack_all(); teardown();
}

static void config_limits(void) {
    const char* values[] = {"0", "8191", "-1", "2147483648", "999999999999999999999", "abc", "8192x", "8192", "65536"};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        char text[128]; snprintf(text, sizeof(text), "[global]\nbbr_max_cwnd=%s\n", values[i]);
        struct utun_config* parsed = parse_config_from_buf(text, strlen(text), "window_config");
        CHECK((parsed != NULL) == (i >= 7));
        free_config(parsed);
    }
}

/* Отказ TX-таймера закрывает транспорт после выхода из вызывающего стека, без UAF очереди/линков. */
static void timer_failure(void) {
    setup(); CHECK(!conn->tx_token && etcp_conn_ref_take(conn) == 0);
    fail_tx_timer = 1; enqueue(100);
    CHECK(conn->close_requested && conn->close_token && conn->state != 2 && conn->links);
    uasync_drain_immediate(ua);
    CHECK(conn->state == 2 && !conn->tx_token && !conn->links);
    etcp_conn_ref_free(conn); teardown();
}

static void immediate_send(void) {
    setup(); enqueue(100);
    CHECK(conn->tx_token && !conn->tx_delayed);
    uasync_drain_immediate(ua);
    CHECK(sends == 1 && conn->input_wait_ack->count == 1);
    ack_all(); teardown();
}

/* Подтверждения входящего потока не должны ждать pacing собственного исходящего DATA. */
static void ack_without_data_pacing(void) {
    setup(); b->link_status = 0; etcp_loadbalancer_set_pacing_rate(a, 125);
    a->shaper_timer = uasync_set_timeout(ua, 1000, a, shaper_ready, "test_shaper"); CHECK(a->shaper_timer);
    void* timer = a->shaper_timer;
    uint64_t load = a->shaper_load_time_tb, fraction = a->shaper_sub_nanotime;
    struct ACK_PACKET* confirmation = (struct ACK_PACKET*)queue_entry_new_from_pool(inst.ack_pool); CHECK(confirmation);
    confirmation->seq = 777;
    confirmation->attempt_id = etcp_tx_attempt_id(777, 1);
    CHECK(queue_data_put_with_index(a->ack_q, &confirmation->ll) == 0);
    enqueue(100); uasync_drain_immediate(ua);
    CHECK(sends == 1 && conn->input_send_q->count == 1 && !a->ack_q->count);
    CHECK(a->shaper_timer == timer && a->shaper_load_time_tb == load && a->shaper_sub_nanotime == fraction);
    uasync_cancel_timeout(ua, timer); a->shaper_timer = NULL;
    etcp_conn_wake_sender(conn); wait_sends(2); ack_all(); teardown();
}

/* ACK одной пачки приходят в один tick: скорость определяется доставкой всей группы, а не промежутком между ACK. */
static void bbr_delivery_clock(void) {
    setup(); b->link_status = 0; a->data_rtt_us = 20000;
    a->bbr = u_calloc(1, sizeof(*a->bbr)); CHECK(a->bbr); bbr_init(a->bbr);
    fill(a, 8, 1000);
    uint64_t sent_tb = get_time_tb();
    test_clock_run_for(ua, 200);
    for (unsigned seq = 1; seq <= 8; seq++) {
        ack(seq);
        uint64_t elapsed_us = (get_time_tb() - sent_tb) * 100;
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Delivery sample: seq=%u delivered=%u acked=%u interval_us=%u flight_us=%llu",
            seq, last_sample.delivered, last_sample.acked_sacked, last_sample.interval_us, (unsigned long long)elapsed_us);
        CHECK(last_sample.delivered == seq * 1000 && last_sample.acked_sacked == 1000);
        CHECK(last_sample.interval_us >= 19000 && last_sample.interval_us <= elapsed_us + 1000);
    }
    CHECK(a->bbr->delivered == 8000);
    teardown();
}

/* Модель и транспорт используют один DATA MSS и одно окно, в том числе ниже 8KiB. */
static void bbr_probe_window(void) {
    setup(); b->link_status = 0; a->data_rtt_us = 1000000;
    etcp_update_mtu(conn);
    uint32_t minimum = BBR_CWND_MIN_TARGET * conn->normalizer->frag_size;
    a->rtt_last = 100;
    a->bbr = u_calloc(1, sizeof(*a->bbr)); CHECK(a->bbr);
    a->bbr->now_tb = 10000; bbr_init(a->bbr);
    a->bbr->mode = BBR_PROBE_BW; a->bbr->full_bw_reached = 1; a->bbr->cycle_idx = BBR_BW_PROBE_CRUISE;
    a->bbr->min_rtt_us = a->bbr->probe_rtt_min_us = 10000; a->bbr->has_seen_rtt = 1;
    a->bbr->bw_hi[0] = BW_UNIT / 10; a->bbr->now_tb += BBR_PROBE_RTT_WIN_MS * 10 + 1;
    fill(a, 8, 1000); test_clock_run_for(ua, 200); ack(1);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PROBE_RTT transport: DATA_MSS=%u model_MSS=%u window=%u expected=%u",
        conn->normalizer->frag_size, last_mss, a->inflight_lim_bytes, minimum);
    CHECK(last_mss == conn->normalizer->frag_size && a->inflight_lim_bytes == minimum && minimum < INFLIGHT_LIM_MIN);
    struct INFLIGHT_PACKET* probe = enqueue(1000); CHECK(!request());
    ack(2); ack(3); ack(4); CHECK(a->bbr->probe_rtt_done_stamp_tb);
    ack(5); // оставшиеся 3000 байт и новый фрагмент целиком помещаются в 4388
    struct ETCP_DGRAM* pkt = request(); CHECK(pkt && last_attempt(probe)->is_app_limited);
    memory_pool_free(inst.pkt_pool, pkt);
    test_clock_run_for(ua, 10); a->bbr->now_tb += BBR_PROBE_RTT_MS * 10;
    ack(probe->seq); CHECK(a->bbr->mode == BBR_PROBE_BW);
    uint32_t window = a->inflight_lim_bytes;
    etcp_link_update_inflight_lim(a, 0); CHECK(a->inflight_lim_bytes == window);
    etcp_link_update_inflight_lim(a, UINT32_MAX); CHECK(a->inflight_lim_bytes == conn->max_inflight);
    ack_all(); teardown();
}

/* Размер STCP-кадра 65535 не является размером DATA-фрагмента и не задаёт начальное окно. */
static void tcp_initial_window(void) {
    setup();
    struct ETCP_LINK* link = etcp_link_new(conn, NULL, NULL, 1); CHECK(link && link->is_tcp);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "STCP initial window: frame=%u DATA_MSS=%u window=%u", link->mtu,
        conn->normalizer->frag_size, link->inflight_lim_bytes);
    CHECK(link->inflight_lim_bytes == BBR_CWND_MIN_TARGET * conn->normalizer->frag_size);
    CHECK(link->bbr->init_cwnd == BBR_CWND_MIN_TARGET);
    teardown();
}

static void enable_models(void) {
    for (struct ETCP_LINK* link = conn->links; link; link = link->next) {
        link->bbr = u_calloc(1, sizeof(*link->bbr)); CHECK(link->bbr);
        bbr_init(link->bbr); link->bbr->link_id = link->local_link_id;
    }
}

/* Скачок RTT не уничтожает попытку до прихода ACK; уже назначенный ранний таймер проверяет новый RTO. */
static void rto_path_rtt_growth(void) {
    const struct { uint16_t ka_rtt_tb; unsigned wait_tb; } cases[] = {{1000, 1000}, {20000, 15000}};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        setup(); enable_models(); b->link_status = 0;
        a->data_rtt_us = 20000; a->rtt_last = 200;
        fill(a,1,1000);
        struct INFLIGHT_PACKET* packet = (struct INFLIGHT_PACKET*)conn->input_wait_ack->head;
        uint32_t seq = packet->seq; uint64_t id = packet->last_attempt_id;
        a->rtt_last = cases[i].ka_rtt_tb;
        test_clock_run_for(ua,cases[i].wait_tb);
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "RTO after path RTT growth: DATA_us=%u KA_us=%u RTO_us=%u attempts=%u retries=%u stalled=%u",
            a->data_rtt_us,a->rtt_last*100,etcp_tx_rto_us(a),a->inflight_packets,conn->retransmissions_count,a->tx_stalled);
        CHECK(queue_find_data_by_index(a->attempts,&id) && !conn->retransmissions_count && !a->tx_stalled);
        etcp_tx_ack(a,seq,id,0,0,0);
        CHECK(a->data_rtt_us > 20000 && bbr_calls == 1 && last_sample.rtt_us >= cases[i].wait_tb * 100);
        CHECK(a->acked_packets == 1 && !a->inflight_bytes && !conn->unacked_bytes);
        teardown();
    }
}

/* Timestamp сокращает срок без DATA/ACK; проверяем OOM и close из уведомления RTT внутри RX. */
static void rto_timestamp_rearm(void) {
    for (unsigned mode = 0; mode < 3; mode++) {
        setup(); b->link_status = 0; a->data_rtt_us = 20000; a->rtt_last = 1000;
        ack_now_us = get_time_us(); ack_clock_active = 1;
        fill(a,1,1000);
        uint64_t old_deadline = a->retrans_deadline_us;
        CHECK(old_deadline == ack_now_us+123000);
        struct ETCP_DGRAM* pkt = memory_pool_alloc(inst.pkt_pool); CHECK(pkt);
        pkt->link = a; pkt->data_len = 5; memset(pkt->data,0,5); pkt->data[0] = ETCP_SECTION_TIMESTAMP;
        uint16_t ret = get_current_timestamp()-200;
        pkt->data[1] = ret; pkt->data[2] = ret>>8;
        fail_rto_timer = mode == 1; close_rtt_callback = mode == 2;
        etcp_conn_input(pkt);
        if (mode) CHECK(conn->close_requested && !conn->callbacks_running && !fail_rto_timer && !close_rtt_callback);
        else {
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Timestamp RTO rearm: RTO_us=%u old_deadline=%llu new_deadline=%llu expected=%llu",
                etcp_tx_rto_us(a),(unsigned long long)old_deadline,(unsigned long long)a->retrans_deadline_us,
                (unsigned long long)(ack_now_us+27000));
            CHECK(a->rtt_last == 200 && a->retrans_deadline_us == ack_now_us+27000);
            CHECK(!a->acked_packets && !bbr_calls && a->inflight_packets == 1);
        }
        teardown();
    }
}

static void receive_till(struct ETCP_LINK* link, uint32_t till, int query) {
    struct ETCP_DGRAM* pkt = memory_pool_alloc(inst.pkt_pool); CHECK(pkt);
    pkt->link = link; pkt->data_len = ETCP_ACK_BASE_SIZE + query;
    memset(pkt->data, 0, pkt->data_len); pkt->data[0] = ETCP_SECTION_ACK;
    for (unsigned i = 0; i < 4; i++) pkt->data[2 + i] = till >> (8 * i);
    if (query) pkt->data[ETCP_ACK_BASE_SIZE] = ETCP_SECTION_ACK_REQUEST;
    etcp_conn_input(pkt); check_accounting();
}

static void receive_fragment(struct ETCP_LINK* link, uint32_t seq, uint64_t id) {
    struct ETCP_DGRAM* pkt = memory_pool_alloc(inst.pkt_pool); CHECK(pkt);
    pkt->link = link; pkt->data_len = ETCP_PAYLOAD_HEADER_SIZE + 1;
    memset(pkt->data, 0, pkt->data_len); pkt->data[0] = ETCP_SECTION_PAYLOAD;
    for (unsigned i = 0; i < 4; i++) pkt->data[1 + i] = seq >> (8 * i);
    for (unsigned i = 0; i < 2; i++) pkt->data[5 + i] = id >> (8 * i);
    pkt->data[ETCP_PAYLOAD_HEADER_SIZE] = 0x5a;
    etcp_conn_input(pkt);
}

/* Повтор обходит ещё UP-канал, даже если у него меньше inflight; общий RR не определяет курсор пакета. */
static void retry_rotation(void) {
    setup(); fill(a, 1, 100); fill(b, 1, 1000); b->data_rtt_us = 1000000;
    conn->initialized = 0; test_clock_run_for(ua, 120); conn->initialized = 1;
    CHECK(a->link_status && b->link_status && !a->inflight_bytes && b->inflight_bytes == 1000);
    conn->last_rr_link = b;
    struct ETCP_DGRAM* pkt = request(); CHECK(pkt && pkt->link == b);
    memory_pool_free(inst.pkt_pool, pkt); ack_all(); teardown();

    setup();
    struct ETCP_LINK* c = u_calloc(1, sizeof(*c)); CHECK(c);
    c->etcp = conn; c->local_link_id = 3; c->initialized = c->link_status = 1;
    c->mtu = 1280; c->remote_addr.ss_family = AF_INET; c->rtt_last = 1; c->inflight_lim_bytes = 65536;
    CHECK(etcp_tx_link_init(c) == 0); b->next = c;
    fill(a, 1, 100);
    struct ETCP_LINK* expected[] = {b, c, a};
    for (unsigned i = 0; i < 3; i++) {
        conn->initialized = 0; test_clock_run_for(ua, 120); conn->initialized = 1;
        conn->last_rr_link = expected[i]; // посторонний выбор/ACK меняет общий RR, но не порядок повторов
        pkt = request(); CHECK(pkt && pkt->link == expected[i]);
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Retry rotation: step=%u selected=%u", i, pkt->link->local_link_id);
        memory_pool_free(inst.pkt_pool, pkt);
    }
    ack_all(); teardown();
}

/* Готовый прежний путь не обходит занятую UP-альтернативу; ждём её события без новой попытки/RTO. */
static void retry_waits_for_alternative(void) {
    setup(); fill(a, 1, 100);
    conn->initialized = 0; test_clock_run_for(ua, 120); conn->initialized = 1;
    struct INFLIGHT_PACKET* packet = (struct INFLIGHT_PACKET*)conn->input_send_q->head;
    CHECK(packet && packet->last_link == a && !packet->active_attempts && packet->attempt_count == 1);
    b->shaper_timer = uasync_set_timeout(ua, 300, b, shaper_ready, "test_retry_alternative"); CHECK(b->shaper_timer);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Retry must wait: seq=%u previous=%u ready=1 alternative=%u pacing_blocked=1",
        packet->seq, a->local_link_id, b->local_link_id);
    CHECK(!request());
    uint64_t allocated = ua->timer_alloc_count;
    test_clock_run_for(ua, 5);
    CHECK(!sends && packet->attempt_count == 1 && !packet->active_attempts && !a->retrans_timer && !b->retrans_timer);
    CHECK(packet->state == INFLIGHT_STATE_WAIT_SEND && packet->last_link == a && ua->timer_alloc_count == allocated);
    CHECK(etcp_tx_queue_ack(a, 99, etcp_tx_attempt_id(99, 1)) == 0);
    mature_acks(a);
    struct ETCP_DGRAM* pkt = request(); CHECK(pkt && pkt->link == a && !pkt->payload_len && !pkt->attempt);
    memory_pool_free(inst.pkt_pool, pkt);
    a->burst_active = 1; a->burst_count = 1;
    pkt = request(); CHECK(pkt && pkt->link == a && pkt->burst_packet && !pkt->payload_len && !pkt->attempt);
    memory_pool_free(inst.pkt_pool, pkt);
    CHECK(packet->last_link == a && packet->attempt_count == 1 && !packet->active_attempts);
    wait_sends(1); CHECK(last_sent_link == b && packet->last_link == b && packet->attempt_count == 2);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Retry resumed: seq=%u selected=%u attempts=%u", packet->seq, b->local_link_id, packet->attempt_count);
    ack_all(); teardown();

    setup(); fill(a, 1, 100); fill(b, 1, 100); b->data_rtt_us = 1000000; b->inflight_lim_bytes = 100;
    conn->initialized = 0; test_clock_run_for(ua, 120); conn->initialized = 1;
    packet = (struct INFLIGHT_PACKET*)conn->input_send_q->head;
    CHECK(packet && packet->last_link == a && !request());
    CHECK(a->inflight_bytes == 0 && b->inflight_bytes == 100 && !packet->active_attempts && packet->attempt_count == 1);
    ack(2); wait_sends(1); CHECK(last_sent_link == b && packet->last_link == b);
    ack_all(); teardown();
}

/* Потеря последней альтернативы будит отправителя; поздний ACK убирает ожидающий повтор. */
static void retry_alternative_lifecycle(void) {
    setup(); fill(a, 1, 100); b->inflight_lim_bytes = 1;
    conn->initialized = 0; test_clock_run_for(ua, 120); conn->initialized = 1;
    CHECK(!request() && !conn->tx_token);
    b->link_status = 0; etcp_on_link_down(conn, b);
    CHECK(conn->tx_token); uasync_drain_immediate(ua);
    CHECK(sends == 1 && last_sent_link == a); ack_all(); teardown();

    setup(); fill(a, 1, 100); b->inflight_lim_bytes = 1;
    conn->initialized = 0; test_clock_run_for(ua, 120); conn->initialized = 1;
    CHECK(!request());
    etcp_link_close(b); b = NULL;
    uasync_drain_immediate(ua); CHECK(sends == 1 && last_sent_link == a && conn->links == a && !a->next);
    ack_all(); teardown();

    setup(); fill(a, 1, 100);
    conn->initialized = 0; test_clock_run_for(ua, 120); conn->initialized = 1;
    struct INFLIGHT_PACKET* packet = (struct INFLIGHT_PACKET*)conn->input_send_q->head;
    CHECK(packet); uint32_t seq = packet->seq; uint64_t old_id = packet->last_attempt_id;
    b->shaper_timer = uasync_set_timeout(ua, 300, b, shaper_ready, "test_retry_late_ack"); CHECK(b->shaper_timer);
    CHECK(!request()); etcp_tx_ack(a, seq, old_id, 0, 0, 0);
    test_clock_run_for(ua, 40); CHECK(!sends && !conn->unacked_bytes && !conn->active_attempts && !conn->input_send_q->count);
    teardown();

    setup(); fill(a, 1, 100);
    conn->initialized = 0; test_clock_run_for(ua, 120); conn->initialized = 1;
    b->initialized = 0;
    struct ETCP_DGRAM* pkt = request(); CHECK(pkt && pkt->link == a);
    memory_pool_free(inst.pkt_pool, pkt); ack_all(); teardown();
}

/* Заблокированный повтор не держит следующие DATA; среди готовых пакетов повтор имеет приоритет. */
static void send_queue_scan(void) {
    setup(); fill(a, 1, 100);
    conn->initialized = 0; test_clock_run_for(ua, 120); conn->initialized = 1;
    struct INFLIGHT_PACKET* blocked = (struct INFLIGHT_PACKET*)conn->input_send_q->head; CHECK(blocked);
    b->inflight_lim_bytes = 1;
    struct INFLIGHT_PACKET* fresh = enqueue(80);
    struct ETCP_DGRAM* pkt = request();
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Send scan: blocked=%u fresh=%u selected=%u", blocked->seq, fresh->seq,
        pkt && pkt->attempt ? pkt->attempt->seq : fresh->last_link ? fresh->seq : 0);
    CHECK(pkt && pkt->link == a && fresh->state == INFLIGHT_STATE_WAIT_ACK && blocked->state == INFLIGHT_STATE_WAIT_SEND);
    CHECK(blocked->attempt_count == 1 && !blocked->active_attempts);
    memory_pool_free(inst.pkt_pool, pkt); ack_all(); teardown();

    setup(); fill(a, 1, 100); fill(b, 1, 100);
    a->data_rtt_us = 10000; b->data_rtt_us = 20000;
    conn->initialized = 0; test_clock_run_for(ua, 400); conn->initialized = 1;
    blocked = (struct INFLIGHT_PACKET*)queue_find_data_by_index(conn->input_send_q, &(uint32_t){1});
    struct INFLIGHT_PACKET* later = (struct INFLIGHT_PACKET*)queue_find_data_by_index(conn->input_send_q, &(uint32_t){2});
    CHECK(blocked && later && conn->input_send_q->head == &blocked->ll);
    b->inflight_lim_bytes = 1;
    pkt = request(); CHECK(pkt && pkt->link == a && later->last_link == a && blocked->state == INFLIGHT_STATE_WAIT_SEND);
    memory_pool_free(inst.pkt_pool, pkt); ack_all(); teardown();

    setup(); fill(a, 1, 100); fresh = enqueue(80);
    conn->initialized = 0; test_clock_run_for(ua, 120); conn->initialized = 1;
    CHECK(conn->input_send_q->head == &fresh->ll);
    pkt = request(); CHECK(pkt && pkt->link == b && fresh->state == INFLIGHT_STATE_WAIT_SEND);
    memory_pool_free(inst.pkt_pool, pkt);
    pkt = request(); CHECK(pkt && fresh->state == INFLIGHT_STATE_WAIT_ACK);
    memory_pool_free(inst.pkt_pool, pkt); ack_all(); teardown();

    setup(); fill(a, 1, 100);
    conn->initialized = 0; test_clock_run_for(ua, 120); conn->initialized = 1;
    fresh = enqueue(80); a->burst_active = 1; a->burst_count = 1;
    pkt = request(); CHECK(pkt && pkt->link == a && pkt->burst_packet && !pkt->payload_len);
    memory_pool_free(inst.pkt_pool, pkt); CHECK(fresh->state == INFLIGHT_STATE_WAIT_SEND);
    pkt = request(); CHECK(pkt && pkt->link == b && fresh->state == INFLIGHT_STATE_WAIT_SEND);
    memory_pool_free(inst.pkt_pool, pkt);
    pkt = request(); CHECK(pkt && fresh->state == INFLIGHT_STATE_WAIT_ACK);
    memory_pool_free(inst.pkt_pool, pkt); ack_all(); teardown();

    setup(); fill(a, 256, 100);
    conn->initialized = 0; test_clock_run_for(ua, 120); conn->initialized = 1;
    b->inflight_lim_bytes = 1; fresh = enqueue(80);
    uint64_t started = get_time_us(); pkt = request();
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Send scan: blocked_prefix=256 elapsed_us=%llu", (unsigned long long)(get_time_us() - started));
    CHECK(pkt && pkt->link == a && fresh->state == INFLIGHT_STATE_WAIT_ACK && conn->input_send_q->count == 256);
    memory_pool_free(inst.pkt_pool, pkt); ack_all(); teardown();
}

static void queued_input_fragment(unsigned len) {
    struct ETCP_FRAGMENT* fragment = (struct ETCP_FRAGMENT*)queue_entry_new_from_pool(conn->io_pool); CHECK(fragment);
    fragment->ll.dgram = memory_pool_alloc(inst.data_pool); CHECK(fragment->ll.dgram);
    fragment->ll.dgram_pool = inst.data_pool; fragment->ll.len = len; memset(fragment->ll.dgram, 0x5a, len);
    conn->input_queue->callback_suspended = 1;
    CHECK(queue_data_put(conn->input_queue, &fragment->ll) == 0);
}

/* Полностью заблокированная send_q не мешает подгрузке, но общий лимит и смена эпохи сохраняются. */
static void send_queue_input_admission(void) {
    setup(); fill(a, 1, 100);
    conn->initialized = 0; test_clock_run_for(ua, 120); conn->initialized = 1;
    b->inflight_lim_bytes = 1; queued_input_fragment(80);
    CHECK(conn->input_queue->count == 1 && conn->input_send_q->count == 1);
    struct ETCP_DGRAM* pkt = request(); CHECK(pkt && pkt->link == a && pkt->payload_len == 80);
    CHECK(!conn->input_queue->count && conn->input_send_q->count == 1 && conn->input_wait_ack->count == 1);
    memory_pool_free(inst.pkt_pool, pkt); ack_all(); teardown();

    setup(); fill(a, 1, 100);
    conn->initialized = 0; test_clock_run_for(ua, 120); conn->initialized = 1;
    b->inflight_lim_bytes = 1; conn->optimal_inflight = 100; queued_input_fragment(80);
    CHECK(!request() && conn->input_queue->count == 1 && conn->next_tx_id == 2 && conn->unacked_bytes == 100);
    ack(1); pkt = request(); CHECK(pkt && pkt->payload_len == 80 && !conn->input_queue->count);
    memory_pool_free(inst.pkt_pool, pkt); ack_all(); teardown();

    setup(); fill(a, 1, 100);
    conn->initialized = 0; test_clock_run_for(ua, 120); conn->initialized = 1;
    b->inflight_lim_bytes = 1; queued_input_fragment(80); fail_tx_inflight = 1;
    uint32_t reset = conn->reset_count;
    CHECK(!request() && !fail_tx_inflight && conn->reset_count != reset && conn->reinit_pending);
    CHECK(!conn->unacked_bytes && !conn->active_attempts && !conn->input_send_q->count && !conn->input_queue->count);
    teardown();

    // Остаток байтового окна не вмещает следующий фрагмент: BBR должен видеть ограничение даже при пустой send_q.
    setup(); b->link_status = 0; b->inflight_lim_bytes = 0; a->inflight_lim_bytes = 4250;
    etcp_conn_on_inflight_lim_changed(conn); enable_models(); fill(a, 4, 1000);
    queued_input_fragment(1000);
    CHECK(!request() && !conn->input_send_q->count && conn->input_queue->count == 1);
    CHECK(conn->unacked_bytes == 4000 && a->inflight_bytes == 4000 && a->cwnd_limited);
    ack(1); CHECK(bbr_calls == 1 && saw_cwnd_limited);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Admission window pressure: flight_before=4000 window=4250 fragment=1000 limited=%u",
        saw_cwnd_limited);
    ack_all(); teardown();
}

static struct ETCP_LINK* add_test_link(uint8_t id) {
    struct ETCP_LINK* link = u_calloc(1, sizeof(*link)); CHECK(link);
    link->etcp = conn; link->local_link_id = id; link->initialized = link->link_status = 1;
    link->mtu = 1280; link->remote_addr.ss_family = AF_INET; link->rtt_last = 1; link->inflight_lim_bytes = 65536;
    CHECK(etcp_tx_link_init(link) == 0);
    struct ETCP_LINK* tail = conn->links;
    while (tail->next) tail = tail->next;
    tail->next = link; etcp_conn_on_inflight_lim_changed(conn);
    return link;
}

/* Не возвращаемся в B после B->C, пока не опробован A; неудавшийся send не расходует новый обход. */
static void retry_full_round_history(void) {
    setup(); struct ETCP_LINK* c = add_test_link(255);
    fill(b, 1, 100);
    struct INFLIGHT_PACKET* packet = (struct INFLIGHT_PACKET*)conn->input_wait_ack->head; CHECK(packet);
    conn->initialized = 0; test_clock_run_for(ua, 120); conn->initialized = 1;
    struct ETCP_DGRAM* pkt = request(); CHECK(pkt && pkt->link == c); memory_pool_free(inst.pkt_pool, pkt);
    CHECK(etcp_packet_link_used(packet, b->local_link_id) && etcp_packet_link_used(packet, c->local_link_id));
    conn->initialized = 0; test_clock_run_for(ua, 120); conn->initialized = 1;
    a->inflight_lim_bytes = 1;
    uint8_t history[ETCP_LINK_HISTORY_BYTES]; memcpy(history, packet->used_links, sizeof(history));
    struct ETCP_LINK* previous_rr = conn->last_rr_link;
    for (unsigned i = 0; i < 3; i++) CHECK(!request());
    CHECK(packet->attempt_count == 2 && !memcmp(history, packet->used_links, sizeof(history)) && conn->last_rr_link == previous_rr);
    struct INFLIGHT_PACKET* fresh = enqueue(80); uint32_t fresh_seq = fresh->seq;
    pkt = request(); CHECK(pkt && fresh->state == INFLIGHT_STATE_WAIT_ACK && packet->state == INFLIGHT_STATE_WAIT_SEND);
    memory_pool_free(inst.pkt_pool, pkt); ack(fresh_seq);
    etcp_link_update_inflight_lim(a, 100);
    pkt = request(); CHECK(pkt && pkt->link == a); memory_pool_free(inst.pkt_pool, pkt);
    CHECK(etcp_packet_link_used(packet, a->local_link_id) && etcp_packet_link_used(packet, b->local_link_id));
    CHECK(etcp_packet_link_used(packet, c->local_link_id));
    conn->initialized = 0; test_clock_run_for(ua, 120); conn->initialized = 1;
    memcpy(history, packet->used_links, sizeof(history)); previous_rr = conn->last_rr_link;
    pkt = etcp_request_pkt(conn); CHECK(pkt && pkt->link == b);
    etcp_tx_complete(pkt, 0); memory_pool_free(inst.pkt_pool, pkt);
    CHECK(!memcmp(history, packet->used_links, sizeof(history)) && conn->last_rr_link == previous_rr);
    pkt = request(); CHECK(pkt && pkt->link == b); memory_pool_free(inst.pkt_pool, pkt);
    CHECK(etcp_packet_link_used(packet, b->local_link_id) && !etcp_packet_link_used(packet, a->local_link_id));
    CHECK(!etcp_packet_link_used(packet, c->local_link_id));
    ack_all(); teardown();
}

struct reused_slot { uint8_t id; struct ETCP_LINK* replacement; };
static void recreate_link_from_close(struct ETCP_CONN* owner, struct ETCP_LINK* link, int old_state, int old_status, void* arg) {
    (void)old_state; (void)old_status;
    struct reused_slot* slot = arg;
    if (owner != conn || !link->closing || link->local_link_id != slot->id || slot->replacement) return;
    conn->last_assigned_link_id = slot->id - 1;
    int id = etcp_find_free_local_link_id(conn); CHECK(id == slot->id);
    slot->replacement = add_test_link((uint8_t)id);
}

/* Callback удаления вправе немедленно создать новый линк с тем же wire ID; история старого слота уже снята. */
static void retry_history_slot_reuse(void) {
    setup(); struct ETCP_LINK* c = add_test_link(255);
    fill(b, 1, 100);
    struct INFLIGHT_PACKET* packet = (struct INFLIGHT_PACKET*)conn->input_wait_ack->head; CHECK(packet);
    conn->initialized = 0; test_clock_run_for(ua, 120); conn->initialized = 1;
    struct ETCP_DGRAM* pkt = request(); CHECK(pkt && pkt->link == c); memory_pool_free(inst.pkt_pool, pkt);
    struct reused_slot slot = {.id=b->local_link_id};
    etcp_add_link_status_cbk(&inst, recreate_link_from_close, &slot);
    etcp_link_close(b); b = NULL;
    etcp_remove_link_status_cbk(&inst, recreate_link_from_close, &slot);
    CHECK(slot.replacement && packet->last_link == c && !etcp_packet_link_used(packet, slot.id));
    CHECK(etcp_packet_link_used(packet, c->local_link_id));
    conn->initialized = 0; test_clock_run_for(ua, 120); conn->initialized = 1;
    pkt = request(); CHECK(pkt && pkt->link == slot.replacement); memory_pool_free(inst.pkt_pool, pkt);
    CHECK(etcp_packet_link_used(packet, slot.id) && !etcp_packet_link_used(packet, a->local_link_id));
    ack_all(); teardown();
}

/* Три потери могут быть обнаружены одним вызовом таймера; старые ACK не доказывают восстановление. */
static void fast_stall_progress(void) {
    setup(); fill(a, 2, 100);
    conn->initialized = 0; test_clock_run_for(ua, 120); conn->initialized = 1;
    CHECK(a->data_rto_streak == 2 && !a->tx_stalled);
    etcp_ack_recv(conn, 1); etcp_ack_recv(conn, 2);
    fill(a, 1, 100);
    conn->initialized = 0; test_clock_run_for(ua, 120); conn->initialized = 1;
    CHECK(a->data_rto_streak == ETCP_TX_STALL_THRESHOLD && a->tx_stalled && a->tx_stalled_since_us);
    struct INFLIGHT_PACKET* packet = (struct INFLIGHT_PACKET*)conn->input_send_q->head; CHECK(packet);
    etcp_tx_ack(a, packet->seq, packet->last_attempt_id, 0, 0, 0);
    CHECK(a->tx_stalled && !conn->unacked_bytes);
    packet = enqueue(100); uint32_t seq = packet->seq;
    b->link_status = 0;
    struct ETCP_DGRAM* pkt = request(); CHECK(pkt && pkt->link == a); memory_pool_free(inst.pkt_pool, pkt);
    uint64_t id = packet->last_attempt_id;
    etcp_tx_ack(b, seq, id, 0, 0, 0); CHECK(a->tx_stalled && a->inflight_bytes == 100);
    etcp_tx_ack(a, seq, id, 0, 0, 0); CHECK(!a->tx_stalled && !a->data_rto_streak && !a->inflight_bytes);
    b->link_status = 1; teardown();

    setup(); b->link_status = 0; a->data_rtt_us = 100000;
    fill(a, 3, 100); conn->initialized = 0; test_clock_run_for(ua, 300); conn->initialized = 1;
    fill(a, 1, 100);
    packet = (struct INFLIGHT_PACKET*)queue_find_data_by_index(conn->input_wait_ack, &(uint32_t){4}); CHECK(packet);
    id = packet->last_attempt_id;
    conn->initialized = 0; test_clock_run_for(ua, 1000); conn->initialized = 1;
    CHECK(a->tx_stalled && a->attempts->count == 1 && last_attempt(packet)->sent_at_us < a->tx_stalled_since_us);
    etcp_tx_ack(a, 4, id, 0, 0, 0); CHECK(a->tx_stalled && a->data_rto_streak == ETCP_TX_STALL_THRESHOLD);
    for (seq = 1; seq <= 3; seq++) etcp_ack_recv(conn, seq);
    packet = enqueue(100); pkt = request(); CHECK(pkt); memory_pool_free(inst.pkt_pool, pkt);
    ack(packet->seq); CHECK(!a->tx_stalled && !a->data_rto_streak); teardown();
}

static void stalled_retry_selection(void) {
    setup(); fill(a, 3, 100);
    conn->initialized = 0; test_clock_run_for(ua, 120); conn->initialized = 1;
    CHECK(a->tx_stalled && !b->tx_stalled);
    for (uint32_t seq = 1; seq <= 3; seq++) etcp_ack_recv(conn, seq);
    fill(b, 1, 100);
    conn->initialized = 0; test_clock_run_for(ua, 120); conn->initialized = 1;
    struct INFLIGHT_PACKET* packet = (struct INFLIGHT_PACKET*)conn->input_send_q->head; CHECK(packet);
    b->inflight_lim_bytes = 1; CHECK(!request());
    struct INFLIGHT_PACKET* fresh = enqueue(80); uint32_t fresh_seq = fresh->seq;
    struct ETCP_DGRAM* pkt = request(); CHECK(pkt && pkt->link == a && fresh->state == INFLIGHT_STATE_WAIT_ACK);
    memory_pool_free(inst.pkt_pool, pkt); CHECK(a->tx_stalled);
    etcp_link_update_inflight_lim(b, 100);
    pkt = request(); CHECK(pkt && pkt->link == b && packet->last_link == b && a->tx_stalled);
    memory_pool_free(inst.pkt_pool, pkt);
    ack(fresh_seq); CHECK(!a->tx_stalled); ack_all(); teardown();

    setup(); fill(a, 3, 100); fill(b, 3, 100);
    conn->initialized = 0; test_clock_run_for(ua, 120); conn->initialized = 1;
    CHECK(a->tx_stalled && b->tx_stalled);
    packet = (struct INFLIGHT_PACKET*)conn->input_send_q->head; CHECK(packet);
    struct ETCP_LINK* fallback = packet->last_link == a ? b : a;
    struct ETCP_LINK* other = fallback == a ? b : a;
    pkt = request(); CHECK(pkt && pkt->payload_len && pkt->link == fallback);
    memory_pool_free(inst.pkt_pool, pkt); ack_all();
    CHECK(!fallback->tx_stalled); etcp_tx_link_reset(other, 0); CHECK(!other->tx_stalled && !other->data_rto_streak);
    teardown();
}

static void abort_input_admission(struct ll_queue* q, void* arg) {
    (void)q;
    unsigned action = (unsigned)(uintptr_t)arg;
    if (action == 1) etcp_conn_reinit_id(conn, "input admission callback", conn->reset_id);
    else if (action == 2) etcp_connection_close(conn);
    else { etcp_link_close(b); b = NULL; }
}

/* queue_data_get вызывает waiter синхронно: reset/close прекращают старый проход, удалённый burst не переиспользуется. */
static void input_admission_callbacks(void) {
    for (unsigned action = 1; action <= 3; action++) {
        setup(); fill(a, 1, 100);
        conn->initialized = 0; test_clock_run_for(ua, 120); conn->initialized = 1;
        b->inflight_lim_bytes = 1; queued_input_fragment(80); CHECK(etcp_conn_ref_take(conn) == 0);
        if (action == 3) { b->burst_active = 1; b->burst_count = 1; }
        struct queue_waiter_handle waiter = {0};
        queue_set_waiter_defer(conn->input_queue, 0);
        CHECK(queue_waiter_wait(conn->input_queue, &waiter, abort_input_admission, (void*)(uintptr_t)action) == 0);
        struct ETCP_DGRAM* pkt = request();
        CHECK(!waiter.internal && !conn->input_queue->count);
        if (action == 1) CHECK(!pkt && conn->reinit_pending && !conn->unacked_bytes);
        if (action == 2) CHECK(!pkt && conn->state == 2 && conn->unacked_bytes == 100 && conn->input_send_q->count == 1);
        if (action == 3) {
            CHECK(pkt && pkt->link == a && !a->next); memory_pool_free(inst.pkt_pool, pkt);
            pkt = request(); CHECK(pkt && pkt->link == a); memory_pool_free(inst.pkt_pool, pkt); ack_all();
        }
        etcp_conn_ref_free(conn); teardown();
    }
}

/* Поздняя копия и общий till не освобождают окно/не обучают модель другого канала. */
static void independent_attempt_acks(void) {
    setup(); enable_models(); fill(a, 1, 100);
    struct INFLIGHT_PACKET* packet = (struct INFLIGHT_PACKET*)conn->input_wait_ack->head;
    uint64_t old_id = packet->last_attempt_id;
    conn->initialized = 0; test_clock_run_for(ua, 120); conn->initialized = 1;
    struct ETCP_DGRAM* pkt = request(); CHECK(pkt && pkt->link == b);
    memory_pool_free(inst.pkt_pool, pkt);
    packet = (struct INFLIGHT_PACKET*)conn->input_wait_ack->head; uint64_t new_id = packet->last_attempt_id;
    CHECK(old_id != new_id && !a->inflight_bytes && b->inflight_bytes == 100);
    etcp_tx_ack(a, packet->seq, old_id, 0, 0, 0);
    CHECK(!conn->unacked_bytes && conn->ack_packets_count == 1 && b->inflight_bytes == 100);
    CHECK(!a->bbr->delivered && !b->bbr->delivered);
    etcp_tx_ack(b, 1, new_id, 0, 0, 0); etcp_tx_ack(b, 1, new_id, 0, 0, 0);
    CHECK(!b->inflight_bytes && b->bbr->delivered == 100 && conn->ack_packets_count == 1);
    check_accounting(); teardown();

    setup(); enable_models(); fill(a, 1, 100);
    packet = (struct INFLIGHT_PACKET*)conn->input_wait_ack->head; old_id = packet->last_attempt_id;
    receive_till(b, 1, 0);
    CHECK(!conn->unacked_bytes && a->inflight_bytes == 100 && !a->bbr->delivered && !b->bbr->delivered);
    etcp_tx_ack(a, 1, old_id, 0, 0, 0); CHECK(!a->inflight_bytes && a->bbr->delivered == 100);
    teardown();

    setup(); enable_models(); fill(a, 1, 100);
    packet = (struct INFLIGHT_PACKET*)queue_data_get(conn->input_wait_ack); old_id = packet->last_attempt_id;
    packet->state = INFLIGHT_STATE_WAIT_SEND; CHECK(queue_data_put_with_index(conn->input_send_q, &packet->ll) == 0);
    pkt = request(); CHECK(pkt && pkt->link == b && packet->active_attempts == 2);
    memory_pool_free(inst.pkt_pool, pkt); new_id = packet->last_attempt_id;
    etcp_tx_ack(a, 1, old_id, 0, 0, 0);
    CHECK(!conn->unacked_bytes && !a->inflight_bytes && b->inflight_bytes == 100);
    CHECK(a->bbr->delivered == 100 && !b->bbr->delivered);
    etcp_tx_ack(b, 1, new_id, 0, 0, 0); CHECK(b->bbr->delivered == 100 && conn->ack_packets_count == 1);
    check_accounting(); teardown();
}

/* Посторонний wake не снимает задержку ACK; точные границы проверяются без опроса uasync при ручных часах. */
static void ack_absolute_deadline(void) {
    setup(); conn->got_initial_pkt = 1;
    ack_now_us = get_time_us(); ack_clock_active = capture_ack_timer = 1;
    receive_fragment(a, 2, 1); CHECK(a->ack_q->count == 1 && conn->ack_resp_timer);
    etcp_conn_wake_sender(conn);
    struct ETCP_DGRAM* pkt = request();
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "ACK deadline: age_us=0 queued=%d sent_early=%u", a->ack_q->count, pkt != NULL);
    CHECK(!pkt && a->ack_q->count == 1);
    ack_now_us += 1999; CHECK(!request() && a->ack_q->count == 1);
    ack_now_us++; pkt = request(); CHECK(pkt && pkt->link == a && pkt->data[1] == 1);
    memory_pool_free(inst.pkt_pool, pkt); CHECK(!a->ack_q->count && !conn->ack_resp_timer);
    teardown();
}

static void ack_piggyback_deadlines(void) {
    setup(); conn->got_initial_pkt = 1;
    ack_now_us = get_time_us(); ack_clock_active = capture_ack_timer = 1;
    uint64_t begin = ack_now_us;
    receive_fragment(a, 2, 1); ack_now_us += 1000; receive_fragment(b, 3, 1);
    CHECK(conn->ack_resp_deadline_us == begin + ETCP_ACK_DELAY_US);
    enqueue(100);
    struct ETCP_DGRAM* pkt = request(); CHECK(pkt && pkt->link == a && pkt->payload_len && pkt->data[1] == 1);
    memory_pool_free(inst.pkt_pool, pkt); CHECK(!a->ack_q->count && b->ack_q->count == 1);
    CHECK(conn->ack_resp_timer && conn->ack_resp_deadline_us == begin + 3000);
    ack_now_us = begin + 2000; CHECK(!request());
    ack_now_us = begin + 2999; CHECK(!request());
    ack_now_us++; pkt = request(); CHECK(pkt && pkt->link == b && !pkt->payload_len && pkt->data[1] == 1);
    memory_pool_free(inst.pkt_pool, pkt); CHECK(!conn->ack_resp_timer && !conn->ack_resp_deadline_us);
    ack_all(); teardown();
}

/* Частичный piggyback меняет голову: прежний deadline больше не делает молодой ACK готовым. */
static void ack_partial_piggyback(void) {
    setup(); conn->got_initial_pkt = 1; b->link_status = 0;
    ack_now_us = get_time_us(); ack_clock_active = capture_ack_timer = 1;
    uint64_t begin = ack_now_us;
    receive_fragment(a, 2, 1); ack_now_us += 500; receive_fragment(a, 3, 1);
    unsigned max_body = a->mtu - WIRE_OVERHEAD(AF_INET);
    enqueue(max_body - ETCP_ACK_BASE_SIZE - ETCP_PAYLOAD_HEADER_SIZE - ETCP_ACK_ENTRY_SIZE - ETCP_ACK_RX_POINT_SIZE);
    ack_now_us = begin + 1999;
    struct ETCP_DGRAM* pkt = request(); CHECK(pkt && pkt->payload_len && pkt->data[1] == 1 && a->ack_q->count == 1);
    memory_pool_free(inst.pkt_pool, pkt); CHECK(conn->ack_resp_deadline_us == begin + 2500);
    timeout_callback_t callback = captured_ack_callback; void* arg = captured_ack_arg;
    ack_now_us = begin + 2000; uasync_cancel_timeout(ua, conn->ack_resp_timer); callback(arg);
    CHECK(conn->ack_resp_timer && conn->ack_resp_deadline_us == begin + 2500 && !request());
    ack_now_us = begin + 2100; receive_fragment(a, 4, 1);
    CHECK(conn->ack_resp_deadline_us == begin + 2500);
    ack_now_us = begin + 2500; pkt = request(); CHECK(pkt && !pkt->payload_len && pkt->data[1] == 2);
    memory_pool_free(inst.pkt_pool, pkt); CHECK(!a->ack_q->count && !conn->ack_resp_timer);
    ack_all(); teardown();
}

/* Новый RX между timer и отложенным sender не запускает для старой головы очередные 2мс. */
static void ack_timer_interleaving(void) {
    setup(); conn->got_initial_pkt = 1;
    ack_now_us = get_time_us(); ack_clock_active = capture_ack_timer = 1;
    uint64_t begin = ack_now_us;
    receive_fragment(a, 2, 1);
    timeout_callback_t callback = captured_ack_callback; void* arg = captured_ack_arg;
    ack_now_us += ETCP_ACK_DELAY_US;
    uasync_cancel_timeout(ua, conn->ack_resp_timer); callback(arg);
    CHECK(!conn->ack_resp_timer && conn->tx_token);
    receive_fragment(a, 3, 1); CHECK(!conn->ack_resp_timer && conn->tx_token);
    struct ETCP_DGRAM* pkt = request(); CHECK(pkt && pkt->data[1] == 2 && !pkt->payload_len);
    const uint8_t* entry = pkt->data + ETCP_ACK_BASE_SIZE;
    uint32_t delay = (uint32_t)entry[6] | ((uint32_t)entry[7] << 8) | ((uint32_t)entry[8] << 16) | ((uint32_t)entry[9] << 24);
    CHECK(delay == ETCP_ACK_DELAY_US && ack_now_us == begin + ETCP_ACK_DELAY_US);
    memory_pool_free(inst.pkt_pool, pkt); teardown();
}

static void ack_urgent_and_oom(void) {
    for (unsigned mode = 0; mode < 2; mode++) {
        setup(); conn->got_initial_pkt = 1;
        ack_now_us = get_time_us(); ack_clock_active = 1;
        fail_ack_timer = mode;
        receive_fragment(a, 2, 1);
        if (!mode) receive_till(a, 0, 1);
        CHECK(a->ack_reply_pending && !fail_ack_timer);
        CHECK(((struct ACK_PACKET*)a->ack_q->head)->received_at_us == ack_now_us);
        struct ETCP_DGRAM* pkt = request(); CHECK(pkt && pkt->data[1] == 1 && !pkt->payload_len);
        memory_pool_free(inst.pkt_pool, pkt);
        CHECK(!a->ack_reply_pending && !conn->ack_resp_timer && !conn->ack_resp_deadline_us);
        teardown();
    }
}

static void ack_partial_frame_and_lifecycle(void) {
    setup(); ack_now_us = get_time_us(); ack_clock_active = 1;
    for (unsigned n = 0; n < 200; n++) CHECK(etcp_tx_queue_ack(a, 100+n, etcp_tx_attempt_id(100+n, 1)) == 0);
    ack_now_us += ETCP_ACK_DELAY_US;
    struct ETCP_DGRAM* pkt = request(); CHECK(pkt && pkt->data[1] && a->ack_q->count);
    memory_pool_free(inst.pkt_pool, pkt); CHECK(!conn->ack_resp_timer);
    pkt = request(); CHECK(pkt && pkt->data[1] && !a->ack_q->count);
    memory_pool_free(inst.pkt_pool, pkt); teardown();

    setup(); ack_now_us = get_time_us(); ack_clock_active = 1;
    conn->ack_till_deadline_us = ack_now_us + ETCP_ACK_DELAY_US; conn->last_delivered_id = 17;
    CHECK(!request() && conn->ack_resp_timer);
    ack_now_us += ETCP_ACK_DELAY_US;
    pkt = request(); CHECK(pkt && !pkt->data[1] && pkt->data[2] == 17);
    memory_pool_free(inst.pkt_pool, pkt); CHECK(!conn->ack_till_deadline_us && !conn->ack_resp_timer);
    CHECK(etcp_tx_queue_ack(a, 99, etcp_tx_attempt_id(99, 1)) == 0); CHECK(!request() && conn->ack_resp_timer);
    etcp_conn_reset(conn); CHECK(!conn->ack_resp_timer && !conn->ack_resp_deadline_us && !conn->ack_till_deadline_us);
    teardown();
}

static void ack_sender_failure(void) {
    setup(); conn->got_initial_pkt = 1; CHECK(etcp_conn_ref_take(conn) == 0);
    ack_now_us = get_time_us(); ack_clock_active = capture_ack_timer = 1;
    receive_fragment(a, 2, 1);
    timeout_callback_t callback = captured_ack_callback; void* arg = captured_ack_arg;
    ack_now_us += ETCP_ACK_DELAY_US; fail_tx_timer = 1;
    uasync_cancel_timeout(ua, conn->ack_resp_timer); callback(arg);
    CHECK(!fail_tx_timer && conn->close_requested && !conn->ack_resp_timer);
    ack_clock_active = 0; uasync_drain_immediate(ua);
    CHECK(conn->state == 2 && !conn->links && !conn->ack_resp_deadline_us && !conn->ack_till_deadline_us);
    etcp_conn_ref_free(conn); teardown();
}

/* DOWN не мешает срокам других путей; UP возвращает старые ACK сразу. Удаление головы не оставляет чужой deadline. */
static void ack_link_lifecycle(void) {
    for (unsigned remove = 0; remove < 2; remove++) {
        setup(); conn->got_initial_pkt = 1;
        ack_now_us = get_time_us(); ack_clock_active = 1;
        uint64_t begin = ack_now_us;
        receive_fragment(a, 2, 1); ack_now_us += 1000; receive_fragment(b, 3, 1);
        if (remove) { etcp_link_close(a); a = NULL; }
        else a->link_status = 0;
        ack_now_us = begin + 2000; CHECK(!request() && conn->ack_resp_deadline_us == begin + 3000);
        ack_now_us = begin + 3000;
        struct ETCP_DGRAM* pkt = request(); CHECK(pkt && pkt->link == b && pkt->data[1] == 1);
        memory_pool_free(inst.pkt_pool, pkt); CHECK(!conn->ack_resp_timer);
        if (!remove) {
            a->link_status = 1; etcp_conn_wake_sender(conn);
            pkt = request(); CHECK(pkt && pkt->link == a && pkt->data[1] == 1);
            memory_pool_free(inst.pkt_pool, pkt);
        }
        teardown();
    }
}

/* Готовый DATA другого пути не должен обходить уже наступивший срок ACK. */
static void ack_due_other_link(void) {
    setup(); conn->got_initial_pkt = 1;
    ack_now_us = get_time_us(); ack_clock_active = 1;
    receive_fragment(b, 2, 1); b->inflight_lim_bytes = 1;
    ack_now_us += ETCP_ACK_DELAY_US;
    struct INFLIGHT_PACKET* fresh = enqueue(100);
    struct ETCP_DGRAM* pkt = request();
    CHECK(pkt && pkt->link == b && !pkt->payload_len && pkt->data[1] == 1 && fresh->state == INFLIGHT_STATE_WAIT_SEND);
    memory_pool_free(inst.pkt_pool, pkt);
    pkt = request(); CHECK(pkt && pkt->link == a && pkt->payload_len == 100);
    memory_pool_free(inst.pkt_pool, pkt); ack_all(); teardown();
}

/* RX-clock оценивает полосу; RTT/KA и ACK-delay используют момент обработки после очереди транспорта. */
static void transport_dispatch_clocks(void) {
    setup(); b->link_status = 0; conn->got_initial_pkt = 1; enable_models();
    ack_now_us = get_time_us(); ack_clock_active = 1;
    fill(a, 1, 1000);
    struct INFLIGHT_PACKET* packet = (struct INFLIGHT_PACKET*)conn->input_wait_ack->head;
    struct ETCP_TX_ATTEMPT* attempt = last_attempt(packet);
    uint64_t sent = attempt->sent_at_us;
    unsigned ack_size = etcp_ack_section_size(1);
    struct ETCP_DGRAM* pkt = memory_pool_alloc(inst.pkt_pool); CHECK(pkt);
    memset(pkt, 0, sizeof(*pkt) + ack_size + 5);
    ack_now_us = sent + 8000;
    pkt->link = a; pkt->received_us = sent + 5000; pkt->data_len = ack_size + 5;
    pkt->data[0] = ETCP_SECTION_ACK; pkt->data[1] = 1;
    for (unsigned i = 0; i < 4; i++) pkt->data[ETCP_ACK_BASE_SIZE + i] = packet->seq >> (8 * i);
    for (unsigned i = 0; i < 2; i++) pkt->data[ETCP_ACK_BASE_SIZE + 4 + i] = attempt->attempt_id >> (8 * i);
    pkt->data[ETCP_ACK_BASE_SIZE + 6] = 2000 & 255; pkt->data[ETCP_ACK_BASE_SIZE + 7] = 2000 >> 8;
    pkt->data[ack_size] = ETCP_SECTION_TIMESTAMP;
    uint16_t timestamp = (uint16_t)(sent / 100);
    pkt->data[ack_size + 1] = timestamp; pkt->data[ack_size + 2] = timestamp >> 8;
    etcp_conn_input(pkt);
    CHECK(last_sample.rtt_us == 6000 && a->data_rtt_us == 6000 && a->rtt_last == 80);
    CHECK(!conn->unacked_bytes && !a->inflight_bytes);

    uint64_t socket_us = ack_now_us - 3000;
    pkt = memory_pool_alloc(inst.pkt_pool); CHECK(pkt);
    memset(pkt, 0, sizeof(*pkt) + ETCP_PAYLOAD_HEADER_SIZE + 1);
    pkt->link = a; pkt->received_us = socket_us; pkt->data_len = ETCP_PAYLOAD_HEADER_SIZE + 1;
    pkt->data[0] = ETCP_SECTION_PAYLOAD; pkt->data[1] = 2; pkt->data[5] = 1;
    pkt->data[ETCP_PAYLOAD_HEADER_SIZE] = 0x5a;
    etcp_conn_input(pkt);
    CHECK(a->rx_data_bytes == 1 && a->rx_data_time_us == socket_us);
    CHECK(a->last_recv_local_time == ack_now_us / 100 && a->ack_q->count == 1);
    CHECK(((struct ACK_PACKET*)a->ack_q->head)->received_at_us == ack_now_us);
    CHECK(!request()); ack_now_us += ETCP_ACK_DELAY_US;
    pkt = request(); CHECK(pkt && pkt->data[1] == 1);
    uint32_t delay = 0;
    for (unsigned i = 0; i < 4; i++) delay |= (uint32_t)pkt->data[ETCP_ACK_BASE_SIZE + 6 + i] << (8 * i);
    CHECK(delay == ETCP_ACK_DELAY_US && a->rx_data_time_us == socket_us);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Transport clocks: dispatch_us=3000 DATA_RTT_us=%u KA_RTT_us=%u ACK_delay_us=%u RX_us=%llu",
        a->data_rtt_us, a->rtt_last * 100, delay, (unsigned long long)a->rx_data_time_us);
    memory_pool_free(inst.pkt_pool, pkt); teardown();
}

static void receive_admission_and_path_acks(void) {
    setup(); conn->got_initial_pkt = 1;
    fail_rx_data = 1; receive_fragment(a, 2, 1);
    CHECK(!fail_rx_data && !conn->recv_q->count && !a->ack_q->count && !conn->last_delivered_id);
    CHECK(!a->rx_data_bytes && !a->rx_data_time_us);
    fail_rx_fragment = 1; receive_fragment(a, 2, 2);
    CHECK(!fail_rx_fragment && !conn->recv_q->count && !a->ack_q->count);
    CHECK(!a->rx_data_bytes && !a->rx_data_time_us);
    queue_set_size_limit(conn->recv_q, 0); receive_fragment(a, 2, 3);
    CHECK(!conn->recv_q->count && !a->ack_q->count); queue_set_size_limit(conn->recv_q, -1);
    CHECK(!a->rx_data_bytes && !a->rx_data_time_us);
    receive_fragment(b, 2, 4);
    CHECK(conn->recv_q->count == 1 && b->ack_q->count == 1 && !a->ack_q->count);
    CHECK(b->rx_data_bytes == 1 && b->rx_data_time_us && !a->rx_data_bytes);
    receive_fragment(a, 2, 5);
    CHECK(conn->recv_q->count == 1 && a->ack_q->count == 1 && b->ack_q->count == 1);
    fail_ack_entry = 1; receive_fragment(a, 3, 6);
    CHECK(!fail_ack_entry && conn->recv_q->count == 2 && a->ack_q->count == 1);
    receive_fragment(b, 3, 7); CHECK(conn->recv_q->count == 2 && b->ack_q->count == 2);
    CHECK(a->rx_data_bytes == 2 && b->rx_data_bytes == 2);
    teardown();
}

/* Обратная очередь растягивает ACK, но не меняет темп DATA; send-интервал защищает от сжатия RX clock. */
static void receiver_delivery_clock(void) {
    const struct { uint32_t ack_us, send_us; uint64_t rx_bytes, rx_us; unsigned anchor; uint32_t expected; } cases[] = {
        {20000,2000,1000,5000,1,5000}, {100000,2000,1000,5000,1,5000},
        {100000,8000,1000,5000,1,8000}, {100000,2000,5000,1000,1,2000},
        {20000,2000,0,0,1,20000}, {20000,2000,1000,0,1,20000},
        {20000,2000,1000,5000,0,20000}
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        setup(); b->link_status = 0; a->data_rtt_us = 1000000;
        ack_now_us = get_time_us(); ack_clock_active = 1;
        enable_models(); fill(a,1,1);
        a->peer_rx_data_bytes = 10000; a->peer_rx_data_time_us = 1000000;
        struct INFLIGHT_PACKET* packet = enqueue(1000);
        struct ETCP_DGRAM* pkt = request(); CHECK(pkt && pkt->link == a); memory_pool_free(inst.pkt_pool,pkt);
        struct ETCP_TX_ATTEMPT* attempt = last_attempt(packet);
        attempt->delivered_time_us = ack_now_us-cases[i].ack_us;
        attempt->sent_at_us = ack_now_us-5000; attempt->first_tx_us = attempt->sent_at_us-cases[i].send_us;
        if (!cases[i].anchor) attempt->peer_rx_bytes_at_send = attempt->peer_rx_time_at_send = 0;
        uint64_t bytes = cases[i].rx_bytes ? 10000+cases[i].rx_bytes : 0;
        uint64_t time = cases[i].rx_bytes ? 1000000+cases[i].rx_us : 0;
        etcp_tx_ack(a,packet->seq,packet->last_attempt_id,0,bytes,time);
        CHECK(last_sample.delivered == 1000 && last_sample.acked_sacked == 1000);
        CHECK(last_sample.interval_us == cases[i].expected && last_sample.rtt_us == 5000);
        CHECK(a->bbr->min_rtt_us == 5000 && !b->delivered_bytes);
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Delivery clock: case=%u ACK_us=%u send_us=%u RX=%llu/%llu anchor=%u sample=%u/%u",
            i,cases[i].ack_us,cases[i].send_us,(unsigned long long)cases[i].rx_bytes,
            (unsigned long long)cases[i].rx_us,cases[i].anchor,last_sample.delivered,last_sample.interval_us);
        teardown();
    }
}

/* Реальный ACK handler: RX clock независим, арифметика округляется вверх без переполнения. */
static void receiver_bandwidth_bounds(void) {
    const struct { uint32_t delivered, local_us; uint64_t rx_bytes, rx_us; uint32_t expected; } cases[] = {
        {170502,150732,170502,186359,186359}, {1000,10000,3000,40001,13334},
        {1000,20000,5000,10000,2000}, {UINT32_MAX,10000,UINT32_MAX,UINT32_MAX,UINT32_MAX},
        {UINT32_MAX,10000,1,UINT32_MAX,0}, {1000,10000,1,(uint64_t)UINT32_MAX+1,0}
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        setup(); b->link_status = 0; a->data_rtt_us = 1000000;
        a->bbr = u_calloc(1, sizeof(*a->bbr)); CHECK(a->bbr); bbr_init(a->bbr);
        fill(a,1,1); // Первая неподтверждённая попытка удерживает flight, пока фиксируется база следующей.
        a->peer_rx_data_bytes = UINT64_C(1)<<60;
        a->peer_rx_data_time_us = UINT64_C(1)<<61;
        struct INFLIGHT_PACKET* packet = enqueue(1000);
        struct ETCP_DGRAM* pkt = request(); CHECK(pkt && pkt->link == a); memory_pool_free(inst.pkt_pool,pkt);
        struct ETCP_TX_ATTEMPT* attempt = last_attempt(packet);
        attempt->delivered_at_send = 10000;
        a->delivered_bytes = 10000 + (uint64_t)cases[i].delivered - 1000;
        a->bbr->delivered = (uint32_t)a->delivered_bytes;
        uint64_t now = get_time_us();
        attempt->delivered_time_us = now-cases[i].local_us;
        attempt->sent_at_us = attempt->first_tx_us = now-5000;
        a->data_rtt_us = 0;
        etcp_tx_ack(a,packet->seq,packet->last_attempt_id,0,
            (UINT64_C(1)<<60)+cases[i].rx_bytes,(UINT64_C(1)<<61)+cases[i].rx_us);
        CHECK(last_sample.delivered == cases[i].delivered && last_sample.acked_sacked == 1000);
        CHECK(last_sample.interval_us == cases[i].expected);
        if (last_sample.interval_us) CHECK((uint64_t)last_sample.delivered*cases[i].rx_us <=
            (uint64_t)last_sample.interval_us*cases[i].rx_bytes);
        CHECK(a->bbr->min_rtt_us == last_sample.rtt_us && a->data_rtt_us == last_sample.rtt_us);
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Receiver rate bound: case=%u sample=%u/%u RX=%llu/%llu RTT_us=%u",
            i,last_sample.delivered,last_sample.interval_us,(unsigned long long)cases[i].rx_bytes,
            (unsigned long long)cases[i].rx_us,last_sample.rtt_us);
        check_accounting(); teardown();
    }
}

/* Переставленные, повторные и чужие ACK не откатывают точку пути; после простоя база начинается заново. */
static void receiver_point_lifecycle(void) {
    setup(); b->link_status = 0; a->data_rtt_us = 1000000; enable_models();
    fill(a,1,1);
    a->peer_rx_data_bytes = 10000; a->peer_rx_data_time_us = 1000000;
    struct INFLIGHT_PACKET* packets[3]; uint32_t seq[3]; uint64_t ids[3];
    for (unsigned i = 0; i < 3; i++) {
        packets[i] = enqueue(1000); seq[i] = packets[i]->seq;
        struct ETCP_DGRAM* pkt = request(); CHECK(pkt); memory_pool_free(inst.pkt_pool,pkt);
        ids[i] = packets[i]->last_attempt_id;
        last_attempt(packets[i])->delivered_time_us = get_time_us()-10000;
    }
    const unsigned order[] = {2,0,1};
    for (unsigned j = 0; j < 3; j++) {
        unsigned i = order[j];
        etcp_tx_ack(a,seq[i],ids[i],0,11000+1000*i,1020000+20000*i);
        CHECK(a->peer_rx_data_bytes == 13000 && a->peer_rx_data_time_us == 1060000);
        CHECK(!b->peer_rx_data_bytes && last_sample.interval_us >= (j+1)*20000);
    }
    unsigned calls = bbr_calls;
    etcp_tx_ack(a,seq[2],ids[2],0,UINT64_MAX,UINT64_MAX);
    etcp_tx_ack(a,999,UINT64_MAX,0,UINT64_MAX,UINT64_MAX);
    struct INFLIGHT_PACKET* anchor = (struct INFLIGHT_PACKET*)conn->input_wait_ack->head;
    CHECK(anchor && anchor->seq == 1);
    etcp_tx_ack(a,999,anchor->last_attempt_id,0,UINT64_MAX,UINT64_MAX);
    CHECK(bbr_calls == calls && a->peer_rx_data_bytes == 13000 && a->peer_rx_data_time_us == 1060000);
    const uint64_t invalid[][2] = {{14000,0},{0,1070000},{14000,1050000},{12000,1070000},{13000,1070000}};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
        struct INFLIGHT_PACKET* packet = enqueue(1000);
        struct ETCP_DGRAM* pkt = request(); CHECK(pkt); memory_pool_free(inst.pkt_pool,pkt);
        last_attempt(packet)->delivered_time_us = get_time_us()-10000;
        etcp_tx_ack(a,packet->seq,packet->last_attempt_id,0,invalid[i][0],invalid[i][1]);
        CHECK(a->peer_rx_data_bytes == 13000 && a->peer_rx_data_time_us == 1060000);
        CHECK(last_sample.interval_us >= 10000);
    }
    ack_all(); CHECK(!a->inflight_packets);
    struct INFLIGHT_PACKET* first = enqueue(1000);
    struct ETCP_DGRAM* pkt = request(); CHECK(pkt); memory_pool_free(inst.pkt_pool,pkt);
    CHECK(!a->peer_rx_data_time_us && !last_attempt(first)->peer_rx_time_at_send);
    struct INFLIGHT_PACKET* second = enqueue(1000);
    pkt = request(); CHECK(pkt); memory_pool_free(inst.pkt_pool,pkt);
    last_attempt(first)->delivered_time_us = get_time_us()-10000;
    uint64_t after_idle = UINT64_C(3600)*1000000;
    etcp_tx_ack(a,first->seq,first->last_attempt_id,0,14000,after_idle);
    CHECK(last_sample.interval_us >= 10000 && last_sample.interval_us < 1000000);
    struct INFLIGHT_PACKET* third = enqueue(1000);
    pkt = request(); CHECK(pkt); memory_pool_free(inst.pkt_pool,pkt);
    CHECK(last_attempt(third)->peer_rx_time_at_send == after_idle);
    last_attempt(third)->delivered_time_us = get_time_us()-10000;
    etcp_tx_ack(a,third->seq,third->last_attempt_id,0,15000,after_idle+20000);
    CHECK(last_sample.interval_us == 20000);
    CHECK(second->active_attempts == 1);
    a->rx_data_bytes = 55; a->rx_data_time_us = 99;
    etcp_tx_link_reset(a,1);
    CHECK(!a->rx_data_bytes && !a->rx_data_time_us && !a->peer_rx_data_bytes && !a->peer_rx_data_time_us);
    check_accounting(); teardown();
}

/* Точка приёма занимает место только с положительными ACK; проверяем обе стороны границы MTU. */
static void receiver_point_wire(void) {
    setup(); b->link_status = 0; conn->got_initial_pkt = 1;
    receive_fragment(a,2,101); receive_fragment(a,2,101);
    CHECK(a->rx_data_bytes == 2 && a->ack_q->count == 1 && conn->recv_q->count == 1);
    a->rx_data_bytes = UINT64_C(0xfedcba9876543210); a->rx_data_time_us = UINT64_C(0x0011223344556677);
    const uint8_t expected[] = {0x10,0x32,0x54,0x76,0x98,0xba,0xdc,0xfe,0x77,0x66,0x55,0x44,0x33,0x22,0x11,0x00};
    mature_acks(a);
    struct ETCP_DGRAM* pkt = request(); CHECK(pkt && pkt->data[1] == 1 && !pkt->payload_len);
    CHECK(pkt->data_len == etcp_ack_section_size(1) && !memcmp(pkt->data+ETCP_ACK_BASE_SIZE+ETCP_ACK_ENTRY_SIZE,expected,16));
    memory_pool_free(inst.pkt_pool,pkt);
    receive_till(a,0,1); pkt = request(); CHECK(pkt && !pkt->data[1] && pkt->data_len == ETCP_ACK_BASE_SIZE);
    memory_pool_free(inst.pkt_pool,pkt);
    a->rx_data_bytes = 2; a->rx_data_time_us = get_time_us();
    unsigned max_body = a->mtu-WIRE_OVERHEAD(AF_INET);
    for (unsigned space = ETCP_ACK_ENTRY_SIZE+ETCP_ACK_RX_POINT_SIZE-1; space <= ETCP_ACK_ENTRY_SIZE+ETCP_ACK_RX_POINT_SIZE; space++) {
        receive_fragment(a,3+space,200+space);
        struct INFLIGHT_PACKET* packet = enqueue(max_body-ETCP_ACK_BASE_SIZE-ETCP_PAYLOAD_HEADER_SIZE-space);
        uint32_t seq = packet->seq;
        pkt = request(); CHECK(pkt && pkt->payload_len && pkt->data_len <= max_body);
        if (space == ETCP_ACK_ENTRY_SIZE+ETCP_ACK_RX_POINT_SIZE) CHECK(pkt->data[1] == 1 && pkt->data_len == max_body);
        else CHECK(!pkt->data[1] && a->ack_q->count == 1);
        memory_pool_free(inst.pkt_pool,pkt);
        if (a->ack_q->count) {
            mature_acks(a);
            pkt = request(); CHECK(pkt && pkt->data[1] == 1 && !pkt->payload_len);
            memory_pool_free(inst.pkt_pool,pkt);
        }
        ack(seq);
    }
    for (unsigned i = 100; i < 300; i++) receive_fragment(a,i,1000+i);
    mature_acks(a);
    unsigned count = (max_body-ETCP_ACK_BASE_SIZE-ETCP_ACK_RX_POINT_SIZE)/ETCP_ACK_ENTRY_SIZE;
    pkt = request(); CHECK(pkt && pkt->data[1] == count && pkt->data_len <= max_body && a->ack_q->count == 200-(int)count);
    memory_pool_free(inst.pkt_pool,pkt);
    pkt = request(); CHECK(pkt && pkt->data[1] == 200-count && !a->ack_q->count); memory_pool_free(inst.pkt_pool,pkt);
    teardown();
}

/* Усечённый feedback не подтверждает даже корректный entry, не трогает BW и не читает хвост буфера. */
static void receiver_point_truncated(void) {
    setup(); b->link_status = 0; enable_models();
    struct INFLIGHT_PACKET* packet = enqueue(1000);
    struct ETCP_DGRAM* sent = request(); CHECK(sent); memory_pool_free(inst.pkt_pool,sent);
    uint32_t seq = packet->seq; uint64_t id = packet->last_attempt_id;
    unsigned calls = bbr_calls, size = etcp_ack_section_size(1);
    for (unsigned len = ETCP_ACK_BASE_SIZE; len <= size; len++) {
        struct ETCP_DGRAM* pkt = memory_pool_alloc(inst.pkt_pool); CHECK(pkt);
        pkt->link = a; pkt->data_len = len; memset(pkt->data,0,size); pkt->data[0] = ETCP_SECTION_ACK; pkt->data[1] = 1;
        for (unsigned i = 0; i < 4; i++) pkt->data[ETCP_ACK_BASE_SIZE+i] = seq >> (8*i);
        for (unsigned i = 0; i < 2; i++) pkt->data[ETCP_ACK_BASE_SIZE+4+i] = id >> (8*i);
        pkt->data[ETCP_ACK_BASE_SIZE+ETCP_ACK_ENTRY_SIZE] = 1;
        pkt->data[ETCP_ACK_BASE_SIZE+ETCP_ACK_ENTRY_SIZE+8] = 1;
        etcp_conn_input(pkt);
        if (len < size) CHECK(a->inflight_bytes == 1000 && conn->unacked_bytes == 1000 && bbr_calls == calls);
        else CHECK(!a->inflight_bytes && !conn->unacked_bytes && bbr_calls == calls+1);
    }
    teardown();
    setup(); conn->got_initial_pkt = 1; a->rx_data_bytes = UINT64_MAX;
    receive_fragment(a,2,1); CHECK(conn->close_requested && !conn->callbacks_running && !a->ack_q->count);
    teardown();
}

static void peer_window_queries(void) {
    setup(); conn->last_delivered_id = 9; conn->last_rr_link = b;
    receive_till(b, 0, 1);
    struct ETCP_DGRAM* pkt = request(); CHECK(pkt && pkt->link == b && !pkt->payload_len && pkt->data[2] == 9);
    memory_pool_free(inst.pkt_pool, pkt); CHECK(!b->ack_reply_pending && !request()); teardown();

    setup(); conn->next_tx_id = MAX_INFLIGHT_SIZE + 1;
    struct ETCP_FRAGMENT* fragment = (struct ETCP_FRAGMENT*)queue_entry_new_from_pool(conn->io_pool); CHECK(fragment);
    fragment->ll.len = 1; fragment->ll.dgram = memory_pool_alloc(inst.data_pool); CHECK(fragment->ll.dgram);
    fragment->ll.dgram_pool = inst.data_pool;
    CHECK(queue_data_put(conn->input_queue, &fragment->ll) == 0 && conn->ack_window_timer);
    wait_sends(1); CHECK(last_sent_request && !last_sent_payload && last_sent_link);
    receive_till(last_sent_link, MAX_INFLIGHT_SIZE, 0);
    uasync_drain_immediate(ua); // queue_resume_callback выполняется через call_soon
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Peer window reply: next=%u till=%u query_timer=%d input=%d send=%d callback_pending=%d",
        conn->next_tx_id, conn->rx_ack_till, !!conn->ack_window_timer, conn->input_queue->count,
        conn->input_send_q->count, !!conn->input_queue->callback_suspended);
    CHECK(!conn->ack_window_timer && !conn->input_queue->count && conn->unacked_bytes == 1);
    CHECK(conn->input_send_q->count + conn->input_wait_ack->count == 1);
    teardown();
}

static void normalizer_receive(struct ETCP_CONN* c, struct ll_entry* entry) {
    CHECK(entry->len == 2 && entry->dgram[0] == 253 && received < sizeof(received_values));
    received_values[received++] = entry->dgram[1];
    queue_dgram_free(entry); queue_entry_free(entry);
    if (receive_action == 1) etcp_conn_reinit_id(c, "callback reset", c->reset_id + 1);
    if (receive_action == 2) etcp_connection_close(c);
}

static void receive_messages(uint32_t seq, unsigned count) {
    CHECK(count && count <= 3);
    struct ETCP_DGRAM* pkt = memory_pool_alloc(inst.pkt_pool); CHECK(pkt);
    memset(pkt, 0, sizeof(*pkt)); pkt->link = a; pkt->data_len = ETCP_PAYLOAD_HEADER_SIZE + 4 * count;
    pkt->data[0] = ETCP_SECTION_PAYLOAD;
    for (unsigned i = 0; i < 4; i++) pkt->data[1+i] = seq >> (8*i);
    pkt->data[5] = (uint8_t)seq;
    for (unsigned i = 0; i < count; i++) {
        unsigned offset = ETCP_PAYLOAD_HEADER_SIZE + 4*i;
        pkt->data[offset] = 2; pkt->data[offset+2] = 253; pkt->data[offset+3] = 0x20+i;
    }
    etcp_conn_input(pkt);
}

static void confirm_test_session(void) {
    etcp_session_cancel(conn); conn->session_required = 0; etcp_conn_ready(conn);
}

static void normalizer_rx_oom(void) {
    for (unsigned nth = 1; nth <= 2; nth++) {
        setup(); CHECK(etcp_bind(&inst, 253, normalizer_receive) == 0);
        uint64_t epoch = conn->reset_id;
        fail_rx_message_at = nth; receive_messages(1, 3);
        CHECK(!fail_rx_message_at && received == nth-1 && conn->reset_id != epoch);
        CHECK(conn->reinit_pending && conn->session_required && conn->session_timer && !conn->initialized);
        CHECK(!conn->last_delivered_id && !conn->recv_q->count && !conn->output_queue->count && !a->ack_q->count);
        CHECK(!conn->normalizer->recvpart && !conn->normalizer->output->count && !conn->active_attempts);
        CHECK(a->link_status && b->link_status && conn->links_up);
        confirm_test_session(); receive_messages(1, 1);
        CHECK(received == nth && received_values[nth-1] == 0x20 && !conn->reinit_pending);
        teardown();
    }
}

static void normalizer_tx_oom(void) {
    for (unsigned fault = 0; fault < 6; fault++) {
        setup(); uint64_t epoch = conn->reset_id;
        struct ll_entry* entry = ll_alloc_lldgram(64); CHECK(entry); entry->len = 64; memset(entry->dgram, 253, 64);
        if (fault == 0) fail_rx_data = 1;
        if (fault == 1) fail_rx_fragment = 1;
        if (fault == 2) fail_tx_inflight = 1;
        if (fault == 3) fail_normalizer_timer = 1;
        if (fault == 4) {
            conn->input_queue->callback_suspended = 1;
            struct ll_entry* busy = ll_alloc_lldgram(1); CHECK(busy); busy->len = 1;
            CHECK(queue_data_put(conn->input_queue, busy) == 0); fail_normalizer_waiter = 1;
        }
        if (fault == 5) queue_set_size_limit(conn->input_queue, 0);
        CHECK(etcp_send(conn, entry) == 0); uasync_drain_immediate(ua);
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Normalizer TX fault=%u reset=%u pending=%u timer=%d DATA_sends=%u",
            fault, conn->reset_count, conn->reinit_pending, !!conn->session_timer, sends);
        CHECK(conn->reset_id != epoch && conn->reinit_pending && conn->session_timer);
        CHECK(!conn->unacked_bytes && !conn->active_attempts && !conn->input_queue->count && !sends);
        CHECK(!conn->normalizer->input->count && !conn->normalizer->data && !conn->normalizer->flush_timer);
        CHECK(!fail_rx_data && !fail_rx_fragment && !fail_tx_inflight && !fail_normalizer_timer && !fail_normalizer_waiter);
        queue_set_size_limit(conn->input_queue, -1); confirm_test_session();
        entry = ll_alloc_lldgram(16); CHECK(entry); entry->len = 16; memset(entry->dgram, 253, 16);
        CHECK(etcp_send(conn, entry) == 0); wait_sends(1); ack_all(); teardown();
    }
}

static void normalizer_callback_abort(void) {
    for (unsigned deferred = 0; deferred < 2; deferred++) for (unsigned action = 1; action <= 2; action++) {
        setup(); CHECK(etcp_conn_ref_take(conn) == 0); CHECK(etcp_bind(&inst, 253, normalizer_receive) == 0);
        queue_set_callback_defer(conn->normalizer->output, deferred); receive_action = action;
        receive_messages(1, 2); uasync_drain_immediate(ua);
        CHECK(received == 1 && received_values[0] == 0x20 && !conn->callbacks_running);
        if (action == 1) CHECK(conn->reinit_pending && !conn->last_delivered_id && !conn->normalizer->output->count);
        else CHECK(conn->state == 2 && !conn->normalizer && !conn->links);
        etcp_conn_ref_free(conn); teardown();
    }
}

static void ignored_queue_callback(struct ll_queue* q, void* arg) { (void)q; ++*(unsigned*)arg; }

static void reset_without_allocation(void) {
    setup(); fill(a, 1, 64); unsigned notifications = 0;
    struct ll_queue* queues[] = {conn->input_queue, conn->output_queue, conn->recv_q};
    for (unsigned i = 0; i < 3; i++) {
        queue_set_callback(queues[i], NULL, NULL);
        struct ETCP_FRAGMENT* frag = (struct ETCP_FRAGMENT*)queue_entry_new_from_pool(conn->io_pool); CHECK(frag);
        frag->seq = 4+i; frag->ll.len = 8; frag->ll.dgram = memory_pool_alloc(inst.data_pool); CHECK(frag->ll.dgram);
        frag->ll.dgram_pool = inst.data_pool;
        CHECK((i == 2 ? queue_data_put_with_index(queues[i], &frag->ll) : queue_data_put(queues[i], &frag->ll)) == 0);
    }
    struct PKTNORM* pn = conn->normalizer;
    queue_set_callback(pn->input, NULL, NULL); queue_set_callback(pn->output, NULL, NULL);
    CHECK(queue_data_put(pn->input, ll_alloc_lldgram(10)) == 0);
    CHECK(queue_data_put(pn->output, ll_alloc_lldgram(12)) == 0);
    CHECK(queue_waiter_wait(conn->input_queue, &pn->input_waiter_handle, ignored_queue_callback, &notifications) == 0);
    queue_set_empty_callback(pn->input, ignored_queue_callback, &notifications);
    queue_set_on_get(pn->input, ignored_queue_callback, &notifications);
    pn->data = memory_pool_alloc(inst.data_pool); CHECK(pn->data); pn->data_ptr = 20;
    pn->recvpart = ll_alloc_lldgram(32); CHECK(pn->recvpart); pn->recvpart->len = 3;
    no_alloc = 1; etcp_conn_reset(conn); no_alloc = 0;
    CHECK(!unexpected_alloc && !conn->active_attempts && !conn->unacked_bytes && !conn->input_wait_ack->count);
    CHECK(!pn->data && !pn->recvpart && !pn->input->count && !pn->output->count && !pn->input_waiter_handle.internal);
    uasync_drain_immediate(ua); CHECK(!notifications); teardown();
}

static void restart_allocation_failures(void) {
    for (unsigned fault = 0; fault < 4; fault++) {
        setup(); CHECK(etcp_conn_ref_take(conn) == 0);
        if (fault < 2) fail_session_timer_at = fault + 1;
        if (fault == 2) fail_rng = 1;
        etcp_conn_fatal_reinit(conn, "restart fault injection");
        if (fault == 1) test_clock_next_timer(ua);
        if (fault == 3) {
            uint64_t epoch = conn->reset_id; fail_alloc = 1;
            test_clock_next_timer(ua); CHECK(!fail_alloc && conn->session_timer && !session_sends);
            test_clock_next_timer(ua); CHECK(session_sends == 2 && conn->reset_id == epoch && !conn->close_requested);
        } else {
            CHECK(conn->close_requested && !fail_session_timer_at && !fail_rng);
            uasync_drain_immediate(ua); CHECK(conn->state == 2 && !conn->session_timer);
        }
        etcp_conn_ref_free(conn); teardown();
    }
}

/* Проходит настоящий wire decoder: номер может совпадать у разных seq, нулевой запрещён. */
static void receive_attempt_number(struct ETCP_LINK* link, uint32_t seq, uint16_t number) {
    struct ETCP_DGRAM* pkt = memory_pool_alloc(inst.pkt_pool); CHECK(pkt);
    pkt->link = link; pkt->data_len = etcp_ack_section_size(1);
    memset(pkt->data,0,pkt->data_len); pkt->data[0] = ETCP_SECTION_ACK; pkt->data[1] = 1;
    for (unsigned i = 0; i < 4; i++) pkt->data[ETCP_ACK_BASE_SIZE+i] = seq >> (8*i);
    pkt->data[ETCP_ACK_BASE_SIZE+4] = number; pkt->data[ETCP_ACK_BASE_SIZE+5] = number >> 8;
    etcp_conn_input(pkt); check_accounting();
}

/* Один ACK snapshot точно восстанавливает оба приёма, включая времена далеко за оборотом старых uint16 clocks. */
static void ack_timing_batch(void) {
    setup(); b->link_status = 0; conn->got_initial_pkt = 1;
    ack_clock_active = 1; ack_now_us = UINT64_C(5000000000);
    uint64_t first = ack_now_us;
    receive_fragment(a,2,1); ack_now_us += 1234;
    receive_fragment(a,3,1); ack_now_us += ETCP_ACK_DELAY_US;
    struct ETCP_DGRAM* pkt = request(); CHECK(pkt && pkt->data[1] == 2);
    const uint8_t* point = pkt->data + ETCP_ACK_BASE_SIZE + 2 * ETCP_ACK_ENTRY_SIZE;
    uint64_t created = 0;
    for (unsigned i = 0; i < 8; i++) created |= (uint64_t)point[16+i] << (8*i);
    CHECK(created == ack_now_us);
    for (unsigned n = 0; n < 2; n++) {
        const uint8_t* entry = pkt->data + ETCP_ACK_BASE_SIZE + n * ETCP_ACK_ENTRY_SIZE;
        uint32_t hold = 0;
        for (unsigned i = 0; i < 4; i++) hold |= (uint32_t)entry[6+i] << (8*i);
        CHECK(created - hold == first + n * 1234);
    }
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "ACK timing batch: snapshot=%llu first=%llu second=%llu exact",
        (unsigned long long)created, (unsigned long long)first, (unsigned long long)(first+1234));
    memory_pool_free(inst.pkt_pool,pkt); teardown();
}

/* Полный wire-ACK: асимметрия направлений сохраняется независимо от смещения часов; malformed time не ломает доставку. */
static void ack_timing_clocks(void) {
    const int64_t offsets[] = {INT64_C(3000000000), -INT64_C(3000000000)};
    for (unsigned k = 0; k < 2; k++) {
        struct ETCP_ACK_TIMING previous = {0};
        for (unsigned direction = 0; direction < 2; direction++) {
            setup(); b->link_status = 0; conn->got_initial_pkt = 1; enable_models();
            ack_clock_active = 1; ack_now_us = UINT64_C(5000000000);
            fill(a,1,1000);
            struct INFLIGHT_PACKET* packet = (struct INFLIGHT_PACKET*)conn->input_wait_ack->head;
            struct ETCP_TX_ATTEMPT* attempt = last_attempt(packet);
            uint32_t seq = packet->seq; uint64_t id = attempt->attempt_id, sent = attempt->sent_at_us;
            uint32_t forward = direction ? 100000 : 10000, backward = direction ? 10000 : 100000;
            uint64_t peer_data = (int64_t)sent + offsets[k] + forward, peer_ack = peer_data + 2000;
            ack_now_us = sent + forward + 2000 + backward;
            for (unsigned duplicate = 0; duplicate < 2; duplicate++) {
                struct ETCP_DGRAM* pkt = memory_pool_alloc(inst.pkt_pool); CHECK(pkt);
                unsigned size = etcp_ack_section_size(1); memset(pkt,0,sizeof(*pkt)+size);
                pkt->link = a; pkt->data_len = size; pkt->received_us = ack_now_us-1000;
                pkt->data[0] = ETCP_SECTION_ACK; pkt->data[1] = 1;
                uint8_t* entry = pkt->data + ETCP_ACK_BASE_SIZE;
                for (unsigned i = 0; i < 4; i++) { entry[i] = seq >> (8*i); entry[6+i] = 2000 >> (8*i); }
                entry[4] = id; entry[5] = id >> 8;
                uint8_t* point = entry + ETCP_ACK_ENTRY_SIZE;
                for (unsigned i = 0; i < 8; i++) {
                    point[i] = UINT64_C(1000) >> (8*i); point[8+i] = (peer_data-3000) >> (8*i);
                    point[16+i] = (peer_ack + duplicate * 1000) >> (8*i);
                }
                etcp_conn_input(pkt);
                CHECK(a->ack_timing.attempt_id == id && a->ack_timing.local_send_us == sent && a->ack_timing.local_ack_us == ack_now_us);
                CHECK(a->ack_timing.peer_receive_us == peer_data && a->ack_timing.peer_ack_us == peer_ack);
                CHECK(a->ack_timing.forward_clock_us == offsets[k]+forward && a->ack_timing.backward_clock_us == (int64_t)backward-offsets[k]);
                CHECK(a->ack_timing.forward_clock_us + a->ack_timing.backward_clock_us == 110000);
                CHECK(bbr_calls == 1 && last_sample.rtt_us == 110000 && !a->inflight_bytes && !conn->unacked_bytes);
            }
            if (direction) {
                CHECK(a->ack_timing.forward_clock_us-previous.forward_clock_us == 90000);
                CHECK(a->ack_timing.backward_clock_us-previous.backward_clock_us == -90000);
            }
            previous = a->ack_timing;
            const uint64_t invalid[] = {1000, UINT64_MAX, 0};
            for (unsigned n = 0; n < sizeof(invalid)/sizeof(invalid[0]); n++) {
                fill(a,1,1000); packet = (struct INFLIGHT_PACKET*)conn->input_wait_ack->head;
                ack_now_us += 10000;
                etcp_tx_ack_at(a,packet->seq,packet->last_attempt_id,2000,0,0,invalid[n],ack_now_us);
                CHECK(!memcmp(&a->ack_timing,&previous,sizeof(previous)) && !a->inflight_bytes && !conn->unacked_bytes);
            }
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "ACK clocks: offset=%lld forward=%u backward=%u deltas=%lld/%lld RTT=%u valid, duplicate/malformed checked",
                (long long)offsets[k],forward,backward,(long long)previous.forward_clock_us,
                (long long)previous.backward_clock_us,110000u);
            etcp_tx_link_reset(a,0); CHECK(!a->ack_timing.attempt_id && !a->ack_timing.local_ack_us);
            teardown();
        }
    }
}

/* Независимые часы: постоянный offset и ±6с/сутки у каждого узла. Таймеры между точками не опрашиваем. */
static uint64_t drift_clock(uint64_t physical_us, int64_t offset_us, int direction) {
    return INT64_C(5000000000) + (int64_t)physical_us + offset_us + direction * (int64_t)(physical_us / 14400);
}

static void ack_timing_drift(void) {
    unsigned pairs = 0; uint32_t max_error = 0;
    for (int offset_sign = -1; offset_sign <= 1; offset_sign += 2)
    for (int drift_sign = -1; drift_sign <= 1; drift_sign += 2)
    for (unsigned day = 0; day < 2; day++)
    for (unsigned interval = 100000; interval <= 1000000; interval *= 10)
    for (unsigned change = 0; change < 3; change++) {
        setup(); b->link_status = 0; conn->got_initial_pkt = 1; enable_models(); ack_clock_active = 1;
        struct ETCP_ACK_TIMING points[2];
        for (unsigned n = 0; n < 2; n++) {
            uint64_t physical = day * UINT64_C(86400000000) + (uint64_t)n * interval;
            uint32_t forward = 10000 + (n && change == 1 ? 50000 : 0);
            uint32_t backward = 10000 + (n && change == 2 ? 50000 : 0);
            ack_now_us = drift_clock(physical,0,drift_sign);
            fill(a,1,1000);
            struct INFLIGHT_PACKET* packet = (struct INFLIGHT_PACKET*)conn->input_wait_ack->head;
            uint32_t seq = packet->seq; uint64_t id = packet->last_attempt_id, sent = ack_now_us;
            uint64_t peer_receive = drift_clock(physical+forward,offset_sign*INT64_C(3000000000),-drift_sign);
            ack_now_us = peer_receive;
            CHECK(etcp_tx_queue_ack(a,seq,id) == 0);
            ack_now_us = drift_clock(physical+forward+2000,offset_sign*INT64_C(3000000000),-drift_sign);
            uint64_t peer_ack = ack_now_us;
            a->ack_reply_pending = 1;
            struct ETCP_DGRAM* pkt = request(); CHECK(pkt && pkt->data[1] == 1);
            pkt->data_len = etcp_ack_section_size(1); // Проверяем настоящий ACK builder/decoder, без соседних control-секций.
            ack_now_us = drift_clock(physical+forward+2000+backward,0,drift_sign);
            pkt->received_us = ack_now_us; etcp_conn_input(pkt);
            points[n] = a->ack_timing;
            CHECK(points[n].attempt_id == id && points[n].local_send_us == sent && points[n].local_ack_us == ack_now_us);
            CHECK(points[n].peer_receive_us == peer_receive && points[n].peer_ack_us == peer_ack);
            CHECK(!a->inflight_bytes && !conn->unacked_bytes);
        }
        struct ETCP_ACK_TREND trend;
        CHECK(etcp_ack_timing_trend(&points[0],&points[1],2000000,ETCP_ACK_CLOCK_DRIFT_PPM,&trend));
        CHECK(llabs(trend.forward_delta_us - (change == 1 ? 50000 : 0)) <= trend.clock_error_us);
        CHECK(llabs(trend.backward_delta_us - (change == 2 ? 50000 : 0)) <= trend.clock_error_us);
        if (!change) {
            CHECK(llabs(trend.forward_delta_us) <= trend.clock_error_us && llabs(trend.backward_delta_us) <= trend.clock_error_us);
        } else CHECK(llabs(change == 1 ? trend.forward_delta_us : trend.backward_delta_us) > trend.clock_error_us);
        if (trend.clock_error_us > max_error) max_error = trend.clock_error_us;
        CHECK(!etcp_ack_timing_trend(&points[1],&points[1],2000000,ETCP_ACK_CLOCK_DRIFT_PPM,&trend) && !trend.interval_us);
        CHECK(!etcp_ack_timing_trend(&points[1],&points[0],2000000,ETCP_ACK_CLOCK_DRIFT_PPM,&trend));
        CHECK(!etcp_ack_timing_trend(&points[0],&points[1],interval/2,ETCP_ACK_CLOCK_DRIFT_PPM,&trend));
        struct ETCP_ACK_TIMING reordered = points[1]; reordered.peer_receive_us = points[0].peer_receive_us-1;
        CHECK(!etcp_ack_timing_trend(&points[0],&reordered,2000000,ETCP_ACK_CLOCK_DRIFT_PPM,&trend));
        pairs++; teardown();
    }
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "ACK drift: %u pairs offset=+/-3000s node_drift=+/-6s/day age=0/24h "
        "interval=100/1000ms max_error_us=%u; stable path and +50ms DATA/ACK changes distinguished", pairs, max_error);
}

static void compact_attempt_identity(void) {
    setup(); enable_models(); fill(a,2,100); b->link_status = 0;
    struct INFLIGHT_PACKET* first = (struct INFLIGHT_PACKET*)conn->input_wait_ack->head;
    struct INFLIGHT_PACKET* second = (struct INFLIGHT_PACKET*)first->ll.next;
    CHECK(first && second && first->seq == 1 && second->seq == 2);
    CHECK(first->last_attempt_id == etcp_tx_attempt_id(1,1) && second->last_attempt_id == etcp_tx_attempt_id(2,1));
    CHECK(last_attempt(first)->seq == 1 && last_attempt(second)->seq == 2 && a->attempts->count == 2);
    conn->got_initial_pkt = 1;
    receive_fragment(a,2,1); receive_fragment(a,3,1); receive_fragment(a,2,1);
    CHECK(a->ack_q->count == 2 && a->rx_data_bytes == 3);
    mature_acks(a);
    struct ETCP_DGRAM* pkt = request(); CHECK(pkt && !pkt->payload_len && pkt->data[1] == 2);
    const uint8_t entry1[] = {2,0,0,0,1,0}, entry2[] = {3,0,0,0,1,0};
    CHECK(!memcmp(pkt->data+ETCP_ACK_BASE_SIZE,entry1,sizeof(entry1)));
    CHECK(!memcmp(pkt->data+ETCP_ACK_BASE_SIZE+ETCP_ACK_ENTRY_SIZE,entry2,sizeof(entry2)));
    CHECK(pkt->data_len == etcp_ack_section_size(2)); memory_pool_free(inst.pkt_pool,pkt);
    receive_attempt_number(a,1,0); CHECK(a->inflight_bytes == 200 && conn->unacked_bytes == 200 && !a->bbr->delivered);
    receive_attempt_number(b,1,1); CHECK(a->inflight_bytes == 200 && conn->input_wait_ack->count == 1);
    CHECK(conn->unacked_bytes == 100 && !a->bbr->delivered && !b->bbr->delivered);
    receive_attempt_number(a,1,1); CHECK(a->inflight_bytes == 100 && a->bbr->delivered == 100 && !b->bbr->delivered);
    receive_attempt_number(a,1,1); CHECK(a->inflight_bytes == 100 && a->bbr->delivered == 100);
    receive_attempt_number(a,2,1); CHECK(!a->inflight_bytes && !conn->unacked_bytes && a->bbr->delivered == 200);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG,"Compact attempts: same number on different seq stays isolated; zero/foreign/duplicate ACK checked");
    teardown();
}

/* Номер расходуется даже при отказе send; 65535 передаётся полностью, следующая попытка меняет эпоху. */
static void compact_attempt_exhaustion(void) {
    setup(); b->link_status = 0;
    struct INFLIGHT_PACKET* packet = enqueue(1);
    struct ETCP_DGRAM* pkt = etcp_request_pkt(conn); CHECK(pkt && pkt->attempt);
    CHECK(pkt->attempt->attempt_id == etcp_tx_attempt_id(1,1));
    etcp_tx_complete(pkt,0); memory_pool_free(inst.pkt_pool,pkt);
    CHECK(packet->attempt_count == 1 && !packet->last_attempt_id && !packet->send_count && !a->inflight_packets);
    pkt = request(); CHECK(pkt && packet->attempt_count == 2 && packet->last_attempt_id == etcp_tx_attempt_id(1,2));
    memory_pool_free(inst.pkt_pool,pkt); ack_all();
    packet = enqueue(1); packet->attempt_count = UINT16_MAX-1;
    pkt = request(); CHECK(pkt && packet->attempt_count == UINT16_MAX);
    const uint8_t last_data[] = {ETCP_SECTION_PAYLOAD,2,0,0,0,255,255,0x5a};
    CHECK(pkt->data_len == ETCP_ACK_BASE_SIZE+sizeof(last_data));
    CHECK(!memcmp(pkt->data+ETCP_ACK_BASE_SIZE,last_data,sizeof(last_data)));
    CHECK(packet->last_attempt_id == etcp_tx_attempt_id(2,UINT16_MAX)); memory_pool_free(inst.pkt_pool,pkt);
    conn->initialized = 0; test_clock_run_for(ua,120); conn->initialized = 1;
    CHECK(!a->inflight_packets && packet->state == INFLIGHT_STATE_WAIT_SEND);
    uint64_t epoch = conn->reset_id;
    CHECK(!request() && conn->reset_id != epoch && conn->reinit_pending && conn->session_required);
    CHECK(!conn->active_attempts && !conn->unacked_bytes && !conn->input_send_q->count && !conn->input_wait_ack->count);
    confirm_test_session(); packet = enqueue(1); pkt = request(); CHECK(pkt);
    CHECK(packet->seq == 1 && packet->attempt_count == 1 && packet->last_attempt_id == etcp_tx_attempt_id(1,1));
    memory_pool_free(inst.pkt_pool,pkt); ack_all(); teardown();
}

/* Полный круг seq также не должен повторять пару в прежней эпохе, даже с первым номером попытки. */
static void compact_sequence_exhaustion(void) {
    setup(); b->link_status = 0;
    conn->next_tx_id = UINT32_MAX; conn->rx_ack_till = UINT32_MAX-1;
    const uint8_t normalized[] = {1,0,253};
    CHECK(etcp_int_send(conn,normalized,sizeof(normalized)) == 0 && !conn->next_tx_id);
    struct ETCP_DGRAM* pkt = request(); CHECK(pkt);
    CHECK(pkt->attempt == NULL && a->attempts->count == 1);
    struct ETCP_TX_ATTEMPT* attempt = (struct ETCP_TX_ATTEMPT*)a->attempts->head;
    CHECK(attempt->seq == UINT32_MAX && attempt->attempt_id == etcp_tx_attempt_id(UINT32_MAX,1));
    memory_pool_free(inst.pkt_pool,pkt);
    uint64_t epoch = conn->reset_id;
    CHECK(etcp_int_send(conn,normalized,sizeof(normalized)) == 0);
    CHECK(conn->input_queue->count == 1); a->burst_active = 1;
    CHECK(!request()); // input_queue callback выполняется при следующем проходе отправителя.
    CHECK(conn->reset_id != epoch && conn->reinit_pending && conn->session_required && conn->next_tx_id == 1);
    CHECK(!conn->active_attempts && !conn->unacked_bytes && !conn->input_queue->count);
    confirm_test_session(); teardown();
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    if (getenv("UTUN_TEST_DEBUG")) debug_set_category_level(DEBUG_CATEGORY_ETCP, DEBUG_LEVEL_DEBUG);
    test_clock_init(10);
    size_t baseline = u_get_allocated_count();
    void (*cases[])(void) = {selection, boundaries_ack_burst, late_ack_and_migration, lost_ack_and_shrink,
        grow_and_session, allocation_and_send_failure, removed_link_and_bbr, pacing_reset_close, initial_readiness,
        config_limits, timer_failure, immediate_send, ack_without_data_pacing, bbr_delivery_clock, bbr_probe_window, tcp_initial_window,
        rto_path_rtt_growth, rto_timestamp_rearm, retransmission_timeout, pacing_rate_changes, pacing_timer_failure,
        retry_rotation, independent_attempt_acks,
        retry_waits_for_alternative, retry_alternative_lifecycle,
        send_queue_scan, send_queue_input_admission,
        retry_full_round_history, retry_history_slot_reuse, fast_stall_progress, stalled_retry_selection, input_admission_callbacks,
        ack_absolute_deadline, ack_piggyback_deadlines, ack_partial_piggyback, ack_timer_interleaving,
        ack_urgent_and_oom, ack_partial_frame_and_lifecycle, ack_sender_failure, ack_link_lifecycle, ack_due_other_link,
        transport_dispatch_clocks, ack_timing_batch, ack_timing_clocks, ack_timing_drift,
        receive_admission_and_path_acks, peer_window_queries, pacing_continuity, pacing_data_units, attempt_memory_limits,
        normalizer_rx_oom, normalizer_tx_oom, normalizer_callback_abort, reset_without_allocation, restart_allocation_failures,
        receiver_delivery_clock, receiver_bandwidth_bounds, receiver_point_lifecycle, receiver_point_wire, receiver_point_truncated,
        compact_attempt_identity, compact_attempt_exhaustion, compact_sequence_exhaustion,
        pacing_first_ready, pacing_late_wakeup, pacing_rearm_failure, pacing_ready_lifecycle};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        cases[i](); CHECK(u_get_allocated_count() == baseline);
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS ETCP window scenario %u", i);
    }
    test_clock_finish();
    return 0;
}
