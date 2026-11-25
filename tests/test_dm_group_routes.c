/* Личные сообщения проходят через попарные группы; общей группы A/S/M/B нет.
 * Подписанные участники, настоящий ETCP/router и отдельная SQLite каждого узла. */
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "utun_instance.h"
#include "config_updater.h"
#include "chat/chat_core.h"
#include "chat/member_sync.h"
#include "chat/chat_setting.h"
#include "dm/dm_core.h"
#include "dm/dm_mailbox.h"
#include "dm/dm_media.h"
#include "routing_layer/topo_group.h"
#include "routing_layer/topo_node_sqlite.h"
#include "routing_layer/etcp_router.h"
#include "routing_layer/route_crypto.h"
#include "transport_layer/etcp.h"
#include "transport_layer/etcp_api.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_net.h"
#include "test_membership.h"
#include "test_utils.h"

enum { A, S, M, B, NODES };
static const unsigned pairs[][2] = {{A,S}, {A,S}, {S,B}, {S,M}, {A,M}, {M,B}, {A,B}, {S,M}, {M,B}};
#define GROUPS (sizeof(pairs) / sizeof(pairs[0]))
#define REQUIRE(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "DM routes: %s line=%d", #x, __LINE__); return -1; } } while (0)
#define WAIT(x) do { uint64_t end = get_time_tb() + 70000; while (!(x) && get_time_tb() < end) uasync_poll(f.ua, 1); REQUIRE(x); } while (0)

static struct {
    struct UASYNC* ua;
    struct UTUN_INSTANCE* node[NODES];
    struct TOPO_GROUP* group[NODES][GROUPS];
    struct test_group_keys keys[GROUPS];
    char directory[512], conv[64];
    etcp_recv_fn receiver[NODES], media_receiver;
    unsigned media_seen[NODES];
    unsigned have_seen;
    unsigned observed, passed, failed;
} f;

/* Проверки маршрута/ролей обязаны работать в loop без единого SQL statement. */
static __thread int forbid_sql;
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    if (forbid_sql) DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "DM runtime attempted SQL: %s", sql);
    assert(!forbid_sql); return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}

static void check(const char* name, int ok) {
    if (ok) { f.passed++; DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS %s", name); }
    else { f.failed++; DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "FAIL %s", name); }
}

static int value(unsigned node, const char* sql) {
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(f.node[node]->topo_sqlite_db, sql, -1, &st, NULL), result = -1;
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) result = sqlite3_column_int(st, 0);
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_ROW || final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "DM routes SQL: %s rc=%d final=%d", sql, rc, final);
    return result;
}

