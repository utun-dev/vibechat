// test_media_delivery_integration.c — интеграционный тест media_delivery
//
// Проверки:
//   1. media_delivery_init на реальных инстансах (таблицы созданы)
//   2. Подписанная роль суперузла CHAT-группы → обработка объявлений
//   3. SERVE_REG → добавление в served_nodes
//   4. HAVE_BLOCK → вставка в block_availability + ACK
//   5. QUERY → поиск в block_availability → QUERY_RESP
//   6. Смена подписанной роли → очистка каталога этой группы
//   7. SERVE_LEAVE → удаление из served_nodes
//   8. Выключение суперузла → чистка block_availability

#include "media_delivery.h"
#include "media_delivery_proto.h"
#include "media_index.h"
#include "../utun_instance.h"
#include "../transport_layer/etcp.h"
#include "../config_parser.h"
#include "../config_updater.h"
#include "../routing_layer/topo_group.h"
#include "../routing_layer/topo_node.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../routing_layer/route_crypto.h"
#include "../chat/member_sync.h"
#include "../chat/chat_core.h"
#include "../transport_layer/secure_channel.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_net.h"
#include "media_members_fixture.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_passed = 0, g_failed = 0, g_total = 0;
#define TEST(name) do { g_total++; printf("  %-55s", name); fflush(stdout); } while(0)
#define OK()       do { g_passed++; printf("OK\n"); } while(0)
#define FAIL(fmt, ...) do { g_failed++; printf("FAIL: " fmt "\n", ##__VA_ARGS__); } while(0)

#define TIMEOUT_TB 300000
#define POLL_MS 5
#define CHAT_GROUP 111
#define CHAT_CHANNEL "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951"
#define PRIVATE_CHANNEL "535712b7cbfa74cef7d7a3f756c41b636031da4964419ecdbc5d048a67956a16"
static const uint8_t chat_seed[32] = {94};
#include "test_membership.h"

static struct UASYNC* g_ua = NULL;
static struct UTUN_INSTANCE* g_a = NULL, *g_b = NULL;
static uint64_t g_nid_a = 0, g_nid_b = 0;
static int g_connected = 0, g_result = 0;
static char g_tdir[] = "/tmp/utun_mdl_XXXXXX";
static char g_ca[256], g_cb[256];
static uint8_t g_a_pubkey[32], g_b_pubkey[32];

/* Меняем роль через подписанный блок владельца, как в рабочем member_sync. */
static int set_super_role(int enabled) {
    static unsigned version;
    char tags[96]; uint8_t message[MS_OWNER_MSG_MAX], sig[64];
    snprintf(tags, sizeof(tags), "{\"ver\":\"%u\",\"supernode\":\"%s\"}", ++version, enabled ? "yes" : "no");
    int n = member_sync_build_owner_msg(CHAT_CHANNEL, g_a->my_ed25519_pubkey, tags, message, sizeof(message));
    if (n < 0 || sc_ed25519_sign(chat_seed, message, n, sig) != SC_OK) return -1;
    struct UTUN_INSTANCE* nodes[2] = {g_a, g_b};
    for (int i = 0; i < 2; i++) {
        if (member_sync_put(nodes[i], CHAT_CHANNEL, g_a->my_keys.public_key, g_a->my_ed25519_pubkey,
            NULL, 0, NULL, 0, "", tags, sig, 0, 0, NULL) < 0) return -1;
        media_delivery_refresh_roles(nodes[i]);
    }
    return 0;
}

