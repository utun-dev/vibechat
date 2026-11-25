/* Стенд из пяти живых ядер: автор, три резерва и зритель. Включается после общих helpers test_stream_service.c. */
struct failover_packet {
    struct failover_packet *next;
    struct ETCP_CONN *conn;
    struct ll_entry *entry;
    uint64_t due;
    unsigned peer;
};
static struct {
    etcp_recv_fn handlers[5];
    struct failover_packet *held, **tail;
    struct STREAM_HANDLE *publisher;
    struct stream_descriptor descriptor;
    uint64_t target, next_write, last_data_ms, first_query_ms, first_reply_ms, subscribe_ms, next_seq;
    uint64_t probe_nonce[5], last_frame_ms, maximum_gap_ms;
    uint64_t slow_due_ms, slow_closed_ms, slow_progress_ms, slow_attempt_ms;
    unsigned slow_data;
    uint64_t report_ms, control_packets, control_bytes, queue_max, queue_age_max, loop_late_max;
    double cpu_max;
    unsigned reports, cpu_samples;
    unsigned queries[5], background_queries[5], subscribes[5], winner, data, gaps;
    int active, scenario, hold_source;
} failover;

/* Проверяем, что диагностика продолжает работать при молчании DATA, но не печатает сводку на каждом wake. */
static void failover_report(int level, const char *category, const char *message) {
    if (strcmp(category, "stream")) return;
    const char *stopped = strstr(message, "stream rescue attempt stopped ");
    if (stopped && failover.scenario >= 14) {
        char source[65], supplier[65]; unsigned long long elapsed; unsigned sub;
        if (sscanf(stopped, "stream rescue attempt stopped source=%64s sub=%u peer=%64s reason=attempt_deadline elapsed_ms=%llu",
            source, &sub, supplier, &elapsed) == 4 && !strcmp(source, STREAM_ADDRESS_TEXT(failover.descriptor.source)) && !sub && !strcmp(supplier, sc_public_key_hex(nodes[2]->my_ed25519_pubkey, (char[65]){0})))
            failover.slow_attempt_ms = elapsed;
    }
    const char *line = strstr(message, "stream SELECTOR self=");
    if (!line) return;
    char source[65]; unsigned long long self, group, epoch, now;
    CHECK(level == DEBUG_LEVEL_DEBUG);
    CHECK(sscanf(line, "stream SELECTOR self=%llx group=%llx source=%64s epoch=%llx sample_ms=%llu",
        &self, &group, source, &epoch, &now) == 5);
    if (self != nodes[4]->node_id || epoch != failover.descriptor.source_epoch) return;
    CHECK(group == failover.descriptor.group_id && !strcmp(source, STREAM_ADDRESS_TEXT(failover.descriptor.source)));
    CHECK(!failover.report_ms || now - failover.report_ms >= 1000);
    CHECK(strstr(line, " decode_idle_ms=") && strstr(line, " rescue=") && strstr(line, " probes_total="));
    failover.report_ms = now; failover.reports++;
}

static unsigned failover_node(uint64_t node) {
    unsigned i = 0; while (i < 5 && nodes[i]->node_id != node) i++; CHECK(i < 5); return i;
}

static void failover_hold(struct ETCP_CONN *conn, struct ll_entry *entry, unsigned peer, uint64_t due) {
    struct failover_packet *packet = u_calloc(1, sizeof(*packet)); CHECK(packet);
    packet->conn = conn; packet->entry = entry; packet->peer = peer; packet->due = due;
    *failover.tail = packet; failover.tail = &packet->next;
}

