/* Живые пути 0→1→{2,3}→4, затем 0→3→4: общий предок, смена upstream и реальная потеря одной ветви. */
static int paths_hold_ancestor, paths_block_other, paths_delay_overlap;
static unsigned paths_delayed;

/* Зритель подключается после прогрева relay: bootstrap начинает с live-позиции, далее пропуски запрещены. */
static void paths_event(struct UTUN_INSTANCE *inst, const struct stream_event *event, void *arg) {
    if (event->type == STREAM_EVENT_DATA && !failover.data) {
        failover.next_seq = event->first_seq;
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "path fixture first decoded seq=%llu", (unsigned long long)event->first_seq);
    }
    failover_event(inst, event, arg);
}

static void paths_raw(struct ETCP_CONN *conn, struct ll_entry *entry) {
    struct stream_message m; CHECK(stream_fixture_parse(entry->dgram, entry->len, &m) == STREAM_OK);
    unsigned node = failover_node(conn->instance->node_id), peer = failover_node(conn->peer_node_id);
    if (same_stream(&m.descriptor, &failover.descriptor)) {
        if (peer == 4 && (node < 2 || (node == 3 && paths_block_other)) && m.command == STREAM_CMD_INFO_REQ) {
            struct stream_network *net = stream_network_get(nodes[node]); int pressure = net->global_pressure;
            net->global_pressure = 1; failover.handlers[node](conn, entry); net->global_pressure = pressure; return;
        }
        if (node == 1 && peer == 0 && m.command == STREAM_CMD_DATA && paths_hold_ancestor) {
            failover_hold(conn, entry, peer, UINT64_MAX); return;
        }
        if (node == 4 && m.command == STREAM_CMD_DATA && paths_delay_overlap) {
            struct stream_peer *p = peer_find(stream_network_get(nodes[4]), stream_address_read(conn->peer_ed25519_pubkey));
            struct STREAM_RECEIVE *f = p ? flow_by_handle(p, m.handle, 1) : NULL;
            if (f && f->slot && f->slot->replacement == f && f->watch_seq != UINT64_MAX && f->watch_seq != f->requested) {
                paths_delayed++; failover_hold(conn, entry, peer, get_time_tb() / 10 + 200); return;
            }
        }
    }
    failover.handlers[node](conn, entry);
}