static int setup_chat(void) {
    struct UTUN_INSTANCE* nodes[2] = {g_a, g_b};
    uint8_t sig[64], private_sig[64], group_x[32], group_secret[32], group_ed[32];
    struct test_group_keys private_group = {.ed_private={95}};
    if (sc_identity_from_seed(chat_seed, group_x, group_secret, group_ed) ||
        sc_identity_from_seed(private_group.ed_private, private_group.x.public_key, private_group.x.private_key, private_group.ed) || test_group_signature(PRIVATE_CHANNEL, "private media", g_a->my_ed25519_pubkey,
        private_group.x.public_key, private_group.ed, private_group.ed_private, private_sig) || test_group_signature(CHAT_CHANNEL,
        "media", g_a->my_ed25519_pubkey, group_x, group_ed, chat_seed, sig)) return -1;
    for (int i = 0; i < 2; i++) {
        struct UTUN_INSTANCE* inst = nodes[i];
        if (member_sync_init(inst) < 0 || topo_node_sqlite_channel_put(inst->topo_sqlite_db, CHAT_CHANNEL, "media", g_a->my_ed25519_pubkey,
            group_x, NULL, group_ed, NULL, sig) < 0) return -1;
        if (!topo_groups_create_group(inst->topo_groups, CHAT_GROUP, TOPO_GROUP_TYPE_CHAT, CHAT_CHANNEL)) return -1;
        for (int j = 0; j < 2; j++) if (test_member_seed(inst, CHAT_CHANNEL, nodes[j]->my_keys.public_key,
            nodes[j]->my_ed25519_pubkey, nodes[j]->my_ed25519_privkey, chat_seed, "{}", NULL, NULL) < 0) return -1;
        if (topo_node_sqlite_channel_put(inst->topo_sqlite_db, PRIVATE_CHANNEL, "private media", g_a->my_ed25519_pubkey,
                private_group.x.public_key, NULL, private_group.ed, NULL, private_sig) < 0
            || !topo_groups_create_group(inst->topo_groups, 222, TOPO_GROUP_TYPE_CHAT, PRIVATE_CHANNEL)) return -1;
        const char* tags = "{\"ver\":\"1\",\"supernode\":\"yes\"}";
        uint8_t owner_message[MS_OWNER_MSG_MAX], owner_sig[64];
        int bytes = member_sync_build_owner_msg(PRIVATE_CHANNEL, g_a->my_ed25519_pubkey, tags, owner_message, sizeof(owner_message));
        if (bytes < 0 || sc_ed25519_sign(private_group.ed_private, owner_message, bytes, owner_sig) != SC_OK) return -1;
        for (int j = 0; j < 2; j++) if (test_member_seed(inst, PRIVATE_CHANNEL, nodes[j]->my_keys.public_key,
            nodes[j]->my_ed25519_pubkey, nodes[j]->my_ed25519_privkey, private_group.ed_private,
            "{}", j ? NULL : tags, j ? NULL : owner_sig) < 0) return -1;
    }
    return set_super_role(1);
}

/* ── helpers ── */

static int wf(const char* p, const char* f, ...) {
    va_list ap; FILE* fp = fopen(p, "w"); if (!fp) return -1;
    va_start(ap, f); vfprintf(fp, f, ap); va_end(ap); fclose(fp); return 0;
}

static char* gv(const char* p, const char* k) {
    struct utun_config* c = parse_config(p); if (!c) return NULL;
    char* r = (strcmp(k, "pub") == 0) ? u_strdup(c->global.my_public_key_hex) : u_strdup(c->global.my_private_key_hex);
    free_config(c); return r;
}

static void test_mkdtemp(char* tmpl) {
#ifdef _WIN32
    char tmp_path[512]; GetTempPathA(sizeof(tmp_path), tmp_path);
    snprintf(tmpl, 256, "%s\\utun_test_%08x", tmp_path, (unsigned)rand());
    _mkdir(tmpl);
#else
    (void)!mkdtemp(tmpl);
#endif
}

static void test_unlink(const char* p) { unlink(p); }
static void test_rmdir(const char* p) {
    char cmd[512]; snprintf(cmd, sizeof(cmd), "rm -rf %s", p); system(cmd);
}

/* ── helpers ── */

static int count_holders(sqlite3* db, uint64_t group, const uint8_t ed[32]) {
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT count(*) FROM block_availability WHERE group_id=? AND holder=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, group == CHAT_GROUP ? CHAT_CHANNEL : PRIVATE_CHANNEL, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int count = rc == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
    return sqlite3_finalize(st) == SQLITE_OK ? count : -1;
}