static int member(unsigned node, unsigned group, unsigned peer, const char* field, int state) {
    char sql[192];
    char channel[65]; sc_public_key_hex(f.keys[group].ed, channel);
    snprintf(sql, sizeof(sql), "UPDATE peers_%s SET %s=%d WHERE node_id=%lld", channel, field, state,
        (long long)f.node[peer]->node_id);
    REQUIRE(sqlite3_exec(f.node[node]->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK);
    REQUIRE(!test_member_publish_access(f.node[node], channel));
    return 0;
}

static void observed(struct ETCP_CONN* conn, struct ll_entry* entry) {
    for (unsigned i = 0; i < NODES; i++) if (conn->instance == f.node[i]) {
        if (entry->len >= sizeof(struct SVC_ROUTE_HDR)) {
            uint8_t svc = entry->dgram[offsetof(struct SVC_ROUTE_HDR, svc_id)];
            if (i == S && svc == ETCP_RT_ID_DM_MAILBOX) f.observed++;
            if (svc == ETCP_RT_ID_DM_MEDIA) f.media_seen[i]++;
        }
        f.receiver[i](conn, entry); return;
    }
}

/* HAVE наблюдаем после проверки автора и расшифровки на конечном узле M. */
static void media_observed(struct ETCP_CONN* conn, struct ll_entry* entry) {
    if (entry->len > ROUTER_SVC_PAYLOAD_OFF && entry->dgram[ROUTER_SVC_PAYLOAD_OFF] == DM_MEDIA_HAVE) f.have_seen++;
    f.media_receiver(conn, entry);
}

/* Инъекция подписана настоящим A; обходим только исходящую политику отправителя. */
static int inject(unsigned group, const uint8_t* payload, size_t len, int signed_packet) {
    struct SVC_ROUTE_HDR header = {.cmd = ETCP_ID_SVC_ROUTE, .group_id = 101 + group,
        .src_node_id = f.node[A]->node_id, .dst_node_id = f.node[S]->node_id,
        .svc_id = ETCP_RT_ID_DM_MAILBOX, .reset_id = 123, .flags = ROUTER_FLAG_START};
    uint8_t plain[2048]; REQUIRE(sizeof(header) + len <= sizeof(plain));
    memcpy(plain, &header, sizeof(header)); memcpy(plain + sizeof(header), payload, len);
    uint8_t* encoded = NULL; size_t size = 0;
    REQUIRE(!route_crypto_encode(f.node[A], ROUTE_CRYPTO_CHAT, f.node[S]->my_keys.public_key,
        plain, sizeof(header) + len, signed_packet ? ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT : 0, &encoded, &size));
    struct ll_entry* entry = queue_entry_new(0); REQUIRE(entry);
    entry->dgram = u_malloc(size + 9); REQUIRE(entry->dgram);
    memcpy(entry->dgram, encoded, size); u_free(encoded);
    memcpy(entry->dgram + size, &header.src_node_id, 8); entry->dgram[size + 8] = 1;
    entry->len = (uint16_t)(size + 9);
    struct ETCP_CONN* conn = instance_find_conn(f.node[A], f.node[S]->node_id); REQUIRE(conn && conn->links_up);
    unsigned before = f.observed;
    REQUIRE(!etcp_send(conn, entry));
    WAIT(f.observed > before);
    return 0;
}

static int setup(void) {
    snprintf(f.directory, sizeof(f.directory), "/tmp/utun_dm_routes_XXXXXX"); REQUIRE(!test_mkdtemp(f.directory));
    uint8_t device_x[NODES][32], device_seed[NODES][32]; char pub[NODES][65], priv[NODES][65];
    for (unsigned i = 0; i < NODES; i++) {
        REQUIRE(!sc_identity_generate_config(device_x[i], device_seed[i]));
        bytes_to_hex(device_x[i], 32, pub[i], 65); bytes_to_hex(device_seed[i], 32, priv[i], 65);
    }
    f.ua = uasync_create(); REQUIRE(f.ua);
    for (unsigned i = 0; i < NODES; i++) {
        char config[4096];
        int len = snprintf(config, sizeof(config), "[global]\nauto_sockets=no\nmy_public_key=%s\nmy_private_key=%s\n"
            "db_path=%s/node%u\n[server: udp]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=1\n"
            "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=1\n", pub[i], priv[i], f.directory, i);
        for (unsigned j = 0; j < i; j++) len += snprintf(config + len, sizeof(config) - len,
            "[client: peer%u]\npeer_public_key=%s\nlink=udp:127.0.0.1:0\n", j, pub[j]);
        f.node[i] = utun_instance_create_from_str(f.ua, config); REQUIRE(f.node[i]);
    }
    for (unsigned i = 0; i < NODES; i++) {
        for (unsigned j = 0; j < i; j++) {
            char name[32]; snprintf(name, sizeof(name), "peer%u", j); test_etcp_set_peer(f.node[i], name, f.node[j], "udp");
        }
        REQUIRE(!utun_core_start(f.node[i]) && !chat_service_start(f.node[i]));
    }
    struct UTUN_INSTANCE* owner = f.node[A];
    for (unsigned g = 0; g < GROUPS; g++) {
        uint8_t signature[64];
        struct test_group_keys* keys = &f.keys[g]; REQUIRE(!test_group_keys_generate(keys));
        char channel[65]; sc_public_key_hex(keys->ed, channel);
        REQUIRE(!test_group_signature(channel, "dm-routes", owner->my_ed25519_pubkey, keys->x.public_key, keys->ed, keys->ed_private, signature));
        for (unsigned side = 0; side < 2; side++) {
            unsigned local = pairs[g][side]; struct UTUN_INSTANCE* inst = f.node[local];
            REQUIRE(!topo_node_sqlite_channel_put(inst->topo_sqlite_db, channel, "dm-routes", owner->my_ed25519_pubkey,
                keys->x.public_key, NULL, keys->ed, NULL, signature));
            for (unsigned p = 0; p < 2; p++) {
                unsigned index = pairs[g][p]; struct UTUN_INSTANCE* node = f.node[index];
                const char* tags = index == S && g != 1 ? "{\"supernode\":\"yes\",\"ver\":\"1\"}" :
                    index == M ? "{\"storage\":\"yes\",\"ver\":\"1\"}" : "{\"ver\":\"1\"}";
                uint8_t body[256], tag_signature[64];
                int size = member_sync_build_owner_msg(channel, node->my_ed25519_pubkey, tags, body, sizeof(body)); REQUIRE(size > 0);
                REQUIRE(!sc_ed25519_sign(keys->ed_private, body, size, tag_signature));
                REQUIRE(test_member_seed(inst, channel, node->my_keys.public_key, node->my_ed25519_pubkey,
                    node->my_ed25519_privkey, keys->ed_private, "{}", tags, tag_signature) >= 0);
            }
            REQUIRE(topo_groups_create_group(inst->topo_groups, 101 + g, TOPO_GROUP_TYPE_CHAT, channel));
            REQUIRE(!chat_core_ensure_channel_ready(inst, channel));
            f.group[local][g] = topo_groups_find(inst->topo_groups, 101 + g);
            REQUIRE(f.group[local][g]);
        }
    }
    for (unsigned g = 0; g < GROUPS; g++) for (unsigned side = 0; side < 2; side++) {
        unsigned local = pairs[g][side], peer = pairs[g][1 - side];
        struct ETCP_CONN* conn;
        WAIT((conn = instance_find_conn(f.node[local], f.node[peer]->node_id)) && conn->links_up);
        REQUIRE(!topo_group_new_conn(f.group[local][g], conn));
    }
    for (unsigned g = 0; g < GROUPS; g++) for (unsigned side = 0; side < 2; side++)
        WAIT(topo_group_peer_ready(f.group[pairs[g][side]][g], f.node[pairs[g][1-side]]->node_id));
    for (unsigned i = 0; i < NODES; i++) {
        f.receiver[i] = f.node[i]->api_bindings.callbacks[ETCP_ID_SVC_ROUTE]; REQUIRE(f.receiver[i]);
        f.node[i]->api_bindings.callbacks[ETCP_ID_SVC_ROUTE] = observed;
    }
    f.media_receiver = f.node[M]->router_bindings.callbacks[ETCP_RT_ID_DM_MEDIA]; REQUIRE(f.media_receiver);
    f.node[M]->router_bindings.callbacks[ETCP_RT_ID_DM_MEDIA] = media_observed;
    uint64_t conv;
    REQUIRE(!dm_start(owner, f.node[B]->node_id, f.node[B]->my_keys.public_key, f.node[B]->my_ed25519_pubkey,
        "recipient", "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951", DM_MODE_DEVICE, &conv));
    snprintf(f.conv, sizeof(f.conv), "%llu", (unsigned long long)conv);
    return 0;
}

static int run(void) {
    uint64_t route = 0;
    check("role in 101 permits S while route uses 102", dm_peer_role(f.node[A], f.node[S]->node_id, "supernode") &&
        dm_route_group(f.node[A], f.node[S]->node_id, 102, &route) && route == 102);
    sqlite3* saved_db = f.node[A]->topo_sqlite_db; f.node[A]->topo_sqlite_db = NULL; route = 0; forbid_sql = 1;
    int live_route = dm_route_group(f.node[A], f.node[S]->node_id, 102, &route);
    int supernode = dm_peer_role(f.node[A], f.node[S]->node_id, "supernode");
    int storage = dm_peer_role(f.node[A], f.node[M]->node_id, "storage");
    int wrong_role = dm_peer_role(f.node[A], f.node[M]->node_id, "supernode");
    forbid_sql = 0; f.node[A]->topo_sqlite_db = saved_db;
    check("preferred live route needs no SQLite handle", live_route && route == 102);
    check("supernode/storage use committed roles without SQLite", supernode && storage && !wrong_role);
    const char* fields[] = {"admitted", "deleted", "source"};
    for (unsigned i = 0; i < 3; i++) {
        REQUIRE(!member(A, 0, S, fields[i], i ? 1 : 0));
        check("unadmitted/deleted/foreign-source row gives no role", !dm_peer_role(f.node[A], f.node[S]->node_id, "supernode"));
        REQUIRE(!member(A, 0, S, fields[i], i ? 0 : 1));
    }
    REQUIRE(!member(A, 0, A, "admitted", 0));
    check("self revocation removes role from that group", !dm_peer_role(f.node[A], f.node[S]->node_id, "supernode"));
    REQUIRE(!member(A, 0, A, "admitted", 1));
    /* Отдельная A/B-группа нужна только для начала беседы, затем прямого маршрута нет. */
    REQUIRE(!member(A, 6, B, "admitted", 0) && !member(B, 6, A, "admitted", 0));
    /* Синхронизация подписанного состава не должна восстановить прямой путь в этом сценарии. */
    f.group[A][6]->stopping = f.group[B][6]->stopping = 1;
    topo_group_revalidate_members(f.group[A][6]); topo_group_revalidate_members(f.group[B][6]);
    REQUIRE(!dm_send(f.node[A], f.conv, "text", (const uint8_t*)"pairwise mailbox", 16));
    uint8_t packet[2048] = {1};
    memcpy(packet + 1, f.node[B]->my_ed25519_pubkey, MB_RECIPIENT_SIZE);
    sqlite3_stmt* st = NULL;
    REQUIRE(sqlite3_prepare_v2(f.node[A]->topo_sqlite_db, "SELECT body FROM dm_outbox", -1, &st, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW); int bytes = sqlite3_column_bytes(st, 0);
    REQUIRE(bytes > 0 && bytes + 1 + MB_RECIPIENT_SIZE <= (int)sizeof(packet)); memcpy(packet + 1 + MB_RECIPIENT_SIZE, sqlite3_column_blob(st, 0), bytes);
    REQUIRE(sqlite3_finalize(st) == SQLITE_OK);
    REQUIRE(!member(S, 1, A, "admitted", 0));
    REQUIRE(!inject(1, packet, bytes + 1 + MB_RECIPIENT_SIZE, 1));
    check("signed PUT through revoked group does not store pending", value(S, "SELECT count(*) FROM dm_pending_messages") == 0);
    REQUIRE(!member(S, 1, A, "admitted", 1));
    REQUIRE(!inject(1, packet, bytes + 1 + MB_RECIPIENT_SIZE, 0));
    check("unsigned mailbox PUT does not store pending", value(S, "SELECT count(*) FROM dm_pending_messages") == 0);
    struct ETCP_ROUTER_CONN* conn = etcp_router_conn_get(f.node[A], 102, f.node[S]->node_id, ETCP_RT_ID_DM_MAILBOX); REQUIRE(conn);
    REQUIRE(!etcp_router_conn_send(conn, packet, bytes + 1 + MB_RECIPIENT_SIZE, ROUTER_FLAG_SIGNED));
    WAIT(value(B, "SELECT count(*) FROM dm_messages") == 1);
    WAIT(value(A, "SELECT count(*) FROM dm_outbox") == 0);
    check("mailbox crosses A/S and S/B groups without common A/S/B group",
        value(S, "SELECT count(*) FROM dm_pending_messages") == 0 && value(S, "SELECT count(*) FROM dm_mail_receipts") == 1);
    conn = etcp_router_conn_get(f.node[A], 101, f.node[S]->node_id, ETCP_RT_ID_DM_MAILBOX); REQUIRE(conn);
    REQUIRE(!etcp_router_conn_send(conn, packet, bytes + 1 + MB_RECIPIENT_SIZE, ROUTER_FLAG_SIGNED));
    uint64_t end = get_time_tb() + 2000; while (get_time_tb() < end) uasync_poll(f.ua, 1);
    check("same PUT over another group preserves receipt and single message", value(S, "SELECT count(*) FROM dm_pending_messages") == 0 &&
        value(B, "SELECT count(*) FROM dm_messages") == 1 && value(S, "SELECT count(*) FROM dm_mail_receipts") == 1);
    check("mailbox persists identities without route column", value(S,
        "SELECT count(*) FROM pragma_table_info('dm_pending_messages') WHERE name='group_id'") == 0);
    return 0;
}

static int file_delivery(void) {
    REQUIRE(!chat_setting_set(f.node[B], "storage_autoload", "0"));
    char path[768]; snprintf(path, sizeof(path), "%s/source.bin", f.directory);
    FILE* source = fopen(path, "wb"); REQUIRE(source);
    for (unsigned i = 0; i < 131079; i++) REQUIRE(fputc((i * 73 + 19) & 255, source) != EOF);
    REQUIRE(!fclose(source));
    struct attachment_info info = {.kind = ATTACHMENT_FILE};
    snprintf(info.name, sizeof(info.name), "pairwise.bin");
    REQUIRE(!dm_send_file(f.node[A], f.conv, path, &info, 0, 0));
    WAIT(value(A, "SELECT count(*) FROM dm_files WHERE ready=1") == 1);
    sqlite3_stmt* st = NULL;
    REQUIRE(sqlite3_prepare_v2(f.node[A]->topo_sqlite_db, "SELECT body FROM dm_files WHERE ready=1", -1, &st, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW);
    int bytes = sqlite3_column_bytes(st, 0); uint8_t body[8192];
    REQUIRE(bytes > 0 && bytes <= (int)sizeof(body)); memcpy(body, sqlite3_column_blob(st, 0), bytes);
    REQUIRE(sqlite3_finalize(st) == SQLITE_OK);
    REQUIRE(!dm_mailbox_store_file(f.node[A], f.node[B]->my_ed25519_pubkey, body, bytes));
    WAIT(value(M, "SELECT count(*) FROM dm_custody WHERE ready=1") == 1 && value(B, "SELECT count(*) FROM dm_files") == 1);
    REQUIRE(value(B, "SELECT ready FROM dm_files") == 0);
    /* HAVE уже отправлен, но окно выбора источника ещё открыто. Отзыв отменяет эту пробу. */
    WAIT(value(B, "SELECT count(*) FROM dm_file_sources WHERE expires>0") == 1);
    uint64_t seq; memcpy(&seq, body + 8, 8);
    unsigned seen = f.have_seen;
    REQUIRE(!dm_media_user_action(f.node[B], f.conv, f.node[A]->my_ed25519_pubkey, seq, 0));
    WAIT(f.have_seen > seen);
    struct ETCP_CONN* transport = instance_find_conn(f.node[B], f.node[M]->node_id); REQUIRE(transport && transport->links_up);
    f.group[B][5]->stopping = f.group[M][5]->stopping = 1;
    topo_group_revalidate_members(f.group[B][5]); topo_group_revalidate_members(f.group[M][5]);
    WAIT(value(B, "SELECT count(*) FROM dm_files WHERE ready=1") == 1);
    check("revoked HAVE probe retries another group while ETCP and custody survive",
        instance_find_conn(f.node[B], f.node[M]->node_id) == transport && transport->links_up &&
        value(M, "SELECT count(*) FROM dm_custody WHERE ready=1") == 1);
    REQUIRE(sqlite3_prepare_v2(f.node[B]->topo_sqlite_db, "SELECT local_path FROM dm_files WHERE ready=1", -1, &st, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW); snprintf(path, sizeof(path), "%s", sqlite3_column_text(st, 0));
    REQUIRE(sqlite3_finalize(st) == SQLITE_OK);
    FILE* received = fopen(path, "rb"); REQUIRE(received);
    int exact = 1;
    for (unsigned i = 0; i < 131079; i++) if (fgetc(received) != (int)((i * 73 + 19) & 255)) { exact = 0; break; }
    if (exact && fgetc(received) != EOF) exact = 0;
    REQUIRE(!fclose(received));
    check("encrypted file crosses A/S, S/M, A/M, S/B and M/B groups byte for byte", exact);
    check("copy is stored once on M and one assignment exists on S", value(M, "SELECT count(*) FROM dm_custody WHERE ready=1") == 1 &&
        value(S, "SELECT count(*) FROM dm_media_holders") == 1);
    return 0;
}

static int storage_assignments(void) {
    uint8_t request[DM_MEDIA_STORE_HEADER + 8192]; sqlite3_stmt* st = NULL;
    REQUIRE(sqlite3_prepare_v2(f.node[S]->topo_sqlite_db,
        "SELECT h.assignment_id,j.body FROM dm_media_holders h JOIN dm_media_jobs j USING(id)", -1, &st, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_bytes(st, 0) == 16);
    memcpy(request, sqlite3_column_blob(st, 0), 16); memcpy(request + DM_MEDIA_STORE_RECIPIENT_OFF, f.node[B]->my_ed25519_pubkey, 32);
    int len = sqlite3_column_bytes(st, 1); REQUIRE(len > 0 && len <= 8192);
    memcpy(request + DM_MEDIA_STORE_HEADER, sqlite3_column_blob(st, 1), len); REQUIRE(sqlite3_finalize(st) == SQLITE_OK);
    int expires = value(M, "SELECT expires FROM dm_custody"), policy = value(M, "SELECT delete_on_open FROM dm_custody");
    REQUIRE(expires > 0 && policy >= 0);
    REQUIRE(!chat_setting_set(f.node[M], "dm_media_ttl_sec", "600") &&
        !chat_setting_set(f.node[M], "dm_media_delete_on_open", policy ? "no" : "yes"));
    /* После отзыва первого пути прежнее назначение продолжается через 108 при том же ETCP. */
    struct ETCP_CONN* transport = instance_find_conn(f.node[S], f.node[M]->node_id); REQUIRE(transport && transport->links_up);
    f.group[S][3]->stopping = f.group[M][3]->stopping = 1;
    topo_group_revalidate_members(f.group[S][3]); topo_group_revalidate_members(f.group[M][3]);
    uint64_t route = 0; REQUIRE(dm_route_group(f.node[S], f.node[M]->node_id, 104, &route) && route == 108);
    sqlite3* saved_db = f.node[S]->topo_sqlite_db; f.node[S]->topo_sqlite_db = NULL; route = 0;
    int live_route = dm_route_group(f.node[S], f.node[M]->node_id, 104, &route); f.node[S]->topo_sqlite_db = saved_db;
    check("revoked preferred route falls back without SQLite", live_route && route == 108);
    unsigned seen = f.media_seen[S];
    REQUIRE(!dm_media_send_key(f.node[S], 104, f.node[M]->my_ed25519_pubkey, DM_MEDIA_STORE, request, len + DM_MEDIA_STORE_HEADER));
    WAIT(f.media_seen[S] > seen);
    check("revoked S/M group keeps assignment and physical ETCP through second group",
        instance_find_conn(f.node[S], f.node[M]->node_id) == transport && transport->links_up &&
        value(M, "SELECT count(*) FROM dm_custody_requests") == 1 && value(M, "SELECT expires FROM dm_custody") == expires);
    f.group[S][7]->stopping = f.group[M][7]->stopping = 1;
    topo_group_revalidate_members(f.group[S][7]); topo_group_revalidate_members(f.group[M][7]);
    saved_db = f.node[S]->topo_sqlite_db; f.node[S]->topo_sqlite_db = NULL; route = UINT64_C(0xabcdef);
    live_route = dm_route_group(f.node[S], f.node[M]->node_id, 104, &route); f.node[S]->topo_sqlite_db = saved_db;
    check("no live route preserves caller output without SQLite", !live_route && route == UINT64_C(0xabcdef));
    check("no pair route pauses STORE without changing reservation", dm_media_send_key(f.node[S], 0, f.node[M]->my_ed25519_pubkey,
        DM_MEDIA_STORE, request, len + DM_MEDIA_STORE_HEADER) != 0 && value(M, "SELECT count(*) FROM dm_custody_requests") == 1 &&
        value(M, "SELECT expires FROM dm_custody") == expires);
    f.group[S][7]->stopping = f.group[M][7]->stopping = 0;
    topo_group_revalidate_members(f.group[S][7]); topo_group_revalidate_members(f.group[M][7]);
    /* A дополнительно становится вторым S только в своей группе с M. */
    const char* tags = "{\"supernode\":\"yes\",\"ver\":\"2\"}";
    uint8_t owner_body[256], signature[64];
    int size = member_sync_build_owner_msg(f.group[A][4]->channel_id, f.node[A]->my_ed25519_pubkey, tags, owner_body, sizeof(owner_body)); REQUIRE(size > 0);
    REQUIRE(!sc_ed25519_sign(f.keys[4].ed_private, owner_body, size, signature));
    for (unsigned i = 0; i < 2; i++) REQUIRE(test_member_seed(f.node[i ? M : A], f.group[A][4]->channel_id, f.node[A]->my_keys.public_key,
        f.node[A]->my_ed25519_pubkey, f.node[A]->my_ed25519_privkey, f.keys[4].ed_private, "{}", tags, signature) >= 0);
    REQUIRE(dm_peer_role(f.node[M], f.node[A]->node_id, "supernode"));
    memset(request, 0x55, 16); seen = f.media_seen[A];
    REQUIRE(!dm_media_send_key(f.node[A], 105, f.node[M]->my_ed25519_pubkey, DM_MEDIA_STORE, request, len + DM_MEDIA_STORE_HEADER));
    WAIT(value(M, "SELECT count(*) FROM dm_custody_requests") == 2 && f.media_seen[A] > seen);
    check("two coordinators share one copy with first deadline and policy",
        value(M, "SELECT count(*) FROM dm_custody WHERE ready=1") == 1 &&
        value(M, "SELECT expires FROM dm_custody") == expires && value(M, "SELECT delete_on_open FROM dm_custody") == policy);
    REQUIRE(!member(M, 4, A, "admitted", 0));
    /* Без polling: исходящий путь A ещё допустим, входящий M проверяет свежую строку. */
    seen = f.media_seen[M]; request[0]++;
    REQUIRE(!dm_media_send_key(f.node[A], 105, f.node[M]->my_ed25519_pubkey, DM_MEDIA_STORE, request, len + DM_MEDIA_STORE_HEADER));
    WAIT(f.media_seen[M] > seen);
    check("revoked requester cannot create another assignment", value(M, "SELECT count(*) FROM dm_custody_requests") == 2);
    REQUIRE(!member(M, 4, A, "admitted", 1));
    /* Новый объект проверяет реальный цикл S: durable отказ, пауза, новый ID на том же M. */
    f.group[S][2]->stopping = f.group[B][2]->stopping = 1;
    topo_group_revalidate_members(f.group[S][2]); topo_group_revalidate_members(f.group[B][2]);
    uint8_t* body = request + DM_MEDIA_STORE_HEADER;
    uint8_t* desc = (uint8_t*)dm_message_media(body, len); REQUIRE(desc);
    desc[0] ^= 0x80; uint64_t plain = 2 * 1024 * 1024; memcpy(desc + 16, &plain, 8);
    uint64_t seq; memcpy(&seq, body + 8, 8); seq++; memcpy(body + 8, &seq, 8);
    REQUIRE(!sc_ed25519_sign(f.node[A]->my_ed25519_privkey, body, len - DM_MSG_SIG_SIZE, body + len - DM_MSG_SIG_SIZE));
    REQUIRE(!chat_setting_set(f.node[M], "dm_media_storage_mb", "1"));
    REQUIRE(!dm_mailbox_store_file(f.node[A], f.node[B]->my_ed25519_pubkey, body, len));
    WAIT(value(S, "SELECT count(*) FROM dm_media_holders WHERE released=3") == 1);
    REQUIRE(sqlite3_prepare_v2(f.node[S]->topo_sqlite_db,
        "SELECT assignment_id,id FROM dm_media_holders WHERE released=3", -1, &st, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW);
    uint8_t old_reply[33]; memcpy(old_reply, sqlite3_column_blob(st, 0), 16); memcpy(old_reply + 16, sqlite3_column_blob(st, 1), 16);
    old_reply[32] = 1; REQUIRE(sqlite3_finalize(st) == SQLITE_OK);
    check("quota refusal is durable and schedules delayed retry", value(M, "SELECT count(*) FROM dm_custody_requests WHERE rejected=1") == 1 &&
        value(S, "SELECT count(*) FROM dm_media_jobs WHERE holder IS NULL AND retry_at>0") == 1);
    REQUIRE(!chat_setting_set(f.node[M], "dm_media_storage_mb", "1024"));
    REQUIRE(sqlite3_exec(f.node[S]->topo_sqlite_db, "UPDATE dm_media_jobs SET retry_at=0 WHERE holder IS NULL", NULL, NULL, NULL) == SQLITE_OK);
    WAIT(value(M, "SELECT count(*) FROM dm_custody") == 2 && value(S, "SELECT count(*) FROM dm_media_jobs WHERE delete_on_open>=0") == 2);
    REQUIRE(sqlite3_prepare_v2(f.node[S]->topo_sqlite_db,
        "SELECT assignment_id FROM dm_media_holders WHERE id=?", -1, &st, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_bind_blob(st, 1, old_reply + 16, 16, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    int different = sqlite3_column_bytes(st, 0) == 16 && memcmp(old_reply, sqlite3_column_blob(st, 0), 16);
    REQUIRE(sqlite3_finalize(st) == SQLITE_OK);
    check("new attempt on same storage has new ID while old refusal remains", different &&
        value(M, "SELECT count(*) FROM dm_custody_requests") == 4 && value(M, "SELECT count(*) FROM dm_custody_requests WHERE rejected=1") == 1);
    seen = f.media_seen[S];
    REQUIRE(!dm_media_send_key(f.node[M], 108, f.node[S]->my_ed25519_pubkey, DM_MEDIA_REJECT, old_reply, sizeof(old_reply)));
    WAIT(f.media_seen[S] > seen);
    check("old rejection cannot reset new reservation on same storage",
        value(S, "SELECT count(*) FROM dm_media_jobs WHERE holder IS NOT NULL AND delete_on_open>=0") == 2 &&
        value(S, "SELECT count(*) FROM dm_media_holders WHERE released<>0") == 0);
    return 0;
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
    if (getenv("UTUN_TEST_DEBUG")) {
        debug_set_category_level(DEBUG_CATEGORY_DM, DEBUG_LEVEL_TRACE);
        debug_set_category_level(DEBUG_CATEGORY_ETCPROUTE, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_BGP, DEBUG_LEVEL_INFO);
    }
    size_t before = u_get_allocated_count();
    if (setup() || run() || file_delivery() || storage_assignments()) f.failed++;
    for (unsigned i = 0; i < NODES; i++) if (f.node[i]) utun_instance_destroy(f.node[i]);
    if (f.ua) uasync_destroy(f.ua, 0);
    check("fixture releases all allocations", before == u_get_allocated_count());
    printf("test_dm_group_routes: %u PASS %u FAIL artifacts=%s\n", f.passed, f.failed, f.directory);
    return f.failed ? 1 : 0;
}
