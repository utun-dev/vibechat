#include "stream_address_fixture.h"
/* Границы общего бюджета и владение слотом при отказе/отмене двух перекрывающихся выдач. */
#include <stdlib.h>
#include "../src/stream/stream_network.c"

#define CHECK(x) do { if (!(x)) { \
    DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream budget line=%d: %s", __LINE__, #x); exit(1); \
} } while (0)

/* Только ресурсная часть network: ни один пакет не отправляется, callbacks pump отменяются до запуска. */
static void test_limits(struct UASYNC *ua) {
    struct UTUN_INSTANCE inst = {.ua = ua, .node_id = 99, .my_ed25519_pubkey = {0, 99}};
    struct stream_network net = {.inst = &inst};
    struct stream_peer peers[4] = {0};
    for (unsigned i = 0; i < 4; i++) { peers[i].network = &net; peers[i].address = STREAM_TEST_ADDR(10 + i); }
    struct stream_descriptor d = {.group_id = 1, .source = STREAM_TEST_ADDR(1), .source_epoch = 1, .n = 1,
        .block_size = STREAM_RS_DEFAULT_SIZE};
    /* Резерв владельца остаётся при закрытии всех DATA-handles; выделение через public API проверяется в service-тесте. */
    struct stream_receive_slot *slot = u_calloc(1, sizeof(*slot)); CHECK(slot);
    slot->network = &net; slot->descriptor = d; net.slots = slot; net.regular_used = 1;
    struct STREAM_RECEIVE *old = NULL, *reserve = NULL, *refused = NULL;
    CHECK(flow_new(&peers[0], &d, 0, 1, STREAM_FLOW_RESERVED, &old) == STREAM_OK);
    old->slot = slot; slot->primary = old;
    for (unsigned i = 1; i < STREAM_REGULAR_LIMIT; i++) {
        struct STREAM_RECEIVE *f = NULL; d.source = STREAM_TEST_ADDR(i + 1); d.group_id = 1 + i % 2;
        CHECK(flow_new(&peers[i % 4], &d, 0, 1, STREAM_FLOW_REGULAR, &f) == STREAM_OK);
    }
    CHECK(net.regular_used == 192 && net.flow_count == 192);
    CHECK(flow_new(&peers[0], &d, 0, 1, STREAM_FLOW_REGULAR, &refused) == STREAM_LOCAL_BUSY && !refused);
    for (unsigned i = 0; i < STREAM_BACKGROUND_LIMIT; i++) {
        struct STREAM_RECEIVE *f = NULL;
        CHECK(flow_new(&peers[0], &d, 0, 1, STREAM_FLOW_BACKGROUND, &f) == STREAM_OK); f->info.probe_only = 1;
    }
    CHECK(flow_new(&peers[1], &d, 0, 1, STREAM_FLOW_BACKGROUND, &refused) == STREAM_LOCAL_BUSY && !refused);
    for (unsigned i = 16; i < STREAM_TEMP_LIMIT; i++) {
        struct STREAM_RECEIVE *f = NULL;
        CHECK(flow_new(&peers[i / 16], &d, 0, 1, STREAM_FLOW_RESCUE, &f) == STREAM_OK); f->info.probe_only = 1;
        if (!reserve) reserve = f;
    }
    CHECK(net.flow_count == 256 && net.temporary_used == 64 && net.background_used == 16);
    CHECK(flow_new(&peers[1], &d, 0, 1, STREAM_FLOW_RESCUE, &refused) == STREAM_LOCAL_BUSY && !refused);
    reserve->descriptor = slot->descriptor; reserve->info_ready = 1; reserve->info.state = STREAM_RECEIVE_INFO;
    reserve->info.accepting = reserve->info.free_slots = reserve->info.requested_present = 1;
    CHECK(stream_probe_subscribe(reserve, 0, slot) == STREAM_OK);
    CHECK(slot->primary == old && slot->replacement == reserve && net.temporary_used == 64);
    /* Отмена нового поставщика сохраняет старый и постоянный резерв. */
    stream_receive_close(reserve); reserve = NULL;
    CHECK(slot->primary == old && !slot->replacement && net.regular_used == 192 && net.temporary_used == 63);
    CHECK(flow_new(&peers[1], &slot->descriptor, 0, 1, STREAM_FLOW_RESCUE, &reserve) == STREAM_OK);
    reserve->info.probe_only = reserve->info_ready = 1; reserve->info.state = STREAM_RECEIVE_INFO;
    reserve->info.accepting = reserve->info.free_slots = reserve->info.requested_present = 1;
    CHECK(stream_probe_subscribe(reserve, 0, slot) == STREAM_OK);
    /* Закрытие старого передаёт тот же постоянный ресурс замене, не требует свободного нового слота. */
    stream_receive_close(old);
    CHECK(slot->primary == reserve && !slot->replacement && net.regular_used == 192 && net.temporary_used == 63);
    CHECK(flow_new(&peers[0], &d, 0, 1, STREAM_FLOW_REGULAR, &refused) == STREAM_LOCAL_BUSY && !refused);
    stream_receive_close(reserve);
    CHECK(!slot->primary && net.regular_used == 192);
    CHECK(flow_new(&peers[0], &d, 0, 1, STREAM_FLOW_REGULAR, &refused) == STREAM_LOCAL_BUSY && !refused);
    stream_receive_slot_close(slot);
    CHECK(flow_new(&peers[0], &d, 0, 1, STREAM_FLOW_REGULAR, &refused) == STREAM_OK);
    for (unsigned i = 0; i < 4; i++) {
        if (peers[i].pump) uasync_call_soon_cancel(ua, peers[i].pump);
        peers[i].pump = NULL;
        while (peers[i].flows) stream_receive_close(peers[i].flows);
    }
    CHECK(!net.slots && !net.flow_count && !net.regular_used && !net.temporary_used && !net.background_used);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] shared budget: regular192, temporary64, background16; both close orders preserve reservation");
}