static void test_paths(void) {
    memset(&failover, 0, sizeof(failover)); failover.tail = &failover.held;
    struct stream_parameters parameters = {.n = 4, .r = 2, .block_size = STREAM_RS_DEFAULT_SIZE};
    struct STREAM_HANDLE *viewer = NULL, *relays[3] = {0}; struct STREAM_RECEIVE *up[3][4] = {{0}};
    CHECK(stream_publish_open(nodes[0], GROUP1, &parameters, &failover.publisher) == STREAM_OK);
    struct stream_info origin; CHECK(stream_get_info(failover.publisher, &origin) == STREAM_OK); failover.descriptor = origin.descriptor;
    for (unsigned i = 1; i < 5; i++) wait_state(i, 0, GROUP1, origin.descriptor.source_epoch, 0);
    for (unsigned i = 0; i < 5; i++) {
        failover.handlers[i] = nodes[i]->api_bindings.callbacks[ETCP_ID_STREAM]; CHECK(etcp_bind(nodes[i], ETCP_ID_STREAM, paths_raw) == 0);
    }
    for (unsigned i = 1; i <= 3; i++) {
        CHECK(stream_subscribe_manual_open(nodes[i], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &relays[i - 1]) == STREAM_OK);
        if (i > 1) wait_state(i, 1, GROUP1, origin.descriptor.source_epoch, 1);
        for (unsigned sub = 0; sub < 4; sub++)
            CHECK(stream_receive_open(relays[i - 1], stream_address_read(nodes[i == 1 ? 0 : 1]->my_ed25519_pubkey), sub, 0, &up[i - 1][sub]) == STREAM_OK);
        failover_run(600, 1); wait_state(4, i, GROUP1, origin.descriptor.source_epoch, 1);
    }
    /* Первый INFO может включать задержку холодного соединения; сравнение DATA требует актуальных RTT живых upstream. */
    for (unsigned i = 1; i <= 3; i++) {
        struct STREAM_RECEIVE *probe = NULL;
        CHECK(stream_probe_open(relays[i - 1], stream_address_read(nodes[i == 1 ? 0 : 1]->my_ed25519_pubkey), 0, UINT64_MAX, &probe) == STREAM_OK);
        for (unsigned sub = 0; sub < 4; sub++) {
            if (sub) CHECK(stream_probe_next(probe, sub, UINT64_MAX) == STREAM_OK);
            struct stream_receive_info info; uint64_t deadline = get_time_tb() / 10 + 5000;
            do {
                failover_run(10, 1); CHECK(stream_receive_get_info(probe, &info) == STREAM_OK);
                CHECK(info.state != STREAM_RECEIVE_CLOSED && get_time_tb() / 10 < deadline);
            } while (!info.info_ready);
            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "path fixture warm relay=%u sub=%u rtt_ms=%u chain_ms=%u", i, sub, info.rtt_ms, info.path_rtt_ms);
        }
        stream_receive_close(probe);
    }
    stream_set_event_callback(nodes[4], paths_event, NULL);
    paths_block_other = 1;
    CHECK(stream_subscribe_open(nodes[4], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &viewer) == STREAM_OK);
    struct stream_selection_settings settings = {100, 100, 100}; CHECK(stream_selection_configure(viewer, &settings) == STREAM_OK);
    failover_run(1500, 1);
    paths_block_other = 0; paths_delay_overlap = 1;
    stream_relay_ready(nodes[4], &origin.descriptor, stream_address_read(nodes[3]->my_ed25519_pubkey));
    failover_run(2500, 1);
    struct stream_selection_info selection; CHECK(stream_selection_get_info(viewer, &selection) == STREAM_OK);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "path overlap comparison delayed=%u migrations=%llu GAP=%u",
        paths_delayed, (unsigned long long)selection.migrations, failover.gaps);
    CHECK(paths_delayed && !selection.migrations && !failover.gaps);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] fresh INFO and zero-based lag cannot hide slower same-index DATA, delayed=%u", paths_delayed);
    paths_delay_overlap = 0; failover_run(8000, 1); CHECK(stream_selection_get_info(viewer, &selection) == STREAM_OK);
    CHECK(selection.providers == 2 && selection.working_parts == 4 && selection.maximum_dependency == 4 && !selection.protected_by_fec);
    paths_hold_ancestor = failover.hold_source = 1; failover_run(250, 1); unsigned delivered = failover.data;
    failover_run(350, 1); CHECK(failover.data == delivered && !failover.gaps);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] two live suppliers share ancestor: protected=0, ancestor outage stops all four parts");
    paths_hold_ancestor = failover.hold_source = 0; failover_run(1500, 1); CHECK(failover.data > delivered && !failover.gaps);
    /* Один relay получает новый реальный upstream. Старые buffered DATA не объявляем новым путём. */
    struct stream_local local; CHECK(stream_local_get(nodes[3], &origin.descriptor, &local) == STREAM_OK);
    for (unsigned sub = 0; sub < 4; sub++) {
        stream_receive_close(up[2][sub]); up[2][sub] = NULL;
        CHECK(stream_receive_open(relays[2], stream_address_read(nodes[0]->my_ed25519_pubkey), sub, local.next, &up[2][sub]) == STREAM_OK);
    }
    failover_run(3000, 1); CHECK(stream_selection_get_info(viewer, &selection) == STREAM_OK);
    CHECK(selection.providers == 2 && selection.working_parts == 4 && selection.maximum_dependency == 2 && selection.protected_by_fec);
    delivered = failover.data; paths_hold_ancestor = failover.hold_source = 1; failover_run(2500, 1);
    CHECK(failover.data >= delivered + 20 && !failover.gaps);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] changed DATA path creates independence: ancestor outage preserves decode, frames=%u GAP=0",
        failover.data - delivered);
    paths_hold_ancestor = failover.hold_source = 0; failover_run(300, 1); CHECK(!failover.held);
    for (unsigned i = 0; i < 5; i++) CHECK(etcp_bind(nodes[i], ETCP_ID_STREAM, failover.handlers[i]) == 0);
    stream_close(viewer); stream_set_event_callback(nodes[4], NULL, NULL);
    for (unsigned i = 0; i < 3; i++) {
        for (unsigned sub = 0; sub < 4; sub++) stream_receive_close(up[i][sub]);
        stream_close(relays[i]);
    }
    stream_close(failover.publisher); test_clock_run_for(ua, 10000);
}
