#include "stream_address_fixture.h"
/* Массовый обзор с постоянно возникающими отказами: сеть заменена мгновенным INFO, алгоритм расписания настоящий. */
#include <stdlib.h>
#include <time.h>
#include "../src/stream/stream_priv.h"

static int probe_open(struct STREAM_HANDLE *, struct stream_address, uint8_t, uint64_t, struct STREAM_RECEIVE **);
static int local_get(struct UTUN_INSTANCE *, const struct stream_descriptor *, struct stream_local *);
static int receive_info(struct STREAM_RECEIVE *, struct stream_receive_info *);
#define stream_probe_open probe_open
#define stream_local_get local_get
#define stream_receive_get_info receive_info
#include "../src/stream/stream_optimizer.c"
#undef stream_probe_open
#undef stream_local_get
#undef stream_receive_get_info

#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "standby line=%d: %s", __LINE__, #x); exit(1); } } while (0)
#define CANDIDATES 256
static unsigned queried[CANDIDATES + 1], opened, local_busy;
static char probe_token;

static int receive_info(struct STREAM_RECEIVE *receive, struct stream_receive_info *info) {
    CHECK(receive && info); *info = *(struct stream_receive_info *)receive; return STREAM_OK;
}

static int local_get(struct UTUN_INSTANCE *inst, const struct stream_descriptor *d, struct stream_local *local) {
    (void)inst; (void)d; *local = (struct stream_local){.anchored = 1, .next = 42}; return STREAM_OK;
}

static int probe_open(struct STREAM_HANDLE *h, struct stream_address peer, uint8_t sub, uint64_t seq, struct STREAM_RECEIVE **out) {
    uint64_t id = ((unsigned)peer.ed[0] << 8) | peer.ed[1];
    (void)h; CHECK(id && id <= CANDIDATES && sub < 255 && seq == 42 && !*out);
    if (local_busy) return STREAM_LOCAL_BUSY;
    queried[id]++; opened++; *out = (struct STREAM_RECEIVE *)&probe_token; return STREAM_OK;
}

static void test_scan(unsigned parts) {
    memset(queried, 0, sizeof(queried)); opened = 0;
    struct stream_supplier suppliers[CANDIDATES] = {0};
    struct stream_selection *picks = u_calloc(parts, sizeof(*picks)); CHECK(picks);
    struct UTUN_INSTANCE inst = {.node_id = 999, .my_ed25519_pubkey = {3, 231}};
    struct stream_optimizer o = {.inst = &inst, .descriptor = {.n = parts, .r = parts > 192 ? parts - 192 : 0},
        .selected = picks, .random = 12345};
    o.suppliers = suppliers;
    for (unsigned i = 0; i < CANDIDATES; i++) {
        suppliers[i].node = STREAM_TEST_ADDR(i + 1); suppliers[i].online = suppliers[i].compatible = 1; suppliers[i].rank = i;
        suppliers[i].next = i + 1 < CANDIDATES ? &suppliers[i + 1] : NULL;
    }
    /* Первый отказ местного бюджета не должен ни расходовать обход, ни штрафовать удалённый узел. */
    local_busy = 1; probes_schedule(&o, NULL, 1000, 1);
    CHECK(!opened && !suppliers[0].failures && !suppliers[0].retry_ms && !suppliers[0].cycle);
    local_busy = 0;
    for (uint64_t now = 1100; now < 800000 && !queried[CANDIDATES]; now += 1000) {
        /* Три первых узла всё время требуют приоритета; recovery каждые5с не должен заново начинать список. */
        for (unsigned i = 0; i < 3; i++) suppliers[i].refresh = 1;
        unsigned before = opened; probes_schedule(&o, NULL, now, 1);
        CHECK(opened - before <= 1);
        for (unsigned i = 0; i < CANDIDATES; i++) suppliers[i].probe = NULL;
    }
    for (unsigned i = 1; i <= CANDIDATES; i++) CHECK(queried[i]);
    CHECK(opened <= 3 * CANDIDATES && queried[1] > 1 && o.info.failures == 0);
    /* Новый подходящий узел проходит по приоритету, даже если его шаг цикла уже был помечен завершённым. */
    unsigned before = queried[CANDIDATES]; suppliers[CANDIDATES - 1].refresh = 1;
    suppliers[CANDIDATES - 1].checked_ms = 0;
    for (unsigned i = 0; i < 3 && queried[CANDIDATES] == before; i++) {
        probes_schedule(&o, NULL, 900000 + i * 1000, 1);
        for (unsigned j = 0; j < CANDIDATES; j++) suppliers[j].probe = NULL;
    }
    CHECK(queried[CANDIDATES] == before + 1);
    for (unsigned i = 0; i < CANDIDATES; i++) u_free(suppliers[i].offers);
    u_free(picks);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] 256 candidates n=%u: last reached, priority bounded, local exhaustion preserves backoff", parts);
}

