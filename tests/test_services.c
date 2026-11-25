#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "utun_instance.h"
#include "config_parser.h"
#include "topo_group.h"
#include "topo_node_sqlite.h"
#include "route_lib.h"
#include "etcp.h"
#include "pkt_normalizer.h"
#include "etcp_router.h"
#include "media_delivery/media_index.h"
#include "media_delivery/media_delivery_proto.h"
#include "node_conn_direct.h"
#include "chat/chat_core.h"
#include "group/group_protocol.h"
#include "group/member_log.h"
#include "chat/chat_member.h"
#include "chat/chat_member_read.h"
#include "chat/chat_event.h"
#include "member_files/protocol.h"
#include "chat/db_sync.h"
#include "chat/member_sync.h"
#include "media_async/media_async.h"
#include "../lib/debug_config.h"
#include "../lib/json_flat.h"
#include "test_utils.h"
#include "test_membership.h"
#include "media_members_fixture.h"

struct job { int worked, done, error; };
static void work(void* arg) { ((struct job*)arg)->worked = 1; }
static void done(void* arg, int err) { struct job* job = arg; job->done++; job->error = err; }
static void ready(struct ll_queue* queue, void* arg) { (void)queue; (*(int*)arg)++; }
static int accept_journal(void* arg, const struct member_log_record* record) { (void)arg; (void)record; return 1; }

static char monitor_text[256 * 1024];
static int monitor_event, monitor_length;

/* Копируем заимствованный текст: снимок освобождается сразу после callback. */
static void capture_monitor(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    (void)inst;
    assert(type == CHAT_EVT_STATUS_REFRESH || type == CHAT_EVT_CONN_METRICS);
    assert(data && len > 0 && len < (int)sizeof(monitor_text) && !memchr(data, 0, (size_t)len));
    memcpy(monitor_text, data, (size_t)len); monitor_text[len] = '\0';
    monitor_event = type; monitor_length = len;
}

/* Публичные сборщики проходят прежнюю границу 8 КБ и сохраняют последний линк без утечек. */
static void check_monitor_snapshots(struct UTUN_INSTANCE* inst) {
    const size_t before = u_get_allocated_count();
    struct UTUN_INSTANCE fixture = *inst;
    fixture.connections = queue_new(inst->ua, 16, offsetof(struct conn_queue_entry, x25519), SC_PUBKEY_SIZE, "monitor test"); assert(fixture.connections);
    fixture.chat_event_handler = capture_monitor;
    struct PKTNORM normalizer = { .frag_size = 1400 };
    struct ETCP_CONN conn = { .instance = &fixture, .peer_node_id = 7, .normalizer = &normalizer };
    struct ll_entry* entry = queue_entry_new(sizeof(struct conn_queue_entry)); assert(entry);
    struct conn_queue_entry* ce = (struct conn_queue_entry*)entry->data; ce->peer_node_id = 7; ce->conn = &conn;
    assert(!queue_data_put_with_index(fixture.connections, entry));
    struct ETCP_LINK* links = u_calloc(255, sizeof(*links)); assert(links);
    struct ll_queue* ack = queue_new(inst->ua, 0, 0, 0, "monitor ACK"); assert(ack);
    struct ETCP_SOCKET socket = { .name = "monitor IPv6" };
    struct sockaddr_in6* addr = (struct sockaddr_in6*)&socket.interface_addr;
    addr->sin6_family = AF_INET6; addr->sin6_port = htons(65535);
    assert(inet_pton(AF_INET6, "1234:5678:9abc:def0:1234:5678:9abc:def0", &addr->sin6_addr) == 1);
    const ip_str_t address = sockaddr_storage_to_str(&socket.interface_addr);
    for (int i = 0; i < 255; i++) {
        links[i].etcp = &conn; links[i].conn = &socket; links[i].ack_q = ack;
        links[i].remote_addr = socket.interface_addr;
    }
    debug_level_t saved = g_debug_config.category_levels[DEBUG_CATEGORY_CHAT];
    debug_set_category_level(DEBUG_CATEGORY_CHAT, DEBUG_LEVEL_TRACE);
    const int counts[] = {0, 1, 35, 36, 40, 255};
    for (unsigned c = 0; c < sizeof(counts) / sizeof(counts[0]); c++) {
        const int count = counts[c];
        for (int i = 0; i < 255; i++) links[i].next = i + 1 < count ? &links[i + 1] : NULL;
        conn.links = count ? links : NULL;
        char last_link[40]; snprintf(last_link, sizeof(last_link), "LINK#%d:", count - 1);
        const size_t allocated = u_get_allocated_count();
        struct chat_node_arg* req = u_malloc(sizeof(*req)); assert(req); req->inst = &fixture; req->node_id = 7;
        monitor_event = 0; chat_core_collect_conn_metrics_trampoline(req);
        assert(monitor_event == CHAT_EVT_CONN_METRICS && strstr(monitor_text, "--- Normalizer ---"));
        assert(!count || (strstr(monitor_text, last_link) && strstr(monitor_text, address.str)));
        if (count >= 35) assert(monitor_length > 8192);
        assert(u_get_allocated_count() == allocated);
        monitor_event = 0; chat_core_collect_status_trampoline(&fixture);
        assert(monitor_event == CHAT_EVT_STATUS_REFRESH && strstr(monitor_text, "=== Connections (1) ==="));
        assert(!count || strstr(monitor_text, last_link));
        if (count == 255) assert(monitor_length > 8192);
        assert(u_get_allocated_count() == allocated);
        DEBUG_INFO(DEBUG_CATEGORY_CHAT, "[PASS] monitor snapshots: links=%d, full metrics/status, allocations unchanged", count);
    }
    ce->conn = NULL;
    struct chat_node_arg* req = u_malloc(sizeof(*req)); assert(req); req->inst = &fixture; req->node_id = 7;
    chat_core_collect_conn_metrics_trampoline(req); assert(strstr(monitor_text, "has null data"));
    req = u_malloc(sizeof(*req)); assert(req); req->inst = &fixture; req->node_id = 8;
    chat_core_collect_conn_metrics_trampoline(req); assert(strstr(monitor_text, "not found"));
    fixture.chat_core = NULL;
    req = u_malloc(sizeof(*req)); assert(req); req->inst = &fixture; req->node_id = 7;
    chat_core_collect_conn_metrics_trampoline(req); assert(strstr(monitor_text, "not initialized"));
    chat_core_collect_status_trampoline(&fixture); assert(strstr(monitor_text, "not initialized"));
    chat_core_collect_conn_list_trampoline(NULL);
    chat_core_collect_status_trampoline(NULL);
    req = u_calloc(1, sizeof(*req)); assert(req); chat_core_collect_conn_metrics_trampoline(req);
    debug_set_category_level(DEBUG_CATEGORY_CHAT, saved);
    queue_entry_free(queue_data_get(fixture.connections)); queue_free(fixture.connections); queue_free(ack); u_free(links);
    assert(u_get_allocated_count() == before);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "[PASS] monitor snapshots: unavailable/missing/null connection and NULL instance, no leaks");
}