static void failover_raw(struct ETCP_CONN *conn, struct ll_entry *entry) {
    unsigned node = failover_node(conn->instance->node_id), peer = failover_node(conn->peer_node_id);
    struct stream_message m; CHECK(stream_fixture_parse(entry->dgram, entry->len, &m) == STREAM_OK);
    if (failover.active && same_stream(&m.descriptor, &failover.descriptor) && m.command != STREAM_CMD_DATA) {
        failover.control_packets++; failover.control_bytes += entry->len;
    }
    uint64_t now = get_time_tb() / 10;
    /* Точные фоновые INFO используют тот же wire-формат; аварийную пробу отличает её локальный бюджет. */
    int rescue = 0;
    if ((m.command == STREAM_CMD_INFO_REQ && peer == 4) || (m.command == STREAM_CMD_INFO_RESP && node == 4)) {
        unsigned supplier = node == 4 ? peer : node;
        struct stream_peer *p = peer_find(stream_network_get(nodes[4]), stream_address_read(nodes[supplier]->my_ed25519_pubkey));
        uint64_t nonce = stream_read_be(m.payload, 8);
        for (struct STREAM_RECEIVE *f = p ? p->flows : NULL; f; f = f->next)
            if (f->outgoing && f->probe == nonce && f->budget == STREAM_FLOW_RESCUE) rescue = 1;
        if (!rescue && m.command == STREAM_CMD_INFO_REQ) failover.background_queries[node]++;
    }
    if (m.command == STREAM_CMD_DATA) {
        struct stream_path path; size_t used; CHECK(stream_path_parse(m.payload, m.length, &path, &used) == STREAM_OK);
        uint64_t seq = stream_read_be(m.payload + used + STREAM_DATA_HEAD_SIZE + STREAM_FRAME_TIMING_SIZE + STREAM_BLOCK_SEQ_OFF, 8);
        if (node == 4 && peer == 0 && !m.subblock) {
            if (failover.hold_source) {
                if (failover.scenario >= 14 && seq == failover.target) {
                    entry->dgram[2] = STREAM_CMD_GAP; entry->len = STREAM_WIRE_HEADER + 16;
                    stream_write_be(entry->dgram + STREAM_WIRE_HEADER, seq, 8);
                    stream_write_be(entry->dgram + STREAM_WIRE_HEADER + 8, seq + 1, 8);
                }
                failover_hold(conn, entry, peer, UINT64_MAX); return;
            }
            failover.last_data_ms = now;
        }
        if (failover.active && failover.scenario >= 14 && node == 4 && peer == 2 && !m.subblock) {
            if (failover.slow_due_ms < now) failover.slow_due_ms = now;
            failover.slow_due_ms += 200; failover_hold(conn, entry, peer, failover.slow_due_ms); return;
        }
        if (failover.active && (failover.scenario == 4 || (failover.scenario == 6 && node == 1))
            && node >= 1 && node <= 3 && !m.subblock && seq == failover.target) {
            entry->dgram[2] = STREAM_CMD_GAP; entry->len = STREAM_WIRE_HEADER + 16;
            stream_write_be(entry->dgram + STREAM_WIRE_HEADER, seq, 8); stream_write_be(entry->dgram + STREAM_WIRE_HEADER + 8, seq + 1, 8);
        }
        if (failover.active && failover.scenario == 12 && node == 4 && peer == 2 && !m.subblock && seq == failover.target) {
            entry->dgram[2] = STREAM_CMD_GAP; entry->len = STREAM_WIRE_HEADER + 16;
            stream_write_be(entry->dgram + STREAM_WIRE_HEADER, seq, 8); stream_write_be(entry->dgram + STREAM_WIRE_HEADER + 8, seq + 1, 8);
        }
        if (failover.active && node == 4 && peer == 2 && !m.subblock
            && (failover.scenario == 2 || failover.scenario == 7 || failover.scenario == 10)) {
            queue_dgram_free(entry); queue_entry_free(entry); return;
        }
    }
    if (failover.active && rescue && m.command == STREAM_CMD_INFO_REQ && peer == 4 && m.length == STREAM_INFO_REQUEST_SIZE
        && stream_read_be(m.payload + 9, 8) != UINT64_MAX) {
        CHECK(!m.subblock || failover.scenario == 5);
        if (failover.scenario == 11 && !failover.first_query_ms) failover.target = stream_read_be(m.payload + 9, 8);
        failover.queries[node]++;
        failover.probe_nonce[node] = stream_read_be(m.payload, 8);
        if (!failover.first_query_ms) failover.first_query_ms = now;
        if (failover.scenario == 3) { queue_dgram_free(entry); queue_entry_free(entry); return; }
        if (failover.scenario == 9 && node < 3) {
            struct stream_network *net = stream_network_get(nodes[node]); int pressure = net->global_pressure;
            net->global_pressure = 1; failover.handlers[node](conn, entry); net->global_pressure = pressure; return;
        }
    }
    if (failover.active && rescue && node == 4 && m.command == STREAM_CMD_INFO_RESP
        && stream_read_be(m.payload + STREAM_INFO_POSITION_OFF, 8) != UINT64_MAX) {
        CHECK(peer >= 1 && peer <= 3);
        /* Фиксируем одинаковый реально имеющийся конец предложений: публикация нового блока между
         * запросами не должна подменять проверку CPU/окна случайной разницей head. */
        uint64_t head = stream_read_be(m.payload + 24, 8), end = stream_read_be(m.payload + STREAM_INFO_POSITION_OFF + 8, 8);
        if (head != UINT64_MAX && head > failover.target + 2) {
            stream_write_be(entry->dgram + STREAM_WIRE_HEADER + 24, failover.target + 2, 8);
            if (stream_read_be(m.payload + 8, 8) > failover.target + 2)
                stream_write_be(entry->dgram + STREAM_WIRE_HEADER + 8, failover.target + 2, 8);
            if (end != UINT64_MAX && end > failover.target + 2)
                stream_write_be(entry->dgram + STREAM_WIRE_HEADER + STREAM_INFO_POSITION_OFF + 8, failover.target + 2, 8);
        }
        /* Обычно ответы задержаны на 0/20/80 мс. В сценарии 6 ответ 1 непригоден, а 2/3 должны войти в общее окно 50 мс. */
        entry->dgram[STREAM_WIRE_HEADER + STREAM_INFO_OFFER_OFF] |= 8;
        stream_write_be(entry->dgram + STREAM_WIRE_HEADER + STREAM_INFO_OFFER_OFF + 3, (3 - peer) * 1000, 2);
        unsigned delay = peer == 1 ? 0 : peer == 2 ? 20 : failover.scenario == 6 ? 45 : 80;
        failover_hold(conn, entry, peer, now + delay); return;
    }
    if (failover.active && m.command == STREAM_CMD_SUBSCRIBE && peer == 4) {
        CHECK(!m.subblock); failover.subscribes[node]++; failover.winner = node;
        if (!failover.subscribe_ms) failover.subscribe_ms = now;
        CHECK(failover.first_reply_ms && now >= failover.first_reply_ms + 50);
        struct stream_peer *supplier = peer_find(stream_network_get(nodes[4]), stream_address_read(nodes[node]->my_ed25519_pubkey)); int reused = 0;
        for (struct STREAM_RECEIVE *f = supplier ? supplier->flows : NULL; f; f = f->next)
            if (f->outgoing && f->info.handle == m.handle && f->probe == failover.probe_nonce[node] && f->deadline == UINT64_MAX) reused = 1;
        CHECK(reused); // SUBSCRIBE использует ту же INFO-пробу, без повторного запроса.
        if (failover.scenario == 13 && node == 2) {
            struct stream_network *net = stream_network_get(nodes[node]); int pressure = net->global_pressure;
            net->global_pressure = 1; failover.handlers[node](conn, entry); net->global_pressure = pressure; return;
        }
    }
    if (failover.active && failover.scenario >= 14 && node == 2 && peer == 4 && m.command == STREAM_CMD_UNSUBSCRIBE) {
        failover.slow_closed_ms = now;
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "slow reserve closed elapsed_ms=%llu recent_DATA_ms=%llu DATA_count=%u",
            (unsigned long long)(now - failover.subscribe_ms),
            (unsigned long long)(now - failover.slow_progress_ms), failover.slow_data);
    }
    failover.handlers[node](conn, entry);
}