static void test_standby(void) {
    struct stream_supplier suppliers[5] = {0}; struct stream_offer offers[5] = {0}; struct stream_selection pick = {0};
    struct stream_optimizer o = {.descriptor = {.n = 1}, .selected = &pick, .suppliers = suppliers};
    struct stream_local local = {.anchored = 1, .next = 42};
    for (unsigned i = 0; i < 5; i++) {
        suppliers[i] = (struct stream_supplier){.node = STREAM_TEST_ADDR(i + 1), .online = 1, .compatible = 1, .rank = 5 - i, .offers = &offers[i]};
        suppliers[i].next = i < 4 ? &suppliers[i + 1] : NULL;
        offers[i].info = (struct stream_receive_info){.info_ms = 1000, .first_available = 0, .last_available = 42,
            .accepting = 1, .free_slots = 1, .rtt_valid = 1, .path_rtt_valid = 1};
    }
    standby_update(&o, &local, 2000);
    CHECK(!suppliers[0].standby && !suppliers[1].standby && suppliers[2].standby && suppliers[3].standby && suppliers[4].standby);
    suppliers[4].online = 0; offers[3].info.accepting = 0;
    standby_update(&o, &local, 3000);
    CHECK(suppliers[0].standby && suppliers[1].standby && suppliers[2].standby && !suppliers[3].standby && !suppliers[4].standby);
    standby_update(&o, &local, 70000);
    for (unsigned i = 0; i < 5; i++) CHECK(!suppliers[i].standby);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] standby bound3: random rank, offline/busy replacement, stale offers excluded");
}

static void test_backoff(void) {
    struct stream_offer offer = {.retry_ms = 40000, .failures = 3, .missing_valid = 1, .missing_seq = 42,
        .rescue_retry_ms = 60000, .rescue_failures = 4};
    struct stream_receive_info info = {.local_epoch = 1, .peer_epoch = 2, .requested_seq = 42, .last_available = 44};
    struct stream_supplier supplier = {.node = STREAM_TEST_ADDR(1), .online = 1, .compatible = 1, .offers = &offer};
    struct stream_optimizer o = {0};
    offer_update(&offer, &info);
    CHECK(offer.retry_ms == 40000 && offer.missing_valid && offer.failures == 3);
    offer.info.info_ms = 1000; offer.info.accepting = offer.info.free_slots = 1;
    CHECK(supplier_status(&o, &supplier, 0, 42, STREAM_TEST_ADDR(0), 2000) == SUPPLIER_BACKOFF);
    CHECK(supplier_status(&o, &supplier, 0, 43, STREAM_TEST_ADDR(0), 2000) == SUPPLIER_READY);
    info.requested_present = 1; info.contiguous_last = 44; offer_update(&offer, &info);
    CHECK(!offer.retry_ms && !offer.failures && !offer.missing_valid);
    CHECK(offer.rescue_retry_ms == 60000 && offer.rescue_failures == 4);
    offer.retry_ms = 50000; offer.failures = 2; info.peer_epoch++;
    offer_update(&offer, &info); CHECK(!offer.retry_ms && !offer.failures && offer.rescue_failures == 4);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] backoff resets only on proven change; live INFO cannot pardon silent DATA");
}