/* Активность звонка не создаёт таймер, который сам переведёт мобильный узел в standby. */
static void check_client_activity(struct UASYNC* ua) {
    struct UTUN_INSTANCE mobile = { .ua = ua, .client_type = CLIENT_TYPE_MOBILE, .client_activity = CLIENT_ACTIVITY_STANDBY };
    uint64_t timers = ua->timer_alloc_count;
    utun_set_client_activity(&mobile, 1);
    utun_set_client_activity(&mobile, 1);
    uasync_poll(ua, 0);
    assert(mobile.client_activity == CLIENT_ACTIVITY_ACTIVE && ua->timer_alloc_count == timers);
    utun_set_client_activity(&mobile, 0);
    assert(mobile.client_activity == CLIENT_ACTIVITY_STANDBY);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "[PASS] explicit mobile activity persists without timeout and returns to standby");
}

/* Pending transit shares the physical connection but belongs to its group. */
static void pending_transit(struct ETCP_CONN* conn, uint64_t group_id, int* calls) {
    if (!conn->transit_queues) conn->transit_queues = queue_new(conn->instance->ua, 16, 0, 24, "test_transit");
    assert(conn->transit_queues);
    struct TRANSIT_QUEUE* transit = (struct TRANSIT_QUEUE*)queue_entry_new(sizeof(*transit) - sizeof(struct ll_entry));
    assert(transit); transit->group_id = group_id; transit->conn = conn;
    transit->src_node_id = 123; transit->dst_node_id = conn->peer_node_id;
    transit->q = queue_new(conn->instance->ua, 0, 0, 0, "test_transit_packets"); assert(transit->q);
    assert(queue_data_put_with_index(conn->transit_queues, &transit->ll) == 0);
    struct ll_entry* packet = ll_alloc_lldgram(1); assert(packet); packet->dgram[0] = 0; packet->len = 1;
    assert(queue_data_put(transit->q, packet) == 0);
    assert(queue_waiter_wait(conn->send_input_q, &transit->waiter, ready, calls) == 0);
}