/* Временная выдача с историей ещё не даёт права выбрать старую в SELECT: сначала selector подтверждает догон. */
static void test_unconfirmed_overlap(void) {
    struct stream_network net = {0}; struct stream_peer first = {.network = &net}, second = {.network = &net};
    net.peers = &first; first.next = &second;
    struct stream_receive_slot slot = {0};
    struct STREAM_RECEIVE old = {.outgoing = 1, .slot = &slot}, replacement = {.outgoing = 1, .slot = &slot};
    old.descriptor = replacement.descriptor = (struct stream_descriptor){.group_id = 1, .source = STREAM_TEST_ADDR(1), .source_epoch = 1, .n = 1};
    replacement.info = (struct stream_receive_info){.state = STREAM_RECEIVE_ACTIVE, .blocks = 10, .metrics_valid = 1};
    slot.primary = &old; slot.replacement = &replacement; first.flows = &old; second.flows = &replacement;
    CHECK(removal_cost(&first, &old) >= 4);
    slot.primary = &replacement; slot.replacement = NULL;
    CHECK(removal_cost(&first, &old) < 4);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] OVERLOAD cannot SELECT essential old DATA based on unconfirmed overlap");
}

/* Точная проверка объёма запроса отдельно от изменчивой оценки скорости настоящего транспорта. */
static void test_minimal_relocation(void) {
    struct UTUN_INSTANCE inst = {.node_id = 1, .my_ed25519_pubkey = {0, 1}}; struct stream_network net = {.inst = &inst};
    struct stream_peer peer = {.network = &net, .address = STREAM_TEST_ADDR(2)}; net.peers = &peer;
    struct STREAM_RECEIVE flows[2] = {0}; peer.flows = flows;
    for (unsigned i = 0; i < 2; i++) {
        flows[i].descriptor = (struct stream_descriptor){.group_id = 1, .source = STREAM_TEST_ADDR(3), .source_epoch = 1, .n = 2};
        flows[i].outgoing = 1; flows[i].subblock = i;
        flows[i].info = (struct stream_receive_info){.handle = i + 1, .state = STREAM_RECEIVE_ACTIVE, .blocks = 10, .metrics_valid = 1};
        peer.requested.cuts[i] = (struct stream_cut){.handle = i + 1, .rate = 100};
    }
    flows[0].next = &flows[1]; peer.requested.active = 1; peer.requested.count = 2;
    peer.requested.required_rate = 100; peer.requested.deadline_ms = 5000;
    pressure_select(&peer, 1000);
    CHECK(peer.requested.cuts[0].relocate && !peer.requested.cuts[1].relocate);
    CHECK(!peer.requested.cuts[0].selected && !peer.requested.cuts[1].selected);
    peer.requested.required_rate = 101; pressure_select(&peer, 1100);
    CHECK(peer.requested.cuts[0].relocate && peer.requested.cuts[1].relocate);
    peer.requested.cuts[0].closed = 1; flows[0].info.state = STREAM_RECEIVE_CLOSED;
    peer.requested.required_rate = 100; pressure_select(&peer, 1200);
    CHECK(!peer.requested.cuts[0].relocate && !peer.requested.cuts[1].relocate);
    peer.requested.required_rate = 0; peer.requested.required_count = 2;
    pressure_select(&peer, 1300);
    CHECK(!peer.requested.cuts[0].relocate && peer.requested.cuts[1].relocate);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] OVERLOAD relocates only requested rate; local close cancels excess migration");
}