static int count_rows(sqlite3* db, const char* tbl, const char* where, uint64_t val) {
    char sql[256];
    if (where) snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE %s=?", tbl, where);
    else snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", tbl);
    sqlite3_stmt* s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    int rc = !where ? SQLITE_OK : !strcmp(where, "group_id") ?
        sqlite3_bind_text(s, 1, val == CHAT_GROUP ? CHAT_CHANNEL : PRIVATE_CHANNEL, -1, SQLITE_STATIC) : sqlite3_bind_int64(s, 1, (sqlite3_int64)val);
    if (rc != SQLITE_OK) { sqlite3_finalize(s); return -1; }
    int n = -1;
    if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return n;
}

static void to_cb(void* arg) { (void)arg; fprintf(stderr, "TIMEOUT\n"); g_result = 2; }

/* ── monitor: ждём ETCP и BGP ── */

static void mon(void* arg) {
    struct UTUN_INSTANCE* inst = (struct UTUN_INSTANCE*)arg;
    (void)inst;
    if (!g_connected) {
        int ok = 0;
        { struct ll_entry* e = g_a->connections->head;
          while (e) { struct conn_queue_entry* ce = (struct conn_queue_entry*)e->data;
            if (ce->conn && ce->conn->links) { struct ETCP_LINK* l; for (l = ce->conn->links; l; l = l->next)
              if (l->initialized && ce->conn->crypto_ctx.initialized) ok++; } e = e->next; } }
        { struct ll_entry* e = g_b->connections->head;
          while (e) { struct conn_queue_entry* ce = (struct conn_queue_entry*)e->data;
            if (ce->conn && ce->conn->links) { struct ETCP_LINK* l; for (l = ce->conn->links; l; l = l->next)
              if (l->initialized && ce->conn->crypto_ctx.initialized) ok++; } e = e->next; } }
        if (ok >= 2) g_connected = 1;
    }
    if (!g_result) uasync_set_timeout(g_ua, 10, NULL, mon, "mdl_mon");
}

/* ── send helper ── */

static int msend(struct UTUN_INSTANCE* inst, uint64_t dst, const uint8_t* data, size_t len) {
    struct ll_entry* e = queue_entry_new(0);
    if (!e) return -1;
    e->dgram = u_malloc(len + 1); if (!e->dgram) { queue_entry_free(e); return -1; }
    e->dgram[0] = ETCP_RT_ID_MEDIA_DELIVERY;
    memcpy(e->dgram + 1, data, len); e->len = (uint16_t)(len + 1);
    int rc = etcp_route_send(inst, CHAT_GROUP, dst, e, 1, ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED);
    return rc;
}

static int query_received, query_valid;
static void (*saved_media_recv)(struct ETCP_CONN*, struct ll_entry*);

/* Перехватываем только ожидаемый ответ; прочие пакеты обрабатывает настоящий сервис. */
static void query_recv(struct ETCP_CONN* conn, struct ll_entry* entry) {
    if (entry->len < ROUTER_SVC_PAYLOAD_OFF + 1 || entry->dgram[ROUTER_SVC_PAYLOAD_OFF] != MEDIA_SUBCMD_QUERY_RESP) {
        saved_media_recv(conn, entry); return;
    }
    query_received++;
    size_t len = entry->len - ROUTER_SVC_PAYLOAD_OFF;
    uint64_t group;
    memcpy(&group, entry->dgram + ROUTER_SVC_GROUP_OFF, 8);
    if (len == MEDIA_QUERY_RESP_HDR_SIZE + sizeof(struct media_pkt_query_resp_entry)) {
        const struct media_pkt_query_resp* r = (const struct media_pkt_query_resp*)(entry->dgram + ROUTER_SVC_PAYLOAD_OFF);
        const struct media_pkt_query_resp_entry* e = (const struct media_pkt_query_resp_entry*)((const uint8_t*)r + MEDIA_QUERY_RESP_HDR_SIZE);
        uint8_t block[16], request[16];
        for (int i = 0; i < 16; i++) block[i] = (uint8_t)(0x10 + i);
        memset(request, 0x91, 16);
        query_valid = !memcmp(entry->dgram + ROUTER_SVC_SRC_ED_OFF, g_a->my_ed25519_pubkey, 32) && group == CHAT_GROUP && r->status == MEDIA_QUERY_OK && r->num_blocks == 1
            && !memcmp(r->request_id, request, 16) && !memcmp(r->media_id, block, 16) && !memcmp(e->block_id, block, 16)
            && e->num_holders == 1 && !memcmp(e->holders[0], g_a->my_ed25519_pubkey, 32);
    }
    queue_dgram_free(entry); queue_entry_free(entry);
}

