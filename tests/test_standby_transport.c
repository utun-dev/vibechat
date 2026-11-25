/* Проверка режима и фаз standby на реальных keepalive-пакетах до шифрования.
 * Управляемая отправка исключает сеть; отказ таймера проверяет сохранение рабочего режима. */
#include <assert.h>
#include <string.h>
#include "utun_instance.h"
#include "etcp.h"
#include "etcp_keepalive.h"
#include "standby.h"
#include "debug_config.h"
#include "test_clock.h"

static struct UASYNC* ua;
static struct UTUN_INSTANCE inst;
static struct ETCP_CONN udp, tcp;
static struct ETCP_LINK links[3];
static struct ll_queue connections;
static struct ll_entry *udp_entry, *tcp_entry;
static struct { unsigned count; uint8_t flags; uint16_t period, active, sleep; } sent[3];
static int fail_phase_timer, activity_events, wait_calls;
static unsigned wait_after_packets;
static int probe_delay_tb[3], fail_probe_rng;
static const uint32_t* probe_random;
static size_t probe_random_left;

/* Перехватывает сформированный keepalive, сохраняя его реальные флаги и тайминг. */
int __wrap_etcp_encrypt_send(struct ETCP_DGRAM* pkt) {
    assert(pkt && pkt->data[0] == ETCP_KEEPALIVE && pkt->data_len >= 4);
    int i;
    for (i = 0; i < 3 && pkt->link != &links[i]; i++);
    assert(i < 3);
    size_t body = (pkt->data[1] & KA_FLAG_SLEEP) ? 8 : 4;
    assert(pkt->data_len >= body + pkt->link->etcp->padding.min && pkt->data_len <= body + pkt->link->etcp->padding.max);
    for (size_t j = body; j < pkt->data_len; j++) assert(pkt->data[j] == 0);
    sent[i].count++;
    sent[i].flags = pkt->data[1];
    sent[i].period = pkt->data[2] | ((uint16_t)pkt->data[3] << 8);
    sent[i].active = (pkt->data[1] & KA_FLAG_SLEEP) ? pkt->data[4] | ((uint16_t)pkt->data[5] << 8) : 0;
    sent[i].sleep = (pkt->data[1] & KA_FLAG_SLEEP) ? pkt->data[6] | ((uint16_t)pkt->data[7] << 8) : 0;
    return pkt->data_len;
}

void* __real_uasync_set_timeout(struct UASYNC* ua, int tb, void* arg, timeout_callback_t cb, const char* name);
int __real_RAND_bytes(unsigned char* buf, int len);

/* Запоминает задержки KA и позволяет имитировать отказ таймера фазы. */
void* __wrap_uasync_set_timeout(struct UASYNC* loop, int tb, void* arg, timeout_callback_t cb, const char* name) {
    if (name && !strcmp(name, "link_keepalive")) {
        const struct ETCP_LINK* link = arg;
        assert(link->local_link_id >= 1 && link->local_link_id <= 3);
        probe_delay_tb[link->local_link_id - 1] = tb;
    }
    if (fail_phase_timer && name && !strcmp(name, "standby_phase")) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "[TEST] injected standby phase timer failure");
        return NULL;
    }
    return __real_uasync_set_timeout(loop, tb, arg, cb, name);
}

/* Подставляет граничные значения для точной проверки задержки. */
int __wrap_RAND_bytes(unsigned char* buf, int len) {
    if (!probe_random_left && !fail_probe_rng) return __real_RAND_bytes(buf, len);
    assert(len == (int)sizeof(*probe_random));
    if (fail_probe_rng) return 0;
    memcpy(buf, probe_random++, sizeof(*probe_random));
    probe_random_left--;
    return 1;
}

/* Подписчики активности видят уже согласованные режимы, без промежуточного состояния. */
static void activity_changed(struct UTUN_INSTANCE* owner, int active, void* arg) {
    (void)arg;
    assert(owner == &inst && inst.client_activity == active);
    assert(standby_is_enabled() == !active);
    for (int i = 0; i < 3; i++) assert(links[i].ka_my_sleeping == !active);
    activity_events++;
}