/* Более64 DATA одному пиру не мешают INFO; временный предел независим от постоянных выдач. */
static void test_peer_budget(struct UASYNC *ua) {
    struct UTUN_INSTANCE inst = {.ua = ua, .node_id = 99, .my_ed25519_pubkey = {0, 99}};
    struct stream_network net = {.inst = &inst}; struct stream_peer peer = {.network = &net, .address = STREAM_TEST_ADDR(10)};
    struct stream_descriptor d = {.group_id = 1, .source = STREAM_TEST_ADDR(1), .source_epoch = 1, .n = 192};
    for (unsigned i = 0; i < 192; i++) {
        struct STREAM_RECEIVE *f = NULL; CHECK(flow_new(&peer, &d, i, 1, STREAM_FLOW_REGULAR, &f) == STREAM_OK);
    }
    for (unsigned i = 0; i < 16; i++) {
        struct STREAM_RECEIVE *f = NULL; CHECK(flow_new(&peer, &d, i, 1, STREAM_FLOW_RESCUE, &f) == STREAM_OK);
    }
    struct STREAM_RECEIVE *refused = NULL;
    CHECK(flow_new(&peer, &d, 0, 1, STREAM_FLOW_RESCUE, &refused) == STREAM_LOCAL_BUSY && !refused);
    CHECK(net.temporary_used == 16 && net.regular_used == 192);
    CHECK(chat_setting_set(&inst, "stream_regular_limit", "193") == 0);
    CHECK(flow_new(&peer, &d, 0, 1, STREAM_FLOW_REGULAR, &refused) == STREAM_OK);
    CHECK(chat_setting_set(&inst, "stream_regular_limit", "80") == 0); refused = NULL;
    CHECK(flow_new(&peer, &d, 0, 1, STREAM_FLOW_REGULAR, &refused) == STREAM_LOCAL_BUSY && !refused);
    CHECK(net.regular_used == 193);
    while (peer.flows) stream_receive_close(peer.flows);
    CHECK(!net.regular_used && !net.temporary_used);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] peer DATA192 plus INFO16; temporary quota independent, live increase immediate, decrease preserves DATA");
}

