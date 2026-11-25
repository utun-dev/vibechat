/* Общий реальный UDP/BGP fixture: два узла, две группы, подмена и отзыв на принимающей стороне. */
#define main group_isolation_reference_main
#include "test_chat_group_isolation.c"
#undef main
#include "call/call_priv.h"
#include "call/call_proto.h"
#include "call/call_video_transport.h"
/* Смотрим состояния приглашений/выдач и управляем моментом отправки, сохраняя настоящие обработчики. */
#include "../src/chat/chat_devgroup_invite.c"
#include "../src/stream/stream_network.c"

static unsigned api_audio, api_video, api_caps[2], api_ready;
static int api_video_room;
static struct ll_entry* api_held_keys;
static int api_hold_keys;
int __real_etcp_route_send_key(struct UTUN_INSTANCE*, uint64_t, const uint8_t*, struct ll_entry*, int, int);
int __wrap_etcp_route_send_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], struct ll_entry* e, int ttl, int crypto) {
    if (api_hold_keys && e->len >= 18 && e->dgram[0] == ETCP_RT_ID_DEVGROUP && e->dgram[1] == DG_KEYS) {
        if (!api_held_keys) api_held_keys = e;
        else { queue_dgram_free(e); queue_entry_free(e); }
        return 0;
    }
    return __real_etcp_route_send_key(inst, group, peer, e, ttl, crypto);
}

static int api_admit(uint64_t group, uint64_t node, int allowed) {
    char sql[208]; snprintf(sql, sizeof(sql), "UPDATE peers_%s SET admitted=%d WHERE node_id=%lld AND record_state=0",
        topo_groups_channel(fixture.nodes[0]->topo_groups, group), allowed, (long long)node);
    REQUIRE(access_sql(fixture.nodes[0], sql) == SQLITE_OK); return 0;
}

static int api_restore(void) {
    for (unsigned i = 0; i < 2; i++) for (unsigned g = 0; g < 2; g++) {
        char sql[144]; snprintf(sql, sizeof(sql), "UPDATE peers_%s SET admitted=1 WHERE record_state=0", fixture.groups[i][g]->channel_id);
        REQUIRE(access_sql(fixture.nodes[i], sql) == SQLITE_OK);
        REQUIRE(!topo_group_new_conn(fixture.groups[i][g], instance_find_conn(fixture.nodes[i], fixture.nodes[1 - i]->node_id)));
    }
    uint64_t deadline = get_time_tb() + 50000;
    for (;;) {
        unsigned count = 0;
        for (unsigned i = 0; i < 2; i++) for (unsigned g = 0; g < 2; g++)
            count += topo_group_peer_ready(fixture.groups[i][g], fixture.nodes[1 - i]->node_id);
        if (count == 4) return 0;
        REQUIRE(get_time_tb() < deadline); uasync_poll(fixture.ua, 1);
    }
}

static int api_saved(uint64_t group, uint8_t service, struct SVC_ROUTE_HDR* saved) {
    struct ETCP_ROUTER_CONN* r = etcp_router_conn_get(fixture.nodes[0], group, fixture.nodes[1]->node_id, service);
    REQUIRE(r && r->peer_reset_id);
    *saved = (struct SVC_ROUTE_HDR){.cmd = ETCP_ID_SVC_ROUTE, .svc_id = service, .group_id = group,
        .src_node_id = fixture.nodes[1]->node_id, .dst_node_id = fixture.nodes[0]->node_id,
        .reset_id = r->peer_reset_id, .peer_reset_id = r->reset_id, .seq = r->rx_seq};
    return 0;
}

static int api_send(uint64_t group, uint8_t service, const void* body, size_t size) {
    struct ll_entry* e = ll_alloc_lldgram(size + 1); REQUIRE(e);
    e->dgram[0] = service; memcpy(e->dgram + 1, body, size); e->len = size + 1;
    REQUIRE(!etcp_route_send(fixture.nodes[1], group, fixture.nodes[0]->node_id, e, 0,
        service == 250 ? 0 : ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT));
    for (unsigned i = 0; i < 100; i++) uasync_poll(fixture.ua, 1);
    return 0;
}

static unsigned api_key_packets[2], api_unprotected_packets;
static void api_key_receive(struct ETCP_CONN* conn, struct ll_entry* e) {
    (void)conn;
    uint64_t group; memcpy(&group, e->dgram + ROUTER_SVC_GROUP_OFF, 8);
    if (e->len > ROUTER_SVC_PAYLOAD_OFF && group >= 111 && group <= 112) {
        api_key_packets[group - 111]++;
        if (e->dgram[ROUTER_SVC_FLAGS_OFF] != (ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT)) api_unprotected_packets++;
    }
    queue_dgram_free(e); queue_entry_free(e);
}