/* ── tests ── */

static void phase1_serve_reg(void) {
    TEST("SERVE_REG B→A adds to served_nodes"); {
        struct media_pkt_serve_reg pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.subcmd = MEDIA_SUBCMD_SERVE_REG;
        memcpy(pkt.group_ed, topo_group_wire_id(topo_groups_find(g_b->topo_groups, CHAT_GROUP)), sizeof(pkt.group_ed));

        int rc = msend(g_b, g_nid_a, (const uint8_t*)&pkt, sizeof(pkt));
        /* BGP routes need time — poll up to 10s */
        int attempts = 0;
        while (attempts < 2000) {
            int has = g_a->md.served_nodes && g_a->md.served_nodes->head != NULL;
            if (has) break;
            uasync_poll(g_ua, POLL_MS); attempts++;
        }
        int ok = g_a->md.served_nodes && g_a->md.served_nodes->head != NULL;
        if (ok) OK(); else FAIL("rc=%d B not in served_nodes after %d ms", rc, attempts * POLL_MS);
    }
}

static void phase2_have_block(void) {
    TEST("HAVE_BLOCK B→A inserts into block_availability"); {
        uint8_t block_uuid[16]; for (int i = 0; i < 16; i++) block_uuid[i] = (uint8_t)(0x10 + i);
        uint64_t group_id = CHAT_GROUP;
        struct media_index_result parent = {.file_size=1}; memcpy(parent.media_id, block_uuid, 16);
        media_test_publish_channel(g_a->topo_sqlite_db, CHAT_CHANNEL, g_nid_b, &parent);
        media_test_publish_channel(g_b->topo_sqlite_db, CHAT_CHANNEL, g_nid_b, &parent);

        media_delivery_send_have_block(g_b, group_id, g_a->my_ed25519_pubkey, block_uuid, block_uuid, 0);
        int attempts = 0;
        while (attempts < 2000) {
            int n = count_holders(g_a->md.db, CHAT_GROUP, g_b->my_ed25519_pubkey);
            if (n >= 1) break;
            uasync_poll(g_ua, POLL_MS); attempts++;
        }
        int n = count_holders(g_a->md.db, CHAT_GROUP, g_b->my_ed25519_pubkey);
        if (n >= 1) OK(); else FAIL("block not in A's DB (n=%d after %d ms)", n, attempts * POLL_MS);
    }
}

static void phase3_query(void) {
    TEST("QUERY B→A returns block holder"); {
        uint8_t block_uuid[16]; for (int i = 0; i < 16; i++) block_uuid[i] = (uint8_t)(0x10 + i);
        uint8_t pkt[MEDIA_QUERY_HDR_SIZE + 16];
        struct media_pkt_query* q = (struct media_pkt_query*)pkt;
        memset(q, 0, sizeof(*q));
        q->subcmd = MEDIA_SUBCMD_QUERY;
        memset(q->request_id, 0x91, sizeof(q->request_id));
        memcpy(q->media_id, block_uuid, 16);
        q->num_blocks = 1;
        memcpy(pkt + MEDIA_QUERY_HDR_SIZE, block_uuid, 16);
        media_delivery_send_have_block(g_a, CHAT_GROUP, g_a->my_ed25519_pubkey, block_uuid, block_uuid, 0);
        saved_media_recv = g_b->router_bindings.callbacks[ETCP_RT_ID_MEDIA_DELIVERY];
        g_b->router_bindings.callbacks[ETCP_RT_ID_MEDIA_DELIVERY] = query_recv;
        query_received = query_valid = 0;
        int rc = msend(g_b, g_nid_a, pkt, sizeof(pkt)), poll_count = 0;
        while (!query_received && poll_count++ < 2000) uasync_poll(g_ua, POLL_MS);
        g_b->router_bindings.callbacks[ETCP_RT_ID_MEDIA_DELIVERY] = saved_media_recv;
        if (!rc && query_received == 1 && query_valid) OK();
        else FAIL("send=%d received=%d valid=%d", rc, query_received, query_valid);
    }
}

