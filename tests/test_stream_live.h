/* Медийный live lag на настоящем транспорте: часы автора, узкая полоса и явный переход на полный KEY. */
static etcp_recv_fn live_receive;
static uint64_t live_epoch, live_head;
static unsigned live_history_packets;

/* Проверяем отдельно DATA старой истории: INFO сам по себе не доказывает перенос PTS в DATA. */
static void live_history_receive(struct ETCP_CONN *conn, struct ll_entry *entry) {
    struct stream_message message;
    if (stream_fixture_parse(entry->dgram, entry->len, &message) == STREAM_OK
        && message.descriptor.source_epoch == live_epoch && message.command == STREAM_CMD_DATA) {
        struct stream_path path; size_t used; struct stream_frame_timing timing;
        CHECK(stream_path_parse(message.payload, message.length, &path, &used) == STREAM_OK);
        CHECK(path.count == 2 && stream_address_equal(path.nodes[0], stream_address_read(nodes[0]->my_ed25519_pubkey)) && stream_address_equal(path.nodes[1], stream_address_read(nodes[3]->my_ed25519_pubkey)));
        CHECK(stream_read_be(message.payload + used, STREAM_DATA_HEAD_SIZE) == live_head);
        used += STREAM_DATA_HEAD_SIZE;
        CHECK(stream_frame_timing_read(message.payload + used, message.length - used, &timing) == STREAM_OK);
        if (timing.valid && timing.video && timing.last_pts < live_head) live_history_packets++;
    }
    live_receive(conn, entry);
}

/* Два сетевых перехода: relay уже знает head, но выдаёт историю с seq0 и более старой frame PTS. */
static void test_live_head(void) {
    struct recovery_test r = {0}; struct STREAM_HANDLE *relay = NULL, *reader = NULL;
    struct STREAM_RECEIVE *upstream[8] = {0}, *downstream = NULL;
    int previous_cpu = simulated_cpu; simulated_cpu = 30;
    CHECK(stream_publish_open(nodes[0], GROUP1, NULL, &r.publisher) == STREAM_OK);
    CHECK(stream_media_enable(r.publisher, 0) == STREAM_OK);
    struct stream_info info; CHECK(stream_get_info(r.publisher, &info) == STREAM_OK); r.descriptor = info.descriptor;
    wait_state(3, 0, GROUP1, r.descriptor.source_epoch, 0); wait_state(2, 0, GROUP1, r.descriptor.source_epoch, 0);
    CHECK(stream_subscribe_manual_open(nodes[3], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &relay) == STREAM_OK);
    CHECK(stream_media_enable(relay, 0) == STREAM_OK);
    for (unsigned sub = 0; sub < 8; sub++) CHECK(stream_receive_open(relay, stream_address_read(nodes[0]->my_ed25519_pubkey), sub, 0, &upstream[sub]) == STREAM_OK);
    r.origin_us = get_time_us(); r.next_frame_us = r.origin_us;
    recovery_run(&r, 2200); r.paused = 1; recovery_run(&r, 500);
    struct stream_local source, local;
    CHECK(stream_local_get(nodes[0], &r.descriptor, &source) == STREAM_OK && source.live_head_us != UINT64_MAX);
    CHECK(stream_local_get(nodes[3], &r.descriptor, &local) == STREAM_OK && local.live_head_us == source.live_head_us);
    live_epoch = r.descriptor.source_epoch; live_head = source.live_head_us; live_history_packets = 0;
    stream_local_head(nodes[3], &r.descriptor, 0); stream_local_head(nodes[3], &r.descriptor, UINT64_MAX);
    struct stream_descriptor wrong = r.descriptor; wrong.source_epoch++;
    stream_local_head(nodes[3], &wrong, live_head + 1000000);
    CHECK(stream_local_get(nodes[3], &r.descriptor, &local) == STREAM_OK && local.live_head_us == live_head);
    CHECK(stream_subscribe_manual_open(nodes[2], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &reader) == STREAM_OK);
    live_receive = nodes[2]->api_bindings.callbacks[ETCP_ID_STREAM]; CHECK(etcp_bind(nodes[2], ETCP_ID_STREAM, live_history_receive) == 0);
    CHECK(stream_receive_open(reader, stream_address_read(nodes[3]->my_ed25519_pubkey), 0, 0, &downstream) == STREAM_OK);
    recovery_run(&r, 1500);
    struct stream_receive_info received; CHECK(stream_receive_get_info(downstream, &received) == STREAM_OK);
    CHECK(received.live_head_us == live_head && received.media_pts_us == live_head && received.key_valid);
    CHECK(received.key_pts_us <= received.media_pts_us && live_history_packets > 10);
    CHECK(stream_local_get(nodes[2], &r.descriptor, &local) == STREAM_OK && local.live_head_us == live_head);
    CHECK(etcp_bind(nodes[2], ETCP_ID_STREAM, live_receive) == 0);
    stream_receive_close(downstream); stream_close(reader);
    for (unsigned sub = 0; sub < 8; sub++) stream_receive_close(upstream[sub]);
    stream_close(relay); stream_close(r.publisher); test_clock_run_for(ua, 10000); simulated_cpu = previous_cpu;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] live PTS through relay: DATA history=%u INFO head=%llu monotonic/epoch isolation",
        live_history_packets, (unsigned long long)live_head);
}