/* Работа возобновляется после отправки новой фазы пирам или выхода из standby. */
static void waited(void* arg) {
    (void)arg;
    assert(sent[0].count > wait_after_packets);
    if (standby_is_enabled()) {
        assert(standby_get_active_remaining_tb() > 0 && sent[0].flags == KA_FLAG_SLEEP && sent[0].active > 0);
    } else {
        assert(inst.client_activity == CLIENT_ACTIVITY_ACTIVE && !links[0].ka_my_sleeping && sent[0].flags == 0);
    }
    wait_calls++;
}

/* Мобильный узел с исходящим UDP, входящим UDP и TCP линками. */
static void setup(void) {
    ua = uasync_create(); assert(ua);
    memset(&inst, 0, sizeof(inst)); memset(&udp, 0, sizeof(udp)); memset(&tcp, 0, sizeof(tcp));
    memset(links, 0, sizeof(links)); memset(sent, 0, sizeof(sent));
    memset(probe_delay_tb, 0, sizeof(probe_delay_tb));
    activity_events = wait_calls = fail_phase_timer = 0;
    inst.ua = ua; inst.client_type = CLIENT_TYPE_MOBILE; inst.client_activity = CLIENT_ACTIVITY_STANDBY;
    udp.instance = tcp.instance = &inst;
    udp.padding.min = 7; udp.padding.max = 13;
    tcp.padding.min = 19; tcp.padding.max = 24;
    strcpy(udp.log_name, "standby-test-udp"); strcpy(tcp.log_name, "standby-test-tcp");
    for (int i = 0; i < 3; i++) {
        links[i].etcp = i == 2 ? &tcp : &udp;
        links[i].local_link_id = i + 1; links[i].initialized = links[i].link_status = 1;
        links[i].keepalive_interval = 2000;
        links[i].ka_period_ms = 100; // Исходное локальное значение до согласования.
        links[i].keepalive_timeout = 20000; links[i].last_recv_local_time = get_time_tb();
    }
    links[0].next = &links[1]; links[1].is_server = 1; links[2].is_tcp = 1;
    udp.links = &links[0]; tcp.links = &links[2];
    udp_entry = queue_entry_new(sizeof(struct conn_queue_entry)); assert(udp_entry);
    tcp_entry = queue_entry_new(sizeof(struct conn_queue_entry)); assert(tcp_entry);
    ((struct conn_queue_entry*)udp_entry->data)->conn = &udp;
    ((struct conn_queue_entry*)tcp_entry->data)->conn = &tcp;
    memset(&connections, 0, sizeof(connections));
    connections.head = udp_entry; udp_entry->next = tcp_entry;
    inst.connections = &connections;
    standby_set_intervals_ms(200, 300, 0);
    standby_init(ua); etcp_keepalive_register(&inst);
    utun_add_activity_cbk(&inst, activity_changed, NULL);
    for (int i = 0; i < 3; i++) etcp_keepalive_link_ready(&links[i]);
}

/* Отменяет таймеры и подписки перед повторной инициализацией ядра. */
static void teardown(void) {
    standby_deinit();
    for (int i = 0; i < 3; i++) {
        if (links[i].keepalive_timer) uasync_cancel_timeout(ua, links[i].keepalive_timer);
        if (links[i].rx_timeout_timer) uasync_cancel_timeout(ua, links[i].rx_timeout_timer);
        if (links[i].ka_sleep_announce_timer) uasync_cancel_timeout(ua, links[i].ka_sleep_announce_timer);
    }
    while (inst.activity_cbks) utun_remove_activity_cbk(&inst, inst.activity_cbks->fn, inst.activity_cbks->arg);
    queue_entry_free(udp_entry); queue_entry_free(tcp_entry);
    uasync_poll(ua, 0);
    assert(ua->timer_alloc_count == ua->timer_free_count);
    uasync_destroy(ua, 0);
}