/* Поддельный общий ключ не меняет авторство пакетов/истории в двух независимых группах. */
static int api_group_keys(void) {
    struct UTUN_INSTANCE* server = fixture.nodes[0]; struct UTUN_INSTANCE* client = fixture.nodes[1];
    const uint8_t service = 250, payload = 42;
    for (unsigned i = 0; i < 2; i++) {
        REQUIRE(!etcp_router_bind(fixture.nodes[i], service, api_key_receive));
        REQUIRE(!etcp_router_set_chat_policy(fixture.nodes[i], service, ROUTE_CRYPTO_SIGN,
            topo_group_chat_key_allowed));
    }
    REQUIRE(!api_send(111, service, &payload, 1) && !api_send(112, service, &payload, 1));
    REQUIRE(api_key_packets[0] == 1 && api_key_packets[1] == 1);
    check("router protects CHAT even when service requests plaintext and only requires a signature", !api_unprotected_packets);
    struct TOPO_NODE* known = topo_node_registry_find(server->topo_groups, client->node_id); REQUIRE(known);
    struct TOPO_NODE forged = *known; forged.timestamp++;
    memcpy(forged.ed25519_public_key, server->my_ed25519_pubkey, 32);
    uint8_t body[TOPO_SIG_MSG_MAX_SIZE]; int bytes = topo_node_build_sig_msg(&forged, body, sizeof(body));
    REQUIRE(bytes > 0 && sc_ed25519_sign(server->my_ed25519_privkey, body, bytes, forged.x25519_self_sig) == SC_OK);
    REQUIRE(topo_node_verify(&forged) < 0); /* Подпись верна, привязка Ed25519 к транспорту нарушена. */
    uint8_t wire[TOPO_NODE_WIRE_MAX_SIZE + sizeof(struct TOPOMSG_HEADER)] = {0};
    memcpy(((struct TOPOMSG_HEADER*)wire)->group_key, topo_group_wire_id(fixture.groups[0][1]), 32);
    struct TOPO_GROUP_NODE announcement = {0};
    bytes = topo_node_serialize(&forged, &announcement, 0,
        wire + sizeof(struct TOPOMSG_HEADER), sizeof(wire) - sizeof(struct TOPOMSG_HEADER), 0);
    REQUIRE(bytes > 0);
    struct ETCP_CONN* from = instance_find_conn(server, client->node_id); REQUIRE(from);
    check("NODEINFO rejects correctly self-signed replacement of member Ed25519",
        topo_group_process_nodeinfo(fixture.groups[0][1], from, wire, sizeof(struct TOPOMSG_HEADER) + bytes) < 0
        && !memcmp(known->ed25519_public_key, client->my_ed25519_pubkey, 32));
    memcpy(known->ed25519_public_key, server->my_ed25519_pubkey, 32);
    REQUIRE(!api_send(111, service, &payload, 1) && !api_send(112, service, &payload, 1));
    check("both CHAT groups authenticate valid packets independently of poisoned BGP key",
        api_key_packets[0] == 2 && api_key_packets[1] == 2);
    struct SVC_ROUTE_HDR saved; REQUIRE(!api_saved(112, service, &saved));
    uint8_t private[32]; memcpy(private, client->my_ed25519_privkey, 32);
    memcpy(client->my_ed25519_privkey, server->my_ed25519_privkey, 32);
    int sent = send_routed(&saved, &payload, 1, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT);
    memcpy(client->my_ed25519_privkey, private, 32); REQUIRE(!sent);
    check("CHAT rejects signature matching poisoned BGP key but not its member", api_key_packets[1] == 2);
    struct DB_SYNC db = {.inst = server, .db = server->topo_sqlite_db};
    struct DB_SYNC_INSTANCE history = {.db_sync = &db, .group_id = 112, .ns = GROUP_B, .table_name = "key-test"};
    struct db_record record = {.timestamp = 100, .author = client->node_id, .data = "history", .data_len = 7,
        .sig_len = 64, .revision = {.ver = 1, .nonce = {1}}};
    REQUIRE(!group_policy_message_generation(server->topo_sqlite_db, GROUP_B, record.author, record.revision.generation));
    memcpy(record.accepted_key, client->my_ed25519_pubkey, 32);
    REQUIRE(!db_sync_message_id(record.timestamp, record.accepted_key, &record.revision, record.message_id));
    REQUIRE(!db_sync_sign_message(server->topo_sqlite_db, topo_groups_channel(server->topo_groups, 112), record.timestamp, record.accepted_key, &record.revision,
        record.data, record.data_len, private, record.signature));
    struct db_store store = db_store_for_instance(&history);
    check("history ignores poisoned BGP author key", db_verify_author_sig(&store, &record) == DB_CHECK_VALID);
    REQUIRE(!api_admit(112, client->node_id, 0));
    uint8_t key[32];
    check("revoked member certificate still verifies history but cannot authorize new operations",
        !topo_node_sqlite_member_keys(server->topo_sqlite_db, GROUP_B, client->node_id, 0, NULL, key)
        && topo_node_sqlite_member_keys(server->topo_sqlite_db, GROUP_B, client->node_id, 1, NULL, key) == TOPO_SQLITE_LOOKUP_MISSING
        && db_verify_author_sig(&store, &record) == DB_CHECK_VALID && db_verify_incoming(&store, &record) != DB_CHECK_VALID);
    REQUIRE(!api_admit(112, client->node_id, 1));
    memcpy(known->ed25519_public_key, client->my_ed25519_pubkey, 32);
    for (unsigned i = 0; i < 2; i++) REQUIRE(!etcp_router_unbind(fixture.nodes[i], service));
    return 0;
}