static void failover_event(struct UTUN_INSTANCE *inst, const struct stream_event *event, void *arg) {
    (void)arg; CHECK(inst == nodes[4]);
    if (event->type == STREAM_EVENT_DATA) {
        if (event->first_seq != failover.next_seq)
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "rescue DATA out of order scenario=%d active=%d expected=%llu received=%llu",
                failover.scenario, failover.active, (unsigned long long)failover.next_seq, (unsigned long long)event->first_seq);
        CHECK(event->first_seq == failover.next_seq); failover.next_seq++; failover.data++;
        uint64_t now = get_time_tb() / 10;
        if (failover.last_frame_ms && now - failover.last_frame_ms > failover.maximum_gap_ms)
            failover.maximum_gap_ms = now - failover.last_frame_ms;
        failover.last_frame_ms = now;
    } else if (event->type == STREAM_EVENT_GAP) failover.gaps++;
    else CHECK(event->type == STREAM_EVENT_END || event->type == STREAM_EVENT_CLOSED);
}

static void failover_run(unsigned milliseconds, int publish) {
    uint64_t until = get_time_tb() / 10 + milliseconds;
    while (get_time_tb() / 10 < until) {
        uint64_t now = get_time_tb() / 10;
        if (failover.active) for (unsigned i = 0; i < 5; i++) {
            struct stream_network *net = stream_network_get(nodes[i]);
            if (net->load.queued.bytes > failover.queue_max) failover.queue_max = net->load.queued.bytes;
            if (net->load.snapshot.oldest_age_ms > failover.queue_age_max) failover.queue_age_max = net->load.snapshot.oldest_age_ms;
            if (net->callback_late_ms > failover.loop_late_max) failover.loop_late_max = net->callback_late_ms;
            if (net->cpu.valid) {
                failover.cpu_samples++;
                if (net->cpu.average_percent > failover.cpu_max) failover.cpu_max = net->cpu.average_percent;
            }
        }
        /* Оставляем блок 0 единственным до первого DATA: подписка на live head тогда всегда начинает с 0. */
        if (publish && now >= failover.next_write && (!failover.next_write || failover.data)) {
            uint8_t data[4096] = {0}; size_t length = stream_publish_capacity(failover.publisher), accepted;
            CHECK(length <= sizeof(data));
            CHECK(stream_publish_write(failover.publisher, data, length, &accepted) == STREAM_OK && accepted == length);
            failover.next_write = now + 100;
        }
        if (failover.scenario == 1 && failover.first_reply_ms && now >= failover.first_reply_ms + 25) failover.hold_source = 0;
        if (failover.scenario == 7 && failover.subscribe_ms && now >= failover.subscribe_ms + 25) failover.hold_source = 0;
        if (failover.scenario == 10 && failover.subscribe_ms && now >= failover.subscribe_ms + 5500) failover.hold_source = 0;
        if (failover.scenario >= 14 && failover.subscribe_ms) failover.hold_source = 0;
        struct failover_packet **link = &failover.held;
        while (*link) {
            struct failover_packet *packet = *link;
            if (packet->due > now && (packet->peer || failover.hold_source)) { link = &packet->next; continue; }
            *link = packet->next;
            if (packet->peer && packet->entry->dgram[2] == STREAM_CMD_INFO_RESP) {
                const uint8_t *payload = packet->entry->dgram + STREAM_WIRE_HEADER;
                if (!failover.first_reply_ms && payload[STREAM_INFO_POSITION_OFF + 24]
                    && (payload[STREAM_INFO_OFFER_OFF] & 1) && stream_read_be(payload + STREAM_INFO_OFFER_OFF + 1, 2))
                    failover.first_reply_ms = now;
            } else {
                failover.last_data_ms = now;
                if (failover.scenario >= 14 && packet->peer == 2 && packet->entry->dgram[2] == STREAM_CMD_DATA
                    && !failover.slow_closed_ms) {
                    failover.slow_progress_ms = now; failover.slow_data++;
                }
            }
            failover.handlers[4](packet->conn, packet->entry); u_free(packet);
        }
        failover.tail = link;
        test_clock_poll(ua, 10);
    }
}