/* Первая проба объявляет согласованный период, а повторная использует адаптивный номинал. */
static void check_keepalive_jitter(void) {
    const uint32_t samples[] = {0, UINT32_C(0x80000000), UINT32_MAX, UINT32_MAX, UINT32_MAX};
    probe_random = samples;
    probe_random_left = sizeof(samples) / sizeof(*samples);
    setup();
    assert(probe_delay_tb[0] == 10000 && probe_delay_tb[1] == 20000 && probe_delay_tb[2] == 30000);
    for (int i = 0; i < 3; i++) assert(sent[i].count == 1 && sent[i].period == 2000);

    struct utun_config config = {.global.keepalive_adaptive = 1};
    inst.config = &config;
    test_clock_next_timer(ua);
    assert(sent[0].count == 2 && sent[0].period == 2101 && links[0].ka_period_ms == 2101);
    assert(probe_delay_tb[0] == 31510);

    assert(uasync_cancel_timeout(ua, links[2].keepalive_timer) == ERR_OK);
    links[2].keepalive_timer = NULL;
    links[2].keepalive_interval = UINT16_MAX;
    etcp_keepalive_link_ready(&links[2]);
    assert(sent[2].period == UINT16_MAX && probe_delay_tb[2] == 983020);
    assert(probe_random_left == 0);
    probe_random = NULL;

    assert(uasync_cancel_timeout(ua, links[2].keepalive_timer) == ERR_OK);
    links[2].keepalive_timer = NULL;
    fail_probe_rng = 1;
    start_keepalive_timer(&links[2]);
    fail_probe_rng = 0;
    assert(links[2].keepalive_timer && probe_delay_tb[2] == 655350);
    teardown();
    DEBUG_INFO(DEBUG_CATEGORY_KEEPALIVE, "[PASS] probe jitter boundaries, adaptive period, wide delay and RNG fallback");
}

/* Первый фон при начальном STANDBY, повторы, новые линки, обе причины wake и выход в ACTIVE. */
static void check_transitions(void) {
    setup();
    assert(!standby_is_enabled());
    for (int i = 0; i < 3; i++) assert(sent[i].count == 1 && sent[i].flags == 0 && links[i].keepalive_timer);
    assert(utun_set_client_activity(&inst, 0) == 0 && standby_is_enabled() && activity_events == 0);
    for (int i = 0; i < 3; i++) {
        assert(links[i].ka_my_sleeping && !links[i].keepalive_timer);
        assert(sent[i].count == 2 && sent[i].flags == KA_FLAG_SLEEP && sent[i].active > 0 && sent[i].sleep == 3);
    }
    uint64_t timers = ua->timer_alloc_count;
    assert(utun_set_client_activity(&inst, 0) == 0 && ua->timer_alloc_count == timers);
    assert(sent[0].count == 2 && sent[1].count == 2 && sent[2].count == 2);
    test_clock_next_timer(ua); assert(standby_get_sleep_tb() > 0);
    for (int i = 0; i < 3; i++) {
        etcp_keepalive_link_ready(&links[i]);
        assert(sent[i].flags == KA_FLAG_SLEEP && sent[i].active == 0 && sent[i].sleep > 0 && !links[i].keepalive_timer);
    }
    assert(links[0].ka_sleep_announce_timer && links[1].ka_sleep_announce_timer && !links[2].ka_sleep_announce_timer);
    timers = ua->timer_alloc_count; wait_after_packets = sent[0].count;
    assert(standby_wait(NULL, waited)); assert(ua->timer_alloc_count == timers);
    fail_phase_timer = 1;
    standby_notify_network_activity();
    assert(standby_get_sleep_tb() > 0 && wait_calls == 0 && sent[0].count == wait_after_packets);
    fail_phase_timer = 0;
    standby_notify_network_activity(); assert(wait_calls == 1);
    test_clock_next_timer(ua); assert(standby_get_sleep_tb() > 0);
    wait_after_packets = sent[0].count;
    void* cancelled = standby_wait(NULL, waited); assert(cancelled);
    standby_wait_cancel(cancelled);
    assert(standby_wait(NULL, waited));
    test_clock_next_timer(ua); assert(wait_calls == 2 && standby_get_active_remaining_tb() > 0);
    test_clock_next_timer(ua); assert(standby_get_sleep_tb() > 0);
    wait_after_packets = sent[0].count; assert(standby_wait(NULL, waited));
    links[0].ka_peer_sleeping = 1;
    assert(utun_set_client_activity(&inst, 1) == 0 && activity_events == 1 && wait_calls == 3);
    for (int i = 0; i < 3; i++) {
        assert(!links[i].ka_my_sleeping && sent[i].flags == 0 && !links[i].ka_sleep_announce_timer);
    }
    assert(!links[0].keepalive_timer && links[1].keepalive_timer && links[2].keepalive_timer);
    timers = ua->timer_alloc_count; unsigned packets = sent[0].count;
    assert(utun_set_client_activity(&inst, 1) == 0 && ua->timer_alloc_count == timers);
    assert(activity_events == 1 && sent[0].count == packets);
    assert(utun_set_client_activity(&inst, 0) == 0 && activity_events == 2);
    teardown();
    DEBUG_INFO(DEBUG_CATEGORY_KEEPALIVE, "[PASS] first background, repeated commands, UDP/TCP ready, phase-before-work, peer sleep and cleanup");
}