/* После двух переносов воспроизводим позднюю DATA/старый SEEK и повтор текущего SEEK через настоящий dispatch. */
static void test_live_seek_replays(struct recovery_test *r) {
    struct stream_peer *sender = peer_find(stream_network_get(nodes[0]), stream_address_read(nodes[1]->my_ed25519_pubkey));
    struct stream_peer *receiver = peer_find(stream_network_get(nodes[1]), stream_address_read(nodes[0]->my_ed25519_pubkey));
    CHECK(sender && receiver);
    struct STREAM_RECEIVE *flow = NULL, *receive = NULL;
    for (struct STREAM_RECEIVE *f = sender->flows; f; f = f->next)
        if (!f->outgoing && !f->subblock && f->descriptor.source_epoch == r->descriptor.source_epoch) flow = f;
    for (struct STREAM_RECEIVE *f = receiver->flows; f; f = f->next)
        if (f->outgoing && !f->info.probe_only && !f->subblock && f->descriptor.source_epoch == r->descriptor.source_epoch) receive = f;
    CHECK(flow && receive && flow->info.handle == receive->info.handle && flow->info.handle > 8);
    uint64_t sent_next = flow->info.next, received_next = receive->info.next;
    uint64_t queued = sender->load.queued.bytes, blocks = receive->info.blocks;
    unsigned slots = sender->network->regular_used, flows = sender->network->flow_count;
    uint8_t wire[STREAM_WIRE_HEADER + 1 + 8 + STREAM_DATA_HEAD_SIZE + STREAM_FRAME_TIMING_SIZE + STREAM_BLOCK_MAX_SIZE];
    struct stream_message m = {.descriptor = r->descriptor, .subblock = 0, .command = STREAM_CMD_SEEK,
        .handle = flow->info.handle, .sender_epoch = receiver->own_epoch, .receiver_epoch = sender->own_epoch};
    stream_wire_header(wire, &m); stream_write_be(wire + STREAM_WIRE_HEADER, 1, 8);
    stream_write_be(wire + STREAM_WIRE_HEADER + 8, flow->requested, 8);
    dispatch(sender->network, stream_address_read(nodes[1]->my_ed25519_pubkey), wire, STREAM_WIRE_HEADER + 16, 0);
    CHECK(flow->info.next == sent_next && sender->load.queued.bytes == queued);
    CHECK(sender->network->regular_used == slots && sender->network->flow_count == flows);
    m.handle = 8; stream_wire_header(wire, &m);
    dispatch(sender->network, stream_address_read(nodes[1]->my_ed25519_pubkey), wire, STREAM_WIRE_HEADER + 16, 0);
    CHECK(flow->info.next == sent_next && sender->load.queued.bytes == queued);
    struct stream_local local; CHECK(stream_local_get(nodes[0], &r->descriptor, &local) == STREAM_OK);
    const uint8_t *part; size_t length; uint64_t actual; struct stream_frame_timing timing;
    CHECK(stream_buffer_read(local.buffer, 0, 0, &part, &length, &actual) == STREAM_OK);
    CHECK(stream_buffer_get_timing(local.buffer, 0, &timing) == STREAM_OK);
    m.command = STREAM_CMD_DATA; m.handle = 1; m.sender_epoch = sender->own_epoch; m.receiver_epoch = receiver->own_epoch;
    stream_wire_header(wire, &m);
    struct stream_path path = {.count = 1, .nodes = {nodes[0]->node_id}};
    size_t used = STREAM_WIRE_HEADER + stream_path_write(wire + STREAM_WIRE_HEADER, &path);
    stream_write_be(wire + used, local.live_head_us, STREAM_DATA_HEAD_SIZE); used += STREAM_DATA_HEAD_SIZE;
    stream_frame_timing_write(wire + used, &timing); used += STREAM_FRAME_TIMING_SIZE;
    memcpy(wire + used, part, length);
    dispatch(receiver->network, stream_address_read(nodes[0]->my_ed25519_pubkey), wire, used + length, 0);
    CHECK(receive->info.next == received_next && receive->info.blocks == blocks && receive->info.state == STREAM_RECEIVE_ACTIVE);
    CHECK(stream_receive_seek(receive, receive->info.start) == STREAM_ERR_STATE && receive->info.next == received_next);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] SEEK repeat preserves cursor/ledger/slots; stale SEEK/DATA and backward request rejected");
}