static void phase4_serve_leave(void) {
    TEST("SERVE_LEAVE B→A removes from served_nodes"); {
        struct media_pkt_serve_leave pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.subcmd = MEDIA_SUBCMD_SERVE_LEAVE;
        memcpy(pkt.group_ed, topo_group_wire_id(topo_groups_find(g_b->topo_groups, CHAT_GROUP)), sizeof(pkt.group_ed));

        msend(g_b, g_nid_a, (const uint8_t*)&pkt, sizeof(pkt));
        int attempts = 0;
        while (attempts < 50) {
            int has = g_a->md.served_nodes && g_a->md.served_nodes->head != NULL;
            if (!has) break;
            uasync_poll(g_ua, POLL_MS); attempts++;
        }
        int has = g_a->md.served_nodes && g_a->md.served_nodes->head != NULL;
        if (!has) OK(); else FAIL("B still in served_nodes");
    }
}

static void phase5_supernode_stop(void) {
    TEST("supernode STOP cleans remote catalog entries"); {
        if (set_super_role(0)) { FAIL("role update failed"); return; }
        int n = count_holders(g_a->md.db, CHAT_GROUP, g_b->my_ed25519_pubkey);
        if (n == 0) OK(); else FAIL("%d entries remain", n);
    }

    TEST("supernode START re-enables"); {
        if (set_super_role(1)) { FAIL("role update failed"); return; }
        if (g_a->md.is_supernode) OK(); else FAIL();
    }
}

static etcp_recv_fn raw_receiver, media_receiver;
static unsigned wire_received, service_received;
static uint8_t attack_wire[1024];
static size_t attack_wire_len;
static void observe_wire(struct ETCP_CONN* conn, struct ll_entry* entry) {
    if (entry->len == attack_wire_len && !memcmp(entry->dgram, attack_wire, attack_wire_len)) wire_received++;
    raw_receiver(conn, entry);
}
static void observe_media(struct ETCP_CONN* conn, struct ll_entry* entry) {
    uint64_t group = 0;
    if (entry->len > ROUTER_SVC_PAYLOAD_OFF) memcpy(&group, entry->dgram + ROUTER_SVC_GROUP_OFF, 8);
    if (group == 222) service_received++;
    media_receiver(conn, entry);
}

/* Настоящая UDP/ETCP-доставка. Обходим только локальную исходящую политику атакующего узла. */
static int attack_packet(struct SVC_ROUTE_HDR h, const void* payload, size_t len, int mode, int tamper) {
    struct ETCP_CONN* conn = NULL;
    for (struct ll_entry* e = g_b->connections->head; e; e = e->next) {
        struct conn_queue_entry* ce = (void*)e->data;
        if (ce->conn && ce->conn->peer_node_id == g_nid_a) { conn = ce->conn; break; }
    }
    if (!conn) return -1;
    uint8_t base[SVC_ROUTE_HDR_SIZE + 256];
    if (len > 256) return -1;
    memcpy(base, &h, sizeof(h)); if (len) memcpy(base + sizeof(h), payload, len);
    uint8_t* encoded = NULL; size_t bytes = 0;
    if (route_crypto_encode(g_b, ROUTE_CRYPTO_CHAT, g_a->my_keys.public_key,
        base, sizeof(h) + len, mode, &encoded, &bytes)) return -1;
    if (tamper == 1) ((struct SVC_ROUTE_HDR*)encoded)->src_node_id = g_nid_a;
    if (tamper == 2) ((struct SVC_ROUTE_HDR*)encoded)->group_id = CHAT_GROUP;
    struct ll_entry* e = ll_alloc_lldgram(bytes + 9);
    if (!e) { u_free(encoded); return -1; }
    memcpy(e->dgram, encoded, bytes); u_free(encoded);
    memcpy(e->dgram + bytes, &g_nid_b, 8); e->dgram[bytes + 8] = 1; e->len = bytes + 9;
    attack_wire_len = e->len; memcpy(attack_wire, e->dgram, attack_wire_len);
    unsigned before = wire_received;
    if (etcp_send(conn, e)) { queue_dgram_free(e); queue_entry_free(e); return -1; }
    for (int i = 0; wire_received == before && i < 1000; i++) uasync_poll(g_ua, POLL_MS);
    return wire_received > before ? 0 : -1;
}