static void test_unknown_score(void) {
    struct stream_supplier old = {.node = STREAM_TEST_ADDR(1)};
    struct stream_optimizer o = {.settings = {.score_hysteresis_ms = 20}, .suppliers = &old};
    struct stream_offer offer = {.info = {.rtt_valid = 1, .path_rtt_valid = 1}};
    struct stream_supplier supplier = {.offers = &offer};
    struct stream_receive_info current = {.peer = STREAM_TEST_ADDR(1), .info_ms = 1000, .rtt_valid = 1, .path_rtt_valid = 0};
    CHECK(!improved(&o, 0, &supplier, &current, 2000) && old.refresh);
    current.path_rtt_valid = 1; current.path_rtt_ms = 100;
    CHECK(improved(&o, 0, &supplier, &current, 2000));
    old.refresh = 0;
    CHECK(!improved(&o, 0, &supplier, &current, 10000) && old.refresh);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] unknown chain RTT is refreshed before ordinary score migration");
}

/* Разные непосредственные поставщики не скрывают общего предка; считаются именно различные живые RS-части. */
static void test_dependencies(void) {
    struct stream_receive_info flows[4] = {0}; struct stream_selection picks[4] = {0};
    struct stream_dependency dependencies[4 * (STREAM_PATH_MAX - 1)];
    struct stream_optimizer o = {.descriptor = {.source = STREAM_TEST_ADDR(1), .n = 4, .r = 1}, .selected = picks,
        .dependencies = dependencies, .settings = {.score_hysteresis_ms = 20}, .startup_complete = 1};
    for (unsigned sub = 0; sub < 4; sub++) {
        flows[sub] = (struct stream_receive_info){.state = STREAM_RECEIVE_ACTIVE, .blocks = 10, .metrics_valid = 1,
            .peer = STREAM_TEST_ADDR(20 + sub), .info_ms = 1000, .path = {.count = 3, .nodes = {STREAM_TEST_ADDR(1), STREAM_TEST_ADDR(10), STREAM_TEST_ADDR(20 + sub)}}, .rtt_valid = 1, .path_rtt_valid = 1};
        picks[sub].current = (struct STREAM_RECEIVE *)&flows[sub];
    }
    dependencies_refresh(&o);
    CHECK(o.info.working_parts == 4 && o.info.maximum_dependency == 4 && !o.info.protected_by_fec);
    CHECK(dependency_parts(&o, STREAM_TEST_ADDR(10), 0) == 3 && dependency_parts(&o, STREAM_TEST_ADDR(1), 0) == 0);
    struct stream_offer offers[2] = {{0}}; struct stream_supplier suppliers[2] = {{0}};
    for (unsigned i = 0; i < 2; i++) {
        offers[i].info = flows[0]; offers[i].info.info_ms = 1000; offers[i].info.accepting = offers[i].info.free_slots = 1;
        suppliers[i] = (struct stream_supplier){.node = STREAM_TEST_ADDR(30 + i), .online = 1, .compatible = 1, .offers = &offers[i]};
    }
    suppliers[0].next = &suppliers[1]; o.suppliers = suppliers;
    offers[1].info.path.nodes[1] = STREAM_TEST_ADDR(11); offers[1].info.path.nodes[2] = STREAM_TEST_ADDR(31); offers[1].info.path_rtt_ms = 10;
    CHECK(supplier_choose(&o, 0, 42, STREAM_TEST_ADDR(20), 2000) == &suppliers[1]); // Независимый путь на10мс медленнее.
    CHECK(improved(&o, 0, &suppliers[1], &flows[0], 2000));
    offers[1].info.accepting = 0; CHECK(supplier_choose(&o, 0, 42, STREAM_TEST_ADDR(20), 2000) == &suppliers[0]);
    offers[1].info.accepting = 1; offers[1].info.path_rtt_ms = 100;
    CHECK(supplier_choose(&o, 0, 42, STREAM_TEST_ADDR(20), 2000) == &suppliers[0]);
    for (unsigned sub = 0; sub < 4; sub++) flows[sub].path.nodes[1] = STREAM_TEST_ADDR(10 + sub);
    dependencies_refresh(&o); CHECK(o.info.maximum_dependency == 1 && o.info.protected_by_fec);
    flows[3].metrics.stopped = 1; dependencies_refresh(&o);
    CHECK(o.info.working_parts == 3 && !o.info.protected_by_fec); // m=k, потерять ещё одну часть нельзя.
    o.descriptor.r = 2; dependencies_refresh(&o); CHECK(o.info.protected_by_fec); // Неполный набор m=3,k=2 защищён.
    picks[3].replacement = (struct STREAM_RECEIVE *)&flows[0]; dependencies_refresh(&o);
    CHECK(o.info.working_parts == 3 && o.info.maximum_dependency == 1); // Перекрытие не создаёт новую RS-часть.
    flows[2].close_requested = 1; dependencies_refresh(&o); CHECK(!o.info.protected_by_fec);
    flows[2].close_requested = 0; flows[3].metrics.stopped = 0;
    for (unsigned sub = 0; sub < 4; sub++) flows[sub].path.nodes[1] = STREAM_TEST_ADDR(10);
    dependencies_refresh(&o); CHECK(!o.info.protected_by_fec && o.info.maximum_dependency == 4); // Upstream поменял путь.
    for (unsigned sub = 0; sub < 4; sub++) flows[sub].path.count = 1;
    dependencies_refresh(&o); CHECK(o.info.protected_by_fec && !o.info.maximum_dependency); // Автор не является промежуточным узлом.
    o.descriptor.r = 0; dependencies_refresh(&o); CHECK(!o.info.protected_by_fec);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] full paths: common ancestor, independent branches, partial m/k, overlap, path change, r=0");
}