static void test_live(void) {
    test_live_head();
    struct recovery_test r = {0}; recovery = &r;
    int previous_cpu = simulated_cpu; simulated_cpu = 30;
    CHECK(stream_publish_open(nodes[0], GROUP1, NULL, &r.publisher) == STREAM_OK);
    CHECK(stream_media_enable(r.publisher, 0) == STREAM_OK);
    struct stream_info info; CHECK(stream_get_info(r.publisher, &info) == STREAM_OK); r.descriptor = info.descriptor;
    wait_state(1, 0, GROUP1, r.descriptor.source_epoch, 0);
    r.origin_us = get_time_us(); r.next_frame_us = r.origin_us;
    CHECK(stream_media_open(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), r.descriptor.source_epoch,
        STREAM_MEDIA_TRACK_VIDEO, &r.viewer) == STREAM_OK);
    r.receive = nodes[1]->api_bindings.callbacks[ETCP_ID_STREAM];
    CHECK(etcp_bind(nodes[1], ETCP_ID_STREAM, recovery_receive) == 0);
    recovery_run(&r, 3000); CHECK(r.frames >= 50);
    unsigned accepts = r.accepts;
    r.paused = 1; recovery_run(&r, 15000);
    struct stream_selection_info selection; CHECK(stream_selection_get_info(r.viewer, &selection) == STREAM_OK);
    int pause_ok = selection.live_lag_valid && selection.live_lag_ms < 100 && !selection.live_jumps && !r.media_gaps
        && selection.restarts >= 2 && selection.restarts <= 3 && r.accepts == accepts && selection.subflows == 8;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "author pause15s: content_lag_ms=%llu valid=%d jumps=%llu searches=%llu new_ACCEPT=%u GAP=%u pass=%d",
        (unsigned long long)selection.live_lag_ms, selection.live_lag_valid, (unsigned long long)selection.live_jumps,
        (unsigned long long)selection.restarts, r.accepts - accepts, r.media_gaps, pause_ok);
    r.paused = 0; recovery_run(&r, 1000);
    /* Семь частей уже имеют свежий KEY, одна потеряна после отправки. Новый anchor не зависит от первой быстрой DATA. */
    accepts = r.accepts; r.hold_part = 1; recovery_run(&r, 12000);
    CHECK(stream_selection_get_info(r.viewer, &selection) == STREAM_OK && selection.live_jumps == 1);
    CHECK(r.accepts == accepts + 1 && r.media_gaps == 1); // SEEK только для потерянной части, семь остальных сохраняются.
    r.hold_part = 0;
    for (struct recovery_pending *pending = r.head; pending; pending = pending->next) pending->due_ms = get_time_tb() / 10;
    recovery_run(&r, 1000);
    CHECK(!r.head && stream_selection_get_info(r.viewer, &selection) == STREAM_OK && selection.live_lag_ms < 1000);
    CHECK(selection.live_jumps == 1 && r.media_gaps == 1);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] mixed KEY: seven retained parts, one SEEK after transport loss, old DATA ignored");
    uint64_t before_jumps = selection.live_jumps;
    struct ETCP_CONN *conn = instance_find_conn(nodes[0], nodes[1]->node_id); CHECK(conn && conn->links_up);
    struct ll_queue *queue = conn->send_input_q;
    queue_callback_fn callback = queue->callback; void *arg = queue->callback_arg; CHECK(callback);
    queue_set_callback(queue, NULL, NULL);
    r.pump_queue = queue; r.pump_callback = callback; r.pump_arg = arg; r.pump_Bps = 60000;
    recovery_run(&r, 12000); simulated_cpu = 81; recovery_run(&r, 14000);
    CHECK(stream_selection_get_info(r.viewer, &selection) == STREAM_OK);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "live narrow path frames=%llu content_lag_ms=%llu jumps=%llu gaps=%u waiting_KEY=%d",
        (unsigned long long)r.frames, (unsigned long long)selection.live_lag_ms,
        (unsigned long long)selection.live_jumps, r.media_gaps, selection.live_waiting_key);
    CHECK(selection.live_jumps > before_jumps && r.media_gaps == selection.live_jumps && !selection.live_waiting_key);
    struct stream_network *budget = stream_network_get(nodes[0]);
    CHECK(budget && budget->regular_used == 8 && budget->global_pressure);
    test_live_seek_replays(&r);
    simulated_cpu = 30; recovery_run(&r, 1000);
    /* Поздняя новая ветвь остаётся доступной после освобождения CPU; она не нужна для самого SEEK. */
    struct STREAM_HANDLE *relay = NULL; struct STREAM_RECEIVE *upstream[8] = {0};
    struct stream_media_recovery position = recovery_position(&r);
    wait_state(3, 0, GROUP1, r.descriptor.source_epoch, 0);
    CHECK(stream_subscribe_manual_open(nodes[3], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &relay) == STREAM_OK);
    CHECK(stream_media_enable(relay, 0) == STREAM_OK);
    for (unsigned sub = 0; sub < 8; sub++)
        CHECK(stream_receive_open(relay, stream_address_read(nodes[0]->my_ed25519_pubkey), sub, position.key_seq, &upstream[sub]) == STREAM_OK);
    recovery_run(&r, 7000);
    CHECK(stream_selection_get_info(r.viewer, &selection) == STREAM_OK);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "live fresh relay frames=%llu content_lag_ms=%llu jumps=%llu gaps=%u waiting_KEY=%d",
        (unsigned long long)r.frames, (unsigned long long)selection.live_lag_ms,
        (unsigned long long)selection.live_jumps, r.media_gaps, selection.live_waiting_key);
    CHECK(selection.live_jumps && r.media_gaps);
    r.pump_queue = NULL; queue_set_callback(queue, callback, arg); queue_resume_callback(queue);
    recovery_run(&r, 4000);
    CHECK(stream_selection_get_info(r.viewer, &selection) == STREAM_OK);
    CHECK(selection.live_lag_valid && selection.live_lag_ms < 1000 && !selection.live_waiting_key);
    CHECK(get_time_tb() / 10 - r.last_frame_ms < 300);
    CHECK(etcp_bind(nodes[1], ETCP_ID_STREAM, r.receive) == 0);
    for (unsigned sub = 0; sub < 8; sub++) stream_receive_close(upstream[sub]);
    stream_close(relay);
    stream_close(r.viewer); stream_close(r.publisher); test_clock_run_for(ua, 10000);
    simulated_cpu = previous_cpu; recovery = NULL;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] live10s: sustained60KBps KEY jump, monotonic verified frames, capacity restored");
    CHECK(pause_ok);
}