static void phase_group_isolation(void) {
    TEST("real ETCP: establish a media channel in neighbouring group");
    if (count_rows(g_a->md.db, "peers_" PRIVATE_CHANNEL "", "node_type", 4) != 1) { FAIL("missing signed supernode role"); return; }
    uint8_t query[MEDIA_QUERY_HDR_SIZE + 16] = {0};
    struct media_pkt_query* q = (void*)query;
    q->subcmd = MEDIA_SUBCMD_QUERY; q->num_blocks = 1; memset(q->media_id, 0x41, 16);
    memset(q->request_id, 0x42, 16); memset(query + MEDIA_QUERY_HDR_SIZE, 0x43, 16);
    char path[512]; snprintf(path, sizeof(path), "%s/db_a/secret.bin", g_tdir);
    FILE* file = fopen(path, "wb");
    uint8_t hash[32] = {0};
    if (!file || fputc(0x5a, file) != 0x5a || fclose(file)
        || media_index_register_downloaded(g_a->md.db, q->media_id, query + MEDIA_QUERY_HDR_SIZE,
            hash, PRIVATE_CHANNEL, "secret.bin", g_a->my_ed25519_pubkey, 1, 1, 0, 0)) { FAIL("private file setup"); return; }
    saved_media_recv = g_b->router_bindings.callbacks[ETCP_RT_ID_MEDIA_DELIVERY];
    g_b->router_bindings.callbacks[ETCP_RT_ID_MEDIA_DELIVERY] = query_recv;
    query_received = 0;
    int rc = md_send_key(g_b, 222, g_a->my_ed25519_pubkey, query, sizeof(query), 1);
    for (int i = 0; !query_received && i < 2000; i++) uasync_poll(g_ua, POLL_MS);
    g_b->router_bindings.callbacks[ETCP_RT_ID_MEDIA_DELIVERY] = saved_media_recv;
    if (rc || query_received != 1) { FAIL("send=%d responses=%d", rc, query_received); return; }
    struct ETCP_ROUTER_CONN* c = etcp_router_conn_get(g_a, 222, g_nid_b, ETCP_RT_ID_MEDIA_DELIVERY);
    if (!c || !c->peer_reset_id) { FAIL("no established channel"); return; }
    OK();
    // Меняем только подтверждённый результат membership на получателе, не закрывая старую сессию:
    // каждый следующий wire-пакет обязан заново проверить доступ.
    char sql[256]; snprintf(sql, sizeof(sql), "UPDATE peers_" PRIVATE_CHANNEL " SET admitted=0 WHERE node_id=%lld", (long long)g_nid_b);
    if (sqlite3_exec(g_a->topo_sqlite_db, sql, NULL, NULL, NULL) != SQLITE_OK) { TEST("revoke membership"); FAIL("SQL"); return; }
    if (test_member_publish_access(g_a, PRIVATE_CHANNEL)) { TEST("publish revocation"); FAIL("snapshot"); return; }
    raw_receiver = g_a->api_bindings.callbacks[ETCP_ID_SVC_ROUTE];
    media_receiver = g_a->router_bindings.callbacks[ETCP_RT_ID_MEDIA_DELIVERY];
    g_a->api_bindings.callbacks[ETCP_ID_SVC_ROUTE] = observe_wire;
    g_a->router_bindings.callbacks[ETCP_RT_ID_MEDIA_DELIVERY] = observe_media;
    service_received = 0;
    struct SVC_ROUTE_HDR h = {.cmd = ETCP_ID_SVC_ROUTE, .group_id = 222, .src_node_id = g_nid_b, .dst_node_id = g_nid_a,
        .svc_id = ETCP_RT_ID_MEDIA_DELIVERY, .reset_id = c->peer_reset_id, .peer_reset_id = c->reset_id, .seq = c->rx_seq};
    int modes = ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED;
    TEST("foreign group QUERY cannot disclose holders");
    if (!attack_packet(h, query, sizeof(query), modes, 0) && !service_received && c->rx_seq == h.seq) OK();
    else FAIL("handler=%u seq=%u", service_received, c->rx_seq);
    struct media_pkt_block_req req = {.subcmd = MEDIA_SUBCMD_BLOCK_REQ};
    memcpy(req.media_id, q->media_id, 16); memcpy(req.block_id, query + MEDIA_QUERY_HDR_SIZE, 16); memset(req.transfer_id, 7, 16);
    unsigned streams = g_a->md.streams_started;
    TEST("foreign group BLOCK_REQ cannot start file transfer");
    if (!attack_packet(h, &req, sizeof(req), modes, 0) && !service_received && g_a->md.streams_started == streams) OK();
    else FAIL("handler=%u streams=%u", service_received, g_a->md.streams_started);
    struct media_pkt_have_block hb = {.subcmd = MEDIA_SUBCMD_HAVE_BLOCK, .timestamp = 1};
    memcpy(hb.media_id, q->media_id, 16); memcpy(hb.block_id, req.block_id, 16);
    int rows = count_rows(g_a->md.db, "block_availability", "group_id", 222);
    TEST("foreign group HAVE_BLOCK cannot write catalog");
    if (!attack_packet(h, &hb, sizeof(hb), modes, 0) && !service_received
        && count_rows(g_a->md.db, "block_availability", "group_id", 222) == rows) OK();
    else FAIL("handler=%u", service_received);
    TEST("signed sender/group substitution is rejected");
    rc = attack_packet(h, query, sizeof(query), modes, 1);
    rc |= attack_packet(h, query, sizeof(query), modes, 2);
    if (!rc && !service_received && c->rx_seq == h.seq) OK(); else FAIL("rc=%d handler=%u", rc, service_received);
    TEST("revocation closes only the affected group channel");
    struct ETCP_ROUTER_CONN* neighbour = etcp_router_conn_get(g_a, CHAT_GROUP, g_nid_b, ETCP_RT_ID_MEDIA_DELIVERY);
    etcp_router_revalidate_group(g_a, 222);
    if (c->closed && neighbour && !neighbour->closed && !etcp_router_conn_get(g_a, 222, g_nid_b, ETCP_RT_ID_MEDIA_DELIVERY)) OK();
    else FAIL("wrong channel closure");
    TEST("foreign group cannot reopen with a signed HELLO");
    h.flags = ROUTER_FLAG_START; h.peer_reset_id = h.challenge = 0;
    if (!attack_packet(h, NULL, 0, ROUTER_FLAG_SIGNED, 0)
        && !etcp_router_conn_get(g_a, 222, g_nid_b, ETCP_RT_ID_MEDIA_DELIVERY)) OK(); else FAIL("reopened");
    g_a->api_bindings.callbacks[ETCP_ID_SVC_ROUTE] = raw_receiver;
    g_a->router_bindings.callbacks[ETCP_RT_ID_MEDIA_DELIVERY] = media_receiver;
    phase3_query(); // Положительный контроль после всех атак: соседняя группа и транспорт живы.
}