static void api_audio_received(struct UTUN_INSTANCE* inst, uint64_t id, const uint8_t* data, int size, void* arg) {
    (void)inst; (void)id; (void)data; (void)size; (void)arg; api_audio++;
}
static void api_video_received(void* arg, const struct call_video_frame* frame) { (void)arg; (void)frame; api_video++; }
static int api_video_ready(void* arg, uint32_t bytes) { (void)arg; (void)bytes; api_ready++; return api_video_room; }
static void api_capabilities(void* arg, const struct call_video_caps* caps) { (void)caps; api_caps[(uintptr_t)arg]++; }

static int api_call_open(uint64_t group, uint64_t id) {
    const char* channel = topo_groups_channel(fixture.nodes[1]->topo_groups, group);
    call_start(fixture.nodes[1], channel, fixture.nodes[0]->my_ed25519_pubkey, id);
    uint64_t deadline = get_time_tb() + 50000;
    for (;;) {
        struct call_session* s = call_session_find(fixture.nodes[0]->call, id);
        if (s && s->state == CALL_ST_RINGING) break;
        REQUIRE(get_time_tb() < deadline); uasync_poll(fixture.ua, 1);
    }
    call_accept(fixture.nodes[0], id);
    for (;;) {
        struct call_session* a = call_session_find(fixture.nodes[0]->call, id);
        struct call_session* b = call_session_find(fixture.nodes[1]->call, id);
        if (a && b && a->state == CALL_ST_ACTIVE && b->state == CALL_ST_ACTIVE) return 0;
        REQUIRE(get_time_tb() < deadline); uasync_poll(fixture.ua, 1);
    }
}