/* GUI прерывает даже min_sleep; быстрый возврат в foreground отменяет будущие фазы. */
static void check_foreground_wake(void) {
    setup(); standby_set_intervals_ms(2000, 120000, 90000);
    assert(utun_set_client_activity(&inst, 0) == 0);
    test_clock_run_for(ua, 21000);
    assert(standby_get_sleep_tb() > 1190000);
    standby_notify_network_activity();
    assert(standby_get_sleep_tb() > 1190000); // сеть не отменяет min_sleep
    wait_after_packets = sent[0].count; assert(standby_wait(NULL, waited));
    uint64_t before = get_time_tb();
    assert(utun_set_client_activity(&inst, 1) == 0);
    assert(get_time_tb() - before < 1000 && wait_calls == 1 && !standby_is_enabled());
    for (int i = 0; i < 3; i++) {
        assert(!links[i].ka_my_sleeping && !links[i].ka_sleep_announce_timer && links[i].keepalive_timer);
        assert(sent[i].flags == 0);
    }
    /* При потере первого normal-пакета следующий уходит по обычному таймеру UDP/TCP. */
    unsigned udp_packets = sent[0].count, tcp_packets = sent[2].count;
    test_clock_run_for(ua, 31000);
    assert(sent[0].count > udp_packets && sent[2].count > tcp_packets);
    assert(sent[0].flags == 0 && sent[2].flags == 0 && !standby_is_enabled());
    for (unsigned i = 0; i < 3; i++) {
        assert(utun_set_client_activity(&inst, 0) == 0 && standby_is_enabled());
        test_clock_run_for(ua, 100);
        assert(utun_set_client_activity(&inst, 1) == 0 && !standby_is_enabled());
    }
    test_clock_run_for(ua, 25000);
    assert(!standby_is_enabled() && inst.client_activity == CLIENT_ACTIVITY_ACTIVE);
    for (int i = 0; i < 3; i++) assert(!links[i].ka_my_sleeping && !links[i].ka_sleep_announce_timer);
    assert(utun_set_client_activity(&inst, 0) == 0);
    test_clock_run_for(ua, 21000); assert(standby_get_sleep_tb() > 1190000);
    teardown();
    DEBUG_INFO(DEBUG_CATEGORY_KEEPALIVE, "[PASS] foreground bypasses min_sleep, repeated normal TX, rapid hide/show and final background sleep");
}

/* Ошибка ENTER сохраняет normal; потеря фазового таймера возвращает normal, без зависания сна. */
static void check_timer_failures(void) {
    setup(); assert(utun_set_client_activity(&inst, 1) == 0);
    fail_phase_timer = 1;
    assert(utun_set_client_activity(&inst, 0) < 0);
    assert(inst.client_activity == CLIENT_ACTIVITY_ACTIVE && !standby_is_enabled() && activity_events == 1);
    for (int i = 0; i < 3; i++) assert(!links[i].ka_my_sleeping && links[i].keepalive_timer);
    fail_phase_timer = 0;
    assert(utun_set_client_activity(&inst, 0) == 0);
    fail_phase_timer = 1;
    test_clock_next_timer(ua);
    assert(!standby_is_enabled() && inst.client_activity == CLIENT_ACTIVITY_ACTIVE && activity_events == 3);
    for (int i = 0; i < 3; i++) assert(!links[i].ka_my_sleeping && links[i].keepalive_timer);
    fail_phase_timer = 0; teardown();
    DEBUG_INFO(DEBUG_CATEGORY_KEEPALIVE, "[PASS] timer failure rollback and safe normal fallback");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    test_clock_init(1000);
    check_keepalive_jitter();
    check_transitions(); check_transitions(); check_foreground_wake(); check_timer_failures();
    DEBUG_INFO(DEBUG_CATEGORY_KEEPALIVE, "PASS: standby transport regression tests");
    test_clock_finish();
    return 0;
}