/* ── main ── */

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    utun_instance_set_tun_init_enabled(0);
    srand((unsigned)time(NULL));

    printf("=== test_media_delivery_integration ===\n");
    fflush(stdout);

    /* ── trace for debugging etcp_route_send ── */
    debug_config_init();
    debug_set_level(getenv("UTUN_TEST_DEBUG") ? DEBUG_LEVEL_DEBUG : DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_ETCP, DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_BGP, DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_TIMERS, DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_SOCKET, DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_TRACE);
    debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_TRACE);

    utun_instance_set_tun_init_enabled(0);

    test_mkdtemp(g_tdir);
    char db_a[320], db_b[320];
    snprintf(db_a, sizeof(db_a), "%s/db_a", g_tdir); utun_mkdir(db_a, 0755);
    snprintf(db_b, sizeof(db_b), "%s/db_b", g_tdir); utun_mkdir(db_b, 0755);
    snprintf(g_ca, sizeof(g_ca), "%s/a.conf", g_tdir);
    snprintf(g_cb, sizeof(g_cb), "%s/b.conf", g_tdir);

    /* create configs with db_path */
    wf(g_ca, "[global]\ntun_ip=10.98.1.1/24\ntun_ifname=tun90\ndb_path=%s\n"
        "[server: s1]\naddr=127.0.0.1:0\ntype=public\n"
        "[allowed_keys]\nallow_all=1\n", db_a);
    wf(g_cb, "[global]\ntun_ip=10.98.1.2/24\ntun_ifname=tun91\ndb_path=%s\n"
        "[server: s1]\naddr=127.0.0.1:0\ntype=public\n"
        "[allowed_keys]\nallow_all=1\n", db_b);

    /* generate keys */
    config_ensure_keys(g_ca); config_ensure_keys(g_cb);
    { struct utun_config* ca = parse_config(g_ca); g_nid_a = test_transport_id(ca); free_config(ca);
      struct utun_config* cb = parse_config(g_cb); g_nid_b = test_transport_id(cb); free_config(cb); }

    char *p0 = gv(g_ca,"pub"), *r0 = gv(g_ca,"priv");
    char *p1 = gv(g_cb,"pub"), *r1 = gv(g_cb,"priv");
    wf(g_ca, "[global]\nmy_private_key=%s\nmy_public_key=%s\ntun_ip=10.98.1.1/24\ntun_ifname=tun90\n"
        "db_path=%s\n[server: s1]\naddr=127.0.0.1:0\ntype=public\n"
        "[client: to_b]\nkeepalive=1\npeer_public_key=%s\nlink=s1:127.0.0.1:0\n"
        "[allowed_keys]\nallow_all=1\n", r0, p0, db_a, p1);
    wf(g_cb, "[global]\nmy_private_key=%s\nmy_public_key=%s\ntun_ip=10.98.1.2/24\ntun_ifname=tun91\n"
        "db_path=%s\n[server: s1]\naddr=127.0.0.1:0\ntype=public\n"
        "[allowed_keys]\nallow_all=1\n", r1, p1, db_b);
    u_free(p0); u_free(r0); u_free(p1); u_free(r1);

    /* create instances */
    g_ua = uasync_create();
    g_a = utun_instance_create(g_ua, g_ca); g_b = utun_instance_create(g_ua, g_cb);
    if (!g_a || !g_b) { FAIL("instance create"); goto done; }
    test_etcp_set_peer(g_a, "to_b", g_b, "s1");
    utun_instance_init(g_a); utun_instance_init(g_b);
    char chat_db_a[512], chat_db_b[512];
    snprintf(chat_db_a, sizeof(chat_db_a), "%s/chats.db", db_a);
    snprintf(chat_db_b, sizeof(chat_db_b), "%s/chats.db", db_b);
    if (chat_core_init(g_a, chat_db_a) || chat_core_init(g_b, chat_db_b)
        || media_delivery_init(g_a) || media_delivery_init(g_b)) { FAIL("chat/media initialization"); goto done; }

    if (setup_chat()) { FAIL("CHAT setup failed"); goto done; }

    /* wait for ETCP connection */
    mon(NULL);
    uasync_set_timeout(g_ua, TIMEOUT_TB, NULL, (timeout_callback_t)to_cb, "mdl_to");
    {
        int el = 0;
        while (!g_connected && !g_result && el < TIMEOUT_TB + 5000) {
            uasync_poll(g_ua, POLL_MS); el += POLL_MS;
        }
        if (!g_connected) { FAIL("connection timeout"); goto done; }
    }

    printf("  connected — running phases\n");

    phase1_serve_reg();
    phase2_have_block();
    phase3_query();
    phase_group_isolation();
    phase4_serve_leave();
    phase5_supernode_stop();

    printf("\n%d/%d passed, %d failed\n", g_passed, g_total, g_failed);

done:
    if (g_a) { g_a->running = 0; utun_instance_destroy(g_a); g_a = NULL; }
    if (g_b) { g_b->running = 0; utun_instance_destroy(g_b); g_b = NULL; }
    if (g_ua) { uasync_destroy(g_ua, 0); g_ua = NULL; }
    test_rmdir(g_tdir);
    return g_failed > 0 ? 1 : 0;
}