/* Для скачка нужны k разных частей: разные последние KEY допустимы только при доказанном общем диапазоне. */
static void test_live_key(void) {
    struct stream_supplier suppliers[2] = {0}; struct stream_offer offers[2][3] = {0};
    struct stream_optimizer o = {.descriptor = {.n = 3, .r = 1}, .suppliers = suppliers};
    struct stream_local local = {.next = 42, .live_head_us = 20000000}; uint64_t key;
    for (unsigned i = 0; i < 2; i++) {
        suppliers[i] = (struct stream_supplier){.node = STREAM_TEST_ADDR(i + 1), .online = 1, .compatible = 1, .offers = offers[i]};
        suppliers[i].next = i ? NULL : &suppliers[1];
        offers[i][i].info = (struct stream_receive_info){.info_ms = 1000, .accepting = 1, .free_slots = 1,
            .first_available = 20, .last_available = 110, .key_valid = 1, .key_seq = 100, .key_last_seq = 102,
            .key_pts_us = 19000000, .requested_present = 1, .requested_seq = 42, .contiguous_last = 105};
    }
    CHECK(live_key_choose(&o, &local, 10000000, 2000, &key, NULL) && key == 100);
    offers[1][1].info.key_seq = 90; offers[1][1].info.key_last_seq = 92; offers[1][1].info.key_pts_us = 18000000;
    CHECK(live_key_choose(&o, &local, 10000000, 2000, &key, NULL) && key == 100);
    offers[1][1].info.contiguous_last = 101; offers[1][1].info.key_valid = 0;
    CHECK(!live_key_choose(&o, &local, 10000000, 2000, &key, NULL));
    offers[1][1].info.contiguous_last = 105; offers[1][1].info.accepting = 0;
    CHECK(!live_key_choose(&o, &local, 10000000, 2000, &key, NULL));
    struct stream_selection picks[3] = {0};
    struct stream_receive_info active = {.peer = STREAM_TEST_ADDR(2), .state = STREAM_RECEIVE_ACTIVE};
    o.selected = picks; picks[1].current = (struct STREAM_RECEIVE *)&active;
    CHECK(live_key_choose(&o, &local, 10000000, 2000, &key, NULL) && key == 100);
    offers[1][1].info.requested_seq = 100; offers[1][1].info.requested_present = 0;
    CHECK(!live_key_choose(&o, &local, 10000000, 2000, &key, NULL)); /* SEEK обходит допуск, но не отсутствие KEY-части. */
    offers[1][1].info.requested_seq = 42; offers[1][1].info.requested_present = 1;
    offers[1][1].info.accepting = 1; offers[1][1].info.free_slots = 0;
    CHECK(live_key_choose(&o, &local, 10000000, 2000, &key, NULL) && key == 100);
    active.state = STREAM_RECEIVE_CLOSED;
    CHECK(!live_key_choose(&o, &local, 10000000, 2000, &key, NULL));
    offers[1][1].info.free_slots = 1;
    offers[1][1].info.accepting = 1;
    CHECK(!live_key_choose(&o, &local, 10000000, 70000, &key, NULL));
    local.live_head_us = 31000000;
    CHECK(!live_key_choose(&o, &local, 10000000, 2000, &key, NULL));
    local.live_head_us = 20000000; o.info.live_jumps = 1; o.live_key_seq = 100;
    CHECK(!live_key_choose(&o, &local, 10000000, 2000, &key, NULL));
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] live KEY: common k parts, retained range, missing tail, admission, stale/old/repeated KEY");
}