static struct TOPO_GROUP* chat_group(struct UTUN_INSTANCE* inst) {
    for (struct ll_entry* e = inst->topo_groups->group_list->head; e; e = e->next) {
        struct TOPO_GROUP* group = (struct TOPO_GROUP*)e;
        if (group->group_type == TOPO_GROUP_TYPE_CHAT) return group;
    }
    return NULL;
}

static int interrupt_detail_query(void* arg) { (void)arg; return 1; }

static uint8_t detail_reply[CHAT_MEMBER_DETAIL_MAX_SIZE];
static int detail_reply_size, detail_unscoped_events;

static void capture_member_detail(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    (void)inst;
    if (type == 26) detail_unscoped_events++;
    if (type != CHAT_EVT_MEMBER_DETAIL) return;
    assert(len >= CHAT_MEMBER_DETAIL_HEADER_SIZE && len <= (int)sizeof(detail_reply));
    memcpy(detail_reply, data, (size_t)len); detail_reply_size = len;
}

static struct chat_member_detail_view member_diagnostics(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32], uint64_t id) {
    struct chat_member_detail_req* req = u_calloc(1, sizeof(*req)); assert(req);
    req->inst = inst;
    const uint8_t* key = topo_group_wire_id(topo_groups_find(inst->topo_groups, group));
    if (key) memcpy(req->group, key, 32);
    else req->group[31] = 1;
    uint8_t requested[32]; memcpy(requested, req->group, 32);
    memcpy(req->ed, ed, 32); req->request_id = id;
    detail_reply_size = 0;
    chat_core_collect_member_detail_trampoline(req);
    uint64_t end = get_time_us() + 5000000;
    while (!detail_reply_size && get_time_us() < end) uasync_poll(inst->ua, 1);
    struct chat_member_detail_view view = {0};
    assert(!chat_core_parse_member_detail(detail_reply, (size_t)detail_reply_size, &view));
    assert(view.request_id == id && !memcmp(view.group, requested, 32) && !memcmp(view.ed, ed, 32));
    return view;
}