static struct STREAM_RECEIVE *failover_flow(unsigned node, unsigned peer, unsigned sub) {
    struct stream_peer *p = peer_find(stream_network_get(nodes[node]), stream_address_read(nodes[peer]->my_ed25519_pubkey));
    for (struct STREAM_RECEIVE *f = p ? p->flows : NULL; f; f = f->next)
        if (same_stream(&f->descriptor, &failover.descriptor) && f->outgoing && !f->info.probe_only
            && f->subblock == sub && f->info.state == STREAM_RECEIVE_ACTIVE) return f;
    return NULL;
}

static void test_failover(void) {
    for (unsigned scenario = 0; scenario < 16; scenario++) {
        memset(&failover, 0, sizeof(failover)); failover.tail = &failover.held; failover.scenario = scenario;
        struct STREAM_HANDLE *viewer = NULL, *relays[3] = {0}; struct STREAM_RECEIVE *up[3][2] = {{0}};
        struct stream_parameters parameters = {.n = 2, .r = 0, .block_size = STREAM_RS_DEFAULT_SIZE};
        if (scenario == 11) parameters.r = 1;
        CHECK(stream_publish_open(nodes[0], GROUP1, &parameters, &failover.publisher) == STREAM_OK);
        struct stream_info origin; CHECK(stream_get_info(failover.publisher, &origin) == STREAM_OK); failover.descriptor = origin.descriptor;
        debug_set_log_hook(failover_report);
        for (unsigned i = 1; i < 5; i++) wait_state(i, 0, GROUP1, origin.descriptor.source_epoch, 0);
        for (unsigned i = 0; i < 5; i++) {
            failover.handlers[i] = nodes[i]->api_bindings.callbacks[ETCP_ID_STREAM];
            CHECK(etcp_bind(nodes[i], ETCP_ID_STREAM, failover_raw) == 0);
        }
        stream_set_event_callback(nodes[4], failover_event, NULL);
        CHECK(stream_subscribe_open(nodes[4], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &viewer) == STREAM_OK);
        struct stream_selection_settings settings = {60000, 60000, 100};
        CHECK(stream_selection_configure(viewer, &settings) == STREAM_OK);
        CHECK(chat_setting_set(nodes[4], "stream_stall_timeout_ms", scenario == 5 ? "750"
            : scenario == 10 || scenario == 15 ? "6000" : "500") == 0);
        failover_run(1500, 1); CHECK(failover.data >= 10 && failover_flow(4, 0, 0) && failover_flow(4, 0, 1));
        for (unsigned i = 1; i <= 3; i++) {
            CHECK(stream_subscribe_manual_open(nodes[i], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &relays[i - 1]) == STREAM_OK);
            for (unsigned sub = 0; sub < 2; sub++) CHECK(stream_receive_open(relays[i - 1], stream_address_read(nodes[0]->my_ed25519_pubkey), sub, 0, &up[i - 1][sub]) == STREAM_OK);
            wait_state(4, i, GROUP1, origin.descriptor.source_epoch, 1);
        }
        failover_run(scenario ? 1000 : 10000, 1);
        if (!scenario) {
            for (unsigned i = 1; i <= 3; i++) {
                CHECK(failover.background_queries[i] >= 4); // Две части: первичный обзор и обновление резерва до30с.
                CHECK(!failover_flow(4, i, 0) && !failover_flow(4, i, 1));
            }
            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] standby INFO refreshed all three new peers within10s without duplicate DATA");
        }
        struct STREAM_RECEIVE *old = failover_flow(4, 0, 0); CHECK(old);
        uint64_t old_handle = old->info.handle, last_data = old->info.progress_ms;
        struct stream_local local; CHECK(stream_local_get(nodes[4], &origin.descriptor, &local) == STREAM_OK);
        failover.target = local.next; failover.active = 1; failover.hold_source = scenario != 5; failover.maximum_gap_ms = 0;
        uint64_t disconnected = get_time_tb() / 10;
        if (scenario == 8) { flow_close(old, STREAM_REASON_TRANSPORT, 1); stream_network_wake(stream_network_get(nodes[4])); }
        failover_run(scenario == 10 || scenario == 15 ? 14000 : scenario == 14 ? 7000
            : scenario >= 3 && scenario <= 5 ? 6200 : 2300, scenario != 5);
        struct stream_selection_info info; CHECK(stream_selection_get_info(viewer, &info) == STREAM_OK);
        CHECK(failover.reports >= 2 && failover.control_packets && failover.control_bytes);
        if (failover.gaps || !failover.queries[1] || !failover.queries[2] || !failover.queries[3])
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "rescue verification scenario=%u gaps=%u queries=%u/%u/%u data=%u target=%llu",
                scenario, failover.gaps, failover.queries[1], failover.queries[2], failover.queries[3], failover.data,
                (unsigned long long)failover.target);
        CHECK(!failover.gaps && failover.queries[1] && failover.queries[2] && failover.queries[3]);
        if (scenario == 8) CHECK(failover.first_query_ms >= disconnected && failover.first_query_ms < disconnected + 30);
        else {
            unsigned threshold = scenario == 5 ? 750 : scenario == 10 || scenario == 15 ? 6000 : 500;
            CHECK(failover.first_query_ms >= last_data + threshold);
            CHECK(failover.first_query_ms <= last_data + threshold + 30);
        }
        if (!scenario) {
            CHECK(failover.subscribes[2] == 1 && !failover.subscribes[1] && !failover.subscribes[3]);
            CHECK(failover_flow(4, 2, 0) && !failover_flow(4, 0, 0) && failover_flow(4, 0, 1));
            CHECK(failover.subscribe_ms - failover.first_reply_ms < 80 && info.migrations == 1);
            CHECK(failover.maximum_gap_ms < 800);
        } else if (scenario >= 14) {
            unsigned limit = scenario == 15 ? 6000 : 5000;
            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "slow reserve result scenario=%u attempt_ms=%llu subscribed_ms=%llu closed_ms=%llu"
                " last_DATA_ms=%llu delivered=%u migrations=%llu",
                scenario, (unsigned long long)failover.slow_attempt_ms, (unsigned long long)failover.subscribe_ms, (unsigned long long)failover.slow_closed_ms,
                (unsigned long long)failover.slow_progress_ms, failover.slow_data, (unsigned long long)info.migrations);
            /* Дедлайн начинается при выборе резерва. Поставщик получает SUBSCRIBE уже после этого выбора. */
            CHECK(failover.slow_attempt_ms >= limit && failover.slow_attempt_ms <= limit + 30);
            CHECK(failover.slow_data >= 20 && failover.slow_closed_ms >= failover.first_reply_ms + 50 + limit);
            CHECK(failover.slow_closed_ms <= failover.subscribe_ms + limit + 30);
            CHECK(failover.slow_closed_ms - failover.slow_progress_ms <= 210);
            CHECK(failover.subscribes[2] == 1 && !failover_flow(4, 2, 0) && !info.migrations);
            CHECK(failover_flow(4, 0, 0) && failover_flow(4, 0, 0)->info.handle == old_handle);
        } else if (scenario == 11) {
            CHECK(failover.winner >= 1 && failover.winner <= 3 && failover_flow(4, failover.winner, 0));
            CHECK(!failover_flow(4, 0, 0) && failover_flow(4, 0, 1) && info.migrations == 1);
            CHECK(failover.maximum_gap_ms < 200); // Исправная RS-часть поддерживает непрерывный decode.
        } else if (scenario == 2 || scenario == 12 || scenario == 13) {
            CHECK(failover.subscribes[2] == 1 && failover.subscribes[1] == 1 && failover_flow(4, 1, 0));
            CHECK(!failover_flow(4, 2, 0) && !failover_flow(4, 0, 0) && info.migrations == 1);
            CHECK(failover.maximum_gap_ms < 1500);
        } else if (scenario == 6 || scenario == 9) {
            CHECK(!failover.subscribes[1] && !failover.subscribes[2] && failover.subscribes[3] == 1);
            CHECK(failover_flow(4, 3, 0) && !failover_flow(4, 0, 0) && info.migrations == 1);
            CHECK(failover.subscribe_ms - failover.first_reply_ms < 80 && failover.maximum_gap_ms < 800);
        } else if (scenario == 7 || scenario == 10) {
            CHECK(failover.subscribes[2] == 1 && !failover_flow(4, 2, 0) && !info.migrations);
            CHECK(!failover.subscribes[1] && !failover.subscribes[3]);
            CHECK(failover_flow(4, 0, 0) && failover_flow(4, 0, 0)->info.handle == old_handle && failover.data > 35);
        } else if (scenario == 8) {
            CHECK(info.failures && info.migrations == 1 && !failover_flow(4, 0, 0));
        } else {
            CHECK(!failover.subscribes[1] && !failover.subscribes[2] && !failover.subscribes[3] && !info.migrations);
            CHECK(failover_flow(4, 0, 0) && failover_flow(4, 0, 0)->info.handle == old_handle);
            if (scenario == 1) CHECK(failover.data > 35);
            if (scenario == 3) CHECK(failover.queries[1] <= 4 && failover.queries[2] <= 4 && failover.queries[3] <= 4);
            if (scenario == 4 || scenario == 5) CHECK(!failover.first_reply_ms);
        }
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] rescue scenario=%u queries=%u/%u/%u subscribes=%u/%u/%u data=%u gaps=%u"
            " detect_ms=%llu choose_ms=%llu freeze_ms=%llu pending_idle_ms=%llu migrations=%llu"
            " control_rx=%llu/%llu queue_max_B=%llu queue_age_max_ms=%llu cpu_max_pct=%.1f/%d loop_late_max_ms=%llu",
            scenario, failover.queries[1], failover.queries[2], failover.queries[3], failover.subscribes[1], failover.subscribes[2],
            failover.subscribes[3], failover.data, failover.gaps, (unsigned long long)(failover.first_query_ms - last_data),
            (unsigned long long)(failover.subscribe_ms ? failover.subscribe_ms - failover.first_reply_ms : 0),
            (unsigned long long)failover.maximum_gap_ms,
            (unsigned long long)(failover.last_frame_ms ? get_time_tb() / 10 - failover.last_frame_ms : 0),
            (unsigned long long)info.migrations, (unsigned long long)failover.control_packets, (unsigned long long)failover.control_bytes,
            (unsigned long long)failover.queue_max, (unsigned long long)failover.queue_age_max, failover.cpu_max, failover.cpu_samples != 0,
            (unsigned long long)failover.loop_late_max);
        debug_set_log_hook(NULL);
        failover.active = 0;
        for (unsigned i = 0; i < 5; i++) CHECK(etcp_bind(nodes[i], ETCP_ID_STREAM, failover.handlers[i]) == 0);
        /* После замеров доставляем оставшиеся INFO, чтобы снять их обязательства перед закрытием выдач. */
        while (failover.held) {
            struct failover_packet *packet = failover.held; failover.held = packet->next;
            if (packet->entry->dgram[2] == STREAM_CMD_INFO_RESP) {
                DEBUG_INFO(DEBUG_CATEGORY_STREAM, "rescue teardown delivers delayed INFO scenario=%u peer=%u nonce=%llx",
                    scenario, packet->peer, (unsigned long long)stream_read_be(packet->entry->dgram + STREAM_WIRE_HEADER, 8));
                failover.handlers[4](packet->conn, packet->entry);
            } else {
                queue_dgram_free(packet->entry); queue_entry_free(packet->entry);
            }
            u_free(packet);
        }
        stream_close(viewer); stream_set_event_callback(nodes[4], NULL, NULL);
        for (unsigned i = 0; i < 3; i++) {
            for (unsigned sub = 0; sub < 2; sub++) stream_receive_close(up[i][sub]);
            stream_close(relays[i]);
        }
        stream_close(failover.publisher);
        /* В сценарии 3 запросы теряются после доставки ETCP. Освобождаем их кредиты сменой эпохи между сценариями. */
        unsigned reset_peers = 0;
        if (scenario == 3) for (unsigned i = 1; i <= 3; i++) {
            struct stream_peer *peer = peer_find(stream_network_get(nodes[4]), stream_address_read(nodes[i]->my_ed25519_pubkey));
            if (!peer || !peer->request_count) continue;
            struct ETCP_CONN *conn = node_conn_direct_get_conn(peer->ncd); CHECK(conn);
            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "rescue fixture resets dropped INFO credits peer=%u pending=%u", i, peer->request_count);
            reset_peers |= 1u << i;
            etcp_conn_reinit(conn, "test rescue dropped INFO requests");
        }
        wait_stream_teardown(&failover.descriptor, 5, reset_peers);
    }
}