/* Снижение лимита при пустых очередях всё равно требует64 закрытия; массив кандидатов не переполняется при192 DATA. */
static void test_budget_overload(struct UASYNC *ua) {
    struct UTUN_INSTANCE inst = {.ua = ua, .node_id = 99, .my_ed25519_pubkey = {0, 99}};
    struct TOPO_GROUPS groups = {.instance = &inst, .group_list = queue_new(ua, 16, 0, 8, "budget_groups")};
    struct TOPO_GROUP* group = (struct TOPO_GROUP*)queue_entry_new(sizeof(*group) - sizeof(struct ll_entry)); CHECK(group);
    group->group_id = 1; group->group_type = TOPO_GROUP_TYPE_CHAT; group->instance = &inst; group->wire_id[31] = 1;
    inst.topo_groups = &groups;
    CHECK(groups.group_list && !queue_data_put_with_index(groups.group_list, &group->ll));
    struct stream_network net = {.inst = &inst}; struct stream_peer peer = {.network = &net, .address = STREAM_TEST_ADDR(10), .own_epoch = 1};
    net.peers = &peer; net.peer_count = 1;
    peer.control = queue_new(ua, 0, 0, 0, "budget_test"); CHECK(peer.control);
    struct stream_descriptor d = {.group_id = 1, .source = STREAM_TEST_ADDR(99), .source_epoch = 1, .n = 192};
    for (unsigned i = 0; i < 192; i++) {
        struct STREAM_RECEIVE *f = NULL; CHECK(flow_new(&peer, &d, i, 0, STREAM_FLOW_REGULAR, &f) == STREAM_OK);
        f->info.state = STREAM_RECEIVE_ACTIVE; f->info.handle = i + 1;
    }
    CHECK(chat_setting_set(&inst, "stream_regular_limit", "64") == 0);
    uint64_t now = get_time_tb() / 10;
    stream_load_init(&net.load, now); stream_load_init(&peer.load, now);
    pressure_tick(&net, now);
    CHECK(peer.pressure.active && !peer.pressure.required_rate && peer.pressure.required_count == 64 && peer.pressure.count == 64);
    CHECK(net.regular_used == 192 && !peer.load.queued.packets);
    for (unsigned i = 0; i < peer.pressure.count; i++) {
        struct STREAM_RECEIVE *f = flow_by_handle(&peer, peer.pressure.cuts[i].handle, 0); CHECK(f);
        flow_close(f, STREAM_REASON_STOPPED, 0);
    }
    CHECK(net.regular_used == 128 && peer.pressure.released_count == 64);
    pressure_tick(&net, now + 100); CHECK(!peer.pressure.active);
    pressure_tick(&net, now + 700); pressure_tick(&net, now + 1300); pressure_tick(&net, now + 1400);
    CHECK(peer.pressure.active && peer.pressure.count == 64 && peer.pressure.required_count == 64);
    /* Увеличение отменяет перенос даже без DATA в очередях; активные выдачи сохранены. */
    CHECK(chat_setting_set(&inst, "stream_regular_limit", "192") == 0); pressure_tick(&net, now + 1500);
    CHECK(!peer.pressure.active && peer.pressure.outcome == STREAM_OVERLOAD_DRAINED && net.regular_used == 128);
    if (peer.pump) uasync_call_soon_cancel(ua, peer.pump);
    peer.pump = NULL;
    peer_reset(&peer, STREAM_REASON_STOPPED, 0, NULL);
    queue_free(peer.control); CHECK(!net.regular_used && !net.flow_count);
    queue_remove_data(groups.group_list, &group->ll); queue_entry_free(&group->ll); queue_free(groups.group_list);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] budget OVERLOAD with empty queues,192 candidates bounded64, subsequent batch and cancel on increase");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_STREAM, DEBUG_LEVEL_INFO);
    size_t before = u_get_allocated_count(); struct UASYNC *ua = uasync_create(); CHECK(ua);
    test_limits(ua); test_unconfirmed_overlap(); test_minimal_relocation(); test_peer_budget(ua); test_budget_overload(ua);
    uasync_destroy(ua, 0); CHECK(u_get_allocated_count() == before);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] stream budget: no tracked allocations remain"); return 0;
}