static int api_calls(void) {
    struct UTUN_INSTANCE* server = fixture.nodes[0]; struct UTUN_INSTANCE* client = fixture.nodes[1];
    for (unsigned i = 0; i < 2; i++) REQUIRE(!call_init(fixture.nodes[i]));
    REQUIRE(!api_call_open(112, 10001));
    struct call_session* s = call_session_find(server->call, 10001); REQUIRE(s);
    call_set_media_recv_cb(server, api_audio_received, NULL);
    const uint8_t opus[] = {0xf8, 0xff, 0xfe};
    for (unsigned i = 0; i < 4; i++) REQUIRE(!call_send_media(client, s->call_id, opus, sizeof(opus), NULL));
    uint64_t deadline = get_time_tb() + 30000;
    while (api_audio < 4 && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    REQUIRE(api_audio == 4);
    struct call_video_caps caps = {0};
    caps.encode[0] = caps.decode[0] = (struct call_video_limit){1280, 720, 30, 1280 * 720 * 30, 4000000, CALL_VIDEO_BASELINE, 31};
    const struct call_video_ops ops = {.capabilities = api_capabilities, .receive_ready = api_video_ready, .frame = api_video_received};
    for (unsigned i = 0; i < 2; i++) REQUIRE(!call_video_configure(fixture.nodes[i], s->call_id, &caps, &ops, (void*)(uintptr_t)i));
    while ((!api_caps[0] || !api_caps[1]) && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    REQUIRE(api_caps[0] && api_caps[1]);
    struct call_video_frame frame = {.call_id = s->call_id, .frame_id = 1, .pts_us = 0,
        .format = {1, 640, 480, 30, 0, CALL_VIDEO_H264, 31, 4000000, CALL_VIDEO_BASELINE},
        .size = 3, .length = 3, .flags = CALL_VIDEO_KEY, .data = opus};
    uint8_t video[128]; size_t video_size;
    REQUIRE(!call_video_fragment_encode(&frame, video, sizeof(video), &video_size));
    REQUIRE(!api_send(112, ETCP_RT_ID_CALL_VIDEO, video, video_size));
    check("CALL_VIDEO waits for receiver before revoke", api_ready && !api_video);
    struct call_pkt_hangup hangup = {CALL_SUBCMD_HANGUP, s->call_id, CALL_REASON_REMOTE_HANGUP};
    REQUIRE(!api_send(111, ETCP_RT_ID_CALL, &hangup, sizeof(hangup)));
    check("CALL rejects valid peer in wrong delivery group", s->state == CALL_ST_ACTIVE);
    const uint8_t services[] = {ETCP_RT_ID_CALL, ETCP_RT_ID_CALL_AUDIO, ETCP_RT_ID_CALL_VIDEO, ETCP_RT_ID_CALL_VIDEO_CONTROL};
    struct SVC_ROUTE_HDR saved[4];
    for (unsigned i = 0; i < 4; i++) REQUIRE(!api_saved(112, services[i], &saved[i]));
    REQUIRE(!send_routed(&saved[0], &hangup, sizeof(hangup), 0));
    check("CALL rejects unsigned terminal command", s->state == CALL_ST_ACTIVE);
    REQUIRE(!api_admit(112, client->node_id, 0));
    unsigned ready = api_ready, audio = api_audio, capabilities = api_caps[0];
    uint8_t control[17] = {2}; memcpy(control + 1, &s->call_id, 8);
    struct call_pkt_media media = {.subcmd = CALL_SUBCMD_MEDIA, .call_id = s->call_id, .track_id = CALL_TRACK_AUDIO, .count = 1};
    struct call_pkt_audio_frame part = {.seq = 20, .capture_pts_us = INT64_MIN, .length = 3};
    uint8_t sound[sizeof(media) + sizeof(part) + 3];
    memcpy(sound, &media, sizeof(media)); memcpy(sound + sizeof(media), &part, sizeof(part));
    memcpy(sound + sizeof(media) + sizeof(part), opus, 3);
    const void* payloads[] = {&hangup, sound, video, control};
    size_t sizes[] = {sizeof(hangup), sizeof(sound), video_size, sizeof(control)};
    for (unsigned i = 0; i < 4; i++) REQUIRE(!send_routed(&saved[i], payloads[i], sizes[i], ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT));
    check("all four CALL endpoints deny revoked signed peer before callbacks", s->state == CALL_ST_ACTIVE
        && api_audio == audio && api_caps[0] == capabilities && !api_video && api_ready == ready);
    REQUIRE(!call_send_media(server, s->call_id, opus, sizeof(opus), NULL));
    REQUIRE(s->tx_q && s->tx_q->count && s->video);
    topo_group_revalidate_members(fixture.groups[0][1]);
    check("CALL revoke stops session, media queues, timers and CM immediately", s->state == CALL_ST_ENDED && !call_busy(server)
        && !s->tx_q && !s->video && !s->cm_handle && !s->audio_batch_timer && !s->invite_timer && !s->end_subcmd);
    check("CALL rejects local audio/video after revoke", call_send_media(server, s->call_id, opus, sizeof(opus), NULL) < 0
        && call_video_send_frame(server, &frame) < 0);
    for (unsigned i = 0; i < 4; i++) check("CALL revoked logical channel cannot reopen", !etcp_router_conn_get(server, 112, client->node_id, services[i]));
    call_destroy(client); REQUIRE(!call_init(client));
    REQUIRE(!api_call_open(111, 10002));
    check("CALL works through neighbouring group on same transport", call_busy(server) && fixture.transport->links_up);
    REQUIRE(!api_admit(111, server->node_id, 0)); topo_group_revalidate_members(fixture.groups[0][0]);
    check("CALL self revoke also ends active call", !call_busy(server));
    for (unsigned i = 0; i < 2; i++) call_destroy(fixture.nodes[i]);
    return api_restore();
}

static int api_device_wait(const char* id, unsigned node, unsigned state, struct dg_invite** out) {
    uint8_t binary[16]; REQUIRE(!dg_id(id, binary)); uint64_t deadline = get_time_tb() + 50000;
    for (;;) {
        struct dg_invite* p = dg_find(dg_ctx(fixture.nodes[node]), binary);
        if (p && p->state == state) { if (out) *out = p; return 0; }
        REQUIRE(get_time_tb() < deadline); uasync_poll(fixture.ua, 1);
    }
}

static int api_device_confirm(const char* id, const char* pin) {
    struct dg_invite* p;
    REQUIRE(!api_device_wait(id, 0, DG_WAIT_PIN, &p));
    REQUIRE(!chat_devgroup_accept(fixture.nodes[0], id, pin));
    REQUIRE(!api_device_wait(id, 0, DG_WAIT_RETURN, &p) && !api_device_wait(id, 1, DG_WAIT_RETURN, NULL));
    REQUIRE(!chat_devgroup_accept(fixture.nodes[1], id, p->pin));
    return 0;
}

static int api_devices(void) {
    struct UTUN_INSTANCE* server = fixture.nodes[0]; struct UTUN_INSTANCE* client = fixture.nodes[1];
    for (unsigned i = 0; i < 2; i++) {
        char path[640]; snprintf(path, sizeof(path), "%s/node%u/chats.db", fixture.directory, i);
        REQUIRE(!db_sync_enable(fixture.nodes[i]) && !chat_core_init(fixture.nodes[i], path));
        REQUIRE(!chat_devgroup_init(fixture.nodes[i]));
    }
    char id[33], pin[5]; api_hold_keys = 1;
    REQUIRE(!chat_devgroup_invite(client, 112, server->my_ed25519_pubkey, id, pin) && !api_device_confirm(id, pin));
    struct dg_invite* pending;
    REQUIRE(!api_device_wait(id, 1, DG_KEYS_SENT, NULL) && !api_device_wait(id, 0, DG_WAIT_KEYS, &pending));
    REQUIRE(api_held_keys);
    struct SVC_ROUTE_HDR saved; REQUIRE(!api_saved(112, ETCP_RT_ID_DEVGROUP, &saved));
    const uint8_t* keys = api_held_keys->dgram + 1; size_t size = api_held_keys->len - 1;
    REQUIRE(!api_send(111, ETCP_RT_ID_DEVGROUP, keys, size));
    check("DEVGROUP rejects keys in another group with same invitation ID", pending->state == DG_WAIT_KEYS);
    struct SVC_ROUTE_HDR forged = saved; forged.src_node_id = server->node_id;
    REQUIRE(!send_routed(&forged, keys, size, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT));
    REQUIRE(!send_routed(&saved, keys, size, ROUTE_CRYPTO_SIGN));
    check("DEVGROUP rejects forged sender and unencrypted keys", pending->state == DG_WAIT_KEYS);
    REQUIRE(!api_admit(112, client->node_id, 0));
    REQUIRE(!send_routed(&saved, keys, size, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT));
    struct devgroup_record record; uint8_t ep[32], xp[32];
    check("DEVGROUP delayed keys cannot import after peer revoke", pending->state == DG_WAIT_KEYS
        && devgroup_load_local(server, &record, ep, xp) == 0);
    topo_group_revalidate_members(fixture.groups[0][1]);
    uint8_t zero[5] = {0};
    check("DEVGROUP revoke cancels both PIN phases and clears cached PINs", pending->state == DG_REJECTED
        && !memcmp(pending->pin, zero, 5) && !memcmp(pending->entered_pin, zero, 5)
        && !memcmp(pending->last_pin, zero, 4) && !memcmp(pending->last_return_pin, zero, 4)
        && chat_devgroup_accept(server, id, pin) < 0);
    queue_dgram_free(api_held_keys); queue_entry_free(api_held_keys); api_held_keys = NULL; api_hold_keys = 0;
    chat_devgroup_cancel_pending(client);
    REQUIRE(!chat_devgroup_invite(client, 111, server->my_ed25519_pubkey, id, pin) && !api_device_confirm(id, pin));
    REQUIRE(!api_device_wait(id, 0, DG_JOINED, NULL) && !api_device_wait(id, 1, DG_JOINED, NULL));
    check("DEVGROUP pairing works through neighbouring group", devgroup_load_local(server, &record, ep, xp) == 1
        && fixture.transport->links_up);
    REQUIRE(!api_restore());
    REQUIRE(!chat_devgroup_invite(client, 112, server->my_ed25519_pubkey, id, pin) && !api_device_wait(id, 0, DG_WAIT_PIN, &pending));
    REQUIRE(!api_admit(112, server->node_id, 0)); topo_group_revalidate_members(fixture.groups[0][1]);
    check("DEVGROUP self revoke cancels invitation while keeping imported identity", pending->state == DG_REJECTED
        && devgroup_load_local(server, &record, ep, xp) == 1);
    chat_devgroup_cancel_pending(client); REQUIRE(!api_restore());
    REQUIRE(!chat_devgroup_invite(client, 112, server->my_ed25519_pubkey, id, pin) && !api_device_wait(id, 0, DG_WAIT_PIN, &pending));
    struct TOPO_NODE* node = topo_node_registry_find(server->topo_groups, client->node_id); REQUIRE(node);
    node->ed25519_public_key[0] ^= 1; chat_devgroup_revalidate_members(server, 112); node->ed25519_public_key[0] ^= 1;
    check("DEVGROUP invitation ignores BGP key replacement", pending->state == DG_WAIT_PIN);
    char sql[256]; snprintf(sql, sizeof(sql), "UPDATE peers_" GROUP_B " SET ed25519_pubkey=zeroblob(32) WHERE node_id=%lld AND record_state=0",
        (long long)client->node_id);
    REQUIRE(access_sql(server, sql) == SQLITE_OK);
    chat_devgroup_revalidate_members(server, 112);
    check("DEVGROUP pinned member identity cannot change during invitation", pending->state == DG_REJECTED);
    sqlite3_stmt* restore = NULL;
    REQUIRE(sqlite3_prepare_v2(server->topo_sqlite_db,
        "UPDATE peers_" GROUP_B " SET ed25519_pubkey=? WHERE node_id=? AND record_state=0", -1, &restore, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_bind_blob(restore, 1, client->my_ed25519_pubkey, 32, SQLITE_STATIC) == SQLITE_OK
        && sqlite3_bind_int64(restore, 2, (sqlite3_int64)client->node_id) == SQLITE_OK && sqlite3_step(restore) == SQLITE_DONE);
    REQUIRE(sqlite3_finalize(restore) == SQLITE_OK);
    REQUIRE(!test_member_publish_access(server, GROUP_B));
    for (unsigned i = 0; i < 2; i++) chat_devgroup_destroy(fixture.nodes[i]);
    OPENSSL_cleanse(ep, sizeof(ep)); OPENSSL_cleanse(xp, sizeof(xp));
    return 0;
}

static unsigned api_stream_controls[2];
static etcp_recv_fn api_stream_client;
static void api_stream_observe(struct ETCP_CONN* conn, struct ll_entry* e) {
    struct stream_message m;
    if (stream_packet_parse(conn->instance, e->dgram, e->len, &m) == STREAM_OK && m.command == STREAM_CMD_REJECT
        && m.descriptor.group_id >= 111 && m.descriptor.group_id <= 112) api_stream_controls[m.descriptor.group_id - 111]++;
    api_stream_client(conn, e);
}

static int api_stream_packet(const struct stream_message* m, const void* payload, size_t size) {
    uint8_t wire[512]; REQUIRE(STREAM_WIRE_HEADER + size <= sizeof(wire));
    struct stream_message encoded = *m;
    REQUIRE(stream_packet_header(fixture.nodes[1], wire, &encoded) == STREAM_OK);
    if (size) memcpy(wire + STREAM_WIRE_HEADER, payload, size);
    return inject(wire, STREAM_WIRE_HEADER + size);
}

static int api_streams(void) {
    struct UTUN_INSTANCE* server = fixture.nodes[0]; struct UTUN_INSTANCE* client = fixture.nodes[1];
    /* Предыдущие сценарии запускали модули вручную; перейти к штатному полному lifecycle чата. */
    for (unsigned i = 0; i < 2; i++) { chat_core_destroy(fixture.nodes[i]); REQUIRE(!chat_service_start(fixture.nodes[i])); }
    fixture.receivers[ETCP_ID_STREAM] = server->api_bindings.callbacks[ETCP_ID_STREAM];
    server->api_bindings.callbacks[ETCP_ID_STREAM] = observe;
    api_stream_client = client->api_bindings.callbacks[ETCP_ID_STREAM]; client->api_bindings.callbacks[ETCP_ID_STREAM] = api_stream_observe;
    struct STREAM_HANDLE* pubs[2] = {0}; struct stream_info info[2];
    struct stream_parameters params = {.n = 1, .block_size = STREAM_RS_DEFAULT_SIZE};
    for (unsigned i = 0; i < 2; i++) {
        REQUIRE(stream_publish_open(server, 111 + i, &params, &pubs[i]) == STREAM_OK);
        REQUIRE(stream_get_info(pubs[i], &info[i]) == STREAM_OK);
    }
    struct stream_network* net = stream_network_get(server); REQUIRE(net);
    struct stream_message m = {.descriptor = info[1].descriptor, .command = STREAM_CMD_INFO_REQ, .sender_epoch = 12345};
    uint8_t probe[STREAM_INFO_REQUEST_SIZE] = {0}; stream_write_be(probe, 6789, 8); stream_write_be(probe + 9, UINT64_MAX, 8);
    REQUIRE(!api_stream_packet(&m, probe, sizeof(probe)));
    struct stream_peer* p = peer_find(net, stream_address_read(client->my_ed25519_pubkey)); REQUIRE(p && p->other_epoch == 12345);
    m.command = STREAM_CMD_SUBSCRIBE; m.handle = 1; m.receiver_epoch = p->own_epoch;
    uint8_t position[8] = {0}; REQUIRE(!api_stream_packet(&m, position, sizeof(position)));
    struct STREAM_RECEIVE* f = flow_find(p, &m, 0); REQUIRE(f && f->info.state == STREAM_RECEIVE_ACTIVE);
    check("STREAM valid peer opens a real raw issuance", net->flow_count == 1);
    m.command = STREAM_CMD_RENEW; uint64_t lease = f->deadline;
    for (unsigned attack = 0; attack < 5; attack++) {
        struct stream_message wrong = m;
        if (attack == 0) wrong.descriptor.group_id = 111;
        if (attack == 1) wrong.descriptor.source = stream_address_read(client->my_ed25519_pubkey);
        if (attack == 2) wrong.descriptor.source_epoch++;
        if (attack == 3) wrong.handle++;
        if (attack == 4) wrong.receiver_epoch++;
        REQUIRE(!api_stream_packet(&wrong, NULL, 0));
        f = flow_find(p, &m, 0);
        check("STREAM rejects substituted group/source/epoch/handle/session", f && f->deadline == lease && net->flow_count == 1);
    }
    REQUIRE(!api_stream_packet(&m, NULL, 0)); f = flow_find(p, &m, 0); REQUIRE(f && f->deadline > lease);
    struct stream_message relay = {.descriptor = m.descriptor, .command = STREAM_CMD_RELAY_REQUEST, .sender_epoch = 54321, .handle = 100};
    uint8_t relay_wire[STREAM_WIRE_HEADER];
    REQUIRE(stream_packet_header(client, relay_wire, &relay) == STREAM_OK);
    REQUIRE(!api_send(112, ETCP_RT_ID_STREAM, relay_wire, sizeof(relay_wire)));
    REQUIRE(net->relay_request_count == 1);
    struct SVC_ROUTE_HDR saved; REQUIRE(!api_saved(112, ETCP_RT_ID_STREAM, &saved));
    relay.handle++; stream_wire_header(relay_wire, &relay);
    REQUIRE(!send_routed(&saved, relay_wire, sizeof(relay_wire), 0));
    REQUIRE(!send_routed(&saved, relay_wire, sizeof(relay_wire), ROUTE_CRYPTO_SIGN));
    check("STREAM relay requires both signature and encryption", net->relay_request_count == 1);
    REQUIRE(!api_send(111, ETCP_RT_ID_STREAM, relay_wire, sizeof(relay_wire)));
    check("STREAM routed group must match payload group", net->relay_request_count == 1);
    REQUIRE(!api_admit(112, client->node_id, 0)); lease = f->deadline;
    const uint8_t commands[] = {STREAM_CMD_RENEW, STREAM_CMD_SUBSCRIBE, STREAM_CMD_INFO_REQ,
        STREAM_CMD_ACCEPT, STREAM_CMD_CLOSED, STREAM_CMD_GAP, STREAM_CMD_END, STREAM_CMD_DATA, STREAM_CMD_TELEMETRY,
        STREAM_CMD_OVERLOAD, STREAM_CMD_OVERLOAD_SELECT};
    for (unsigned i = 0; i < sizeof(commands); i++) {
        struct stream_message late = m; late.command = commands[i];
        uint8_t payload[64] = {STREAM_REASON_STOPPED}; size_t length = 1;
        if (late.command == STREAM_CMD_RENEW) length = 0;
        if (late.command == STREAM_CMD_SUBSCRIBE) { late.handle++; memset(payload, 0, 8); length = 8; }
        if (late.command == STREAM_CMD_INFO_REQ) {
            late.handle = late.receiver_epoch = 0; memcpy(payload, probe, sizeof(probe)); length = sizeof(probe);
        }
        if (late.command == STREAM_CMD_OVERLOAD) {
            late.handle = 0; memset(payload, 0, sizeof(payload)); stream_write_be(payload, 1, 8); stream_write_be(payload + 8, 1, 8);
            payload[16] = payload[17] = 1; stream_write_be(payload + 18, 1, 8); stream_write_be(payload + 26, 1, 8); length = 34;
        }
        REQUIRE(!api_stream_packet(&late, payload, length)); f = flow_find(p, &m, 0);
        check("STREAM late raw command cannot mutate revoked issuance", f && f->deadline == lease
            && f->info.state == STREAM_RECEIVE_ACTIVE && net->flow_count == 1 && !p->requested.id);
    }
    REQUIRE(!send_routed(&saved, relay_wire, sizeof(relay_wire), ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT));
    check("STREAM late signed relay cannot allocate after revoke", net->relay_request_count == 1);
    /* Сохранённый запрос разгрузки до отзыва; ACK из A не должен закрыть handle группы B. */
    if (net->timer) uasync_cancel_timeout(fixture.ua, net->timer);
    net->timer = NULL;
    p->pressure = (struct stream_pressure_request){.id = 900, .required_rate = 1, .count = 1,
        .required_count = 1, .active = 1, .deadline_ms = get_time_tb() / 10 + 5000, .cuts = {{.handle = 1, .rate = 1}}};
    struct stream_message pressure = {.descriptor = info[0].descriptor, .command = STREAM_CMD_OVERLOAD_SELECT,
        .sender_epoch = p->other_epoch, .receiver_epoch = p->own_epoch};
    uint8_t selection[17] = {0}; stream_write_be(selection, 900, 8); selection[8] = 1; stream_write_be(selection + 9, 1, 8);
    REQUIRE(!api_stream_packet(&pressure, selection, sizeof(selection))); f = flow_find(p, &m, 0);
    check("STREAM neighbouring pressure cannot act on revoked group handle", f && f->info.state == STREAM_RECEIVE_ACTIVE
        && !p->pressure.released_count);
    p->pressure.active = 0;
    net->timer = uasync_set_timeout(fixture.ua, STREAM_TICK_TB, net, network_tick, "stream_network"); REQUIRE(net->timer);
    REQUIRE(!api_admit(112, client->node_id, 1));
    struct stream_message queued = {.descriptor = info[1].descriptor, .command = STREAM_CMD_REJECT, .handle = 99};
    uint8_t reason = STREAM_REASON_STOPPED;
    struct ll_entry* reply = reply_reserve(p); REQUIRE(reply); reply_commit(p, reply, queued, &reason, 1);
    queued.descriptor = info[0].descriptor;
    reply = reply_reserve(p); REQUIRE(reply); reply_commit(p, reply, queued, &reason, 1);
    unsigned a = api_stream_controls[0], b = api_stream_controls[1];
    REQUIRE(!api_admit(112, client->node_id, 0));
    uint64_t deadline = get_time_tb() + 30000;
    while ((api_stream_controls[0] == a || api_stream_controls[1] == b) && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    check("STREAM completes saved replies after revoke without blocking neighbouring group", api_stream_controls[0] == a + 1
        && api_stream_controls[1] == b + 1 && !p->reply_count && !p->control->count && fixture.transport->links_up);
    REQUIRE(!api_admit(112, client->node_id, 1) && !api_admit(112, server->node_id, 0));
    f = flow_find(p, &m, 0); REQUIRE(f); lease = f->deadline;
    REQUIRE(!api_stream_packet(&m, NULL, 0)); f = flow_find(p, &m, 0);
    check("STREAM self revoke blocks receive and final raw send", f && f->deadline == lease
        && raw_send(p, m, NULL, 0) == STREAM_ERR_NOT_MEMBER);
    m.command = STREAM_CMD_UNSUBSCRIBE;
    REQUIRE(!api_stream_packet(&m, &reason, 1));
    check("STREAM known unsubscribe releases revoked issuance", !flow_find(p, &m, 0) && !net->regular_used);
    REQUIRE(!api_admit(112, server->node_id, 1));
    for (unsigned i = 0; i < 2; i++) stream_close(pubs[i]);
    client->api_bindings.callbacks[ETCP_ID_STREAM] = api_stream_client;
    return 0;
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
    debug_set_category_level(DEBUG_CATEGORY_CALL, DEBUG_LEVEL_INFO);
    size_t baseline = u_get_allocated_count();
    if (setup() || api_group_keys() || api_calls() || api_devices() || api_streams()) fixture.failed++;
    if (api_held_keys) { queue_dgram_free(api_held_keys); queue_entry_free(api_held_keys); }
    for (unsigned i = 0; i < 2; i++) if (fixture.nodes[i]) {
        chat_service_stop(fixture.nodes[i]);
        member_sync_destroy(fixture.nodes[i]); utun_instance_destroy(fixture.nodes[i]);
    }
    if (fixture.ua) { uasync_poll(fixture.ua, 0); uasync_destroy(fixture.ua, 0); }
    check("group API fixture releases all tracked allocations", u_get_allocated_count() == baseline);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "group API allocations baseline=%zu remaining=%zu", baseline, u_get_allocated_count());
    printf("group API isolation: passed=%u failed=%u\n", fixture.passed, fixture.failed);
    return fixture.failed ? 1 : 0;
}