/* Снимок привязан к группе; RTT адреса и факт наличия пути не заменяют живой маршрут. */
static void check_member_snapshots(struct UTUN_INSTANCE* inst, char channels[2][65]) {
    struct TOPO_GROUP* groups[2]; struct TOPO_GROUP_NODE* nodes[2];
    struct ETCP_CONN direct = { .instance = inst, .peer_node_id = 7, .links_up = 1, .rtt_last = 30 };
    struct ETCP_CONN relay = { .instance = inst, .peer_node_id = 8, .links_up = 1, .rtt_last = 70 };
    struct topo_hop direct_hops[] = {{.id=7}}, relay_hops[] = {{.id=7}, {.id=8}};
    uint8_t ed[32], peer_x[32], relay_ed[32], other_ed[32], other_x[32], private_x[32], seed[32];
    assert(!sc_identity_generate(peer_x, private_x, ed, seed));
    assert(!sc_identity_generate(relay.crypto_ctx.peer_public_key, private_x, relay_ed, seed));
    assert(!sc_identity_generate(other_x, private_x, other_ed, seed));
    memcpy(direct.crypto_ctx.peer_public_key, peer_x, 32);
    memcpy(direct.peer_ed25519_pubkey, ed, 32); memcpy(relay.peer_ed25519_pubkey, relay_ed, 32);
    memcpy(direct_hops[0].ed, ed, 32); memcpy(relay_hops[0].ed, ed, 32); memcpy(relay_hops[1].ed, relay_ed, 32);
    uint32_t address; assert(!topo_address_short(ed, &address));
    for (int i = 0; i < 2; i++) {
        assert(!topo_node_sqlite_member_placeholder_put(inst->topo_sqlite_db, channels[i], 7, peer_x, ed));
        char sql[256]; snprintf(sql, sizeof(sql),
            "UPDATE peers_%s SET node_type=4,adm_tags='{\"verified\":\"yes\"}' WHERE node_id=7", channels[i]);
        assert(sqlite3_exec(inst->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK);
        groups[i] = topo_groups_find_channel(inst->topo_groups, channels[i]); assert(groups[i]);
        nodes[i] = (struct TOPO_GROUP_NODE*)queue_entry_new(sizeof(*nodes[i]) - sizeof(struct ll_entry)); assert(nodes[i]);
        memset((uint8_t*)nodes[i] + sizeof(struct ll_entry), 0, sizeof(*nodes[i]) - sizeof(struct ll_entry));
        nodes[i]->node_id = 7; nodes[i]->conn_presence = NCONN_DIRECT | NCONN_BGP;
        nodes[i]->conn_up = i ? NCONN_BGP : NCONN_DIRECT | NCONN_BGP;
        nodes[i]->phone_status = TOPO_FLAG_PHONE_KNOWN | (i ? TOPO_FLAG_PHONE_BUSY : 0);
        assert(queue_data_put_with_index(groups[i]->nodes, &nodes[i]->ll) == 0); topo_address_attach(groups[i], nodes[i], ed, address);
        assert(topo_group_add_path(nodes[i], i ? &relay : &direct, i ? relay_hops : direct_hops, i ? 2 : 1, i ? 20 : 0) == 0);
        uint8_t single[CHAT_MEMBER_DISPLAY_SIZE], *list = NULL; int count = 0;
        assert(!chat_core_get_single_member(inst, channels[i], ed, single));
        assert(!chat_core_get_member_list(inst, channels[i], &list, &count));
        int found = 0;
        for (int j = 0; j < count; j++) {
            struct chat_member_display member; chat_core_deserialize_member(list + j * CHAT_MEMBER_DISPLAY_SIZE, &member);
            if (memcmp(member.ed25519, ed, 32)) continue;
            assert(!memcmp(single, list + j * CHAT_MEMBER_DISPLAY_SIZE, CHAT_MEMBER_DISPLAY_SIZE)); found++;
        }
        assert(found == 1); u_free(list);
        struct chat_member_display snapshot; assert(!chat_core_deserialize_member(single, &snapshot));
        assert(snapshot.online && snapshot.verified && snapshot.node_type == 4);
        assert(snapshot.rtt == (i ? 90 : 30) && snapshot.intermediaries == i);
        assert(snapshot.conn_up == nodes[i]->conn_up && snapshot.conn_presence == nodes[i]->conn_presence);
        assert(!!(snapshot.flags & CHAT_MEMBER_FLAG_PHONE_BUSY) == i);
    }
    chat_event_handler_fn saved_handler = inst->chat_event_handler;
    inst->chat_event_handler = capture_member_detail;
    for (int i = 0; i < 2; i++) {
        struct chat_member_detail_view view = member_diagnostics(inst, groups[i]->group_id, ed, 100 + i);
        assert(view.result == CHAT_MEMBER_DETAIL_OK && view.hop_count == i + 1);
        assert(!memcmp(view.hops, ed, 32) && !memcmp(view.hops + i * 32, i ? relay_ed : ed, 32));
    }
    assert(member_diagnostics(inst, groups[0]->group_id, other_ed, 103).result == CHAT_MEMBER_DETAIL_NOT_MEMBER);
    /* Два Ed25519 с одинаковым локальным числовым индексом не делят карточку или путь. */
    assert(!topo_node_sqlite_member_placeholder_put(inst->topo_sqlite_db, channels[0], 7, other_x, other_ed));
    struct chat_member_detail_view other = member_diagnostics(inst, groups[0]->group_id, other_ed, 106);
    assert(other.result == CHAT_MEMBER_DETAIL_OK && !other.hop_count);
    struct chat_member_detail_view original = member_diagnostics(inst, groups[0]->group_id, ed, 107);
    assert(original.result == CHAT_MEMBER_DETAIL_OK && original.hop_count == 1);
    assert(member_diagnostics(inst, INT64_MAX, ed, 104).result == CHAT_MEMBER_DETAIL_UNAVAILABLE);
    sqlite3_progress_handler(inst->topo_sqlite_db, 1, interrupt_detail_query, NULL);
    int found = 17;
    int failed = chat_member_exists_read(inst->topo_sqlite_db, channels[0], ed, &found);
    sqlite3_progress_handler(inst->topo_sqlite_db, 0, NULL, NULL);
    assert(failed == SQLITE_INTERRUPT && found == 17);
    assert(member_diagnostics(inst, groups[0]->group_id, ed, 108).result == CHAT_MEMBER_DETAIL_OK);
    member_diagnostics(inst, groups[1]->group_id, ed, 105);
    struct chat_member_detail_view untouched = { .request_id = 999 };
    assert(chat_core_parse_member_detail(detail_reply, 0, &untouched) < 0 && untouched.request_id == 999);
    assert(chat_core_parse_member_detail(detail_reply, (size_t)detail_reply_size - 1, &untouched) < 0);
    uint8_t saved_count = detail_reply[CHAT_MEMBER_DETAIL_HOP_COUNT_OFF]; detail_reply[CHAT_MEMBER_DETAIL_HOP_COUNT_OFF] = CHAT_MEMBER_DETAIL_MAX_HOPS + 1;
    assert(chat_core_parse_member_detail(detail_reply, (size_t)detail_reply_size, &untouched) < 0);
    detail_reply[CHAT_MEMBER_DETAIL_HOP_COUNT_OFF] = saved_count;
    struct chat_member_detail_view valid;
    assert(!chat_core_parse_member_detail(detail_reply, (size_t)detail_reply_size, &valid));
    if (valid.sock_count) {
        memset((uint8_t*)valid.socks + 10, 'x', 38);
        assert(chat_core_parse_member_detail(detail_reply, (size_t)detail_reply_size, &untouched) < 0);
    }
    assert(untouched.request_id == 999 && !detail_unscoped_events);
    direct.links_up = 0;
    uint8_t wire[CHAT_MEMBER_DISPLAY_SIZE]; struct chat_member_display snapshot;
    assert(!chat_core_get_single_member(inst, channels[0], ed, wire)); chat_core_deserialize_member(wire, &snapshot);
    assert(!snapshot.online && snapshot.rtt == UINT16_MAX && !(snapshot.flags & CHAT_MEMBER_FLAG_PHONE_KNOWN));
    assert(!chat_core_member_online(inst, channels[0], ed) && chat_core_member_online(inst, channels[1], ed));
    assert(member_diagnostics(inst, groups[0]->group_id, ed, 106).hop_count == 0);
    assert(member_diagnostics(inst, groups[1]->group_id, ed, 107).hop_count == 2);
    assert(!topo_group_add_path(nodes[0], &relay, relay_hops, 2, 20));
    assert(!chat_core_get_single_member(inst, channels[0], ed, wire)); chat_core_deserialize_member(wire, &snapshot);
    assert(snapshot.online && snapshot.rtt == 90 && snapshot.intermediaries == 1);
    relay.rtt_last = 0;
    assert(!chat_core_get_single_member(inst, channels[0], ed, wire)); chat_core_deserialize_member(wire, &snapshot);
    assert(snapshot.online && snapshot.rtt == UINT16_MAX);
    relay.rtt_last = 65530;
    assert(!chat_core_get_single_member(inst, channels[0], ed, wire)); chat_core_deserialize_member(wire, &snapshot);
    assert(snapshot.rtt == UINT16_MAX); /* насыщение, без переполнения и подстановки сохранённого RTT */
    inst->chat_event_handler = saved_handler;
    for (int i = 0; i < 2; i++) {
        topo_address_detach(groups[i], nodes[i]); topo_nodeq_clear_paths(nodes[i]); queue_remove_data(groups[i]->nodes, &nodes[i]->ll); queue_entry_free(&nodes[i]->ll);
        assert(!topo_node_sqlite_member_del(inst->topo_sqlite_db, channels[i], 7));
    }
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "[PASS] member snapshots: groups, full/single, direct/relay/offline, phone, unknown RTT and saturation");
}