/* Фаза всей доставки не должна объявлять восстановление при потере лишь FEC-избытка. */
static void test_delivery_phase(void) {
    struct stream_selection selected[8] = {0};
    struct stream_optimizer o = {.descriptor = {.n = 8, .r = 2}, .selected = selected};
    struct stream_local local = {0};
    phase_update(&o, &local, 1000); CHECK(o.info.phase == STREAM_DELIVERY_BOOTSTRAP);
    o.startup_complete = 1; local.decoded = 1; local.anchored = 1; o.info.working_parts = 6;
    phase_update(&o, &local, 2000); CHECK(o.info.phase == STREAM_DELIVERY_LIVE && o.info.phase_since_ms == 2000);
    phase_update(&o, &local, 3000); CHECK(o.info.phase_since_ms == 2000);
    o.live_since_ms = 2500; phase_update(&o, &local, 3000); CHECK(o.info.phase == STREAM_DELIVERY_CATCHUP);
    selected[0].rescuing = 1; phase_update(&o, &local, 4000); CHECK(o.info.phase == STREAM_DELIVERY_CATCHUP);
    o.info.working_parts = 5; phase_update(&o, &local, 5000); CHECK(o.info.phase == STREAM_DELIVERY_RECOVERY);
    local.finished = 1; local.last = 10; local.next = 9;
    phase_update(&o, &local, 6000); CHECK(o.info.phase == STREAM_DELIVERY_DRAINING);
    local.next = 11; phase_update(&o, &local, 7000); CHECK(o.info.phase == STREAM_DELIVERY_FINISHED);
    local.waiting = 1; phase_update(&o, &local, 8000); CHECK(o.info.phase == STREAM_DELIVERY_WAITING);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] delivery phases: bootstrap, live, catchup, FEC surplus, recovery, END drain, offline wait");
}

/* Отдельный режим измеряет предельный roster и256 независимых метрик без сети/кодеков; обычный тест остаётся коротким. */
static void profile_cost(void) {
    clock_t started = clock(); test_scan(255);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "profile roster candidates=256 n=255 CPU_ms=%.3f",
        1000.0 * (clock() - started) / CLOCKS_PER_SEC);
    struct stream_metrics *metrics = u_calloc(256, sizeof(*metrics)); CHECK(metrics);
    started = clock();
    for (unsigned peer = 0; peer < 256; peer++) stream_metrics_init(&metrics[peer]);
    for (unsigned step = 0; step < 4000; step++) for (unsigned peer = 0; peer < 256; peer++) {
        struct stream_metric_snapshot snapshot;
        CHECK(stream_metrics_observe(&metrics[peer], step, (uint16_t)(step * 10), 0, 1300, step * 10) == STREAM_OK);
        CHECK(stream_metrics_poll(&metrics[peer], step * 10, &snapshot) == STREAM_OK);
        CHECK(!snapshot.stopped && !snapshot.lag_ms && snapshot.blocks);
    }
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "profile metrics flows=256 steps=4000 observe_poll_pairs=1024000 CPU_ms=%.3f",
        1000.0 * (clock() - started) / CLOCKS_PER_SEC);
    u_free(metrics);
}

int main(int argc, char **argv) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_STREAM, DEBUG_LEVEL_INFO);
    size_t before = u_get_allocated_count();
    if (argc == 2 && !strcmp(argv[1], "--profile")) profile_cost();
    else {
        CHECK(argc == 1);
        test_scan(1); test_standby(); test_backoff(); test_unknown_score(); test_dependencies(); test_live_key(); test_delivery_phase();
    }
    CHECK(u_get_allocated_count() == before);
    return 0;
}
