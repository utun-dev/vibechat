#include "etcp_link_io.h"
/* Real ETCP lifecycle, AES-CCM and common ingress; deterministic encrypted wire faults. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "etcp.h"
#include "etcp_api.h"
#include "etcp_connections.h"
#include "etcp_debug.h"
#include "etcp_session.h"
#include "config_parser.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_clock.h"
#include "topo_node_rtt.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__func__,__LINE__,#x); exit(1); } } while (0)
struct endpoint { struct UTUN_INSTANCE inst; struct utun_config cfg; struct ETCP_CONN* c; struct ETCP_SOCKET sock; struct ETCP_LINK links[2]; };
struct frame { int from; size_t len; uint8_t bytes[PACKET_DATA_SIZE]; };
static struct endpoint ep[2];
static struct UASYNC* ua;
static struct frame frames[4096], saved[4];
static unsigned head, tail, drops, reinit[2], down[2];
static int drop[2][3], blackhole, dead_primary;
static int fail_message;
static unsigned messages[2];
struct ll_entry* __real_ll_alloc_lldgram(uint16_t);
struct ll_entry* __wrap_ll_alloc_lldgram(uint16_t len) {
    if (fail_message) { fail_message--; return NULL; }
    return __real_ll_alloc_lldgram(len);
}
static char dump_message[512];
static unsigned dump_count, dump_warnings;

// Перехватывает диагностический вывод без временных файлов и изменения библиотек.
static void capture_dump(int level, const char* category, const char* message) {
    if (strcmp(category, "dump")) return;
    if (level == DEBUG_LEVEL_WARN) { dump_warnings++; return; }
    if (level != DEBUG_LEVEL_DEBUG) return;
    snprintf(dump_message, sizeof(dump_message), "%s", message);
    dump_count++;
}

// ACK/payload должны корректно декодировать все 32 бита, включая границу знакового int.
static void debug_sequence_boundaries(void) {
    const uint32_t values[] = {0, 1, 0x7f000000, 0x7fffffff, 0x80000000, 0xff000000, UINT32_MAX};
    unsigned capacity = ETCP_ACK_BASE_SIZE > ETCP_PAYLOAD_HEADER_SIZE ? ETCP_ACK_BASE_SIZE : ETCP_PAYLOAD_HEADER_SIZE;
    struct ETCP_DGRAM* pkt = u_calloc(1, sizeof(*pkt) + capacity); CHECK(pkt);
    debug_level_t saved = g_debug_config.category_levels[DEBUG_CATEGORY_DUMP];
    int console = g_debug_config.console_enabled;
    debug_set_category_level(DEBUG_CATEGORY_DUMP, DEBUG_LEVEL_DEBUG);
    debug_enable_console(0);
    debug_set_log_hook(capture_dump);
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        for (int ack = 0; ack < 2; ack++) {
            memset(pkt->data, 0, capacity);
            pkt->data[0] = ack ? ETCP_SECTION_ACK : ETCP_SECTION_PAYLOAD;
            pkt->data_len = ack ? ETCP_ACK_BASE_SIZE : ETCP_PAYLOAD_HEADER_SIZE;
            for (int j = 0; j < 4; j++) pkt->data[1 + ack + j] = (uint8_t)(values[i] >> (8 * j));
            const uint16_t attempt_no = UINT16_MAX;
            if (!ack) for (unsigned j = 0; j < 2; j++) pkt->data[5+j] = (uint8_t)(attempt_no >> (8*j));
            if (ack) pkt->data[6] = pkt->data[7] = 255;
            dump_count = 0; dump_message[0] = 0;
            etcp_dump_pkt_sections(pkt, NULL, 0);
            char expected[80];
            if (ack) snprintf(expected, sizeof(expected), " ack=%u/0 dup=65535", values[i]);
            else snprintf(expected, sizeof(expected), " payload=0(seq=%u attempt=%u)", values[i], attempt_no);
            CHECK(dump_count == 1 && strstr(dump_message, expected));
        }
    }
    // Укороченная секция не должна читать отсутствующие байты или печатать выдуманный seq.
    for (int ack = 0; ack < 2; ack++) {
        pkt->data[0] = ack ? ETCP_SECTION_ACK : ETCP_SECTION_PAYLOAD;
        for (unsigned len = 0; len < (ack ? ETCP_ACK_BASE_SIZE : ETCP_PAYLOAD_HEADER_SIZE); len++) {
            pkt->data_len = len; dump_count = 0; dump_message[0] = 0;
            etcp_dump_pkt_sections(pkt, NULL, 1);
            CHECK(dump_count == 1 && !strstr(dump_message, ack ? " ack=" : " payload="));
        }
    }
    // Длинный пакет обязан безопасно усекать дамп, в том числе при выключенной категории.
    u_free(pkt);
    pkt = u_calloc(1, sizeof(*pkt) + 32 * ETCP_ACK_BASE_SIZE); CHECK(pkt);
    for (int ack = 0; ack < 2; ack++) {
        unsigned width = ack ? ETCP_ACK_BASE_SIZE : 5;
        pkt->data_len = 32 * width;
        memset(pkt->data, 0, 32 * ETCP_ACK_BASE_SIZE);
        for (unsigned offset = 0; offset < pkt->data_len; offset += width) {
            pkt->data[offset] = ack ? ETCP_SECTION_ACK : ETCP_SECTION_TIMESTAMP;
            memset(pkt->data + offset + (ack ? 2 : 1), 255, width - (ack ? 2 : 1));
        }
        for (int enabled = 0; enabled < 2; enabled++) {
            debug_set_category_level(DEBUG_CATEGORY_DUMP, enabled ? DEBUG_LEVEL_DEBUG : DEBUG_LEVEL_DISABLED);
            dump_count = dump_warnings = 0; dump_message[0] = 0;
            etcp_dump_pkt_sections(pkt, NULL, enabled);
            if (enabled) CHECK(dump_count == 1 && dump_warnings == 1 && strstr(dump_message, " [truncated]"));
            else CHECK(dump_count == 0 && dump_warnings == 0);
        }
    }
    // Положительный ACK: footer полностью проверяется до чтения, следующая секция не теряется.
    u_free(pkt);
    unsigned ack_size = etcp_ack_section_size(1);
    pkt = u_calloc(1, sizeof(*pkt) + 10 * ack_size); CHECK(pkt);
    pkt->data[0] = ETCP_SECTION_ACK; pkt->data[1] = 1;
    memset(pkt->data + ETCP_ACK_BASE_SIZE + ETCP_ACK_ENTRY_SIZE, 255, ETCP_ACK_RX_POINT_SIZE);
    debug_set_category_level(DEBUG_CATEGORY_DUMP, DEBUG_LEVEL_DEBUG);
    for (unsigned len = ETCP_ACK_BASE_SIZE; len < ack_size; len++) {
        pkt->data_len = len; dump_count = dump_warnings = 0; dump_message[0] = 0;
        etcp_dump_pkt_sections(pkt, NULL, 0);
        CHECK(dump_count == 1 && dump_warnings == 1 && !strstr(dump_message, " rx="));
    }
    pkt->data[ack_size] = ETCP_SECTION_TIMESTAMP;
    pkt->data[ack_size+5] = ETCP_SECTION_PAYLOAD;
    pkt->data[ack_size+6] = 42; pkt->data[ack_size+10] = 99;
    pkt->data_len = ack_size + 5 + ETCP_PAYLOAD_HEADER_SIZE + 1;
    dump_count = dump_warnings = 0; dump_message[0] = 0;
    etcp_dump_pkt_sections(pkt, NULL, 0);
    CHECK(dump_count == 1 && !dump_warnings && strstr(dump_message, " rx=18446744073709551615/18446744073709551615us"));
    CHECK(strstr(dump_message, " ch_ts=0/0") && strstr(dump_message, " payload=1(seq=42 attempt=99)"));
    for (unsigned i = 1; i < 10; i++) memcpy(pkt->data + i*ack_size, pkt->data, ack_size);
    pkt->data_len = 10 * ack_size;
    dump_count = dump_warnings = 0; dump_message[0] = 0;
    etcp_dump_pkt_sections(pkt, NULL, 1);
    CHECK(dump_count == 1 && dump_warnings == 1 && strstr(dump_message, " [truncated]"));
    debug_set_log_hook(NULL);
    debug_enable_console(console);
    debug_set_category_level(DEBUG_CATEGORY_DUMP, saved);
    u_free(pkt);
    puts("PASS diagnostic uint32 boundaries, truncated sections and long ACK/timestamp dumps");
}

static ssize_t capture(socket_t fd, const void* data, size_t len, const struct sockaddr* addr, socklen_t alen,
                       struct ETCP_LINK* link, void* arg) {
    (void)fd; (void)addr; (void)alen;
    int from = (int)(intptr_t)arg;
    CHECK(len <= sizeof(frames[0].bytes));
    if (blackhole || (dead_primary && link->local_link_id == 1)) { drops++; return len; }
    CHECK(tail-head < 4096);
    struct frame* f = &frames[tail++ % 4096]; f->from = from; f->len = len; memcpy(f->bytes,data,len);
    return len;
}
static void event(struct ETCP_CONN* c, int ev, void* arg) {
    (void)c; int i = (int)(intptr_t)arg;
    if (ev == ETCP_CBK_EVENT_REINIT) reinit[i]++;
    if (ev == ETCP_CBK_EVENT_DOWN) down[i]++;
}
static void deliver(const struct frame* f, int allow_drop) {
    int to = 1-f->from;
    struct ETCP_DGRAM* p = memory_pool_alloc(ep[to].inst.pkt_pool); CHECK(p);
    size_t len = 0;
    CHECK(sc_decrypt(&ep[to].c->crypto_ctx,f->bytes,f->len,(uint8_t*)&p->timestamp,&len) == SC_OK);
    CHECK(len >= 4);
    int kind = p->data[0] - ETCP_SESSION_HELLO;
    if (allow_drop && kind >= 0 && kind < 3 && drop[f->from][kind]) {
        drop[f->from][kind]--; drops++; memory_pool_free(ep[to].inst.pkt_pool,p); return;
    }
    if (kind == 0 && !saved[0].len) saved[0] = *f;
    if (kind == 2 && !saved[1].len) saved[1] = *f;
    CHECK(etcp_packet_decrypted(&ep[to].sock,p,&ep[to].links[0],len) == 0);
}
static void pump(void) {
    test_clock_poll(ua,1);
    unsigned end = tail; // callbacks can append replies
    while (head < end) { struct frame f = frames[head++ % 4096]; deliver(&f,1); }
}
static void converge(void) {
    uint64_t deadline = get_time_tb()+40000;
    while (get_time_tb() < deadline) {
        pump();
        if (!ep[0].c->reinit_pending && !ep[1].c->reinit_pending && !ep[0].c->session_timer && !ep[1].c->session_timer && head == tail) break;
    }
    for (int i=0;i<2;i++) {
        CHECK(ep[i].c->initialized && !ep[i].c->session_required && !ep[i].c->reinit_pending);
        CHECK(!ep[i].c->session_timer && !ep[i].c->close_requested && !down[i]);
        CHECK(ep[i].c->peer_reset_id == ep[1-i].c->reset_id);
    }
}
static void setup(int multi) {
    memset(ep,0,sizeof(ep)); memset(drop,0,sizeof(drop)); memset(saved,0,sizeof(saved));
    memset(reinit,0,sizeof(reinit)); memset(down,0,sizeof(down)); head=tail=drops=0; blackhole=dead_primary=0;
    ua = uasync_create(); CHECK(ua);
    for (int i=0;i<2;i++) {
        ep[i].inst.ua=ua; ep[i].inst.node_id=i+1; ep[i].inst.config=&ep[i].cfg;
        ep[i].inst.connections=queue_new(ua,16, offsetof(struct conn_queue_entry, x25519), SC_PUBKEY_SIZE,"session_test");
        ep[i].inst.pkt_pool=memory_pool_init(sizeof(struct ETCP_DGRAM)+PACKET_DATA_SIZE,"session_packets");
        ep[i].inst.ack_pool=memory_pool_init(sizeof(struct ACK_PACKET),"session_acks"); CHECK(ep[i].inst.ack_pool);
        CHECK(sc_generate_keypair(&ep[i].inst.my_keys) == SC_OK);
        ep[i].c=etcp_connection_create(&ep[i].inst,"session_test"); CHECK(ep[i].c);
        ep[i].c->peer_node_id=2-i; ep[i].c->links_up=1;
        etcp_conn_add_cbk(ep[i].c,event,(void*)(intptr_t)i,ETCP_CBK_EVENT_REINIT|ETCP_CBK_EVENT_DOWN);
        CHECK(sc_init_ctx(&ep[i].c->crypto_ctx,&ep[i].inst.my_keys) == SC_OK);
        ep[i].sock.instance=&ep[i].inst;
        for (int j=0;j<=multi;j++) {
            struct ETCP_LINK* l=&ep[i].links[j];
            l->etcp=ep[i].c; l->conn=&ep[i].sock; l->local_link_id=j+1;
            l->initialized=l->link_status=l->recv_keepalive=l->remote_keepalive=1;
            l->link_state=LINK_STATE_CONNECTED; l->mtu=1280; l->remote_addr.ss_family=AF_INET;
            l->send_hook=capture; l->send_hook_ctx=(void*)(intptr_t)i;
            CHECK(etcp_tx_link_init(l) == 0);
        }
        if (multi) ep[i].links[0].next=&ep[i].links[1];
        ep[i].c->links=&ep[i].links[0];
    }
    for (int i=0;i<2;i++) {
        ep[i].c->peer_reset_id=ep[1-i].c->reset_id;
        CHECK(sc_set_peer_public_key(&ep[i].c->crypto_ctx,ep[1-i].inst.my_keys.public_key,0) == SC_OK);
        etcp_conn_ready(ep[i].c);
    }
}
static void teardown(void) {
    for (int i=0;i<2;i++) {
        topo_node_rtt_stop(&ep[i].inst);
        for (int j=0;j<2;j++) {
            etcp_link_io_destroy(&ep[i].links[j]);
            if (ep[i].links[j].attempts) etcp_tx_link_destroy(&ep[i].links[j]);
            if (ep[i].links[j].rx_timeout_timer) uasync_cancel_timeout(ua, ep[i].links[j].rx_timeout_timer);
            if (ep[i].links[j].keepalive_timer) uasync_cancel_timeout(ua, ep[i].links[j].keepalive_timer);
        }
        ep[i].c->links=NULL; // fixture owns synthetic authenticated links
        etcp_connection_close(ep[i].c);
    }
    uasync_poll(ua,0);
    for (int i=0;i<2;i++) {
        queue_free(ep[i].inst.connections); memory_pool_destroy(ep[i].inst.pkt_pool); memory_pool_destroy(ep[i].inst.ack_pool);
    }
    uasync_poll(ua,0); CHECK(ua->timer_alloc_count == ua->timer_free_count); uasync_destroy(ua,0);
}

// Пересоздаём настоящий ETCP_CONN с теми же ключами; линки/сокеты принадлежат fixture.
static void fresh_endpoint(int i) {
    for (int j = 0; j < 2; j++) {
        struct ETCP_LINK* link = &ep[i].links[j];
        etcp_link_io_destroy(link);
        if (link->attempts) etcp_tx_link_destroy(link);
        if (link->keepalive_timer) { uasync_cancel_timeout(ua, link->keepalive_timer); link->keepalive_timer = NULL; }
        if (link->rx_timeout_timer) { uasync_cancel_timeout(ua, link->rx_timeout_timer); link->rx_timeout_timer = NULL; }
    }
    ep[i].c->links = NULL;
    etcp_connection_close(ep[i].c); uasync_poll(ua, 0);
    ep[i].c = etcp_connection_create(&ep[i].inst, "fresh_session"); CHECK(ep[i].c);
    ep[i].c->peer_node_id = 2 - i;
    CHECK(sc_init_ctx(&ep[i].c->crypto_ctx, &ep[i].inst.my_keys) == SC_OK);
    CHECK(sc_set_peer_public_key(&ep[i].c->crypto_ctx, ep[1-i].inst.my_keys.public_key, SC_PEER_PUBKEY_BIN) == SC_OK);
    struct ll_entry* entry = ep[i].c->conn_queue_entry;
    queue_remove_data(ep[i].c->conn_queue, entry);
    ((struct conn_queue_entry*)entry->data)->peer_node_id = ep[i].c->peer_node_id;
    CHECK(queue_data_put_with_index(ep[i].c->conn_queue, entry) == 0);
    ep[i].c->links = &ep[i].links[0];
    for (int j = 0; j < 2; j++) { ep[i].links[j].etcp = ep[i].c; CHECK(etcp_tx_link_init(&ep[i].links[j]) == 0); }
    etcp_conn_add_cbk(ep[i].c, event, (void*)(intptr_t)i, ETCP_CBK_EVENT_REINIT | ETCP_CBK_EVENT_DOWN);
    reinit[i] = down[i] = 0;
}

static void transport_ready(int i, int j) {
    struct ETCP_LINK* link = &ep[i].links[j];
    link->keepalive_interval = link->ka_period_ms = 1000; link->keepalive_timeout = 10000;
    etcp_link_enter_ready_tcp(link);
}

static void run_until(uint64_t end) {
    while (get_time_tb() < end) pump();
}

static void initial_session_case(int scenario) {
    setup(scenario == 4 || scenario == 5);
    uint64_t local = ep[0].c->reset_id;
    ep[0].c->next_tx_id = 73; ep[0].c->last_delivered_id = 42; ep[0].c->got_initial_pkt = 1;
    if (scenario == 5) {
        transport_ready(0, 1);
        CHECK(!ep[0].c->session_required && !ep[0].c->session_timer && !reinit[0]);
        CHECK(ep[0].c->reset_id == local && ep[0].c->next_tx_id == 73 && ep[0].c->last_delivered_id == 42);
    } else {
        if (scenario >= 1 && scenario <= 3) fresh_endpoint(0);
        fresh_endpoint(1);
        etcp_conn_apply_peer_reset_id(ep[1].c, ep[0].c->reset_id);
        if (scenario >= 1 && scenario <= 3) {
            if (scenario != 1) etcp_conn_apply_peer_reset_id(ep[0].c, ep[1].c->reset_id);
            transport_ready(0, 0);
        }
        transport_ready(1, 0);
        CHECK(ep[1].c->session_required && !ep[1].c->initialized);
        if (scenario == 3 || scenario == 4) for (int i = 0; i < 2; i++) drop[i][2] = INT_MAX;
        if (scenario == 3) {
            unsigned before = tail;
            run_until(get_time_tb() + 6000);
            CHECK(ep[0].c->session_required && ep[1].c->session_required && drops);
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Dropped CONFIRM: frames=%u dropped=%u interval=600ms", tail - before, drops);
            CHECK(tail - before < 100); // Без таймера встречные CHALLENGE порождают тысячи ответов.
        } else if (scenario == 4) {
            for (unsigned k = 0; k < 100 && !ep[1].c->session_challenge; k++) pump();
            CHECK(ep[1].c->session_required && ep[1].c->session_challenge);
            uint64_t cookie = ep[1].c->session_challenge, started = ep[1].c->session_started;
            void* timer = ep[1].c->session_timer;
            transport_ready(1, 1);
            CHECK(ep[1].c->session_challenge == cookie && ep[1].c->session_started == started && ep[1].c->session_timer == timer);
        }
        memset(drop, 0, sizeof(drop)); converge();
        if (scenario == 0 || scenario == 4) {
            CHECK(ep[0].c->reset_id == local && ep[0].c->next_tx_id == 1 && ep[0].c->last_delivered_id == 0);
            CHECK(reinit[0] == 1 && reinit[1] == 0);
        }
    }
    teardown();
    printf("PASS initial session scenario %d (restart, cold start, simultaneous, lost CONFIRM, pending/ready second link)\n", scenario);
}

static void restart_during_confirmation(void) {
    setup(0);
    fresh_endpoint(1); etcp_conn_apply_peer_reset_id(ep[1].c, ep[0].c->reset_id); transport_ready(1, 0);
    for (int i = 0; i < 2; i++) drop[i][2] = INT_MAX;
    for (unsigned k = 0; k < 100 && !ep[0].c->session_challenge; k++) pump();
    CHECK(ep[0].c->session_challenge);
    uint64_t old_peer = ep[0].c->peer_reset_id, candidate = ep[1].c->reset_id;
    // Сохраняем правильный ответ первой попытки; после второго рестарта он должен стать stale.
    struct ETCP_DGRAM* packet = memory_pool_alloc(ep[1].inst.pkt_pool); CHECK(packet);
    packet->link = &ep[1].links[0]; packet->data_len = ETCP_SESSION_CONTROL_SIZE; packet->noencrypt_len = 0;
    etcp_session_encode(packet->data, ETCP_SESSION_CONFIRM, candidate, ep[0].c->reset_id);
    uint64_t cookie = htobe64(ep[0].c->session_challenge); memcpy(packet->data + 17, &cookie, sizeof(cookie));
    unsigned last = tail;
    CHECK(etcp_encrypt_send(packet) > 0); memory_pool_free(ep[1].inst.pkt_pool, packet);
    CHECK(tail == last + 1); struct frame stale = frames[last % 4096];
    fresh_endpoint(1); etcp_conn_apply_peer_reset_id(ep[1].c, ep[0].c->reset_id); transport_ready(1, 0);
    CHECK(ep[1].c->reset_id != candidate);
    for (unsigned k = 0; k < 100 && ep[0].c->session_candidate != ep[1].c->reset_id; k++) pump();
    CHECK(ep[0].c->session_candidate == ep[1].c->reset_id);
    deliver(&stale, 0); CHECK(ep[0].c->peer_reset_id == old_peer && !reinit[0]);
    memset(drop, 0, sizeof(drop)); converge(); CHECK(reinit[0] == 1);
    teardown();
    puts("PASS peer restarts during confirmation; old valid CONFIRM cannot adopt the previous epoch");
}
static struct frame stream_packet(int data) {
    struct ETCP_DGRAM* p=memory_pool_alloc(ep[0].inst.pkt_pool); CHECK(p);
    p->link=&ep[0].links[0]; p->noencrypt_len=0; p->data_len=8; memset(p->data,0,8); p->data[0]=data ? 0 : 1; if (data) p->data[1]=99;
    CHECK(etcp_encrypt_send(p)>0); memory_pool_free(ep[0].inst.pkt_pool,p);
    CHECK(tail == head+1); struct frame f=frames[head++ % 4096]; return f;
}
static void data_header_validation(void) {
    setup(0);
    uint8_t data[ETCP_SESSION_HEADER_SIZE + ETCP_ACK_BASE_SIZE] = {0};
    struct ETCP_CONN* c = ep[1].c;
    etcp_session_encode(data, ETCP_SESSION_DATA, c->peer_reset_id, c->reset_id);
    data[ETCP_SESSION_HEADER_SIZE] = ETCP_SECTION_ACK;
    CHECK(etcp_session_accept_data(c, data, sizeof(data)));
    CHECK(!etcp_session_accept_data(c, data, 0));
    CHECK(!etcp_session_accept_data(c, data, ETCP_SESSION_HEADER_SIZE));
    data[0] = ETCP_SECTION_ACK;
    CHECK(!etcp_session_accept_data(c, data, sizeof(data)));
    data[0] = ETCP_SESSION_DATA;
    data[ETCP_SESSION_HEADER_SIZE] = ETCP_SESSION_DATA;
    for (unsigned i = 3; i < 1024; i++) CHECK(!etcp_session_accept_data(c, data, sizeof(data)));
    CHECK(c->session_data_errors == 1024);
    data[ETCP_SESSION_HEADER_SIZE] = ETCP_SECTION_ACK;
    CHECK(etcp_session_accept_data(c, data, sizeof(data)));
    CHECK(c->session_data_errors == 1024 && c->initialized && !c->reinit_pending);
    teardown();
    puts("PASS session DATA header validation and rejection counter");
}
static void timestamp_boundaries(void) {
    setup(0);
    struct ETCP_LINK* link = &ep[0].links[0];
    link->rtt_last = 1000;
    for (int i = 0; i < 2; i++) {
        struct ETCP_DGRAM* p = memory_pool_alloc(ep[0].inst.pkt_pool); CHECK(p);
        p->link = link; p->timestamp = UINT16_MAX; p->data_len = 5; p->data[0] = ETCP_SECTION_TIMESTAMP;
        uint16_t ret = get_current_timestamp() - (i ? 30000 : 10);
        p->data[1] = ret; p->data[2] = ret >> 8;
        p->data[3] = p->data[4] = i ? 0 : 255;
        etcp_conn_input(p);
        CHECK(link->jitter > 0); // предыдущий RTT нельзя перезаписывать до вычисления разницы
        CHECK(link->jitter <= (uint64_t)UINT16_MAX * 65536);
        CHECK(link->recv_dt_avg_tx != 0 && link->recv_dt_avg_rx != 0);
    }
    teardown();
    puts("PASS timestamp wrap, negative offsets and changing RTT");
}
static void receive_window_case(int wrap) {
    setup(0);
    struct ETCP_CONN* c = ep[0].c;
    ep[0].inst.data_pool = memory_pool_init(PACKET_DATA_SIZE, "window payload"); CHECK(ep[0].inst.data_pool);
    queue_set_callback(c->input_send_q, NULL, NULL); // выделенные seq наблюдаем до физической отправки
    c->next_tx_id = wrap ? 5 : MAX_INFLIGHT_SIZE + 1;
    c->rx_ack_till = c->next_tx_id - MAX_INFLIGHT_SIZE - 1;
    uint32_t next = c->next_tx_id, till = c->rx_ack_till;
    struct ETCP_FRAGMENT* f = (struct ETCP_FRAGMENT*)queue_entry_new(sizeof(*f) - sizeof(struct ll_entry)); CHECK(f);
    f->ll.dgram = memory_pool_alloc(ep[0].inst.data_pool); CHECK(f->ll.dgram);
    f->ll.dgram_pool = ep[0].inst.data_pool; f->ll.len = 1;
    CHECK(queue_data_put(c->input_queue, &f->ll) == 0);
    CHECK(c->input_queue->count == 1 && c->next_tx_id == next);
    for (int cumulative = 0; cumulative < 2; cumulative++) {
        struct ETCP_DGRAM* p = memory_pool_alloc(ep[0].inst.pkt_pool); CHECK(p);
        p->link = &ep[0].links[0]; p->data_len = etcp_ack_section_size(!cumulative);
        memset(p->data, 0, p->data_len); p->data[0] = ETCP_SECTION_ACK; p->data[1] = !cumulative;
        if (!cumulative) p->data[ETCP_ACK_BASE_SIZE+4] = 1;
        uint32_t ack = till + cumulative, selective = next - 1;
        for (int i = 0; i < 4; i++) { p->data[2+i] = ack >> (8*i); p->data[8+i] = selective >> (8*i); }
        etcp_conn_input(p); uasync_poll(ua, 0);
        if (!cumulative) CHECK(c->input_queue->count == 1 && c->next_tx_id == next);
        else CHECK(c->input_queue->count == 0 && c->next_tx_id == next + 1 && c->input_send_q->count == 1);
    }
    teardown();
    memory_pool_destroy(ep[0].inst.data_pool); ep[0].inst.data_pool = NULL;
    printf("PASS receive-window backpressure and cumulative ACK resume (wrap=%d)\n", wrap);
}
static void receive_after_oom(struct ETCP_CONN* conn, struct ll_entry* entry) {
    int side = conn == ep[1].c;
    CHECK(entry->len == 2 && entry->dgram[0] == 253 && entry->dgram[1] == 42);
    messages[side]++; queue_dgram_free(entry); queue_entry_free(entry);
}

static struct frame encrypted_message(int from) {
    struct ETCP_DGRAM* pkt = memory_pool_alloc(ep[from].inst.pkt_pool); CHECK(pkt);
    memset(pkt, 0, sizeof(*pkt)); pkt->link = &ep[from].links[0];
    pkt->data_len = ETCP_ACK_BASE_SIZE + ETCP_PAYLOAD_HEADER_SIZE + 4; pkt->payload_len = 4;
    pkt->data[0] = ETCP_SECTION_ACK;
    unsigned offset = ETCP_ACK_BASE_SIZE;
    pkt->data[offset] = ETCP_SECTION_PAYLOAD; pkt->data[offset+1] = 1; pkt->data[offset+5] = 1;
    offset += ETCP_PAYLOAD_HEADER_SIZE;
    pkt->data[offset] = 2; pkt->data[offset+2] = 253; pkt->data[offset+3] = 42;
    unsigned before = tail; CHECK(etcp_encrypt_send(pkt) > 0); CHECK(tail == before+1 && head == before);
    memory_pool_free(ep[from].inst.pkt_pool, pkt);
    return frames[head++ % 4096];
}

static void oom_session_recovery(void) {
    setup(1); memset(messages, 0, sizeof(messages));
    for (int i = 0; i < 2; i++) {
        ep[i].inst.data_pool = memory_pool_init(PACKET_DATA_SIZE, "OOM session payload"); CHECK(ep[i].inst.data_pool);
        CHECK(etcp_bind(&ep[i].inst, 253, receive_after_oom) == 0);
        for (int j = 0; j < 2; j++) {
            ep[i].links[j].keepalive_interval = ep[i].links[j].ka_period_ms = 1000;
            ep[i].links[j].keepalive_timeout = 10000;
        }
    }
    uint64_t source_epoch = ep[0].c->reset_id, failed_epoch = ep[1].c->reset_id;
    struct frame old = encrypted_message(0); fail_message = 1; deliver(&old, 0);
    CHECK(!fail_message && !messages[1] && ep[1].c->reset_id != failed_epoch && ep[1].c->session_required);
    converge();
    CHECK(reinit[0] == 1 && reinit[1] == 1 && !down[0] && !down[1]);
    CHECK(ep[0].c->reset_id == source_epoch && ep[0].c->peer_reset_id == ep[1].c->reset_id);
    for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++) {
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "OOM recovery physical side=%d path=%d initialized=%u up=%u state=%u KA=%u/%u down=%u",
            i, j, ep[i].links[j].initialized, ep[i].links[j].link_status, ep[i].links[j].link_state,
            ep[i].links[j].recv_keepalive, ep[i].links[j].remote_keepalive, down[i]);
        CHECK(ep[i].links[j].initialized && ep[i].links[j].link_status);
    }
    deliver(&old, 0); CHECK(!messages[1] && !ep[1].c->last_delivered_id);
    for (int i = 0; i < 2; i++) { struct frame fresh = encrypted_message(i); deliver(&fresh, 0); }
    CHECK(messages[0] == 1 && messages[1] == 1);
    teardown();
    for (int i = 0; i < 2; i++) { memory_pool_destroy(ep[i].inst.data_pool); ep[i].inst.data_pool = NULL; }
    puts("PASS encrypted OOM recovery: REINIT both owners, preserved links, stale DATA rejected, fresh delivery both ways");
}

/* Настоящий AES/ingress: одинаковая пара seq=1/no=1 в новой эпохе не принимает прежний ACK. */
static struct frame encrypted_attempt_ack(void) {
    struct ETCP_DGRAM* pkt = memory_pool_alloc(ep[1].inst.pkt_pool); CHECK(pkt);
    pkt->link = &ep[1].links[0]; pkt->data_len = etcp_ack_section_size(1); pkt->noencrypt_len = 0;
    memset(pkt->data,0,pkt->data_len); pkt->data[0] = ETCP_SECTION_ACK; pkt->data[1] = 1;
    pkt->data[ETCP_ACK_BASE_SIZE] = 1; pkt->data[ETCP_ACK_BASE_SIZE+4] = 1;
    unsigned before = tail;
    CHECK(etcp_encrypt_send(pkt) > 0 && tail == before+1 && head == before);
    memory_pool_free(ep[1].inst.pkt_pool,pkt);
    return frames[head++ % 4096];
}

