/* Тот же стенд пяти ядер, но ограничивается реальная очередь отправки автора к зрителю; резервные пути свободны. */
static struct {
    struct ll_queue *queue;
    queue_callback_fn callback;
    void *arg;
    uint64_t next_pump_ms, restore_ms, first_request_ms, first_id, first_subscribe_ms;
    unsigned period_ms, requests, ends, scenario, silent_peer;
    int block_backups;
} overload_test;

static void overload_raw(struct ETCP_CONN *conn, struct ll_entry *entry) {
    struct stream_message m; CHECK(stream_fixture_parse(entry->dgram, entry->len, &m) == STREAM_OK);
    unsigned node = failover_node(conn->instance->node_id), peer = failover_node(conn->peer_node_id);
    if (same_stream(&m.descriptor, &failover.descriptor)) {
        if (node == 4 && peer == 0 && m.command == STREAM_CMD_OVERLOAD) {
            overload_test.requests++;
            if (!overload_test.first_request_ms) {
                overload_test.first_request_ms = get_time_tb() / 10; overload_test.first_id = stream_read_be(m.payload, 8);
                if (overload_test.scenario == 2 || overload_test.scenario == 3 || overload_test.scenario == 5)
                    overload_test.restore_ms = overload_test.first_request_ms + (overload_test.scenario == 5 ? 2500 : 100);
            }
        }
        if (node == 4 && peer == 0 && m.command == STREAM_CMD_OVERLOAD_END) {
            overload_test.ends++;
            if (overload_test.scenario == 3) { queue_dgram_free(entry); queue_entry_free(entry); return; }
        }
        if (peer == 4 && node >= 1 && node <= 3 && m.command == STREAM_CMD_INFO_REQ && overload_test.block_backups) {
            queue_dgram_free(entry); queue_entry_free(entry); return;
        }
        if (overload_test.scenario == 4 && overload_test.first_request_ms) {
            if (node == 4 && peer >= 1 && peer <= 3 && m.command == STREAM_CMD_INFO_RESP && !overload_test.first_subscribe_ms) {
                failover_hold(conn, entry, peer, get_time_tb() / 10 + 900); return;
            }
            if (peer == 4 && node >= 1 && node <= 3 && m.command == STREAM_CMD_SUBSCRIBE && !overload_test.first_subscribe_ms) {
                overload_test.silent_peer = node; overload_test.first_subscribe_ms = get_time_tb() / 10;
            }
            if (node == 4 && overload_test.first_subscribe_ms && peer == overload_test.silent_peer && m.command == STREAM_CMD_DATA) {
                queue_dgram_free(entry); queue_entry_free(entry); return;
            }
        }
    }
    failover.handlers[node](conn, entry);
}

static void overload_restore(void) {
    if (!overload_test.queue) return;
    queue_set_callback(overload_test.queue, overload_test.callback, overload_test.arg); queue_resume_callback(overload_test.queue);
    overload_test.queue = NULL; overload_test.restore_ms = 0;
}

static void overload_run(unsigned milliseconds) {
    uint64_t deadline = get_time_tb() / 10 + milliseconds;
    while (get_time_tb() / 10 < deadline) {
        uint64_t now = get_time_tb() / 10;
        if (overload_test.restore_ms && now >= overload_test.restore_ms) overload_restore();
        if (overload_test.queue && overload_test.queue->head && now >= overload_test.next_pump_ms) {
            overload_test.next_pump_ms = now + overload_test.period_ms;
            overload_test.callback(overload_test.queue, overload_test.arg);
        }
        failover_run(1, 1);
    }
}