/* Полный restart с той же БД: новое имя подписано во всех каналах, повторный старт не меняет версии. */
static void check_config_name(struct UASYNC* ua) {
    char directory[512] = "/tmp/utun-config-name-XXXXXX";
    assert(test_mkdtemp(directory) == 0);
    char channels[2][65], config[2048];
    uint8_t previous_hash[2][32], previous_signature[2][64];
    uint64_t previous_version[2] = {0};
    const char* names[] = {"Old server", "New \"server\"", "New \"server\"", "Other name"};
    for (unsigned run = 0; run < 4; run++) {
        snprintf(config, sizeof(config), "[global]\nmy_node_name=%s\ndb_path=%s\n"
            "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
            "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
            "[server: udp]\naddr=127.0.0.1:0\ntype=public\n%s", names[run], directory,
            run < 3 ? "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n" : "");
        struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config); assert(inst);
        assert(utun_core_start(inst) == 0 && chat_service_start(inst) == 0);
        if (!run) {
            chat_core_create_channel_auto(inst, "config name 1");
            chat_core_create_channel_auto(inst, "config name 2");
            unsigned count = 0;
            for (struct ll_entry* e = inst->topo_groups->group_list->head; e; e = e->next) {
                struct TOPO_GROUP* group = (struct TOPO_GROUP*)e;
                assert(group->group_type == TOPO_GROUP_TYPE_CHAT && count < 2);
                snprintf(channels[count], sizeof(channels[count]), "%s", group->channel_id);
                struct chat_profile_option option = {"extra", "retained"};
                assert(chat_core_set_my_profile_options(inst, channels[count], &option, 1) == 0);
                count++;
            }
            assert(count == 2 && strcmp(channels[0], channels[1]));
            check_member_snapshots(inst, channels);
        }
        for (unsigned i = 0; i < 2; i++) {
            const char* ch = channels[i];
            if (!run) {
                struct member_log_record record; uint8_t scope[32] = {0}, grant[32] = {0};
                assert(sqlite3_exec(inst->topo_sqlite_db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
                assert(!member_log_make(inst->topo_sqlite_db, ch, inst->my_ed25519_pubkey, 1,
                    scope, grant, (const uint8_t*)"restart", 7, inst->my_ed25519_privkey, &record));
                assert(member_log_accept(inst->topo_sqlite_db, &record, inst->my_ed25519_pubkey, accept_journal, NULL) == 1);
                assert(!member_log_commit(inst->topo_sqlite_db));
            }
            struct member_log_head journal_head;
            assert(!member_log_head(inst->topo_sqlite_db, ch, inst->my_ed25519_pubkey, &journal_head));
            assert(journal_head.seq == 2 && !journal_head.blocked); /* Приглашение и тестовая запись restart. */
            char profile[MS_USERINFO_MAX + 1], value[128];
            uint8_t signature[64], join[64], hash[32], message[MS_USERINFO_MAX + 73];
            uint64_t version, join_ts;
            assert(chat_core_get_my_profile(inst, channels[i], profile, sizeof(profile), signature, &version) == 0);
            assert(json_flat_get(profile, "name", value, sizeof(value)) == 0 && !strcmp(value, names[run == 3 ? 2 : run]));
            assert(json_flat_get(profile, "extra", value, sizeof(value)) == 0 && !strcmp(value, "retained"));
            assert(topo_node_sqlite_member_get_join(inst->topo_sqlite_db, channels[i], inst->my_ed25519_pubkey, join, &join_ts) == 0);
            uint8_t join_id[32]; assert(!member_sync_join_id_read(inst->topo_sqlite_db, channels[i], inst->my_ed25519_pubkey, inst->my_keys.public_key, join_id));
            int length = member_sync_build_update_msg(join_id, version, profile, message, sizeof(message)); assert(length > 0);
            assert(sc_ed25519_verify(inst->my_ed25519_pubkey, message, length, signature) == SC_OK);
            assert(merkle_sync_read_hash(inst, MT_DATA_MEMBERS, channels[i], 0, 0, hash) == 0);
            if (run == 1) {
                assert(version > previous_version[i] && memcmp(signature, previous_signature[i], 64));
                assert(memcmp(hash, previous_hash[i], 32));
            } else if (run > 1) {
                assert(version == previous_version[i] && !memcmp(signature, previous_signature[i], 64));
                assert(!memcmp(hash, previous_hash[i], 32));
            }
            previous_version[i] = version;
            memcpy(previous_hash[i], hash, 32); memcpy(previous_signature[i], signature, 64);
        }
        if (run == 3) assert(sqlite3_exec(inst->topo_sqlite_db, "UPDATE member_log SET body=X'00'", NULL, NULL, NULL) == SQLITE_OK);
        utun_instance_destroy(inst); uasync_poll(ua, 0);
    }
    assert(!utun_instance_create_from_str(ua, config)); uasync_poll(ua, 0);
    char path[1024]; snprintf(path, sizeof(path), "%s/chats.db", directory); test_unlink(path); test_rmdir(directory);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT,
        "[PASS] config name: restart updates signed profiles and Merkle in all channels; unchanged/no chatserver keeps versions");
    DEBUG_INFO(DEBUG_CATEGORY_GROUP, "[PASS] own journal: durable writes survive restart; corrupt tail stops core startup");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    struct UASYNC* ua = uasync_create(); assert(ua);
    check_client_activity(ua);
    check_config_name(ua);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua,
        "[global]\nmy_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
        "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
        "[routing]\nmy_subnet=192.168.42.0/24\n[server: udp]\naddr=127.0.0.1:0\ntype=public\n"
        "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n");
    assert(inst && !topo_groups_get_default(inst->topo_groups) && !inst->chat_core);
    inst->config->global.db_sync_enabled = 0;
    assert(utun_core_start(inst) == 0 && utun_core_start(inst) == 0);
    struct ETCP_SOCKET* socket = inst->etcp_sockets;
    sqlite3* db = inst->topo_sqlite_db;
    struct TOPO_NODE* identity = topo_node_registry_find(inst->topo_groups, inst->node_id);
    assert(identity && identity->v4_addrs);
    assert(chat_service_start(inst) == 0 && chat_service_start(inst) == 0);
    check_monitor_snapshots(inst);
    assert(!topo_groups_get_default(inst->topo_groups));
    chat_core_create_channel_auto(inst, "lifecycle");
    struct TOPO_GROUP* chat = chat_group(inst); assert(chat);
    uint64_t chat_id = chat->group_id; char chat_key[65]; strcpy(chat_key, chat->channel_id);
    assert(utun_service_start(inst) == 0 && utun_service_start(inst) == 0);
    assert(inst->rt->count > 0);
    struct TOPO_GROUP* vpn = topo_groups_get_default(inst->topo_groups); assert(vpn);

    struct SC_MYKEYS keys; uint8_t peer_ed[32], peer_private[32], group_private[32];
    assert(!sc_identity_generate(keys.public_key, keys.private_key, peer_ed, peer_private));
    struct TOPO_ADDR4 address = { .addr = {127, 0, 0, 1}, .port = 9, .protocol = TOPO_PROTO_UDP };
    struct TOPO_NODE node = { .v4_addrs = &address };
    memcpy(node.public_key, keys.public_key, 32); node.node_id = sc_derive_node_id_from_pubkey(keys.public_key);

    assert(!topo_node_sqlite_channel_get_priv(db, chat->channel_id, group_private));
    assert(test_member_seed(inst, chat->channel_id, keys.public_key, peer_ed, peer_private,
        group_private, "{}", NULL, NULL) >= 0);
    struct NODE_CONN_DIRECT* seed = NULL;
    assert(node_conn_direct_open_node(inst, node.node_id, NULL, NULL, &seed, &node, NULL) >= 0);
    struct ETCP_CONN* conn = node_conn_direct_get_conn(seed); assert(conn);
    struct TOPO_PEER_REQUEST *chat_request = NULL, *vpn_request = NULL;
    assert(topo_group_peer_open(chat, node.node_id, &chat_request) == 0);
    assert(topo_group_peer_open(vpn, node.node_id, &vpn_request) == 0);
    node_conn_direct_close(seed);
    utun_service_stop(inst); utun_service_stop(inst);
    assert(!conn->close_requested && topo_group_peer_conn(chat_request) == conn);
    assert(topo_group_peer_phase(vpn_request) == TOPO_PEER_FAILED && inst->rt->count == 0);
    topo_group_peer_close(vpn_request);
    assert(inst->etcp_sockets == socket && inst->topo_sqlite_db == db && inst->chat_started);
    assert(topo_node_registry_find(inst->topo_groups, inst->node_id) == identity);
    assert(topo_node_update_my_addresses(inst) >= 0);

    assert(utun_service_start(inst) == 0);
    vpn = topo_groups_get_default(inst->topo_groups);
    assert(topo_group_peer_open(vpn, node.node_id, &vpn_request) == 0);
    /* Stop a real stream while its router send queue is blocked. */
    char path[] = "/tmp/utun-service-stream-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0);
    char bytes[8192] = {0}; assert(write(fd, bytes, sizeof(bytes)) == sizeof(bytes)); close(fd);
    chat_core_save_ui_state(inst, "media_base", "/tmp");
    uint8_t media_id[16] = {1}, block_id[16] = {2}, hash[32] = {3};
    assert(media_index_register_downloaded(db, media_id, block_id, hash, chat->channel_id,
        strrchr(path, '/') + 1, inst->my_ed25519_pubkey, sizeof(bytes), sizeof(bytes), 0, 0) == 0);
    struct media_index_result published = {.file_size=sizeof(bytes)};
    memcpy(published.media_id, media_id, 16); memcpy(published.content_hash, hash, 32);
    media_test_publish_channel(db, chat_key, inst->node_id, &published);
    struct ETCP_ROUTER_CONN* routed = etcp_router_conn_get(inst, chat_id, node.node_id, ETCP_RT_ID_MEDIA_DELIVERY);
    assert(routed);
    struct queue_waiter_handle waiter = {0}; int ready_calls = 0;
    struct ll_queue* old_queue = routed->send_q;
    queue_set_threshold(old_queue, -1, 0);
    etcp_router_conn_on_send_ready(routed, &waiter, ready, &ready_calls);
    assert(waiter.internal);
    etcp_router_conn_restart(inst, chat_id, node.node_id, ETCP_RT_ID_MEDIA_DELIVERY);
    assert(routed->send_q != old_queue);
    queue_set_threshold(routed->send_q, -1, 0);
    etcp_router_conn_on_send_ready(routed, &waiter, ready, &ready_calls);
    assert(!old_queue->waiter_head && waiter.internal);
    etcp_router_cancel_send_ready(&waiter);
    assert(!routed->send_q->waiter_head && !waiter.internal);
    queue_set_threshold(routed->send_q, 0, 0);
    routed->send_q->waiter_defer = 1;
    etcp_router_conn_on_send_ready(routed, &waiter, ready, &ready_calls);
    assert(waiter.call_soon_id);
    etcp_router_cancel_send_ready(&waiter);
    struct media_pkt_block_req request = {.subcmd = MEDIA_SUBCMD_BLOCK_REQ, .transfer_id = {1}};
    memcpy(request.media_id, media_id, 16); memcpy(request.block_id, block_id, 16);
    struct ll_entry* packet = ll_alloc_lldgram(ROUTER_SVC_PAYLOAD_OFF + sizeof(request)); assert(packet);
    memset(packet->dgram, 0, ROUTER_SVC_PAYLOAD_OFF);
    packet->dgram[0] = ETCP_RT_ID_MEDIA_DELIVERY;
    memcpy(packet->dgram + ROUTER_SVC_SRC_ED_OFF, peer_ed, 32);
    memcpy(packet->dgram + ROUTER_SVC_GROUP_OFF, &chat_id, 8);
    memcpy(packet->dgram + ROUTER_SVC_PAYLOAD_OFF, &request, sizeof(request));
    packet->len = ROUTER_SVC_PAYLOAD_OFF + sizeof(request);
    inst->router_bindings.callbacks[ETCP_RT_ID_MEDIA_DELIVERY](conn, packet);
    assert(inst->md.streams && inst->md.active_streams == 1);
    struct job cancelled = {0};
    media_async_submit(inst->media_async, ua, work, &cancelled, done, &cancelled);
    queue_set_threshold(conn->send_input_q, -1, 0);
    pending_transit(conn, chat_id, &ready_calls);
    pending_transit(conn, vpn->group_id, &ready_calls);
    chat_service_stop(inst); chat_service_stop(inst);
    assert(chat_group(inst) && inst->chat_core && inst->chat_sync && inst->group_started);
    assert(!db_sync_instance_find(inst, chat_id));
    group_service_stop(inst);
    assert(queue_entry_count(conn->transit_queues) == 1);
    assert(((struct TRANSIT_QUEUE*)conn->transit_queues->head)->group_id == vpn->group_id);
    assert(!inst->md.streams && routed->closed); unlink(path);
    assert(cancelled.worked && cancelled.done == 1 && cancelled.error == MEDIA_ASYNC_CANCELLED);
    assert(!chat_group(inst) && !inst->chat_core && !inst->chat_sync && !inst->media_async && !inst->md.initialized);
    assert(!db_sync_instance_find(inst, chat_id));
    assert(!conn->close_requested && topo_group_peer_conn(vpn_request) == conn);
    assert(topo_group_peer_phase(chat_request) == TOPO_PEER_FAILED);
    topo_group_peer_close(chat_request);
    /* Remove the other group's pending transit before processing the event loop. */
    etcp_router_close_group(inst, vpn->group_id);
    assert(queue_entry_count(conn->transit_queues) == 0);
    queue_set_threshold(conn->send_input_q, 0, 0);
    for (int i = 0; i < 3; i++) {
        assert(chat_service_start(inst) == 0 && !strcmp(chat_group(inst)->channel_id, chat_key));
        assert(db_sync_instance_find(inst, topo_groups_channel_slot(inst->topo_groups, chat_key)));
        chat_service_stop(inst);
        assert(inst->etcp_sockets == socket && inst->topo_sqlite_db == db && inst->utun_started);
        uasync_poll(ua, 0);
    }
    topo_group_peer_close(vpn_request);
    utun_service_stop(inst);
    struct job completed = {0};
    struct media_async* jobs = media_async_create(); assert(jobs);
    media_async_submit(jobs, ua, work, &completed, done, &completed);
    while (!completed.done) uasync_poll(ua, 100);
    assert(completed.worked && completed.done == 1 && !completed.error);
    media_async_destroy(jobs);
    utun_instance_destroy(inst); uasync_poll(ua, 0);
    assert(!ready_calls);
    assert(ua->timer_alloc_count == ua->timer_free_count && ua->socket_alloc_count == ua->socket_free_count);
    uasync_destroy(ua, 0);
    return 0;
}