static void compact_attempt_epoch_reuse(void) {
    setup(0);
    ep[0].inst.data_pool = memory_pool_init(PACKET_DATA_SIZE,"attempt epoch payload"); CHECK(ep[0].inst.data_pool);
    struct frame old_ack = {0};
    uint64_t first_epoch = ep[0].c->reset_id;
    for (unsigned round = 0; round < 2; round++) {
        struct ETCP_CONN* c = ep[0].c; struct ETCP_LINK* link = &ep[0].links[0];
        link->inflight_lim_bytes = 65536;
        const uint8_t data[] = {1,0,253};
        CHECK(etcp_int_send(c,data,sizeof(data)) == 0);
        struct ETCP_DGRAM* pkt = etcp_request_pkt(c); CHECK(pkt && pkt->attempt);
        CHECK(pkt->attempt->seq == 1 && pkt->attempt->attempt_id == etcp_tx_attempt_id(1,1));
        etcp_tx_complete(pkt,1); memory_pool_free(ep[0].inst.pkt_pool,pkt);
        if (!round) {
            old_ack = encrypted_attempt_ack();
            etcp_conn_fatal_reinit(c,"attempt epoch reuse test"); converge();
            CHECK(c->reset_id != first_epoch);
        } else {
            CHECK(c->unacked_bytes == sizeof(data) && link->inflight_bytes == sizeof(data));
            uint64_t last_rx = link->last_recv_local_time;
            deliver(&old_ack,0);
            CHECK(c->unacked_bytes == sizeof(data) && link->inflight_bytes == sizeof(data));
            CHECK(!c->ack_packets_count && !link->peer_rx_data_time_us && link->last_recv_local_time == last_rx);
            struct frame fresh_ack = encrypted_attempt_ack(); deliver(&fresh_ack,0);
            CHECK(!c->unacked_bytes && !link->inflight_bytes && c->ack_packets_count == 1);
        }
    }
    teardown(); memory_pool_destroy(ep[0].inst.data_pool); ep[0].inst.data_pool = NULL;
    puts("PASS encrypted ACK isolation when compact attempt pair is reused in a new epoch");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    test_clock_init(10);
    size_t baseline=u_get_allocated_count();
    debug_sequence_boundaries(); CHECK(u_get_allocated_count() == baseline);
    const char* log=getenv("SESSION_TEST_LOG");
    if (log) { debug_enable_file_output(log,1); debug_set_category_level(DEBUG_CATEGORY_ETCP,DEBUG_LEVEL_DEBUG); }
    data_header_validation(); CHECK(u_get_allocated_count() == baseline);
    timestamp_boundaries(); CHECK(u_get_allocated_count() == baseline);
    receive_window_case(0); CHECK(u_get_allocated_count() == baseline);
    receive_window_case(1); CHECK(u_get_allocated_count() == baseline);
    for (int scenario = 0; scenario < 6; scenario++) {
        initial_session_case(scenario); CHECK(u_get_allocated_count() == baseline);
    }
    restart_during_confirmation(); CHECK(u_get_allocated_count() == baseline);
    oom_session_recovery(); CHECK(u_get_allocated_count() == baseline);
    compact_attempt_epoch_reuse(); CHECK(u_get_allocated_count() == baseline);
    for (int scenario=0;scenario<9;scenario++) {
        setup(scenario == 5);
        saved[2]=stream_packet(0); saved[3]=stream_packet(1);
        if (scenario == 2 || scenario == 3) for (int i=0;i<2;i++) for (int j=0;j<3;j++) drop[i][j]=1;
        if (scenario == 5) dead_primary=1;
        etcp_conn_fatal_reinit(ep[scenario == 1 ? 1 : 0].c,"fault test");
        if (scenario == 3) etcp_conn_fatal_reinit(ep[1].c,"simultaneous reset");
        if (scenario == 4) { pump(); etcp_conn_fatal_reinit(ep[0].c,"second reset before confirmation"); }
        if (scenario == 6) {
            blackhole=1;
            uint64_t started = ep[0].c->session_started;
            CHECK(etcp_conn_ref_take(ep[0].c)==0);
            for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++) {
                etcp_link_io_destroy(&ep[i].links[j]);
                if (ep[i].links[j].attempts) etcp_tx_link_destroy(&ep[i].links[j]);
            }
            ep[0].c->links=NULL; // timeout closes a real connection, links belong to fixture
            while (ep[0].c->state!=2 && get_time_tb() < started + 110000) pump();
            CHECK(ep[0].c->state == 2 && get_time_tb() - started >= 100000);
            CHECK(!ep[0].c->session_timer && ep[0].c->delete_complete);
            etcp_conn_ref_free(ep[0].c); ep[0].c=NULL;
            ep[1].c->links=NULL; etcp_connection_close(ep[1].c); uasync_poll(ua,0);
            for (int i=0;i<2;i++) {
                queue_free(ep[i].inst.connections); memory_pool_destroy(ep[i].inst.pkt_pool); memory_pool_destroy(ep[i].inst.ack_pool);
            }
            uasync_poll(ua,0); CHECK(ua->timer_alloc_count==ua->timer_free_count); uasync_destroy(ua,0);
        } else {
            converge();
            if (scenario==2 || scenario==3 || scenario==5) CHECK(drops>0);
            uint64_t peer=ep[1].c->peer_reset_id, local=ep[0].c->reset_id;
            unsigned before0=reinit[0], before1=reinit[1];
            ep[1].links[0].last_recv_local_time=123;
            deliver(&saved[2],0); deliver(&saved[3],0); CHECK(ep[1].links[0].last_recv_local_time==123); // stale ACK never reaches stream/liveness
            if (scenario==7) {
                if (saved[0].len) deliver(&saved[0],0);
                if (saved[1].len) { deliver(&saved[1],0); deliver(&saved[1],0); }
                converge();
            }
            if (scenario==8) {
                uint8_t short_frame[25]={ETCP_SESSION_CONFIRM};
                CHECK(etcp_session_receive(ep[0].c,short_frame,1));
                CHECK(!etcp_session_accept_data(ep[0].c,short_frame,0));
                etcp_conn_apply_peer_reset_id(ep[0].c,local); // stale/unproven physical INIT cannot switch peer
                CHECK(ep[0].c->peer_reset_id==ep[1].c->reset_id);
            }
            CHECK(ep[1].c->peer_reset_id==peer && ep[0].c->reset_id==local);
            CHECK(reinit[0]==before0 && reinit[1]==before1);
            teardown();
        }
        CHECK(u_get_allocated_count()==baseline); printf("PASS session scenario %d\n",scenario+1);
    }
    puts("All ETCP session scenarios passed"); test_clock_finish(); return 0;
}