static void test_overload_migration(void) {
    debug_set_category_level(DEBUG_CATEGORY_VIDEO, DEBUG_LEVEL_DEBUG);
    for (unsigned scenario = 0; scenario < 7; scenario++) {
        memset(&failover, 0, sizeof(failover)); failover.tail = &failover.held;
        memset(&overload_test, 0, sizeof(overload_test)); overload_test.scenario = scenario; overload_test.period_ms = scenario ? 80 : 40;
        struct STREAM_HANDLE *viewer = NULL, *relays[3] = {0}; struct STREAM_RECEIVE *up[3][2] = {{0}};
        struct stream_parameters parameters = {.n = 2, .r = 0, .block_size = STREAM_RS_DEFAULT_SIZE};
        if (scenario == 4) parameters.n = 1;
        CHECK(stream_publish_open(nodes[0], GROUP1, &parameters, &failover.publisher) == STREAM_OK);
        struct stream_info origin; CHECK(stream_get_info(failover.publisher, &origin) == STREAM_OK); failover.descriptor = origin.descriptor;
        for (unsigned i = 1; i < 5; i++) wait_state(i, 0, GROUP1, origin.descriptor.source_epoch, 0);
        for (unsigned i = 0; i < 5; i++) {
            failover.handlers[i] = nodes[i]->api_bindings.callbacks[ETCP_ID_STREAM];
            CHECK(etcp_bind(nodes[i], ETCP_ID_STREAM, overload_raw) == 0);
        }
        stream_set_event_callback(nodes[4], failover_event, NULL);
        CHECK(chat_setting_set(nodes[4], "stream_stall_timeout_ms", scenario == 4 ? "6000" : "2000") == 0);
        CHECK(stream_subscribe_open(nodes[4], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &viewer) == STREAM_OK);
        struct stream_selection_settings settings = {60000, 60000, 100};
        CHECK(stream_selection_configure(viewer, &settings) == STREAM_OK);
        failover_run(1500, 1);
        for (unsigned i = 1; i <= 3; i++) {
            CHECK(stream_subscribe_manual_open(nodes[i], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &relays[i - 1]) == STREAM_OK);
            for (unsigned sub = 0; sub < parameters.n; sub++)
                CHECK(stream_receive_open(relays[i - 1], stream_address_read(nodes[0]->my_ed25519_pubkey), sub, 0, &up[i - 1][sub]) == STREAM_OK);
            wait_state(4, i, GROUP1, origin.descriptor.source_epoch, 1);
        }
        failover_run(2500, 1);
        uint64_t handles[2];
        for (unsigned sub = 0; sub < parameters.n; sub++) {
            struct STREAM_RECEIVE *f = failover_flow(4, 0, sub); CHECK(f); handles[sub] = f->info.handle;
        }
        overload_test.block_backups = scenario && scenario != 4 && scenario != 6;
        struct ETCP_CONN *conn = instance_find_conn(nodes[0], nodes[4]->node_id); CHECK(conn && conn->links_up);
        while (conn->send_input_q->count) test_clock_poll(ua, 10);
        if (scenario != 6) {
            overload_test.queue = conn->send_input_q; overload_test.callback = overload_test.queue->callback;
            overload_test.arg = overload_test.queue->callback_arg; CHECK(overload_test.callback);
            queue_set_callback(overload_test.queue, NULL, NULL);
        } else {
            /* Ручные relay не могут перенести необходимые части: обход должен дойти до автоматического зрителя. */
            CHECK(stream_network_get(nodes[0])->regular_used == 8);
            CHECK(chat_setting_set(nodes[0], "stream_regular_limit", "7") == 0);
        }
        struct stream_peer *source = peer_find(stream_network_get(nodes[0]), stream_address_read(nodes[4]->my_ed25519_pubkey)); CHECK(source);
        uint64_t deadline = get_time_tb() / 10 + (scenario == 6 ? 15000 : 8000);
        struct stream_selection_info selection; int saw_shortfall = 0;
        do {
            overload_run(10); CHECK(stream_selection_get_info(viewer, &selection) == STREAM_OK);
            struct stream_peer_load_info load; CHECK(stream_peer_get_load(nodes[0], stream_address_read(nodes[4]->my_ed25519_pubkey), &load) == STREAM_OK);
            if (load.shortfall_rate) {
                CHECK(load.request_outcome == STREAM_OVERLOAD_EXPIRED && !load.request_active);
                CHECK(load.shortfall_rate == load.required_rate - load.released_rate); saw_shortfall = 1;
            }
            if ((!scenario || scenario == 4) && source->pressure.released_rate >= source->pressure.required_rate
                && source->pressure.required_rate) break;
            if (scenario == 6 && stream_network_get(nodes[0])->regular_used == 7) break;
            if (scenario == 5 && overload_test.ends >= 2) break;
            if (scenario && scenario != 4 && scenario != 6 && overload_test.first_request_ms
                && get_time_tb() / 10 >= overload_test.first_request_ms + 5500) break;
        } while (get_time_tb() / 10 < deadline);
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "OVERLOAD fixture scenario=%u requests=%u END=%u id=%llu first=%llu release=%llu/%llu"
            " migrations=%llu DATA=%u GAP=%u overlap=%u background=%u temporary=%u",
            scenario, overload_test.requests, overload_test.ends, (unsigned long long)source->pressure.id,
            (unsigned long long)overload_test.first_id, (unsigned long long)source->pressure.released_rate,
            (unsigned long long)source->pressure.required_rate, (unsigned long long)selection.migrations,
            failover.data, failover.gaps, selection.overlaps, stream_network_get(nodes[4])->background_used,
            stream_network_get(nodes[4])->temporary_used);
        CHECK(overload_test.requests && failover.data > 35 && !failover.gaps);
        if (!scenario || scenario == 4 || scenario == 6) {
            CHECK(selection.migrations == source->pressure.required_count && source->pressure.released_count == selection.migrations);
            CHECK(source->pressure.released_rate >= source->pressure.required_rate);
            CHECK((failover_flow(4, 0, 0) != NULL) + (failover_flow(4, 0, 1) != NULL) == parameters.n - (int)selection.migrations);
            if (scenario == 6) {
                CHECK(!source->pressure.required_rate && source->pressure.released_count == 1);
                CHECK(stream_network_get(nodes[0])->regular_used == 7);
                CHECK(chat_setting_set(nodes[0], "stream_regular_limit", "192") == 0);
                DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] hot budget shrink8->7 migrates one necessary DATA after unmovable peers; GAP=0");
            }
            if (scenario == 4) {
                uint64_t elapsed = get_time_tb() / 10 - overload_test.first_request_ms;
                CHECK(overload_test.first_subscribe_ms && selection.failures && elapsed > 2000 && elapsed <= 5000);
                CHECK(!failover_flow(4, overload_test.silent_peer, 0));
                DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] OVERLOAD survives request expiry and replaces silent DATA peer=%u elapsed_ms=%llu",
                    overload_test.silent_peer, (unsigned long long)elapsed);
            }
        } else {
            CHECK(!selection.migrations);
            for (unsigned sub = 0; sub < 2; sub++) {
                struct STREAM_RECEIVE *f = failover_flow(4, 0, sub); CHECK(f && f->info.handle == handles[sub]);
            }
            if (scenario == 1) CHECK(saw_shortfall && source->pressure.id > overload_test.first_id);
            else CHECK(overload_test.ends && !peer_find(stream_network_get(nodes[4]), stream_address_read(nodes[0]->my_ed25519_pubkey))->requested.active);
            if (scenario == 5) {
                struct stream_peer *supplier = peer_find(stream_network_get(nodes[4]), stream_address_read(nodes[0]->my_ed25519_pubkey));
                CHECK(saw_shortfall && overload_test.ends == 2 && supplier->requested.outcome == STREAM_OVERLOAD_DRAINED);
                CHECK(stream_network_get(nodes[4])->temporary_used == stream_network_get(nodes[4])->background_used);
            }
        }
        overload_restore(); overload_run(300);
        /* END старого id не отменяет более новый запрос; чужая эпоха не может отменить текущий. */
        struct stream_peer *receiver = peer_find(stream_network_get(nodes[4]), stream_address_read(nodes[0]->my_ed25519_pubkey)); CHECK(receiver);
        struct stream_pressure_request saved = receiver->requested;
        receiver->requested.id = source->pressure.id + 2; receiver->requested.active = 1;
        uint8_t payload[9], packet[STREAM_WIRE_HEADER + 9]; stream_write_be(payload, receiver->requested.id - 1, 8);
        payload[8] = STREAM_OVERLOAD_DRAINED;
        struct stream_message end = {.descriptor = origin.descriptor, .command = STREAM_CMD_OVERLOAD_END,
            .sender_epoch = receiver->other_epoch, .receiver_epoch = receiver->own_epoch};
        stream_wire_header(packet, &end); memcpy(packet + STREAM_WIRE_HEADER, payload, sizeof(payload));
        dispatch(stream_network_get(nodes[4]), stream_address_read(nodes[0]->my_ed25519_pubkey), packet, sizeof(packet), 0); CHECK(receiver->requested.active);
        stream_write_be(packet + STREAM_WIRE_HEADER, receiver->requested.id, 8); end.sender_epoch++;
        stream_wire_header(packet, &end);
        dispatch(stream_network_get(nodes[4]), stream_address_read(nodes[0]->my_ed25519_pubkey), packet, sizeof(packet), 0); CHECK(receiver->requested.active);
        /* Поздний drained после локального timeout всё ещё отменяет продолжающийся перенос этого id. */
        end.sender_epoch = receiver->other_epoch; stream_wire_header(packet, &end);
        receiver->requested.active = 0; receiver->requested.outcome = STREAM_OVERLOAD_EXPIRED;
        dispatch(stream_network_get(nodes[4]), stream_address_read(nodes[0]->my_ed25519_pubkey), packet, sizeof(packet), 0);
        CHECK(receiver->requested.outcome == STREAM_OVERLOAD_DRAINED);
        receiver->requested = saved;
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] OVERLOAD scenario=%u: bounded migration/cancel/retry; stale id/epoch ignored", scenario);
        for (unsigned i = 0; i < 5; i++) CHECK(etcp_bind(nodes[i], ETCP_ID_STREAM, failover.handlers[i]) == 0);
        while (failover.held) {
            struct failover_packet *packet = failover.held; failover.held = packet->next;
            queue_dgram_free(packet->entry); queue_entry_free(packet->entry); u_free(packet);
        }
        stream_close(viewer); stream_set_event_callback(nodes[4], NULL, NULL);
        for (unsigned i = 0; i < 3; i++) {
            for (unsigned sub = 0; sub < 2; sub++) stream_receive_close(up[i][sub]);
            stream_close(relays[i]);
        }
        stream_close(failover.publisher); test_clock_run_for(ua, 10000);
    }
}
