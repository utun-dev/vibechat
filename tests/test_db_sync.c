// test_db_sync.c — реальный MESSAGES/PUSH: пустой пир, новые записи, расхождение и слияние истории.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include "../lib/platform_compat.h"
#include "test_utils.h"
#include "test_net.h"
#include "db_sync_fixture.h"
#include "test_membership.h"
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "etcp.h"
#include "etcp_connections.h"
#include "../src/config_parser.h"
#include "../src/config_updater.h"
#include "../src/utun_instance.h"
#include "routing.h"
#include "topo_node.h"
#include "topo_node_sqlite.h"
#include "../src/routing_layer/topo_group.h"
#include "../src/tun_if.h"
#include "secure_channel.h"
#include "secure_channel.h"
#include "../src/chat/db_sync.h"
#include "../src/chat/merkle_sync.h"
#include "../src/chat/chat_feedback.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

#define TEST_TIMEOUT_TB  1200000 // 120s на полный сценарий, включая инструментированные длинные истории.
#define PHASE_TIMEOUT_TB 100000  // 10s per phase
#define LONG_HISTORY_TIMEOUT_TB 300000 // 30s на preflight и обмен 200 подписанными записями.
#define POLL_INTERVAL_MS 1

static struct UTUN_INSTANCE* inst_a = NULL;
static struct UTUN_INSTANCE* inst_b = NULL;
static struct UTUN_INSTANCE* inst_c = NULL;
static struct DB_SYNC_INSTANCE* si_a = NULL;
static struct DB_SYNC_INSTANCE* si_b = NULL;
static struct DB_SYNC_INSTANCE* si_c = NULL;
static struct UASYNC* ua = NULL;
static int test_phase = 0;
static void* timeout_id = NULL;

static volatile int g_done_a = 0, g_done_b = 0, g_done_c = 0;

static void test_done_cb(struct DB_SYNC_INSTANCE* si, const uint8_t peer_node_id[32], void* arg) {
    (void)peer_node_id; (void)si;
    int idx = (int)(intptr_t)arg;
    if (idx == 0) g_done_a = 1;
    else if (idx == 1) g_done_b = 1;
    else if (idx == 2) g_done_c = 1;
}
static void reset_done_flags(void) { g_done_a = g_done_b = g_done_c = 0; }

static char temp_dir[] = "/tmp/utun_dbsync_XXXXXX";
static char config_a[256], config_b[256], config_c[256];
static uint32_t ca_target, cb_target, cc_target;
static uint64_t delivery_timestamp;
static uint64_t expected_revision;
static int history_ready(struct DB_SYNC_INSTANCE* si);

/* Оба сообщения одного автора в одной ms получают отдельные реальные ACK. */
static int cond_delivery(void) {
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(inst_a->topo_sqlite_db,
        "SELECT count(*) FROM push_t WHERE timestamp=? AND accepted_key=? AND delivered_peers=1 AND (flags & 1)=1", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)delivery_timestamp);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(stmt, 2, inst_a->my_ed25519_pubkey, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    int complete = rc == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 2;
    if (rc != SQLITE_ROW) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "delivery test lookup rc=%d: %s", rc, sqlite3_errmsg(inst_a->topo_sqlite_db));
    sqlite3_finalize(stmt); return complete;
}

static int write_file(const char* path, const char* fmt, ...) {
    va_list ap; FILE* f = fopen(path, "w");
    if (!f) return -1;
    va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fclose(f); return 0;
}
static char* get_pubkey(const char* path) {
    struct utun_config* cfg = parse_config(path);
    if (!cfg) return NULL;
    char* pub = strdup(cfg->global.my_public_key_hex);
    free_config(cfg); return pub;
}

static int create_temp_configs(void) {
    if (test_mkdtemp(temp_dir) != 0) { fprintf(stderr, "mkdtemp failed\n"); return -1; }
    snprintf(config_a, sizeof(config_a), "%s/a.conf", temp_dir);
    snprintf(config_b, sizeof(config_b), "%s/b.conf", temp_dir);
    snprintf(config_c, sizeof(config_c), "%s/c.conf", temp_dir);
    char db_path[320];
    snprintf(db_path, sizeof(db_path), "%s/db_a", temp_dir); utun_mkdir(db_path, 0755);
    snprintf(db_path, sizeof(db_path), "%s/db_b", temp_dir); utun_mkdir(db_path, 0755);
    snprintf(db_path, sizeof(db_path), "%s/db_c", temp_dir); utun_mkdir(db_path, 0755);

    write_file(config_a,
        "[global]\ntun_ip=10.200.0.1/24\n"
        "tun_ifname=tun200\nkeepalive_adaptive=0\ndb_path=%s/db_a\ndb_sync_enabled=1\n\n"
        "[server: srv_a]\naddr=127.0.0.1:0\ntype=public\n\n"
        "[allowed_keys]\nallow_all=1\n", temp_dir);
    config_ensure_keys(config_a);
    char* pub_a = get_pubkey(config_a); if (!pub_a) { fprintf(stderr, "pub_a fail\n"); return -1; }

    write_file(config_b,
        "[global]\ntun_ip=10.200.0.2/24\n"
        "tun_ifname=tun201\nkeepalive_adaptive=0\ndb_path=%s/db_b\ndb_sync_enabled=1\n\n"
        "[server: srv_b]\naddr=127.0.0.1:0\ntype=public\n\n"
        "[client: to_a]\nkeepalive=1\npeer_public_key=%s\nlink=srv_b:127.0.0.1:0\n\n"
        "[allowed_keys]\nallow_all=1\n", temp_dir, pub_a);

    write_file(config_c,
        "[global]\ntun_ip=10.200.0.3/24\n"
        "tun_ifname=tun202\nkeepalive_adaptive=0\ndb_path=%s/db_c\ndb_sync_enabled=1\n\n"
        "[server: srv_c]\naddr=127.0.0.1:0\ntype=public\n\n"
        "[client: to_a]\nkeepalive=1\npeer_public_key=%s\nlink=srv_c:127.0.0.1:0\n\n"
        "[allowed_keys]\nallow_all=1\n", temp_dir, pub_a);
    free(pub_a);
    config_ensure_keys(config_b);
    config_ensure_keys(config_c);
    return 0;
}

static void cleanup_temp_configs(void) {
    unlink(config_a); unlink(config_b); unlink(config_c);
    char pa[320];
    snprintf(pa, sizeof(pa), "%s/db_a/chats.db", temp_dir); unlink(pa);
    snprintf(pa, sizeof(pa), "%s/db_a/chats.db-wal", temp_dir); unlink(pa);
    snprintf(pa, sizeof(pa), "%s/db_a/chats.db-shm", temp_dir); unlink(pa);
    snprintf(pa, sizeof(pa), "%s/db_b/chats.db", temp_dir); unlink(pa);
    snprintf(pa, sizeof(pa), "%s/db_b/chats.db-wal", temp_dir); unlink(pa);
    snprintf(pa, sizeof(pa), "%s/db_b/chats.db-shm", temp_dir); unlink(pa);
    snprintf(pa, sizeof(pa), "%s/db_c/chats.db", temp_dir); unlink(pa);
    snprintf(pa, sizeof(pa), "%s/db_c/chats.db-wal", temp_dir); unlink(pa);
    snprintf(pa, sizeof(pa), "%s/db_c/chats.db-shm", temp_dir); unlink(pa);
    snprintf(pa, sizeof(pa), "%s/db_a", temp_dir); test_rmdir(pa);
    snprintf(pa, sizeof(pa), "%s/db_b", temp_dir); test_rmdir(pa);
    snprintf(pa, sizeof(pa), "%s/db_c", temp_dir); test_rmdir(pa);
    test_rmdir(temp_dir);
}

static void test_timeout(void* arg) {
    (void)arg; DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "db_sync test global timeout budget_ms=%u", TEST_TIMEOUT_TB / 10); test_phase = 2;
}

static int wait_for(const char* desc, int (*cond)(void), int timeout_tb) {
    uint64_t start = get_time_tb();
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "db_sync test wait phase=%s budget_ms=%u", desc, timeout_tb / 10);
    while (!cond() && (get_time_tb() - start) < (uint64_t)timeout_tb && test_phase == 0)
        uasync_poll(ua, POLL_INTERVAL_MS);
    uint64_t elapsed_ms = (get_time_tb() - start) / 10;
    if (cond()) {
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "db_sync test wait completed phase=%s elapsed_ms=%llu budget_ms=%u",
            desc, (unsigned long long)elapsed_ms, timeout_tb / 10); return 1;
    }
    if (test_phase == 0) {
        fprintf(stderr, "TIMEOUT: %s elapsed_ms=%llu budget_ms=%u — ca=%u/%u cb=%u/%u cc=%u/%u\n", desc,
                (unsigned long long)elapsed_ms, timeout_tb / 10,
                si_a ? db_sync_count(si_a) : 0, ca_target,
                si_b ? db_sync_count(si_b) : 0, cb_target,
                si_c ? db_sync_count(si_c) : 0, cc_target);
        test_phase = 2;
    }
    return 0;
}

static int cond_links_init(void) {
    if (!inst_a || !inst_b) return 0;
    struct ll_entry* e = inst_a->connections->head;
    int links = 0;
    while (e) { struct conn_queue_entry* ce = (struct conn_queue_entry*)e->data;
        struct ETCP_LINK* l = ce->conn->links; while (l) { if (l->initialized) links++; l = l->next; }
        e = e->next; }
    return links >= (inst_c ? 2 : 1);
}
static int pubkey_known(struct UTUN_INSTANCE* inst, uint64_t node_id) {
    if (!inst || !inst->topo_groups || !node_id) return 0;
    struct TOPO_NODE* ni = topo_node_registry_find(inst->topo_groups, node_id);
    if (!ni) return 0;
    uint64_t chk; memcpy(&chk, ni->ed25519_public_key, 8);
    return chk != 0;
}
static int cond_pubkeys_ready(void) {
    return inst_b && inst_c && pubkey_known(inst_b, inst_c->node_id) && pubkey_known(inst_c, inst_b->node_id);
}
static int _cond_ca(void) { return g_done_a && si_a && db_sync_count(si_a) == ca_target; }
static int _cond_cb(void) { return g_done_b && si_b && db_sync_count(si_b) == cb_target; }
static int _cond_cc(void) { return g_done_c && si_c && db_sync_count(si_c) == cc_target; }
static int _cond_both(void) { return _cond_ca() && _cond_cb(); }
static int _cond_all(void) { return _cond_ca() && _cond_cb() && _cond_cc(); }
static int _cond_cb_count(void) { return si_b && !history_ready(si_b) && db_sync_count(si_b) == cb_target; }
static int _cond_both_count(void) {
    return si_a && si_b && !history_ready(si_a) && !history_ready(si_b) &&
        db_sync_count(si_a) == ca_target && db_sync_count(si_b) == cb_target;
}
static int _cond_all_count(void) {
    return si_a && si_b && si_c && !history_ready(si_a) && !history_ready(si_b) && !history_ready(si_c) &&
        db_sync_count(si_a) == ca_target && db_sync_count(si_b) == cb_target && db_sync_count(si_c) == cc_target;
}

static int publish(struct DB_SYNC_INSTANCE* si, struct UTUN_INSTANCE* inst, const char* data, size_t len, uint64_t ts) {
    struct db_message_revision rev = {.ver = 1}; uint8_t hash[32], signature[64];
    if (db_sync_local_revision(si, &rev)) return -1;
    if (EVP_Digest(data, len, hash, NULL, EVP_sha256(), NULL) != 1) return -1;
    memcpy(rev.nonce, hash, 16);
    if (db_sync_sign_message(inst->topo_sqlite_db, db_sync_instance_channel(si), ts, inst->my_ed25519_pubkey, &rev, data, len,
        inst->my_ed25519_privkey, signature) < 0) return -1;
    return db_sync_insert_signed(si, data, len, signature, 64, ts, NULL, &rev);
}

static int insert_many(struct DB_SYNC_INSTANCE* si, struct UTUN_INSTANCE* inst, int start, int count) {
    char buf[128];
    for (int i = start; i < start + count && test_phase == 0; i++) {
        snprintf(buf, sizeof(buf), "{\"idx\":%d,\"val\":\"data_%d\"}", i, i);
        uint64_t ts = db_sync_next_timestamp(si);
        if (publish(si, inst, buf, strlen(buf), ts) < 0) { fprintf(stderr,"insert fail\n"); return -1; }
    }
    return 0;
}

static void remove_si(struct DB_SYNC_INSTANCE** psi) {
    if (*psi) { db_sync_instance_remove(*psi); *psi = NULL; }
}

/* Изолированная фикстура db_sync задаёт полное членство; join-подписи проверяет test_chat_join_e2e. */
static void seed_members(struct UTUN_INSTANCE* self, struct UTUN_INSTANCE** peers, int peer_count) {
    static const uint64_t slots[] = {1, 2, 30, 40, 70, 80, 90};
    static char groups[sizeof(slots) / sizeof(slots[0])][65];
    sqlite3* db = self ? self->topo_sqlite_db : NULL;
    if (!db) return;
    static struct test_group_keys keys[sizeof(groups) / sizeof(groups[0])];
    static uint8_t descriptions[sizeof(groups) / sizeof(groups[0])][64];
    static int initialized;
    if (!initialized) {
        for (unsigned gi = 0; gi < sizeof(groups) / sizeof(groups[0]); gi++) {
            if (test_group_keys_generate(&keys[gi])) abort();
            sc_public_key_hex(keys[gi].ed, groups[gi]);
            if (test_group_signature(groups[gi], "test", (const uint8_t[32]){1}, keys[gi].x.public_key,
                    keys[gi].ed, keys[gi].ed_private, descriptions[gi])) abort();
        }
        initialized = 1;
    }
    uint8_t z64[64] = {0};
    for (unsigned gi = 0; gi < sizeof(groups) / sizeof(groups[0]); gi++) {
        if (topo_node_sqlite_channel_put(db, groups[gi], "test", (const uint8_t[32]){1}, keys[gi].x.public_key, NULL, keys[gi].ed, NULL, descriptions[gi])) abort();
        for (int i = 0; i < peer_count; i++) {
            struct UTUN_INSTANCE* peer = peers[i];
            if (!peer || !peer->node_id) continue;
            if (topo_node_sqlite_node_update_verified(db, peer->node_id, "sync peer", peer->my_keys.public_key,
                peer->my_ed25519_pubkey, 1, 0) < 0 || topo_node_sqlite_member_block_put(db, groups[gi], peer->node_id, z64, 1,
                NULL, 0, peer->my_keys.public_key, peer->my_ed25519_pubkey, "{}", 0, z64) < 0) {
                DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member fixture failed peer=%016llx", (unsigned long long)peer->node_id); abort();
            }
        }
        if (topo_node_sqlite_member_block_put(db, groups[gi], self->node_id, z64, 1, NULL, 0,
            self->my_keys.public_key, self->my_ed25519_pubkey, "{}", 0, z64) < 0) abort();
        char admission[128]; snprintf(admission, sizeof(admission), "UPDATE peers_%.64s SET admitted=1,identity_verified=1", groups[gi]);
        if (sqlite3_exec(db, admission, NULL, NULL, NULL) != SQLITE_OK) abort();
        if (test_member_publish_access(self, groups[gi])) abort();
        uint64_t gid = slots[gi];
        if (!topo_groups_find(self->topo_groups, gid))
            topo_groups_create_group(self->topo_groups, gid, TOPO_GROUP_TYPE_CHAT, groups[gi]);
    }
}

/* Протокольные тесты сами подготовили участников; включение обмена явно отделено от регистрации. */
static struct DB_SYNC_INSTANCE* add_table(struct UTUN_INSTANCE* inst, const char* table, uint64_t gid) {
    struct DB_SYNC_INSTANCE* si = db_sync_instance_add(inst, table, gid);
    if (si) db_sync_instance_set_gated(si, 0);
    return si;
}


static int history_ready(struct DB_SYNC_INSTANCE* si) { uint8_t root[32]; return db_sync_read_root(si, root); }
static int same_roots(struct DB_SYNC_INSTANCE* a, struct DB_SYNC_INSTANCE* b, struct DB_SYNC_INSTANCE* c) {
    uint8_t ra[32], rb[32], rc[32];
    if (db_sync_read_root(a, ra) || db_sync_read_root(b, rb) || memcmp(ra, rb, 32)) return 0;
    return !c || (!db_sync_read_root(c, rc) && !memcmp(ra, rc, 32));
}

static int cond_revision(void) {
    struct UTUN_INSTANCE* nodes[] = {inst_a, inst_b};
    for (unsigned i = 0; i < 2; i++) {
        sqlite3_stmt* st = NULL;
        int rc = sqlite3_prepare_v2(nodes[i]->topo_sqlite_db, "SELECT max(ver) FROM push_t", -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        int matches = rc == SQLITE_ROW && (uint64_t)sqlite3_column_int64(st, 0) == expected_revision;
        sqlite3_finalize(st); if (!matches) return 0;
    }
    return same_roots(si_a, si_b, NULL);
}

int main(void) {
    printf("=== test_db_sync ===\n");
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_category_level(DEBUG_CATEGORY_CHAT_SYNC, DEBUG_LEVEL_DEBUG);
    if (create_temp_configs() != 0) { cleanup_temp_configs(); return 1; }

    utun_instance_set_tun_init_enabled(0);
    ua = uasync_create(); if (!ua) { cleanup_temp_configs(); return 1; }

    inst_a = utun_instance_create(ua, config_a);
    inst_b = utun_instance_create(ua, config_b);
    if (inst_a && inst_b) test_etcp_set_peer(inst_b, "to_a", inst_a, "srv_a");
    if (!inst_a || !inst_b || utun_instance_init(inst_a) != 0 || utun_instance_init(inst_b) != 0)
        { fprintf(stderr, "init fail\n"); cleanup_temp_configs(); return 1; }
    if (setup_history_model(inst_a) < 0 || setup_history_model(inst_b) < 0) { test_phase = 2; goto done; }
    seed_members(inst_a, &inst_b, 1);
    seed_members(inst_b, &inst_a, 1);
    db_sync_add_done_cbk(inst_a, test_done_cb, (void*)0);
    db_sync_add_done_cbk(inst_b, test_done_cb, (void*)1);
    timeout_id = uasync_set_timeout(ua, TEST_TIMEOUT_TB, NULL, test_timeout, "global_timeout");
    if (!wait_for("links up", cond_links_init, PHASE_TIMEOUT_TB)) { test_phase = 2; goto done; }

    // ===================================================================
    // Phase 1: пустой пир получает пять проверенных записей через общий Merkle.
    // ===================================================================
    printf("Phase 1: peer_empty\n");
    reset_done_flags();
    si_a = add_table(inst_a, "test", 1);
    si_b = add_table(inst_b, "test", 1);
    if (!si_a || !si_b) { test_phase = 2; goto done; }
    if (insert_many(si_a, inst_a, 0, 5) != 0) { test_phase = 2; goto done; }
    db_sync_reinitiate(si_b, inst_a->my_ed25519_pubkey);
    cb_target = 5;
    if (!wait_for("B=5", _cond_cb, PHASE_TIMEOUT_TB)) { test_phase = 2; goto done; }
    if (db_sync_count(si_b) != 5 || history_ready(si_a) || history_ready(si_b))
        { test_phase = 2; goto done; }
    printf("  PASS\n");

    // ===================================================================
    // Phase 2: приостанавливаем обмен, добавляем три записи и возобновляем обе стороны.
    // ===================================================================
    printf("Phase 2: hash_MATCH tail-send\n");
    reset_done_flags();
    db_sync_peer_set_paused(si_a, inst_b->my_ed25519_pubkey, 1);
    if (insert_many(si_a, inst_a, 5, 3) != 0) { test_phase = 2; goto done; }
    db_sync_reinitiate(si_a, inst_b->my_ed25519_pubkey);
    db_sync_reinitiate(si_b, inst_a->my_ed25519_pubkey);
    cb_target = 8;
    if (!wait_for("B=8", _cond_cb, PHASE_TIMEOUT_TB)) { test_phase = 2; goto done; }
    if (db_sync_count(si_b) != 8 || history_ready(si_a) || history_ready(si_b))
        { test_phase = 2; goto done; }
    printf("  PASS\n");

    // ===================================================================
    // Phase 3: divergence merge. remove si, add_quiet → insert A=3 B=2 →
    // reinitiate both → divergence → REFINE merge to 5.
    // ===================================================================
    printf("Phase 3: divergence merge\n");
    reset_done_flags();
    remove_si(&si_a); remove_si(&si_b);
    si_a = add_table(inst_a, "test2", 2);
    si_b = add_table(inst_b, "test2", 2);
    if (!si_a || !si_b) { test_phase = 2; goto done; }
    if (insert_many(si_a, inst_a, 0, 3) != 0 || insert_many(si_b, inst_b, 10, 2) != 0)
        { test_phase = 2; goto done; }
    db_sync_reinitiate(si_b, inst_a->my_ed25519_pubkey);
    ca_target = 5; cb_target = 5;
    if (!wait_for("both=5", _cond_both, PHASE_TIMEOUT_TB)) { test_phase = 2; goto done; }
    if (history_ready(si_a) || history_ready(si_b)) { test_phase = 2; goto done; }
    printf("  PASS\n");

    // ===================================================================
    // Phase 4: divergence merge. remove si, add_quiet → insert A=2 B=2 →
    // reinitiate both → merge to 4.
    // ===================================================================
    printf("Phase 4: divergence merge 2\n");
    reset_done_flags();
    remove_si(&si_a); remove_si(&si_b);
    si_a = add_table(inst_a, "test_div", 30);
    si_b = add_table(inst_b, "test_div", 30);
    if (!si_a || !si_b) { test_phase = 2; goto done; }
    if (insert_many(si_a, inst_a, 0, 2) != 0 || insert_many(si_b, inst_b, 10, 2) != 0)
        { test_phase = 2; goto done; }
    db_sync_reinitiate(si_b, inst_a->my_ed25519_pubkey);
    ca_target = 4; cb_target = 4;
    if (!wait_for("both=4", _cond_both, PHASE_TIMEOUT_TB)) { test_phase = 2; goto done; }
    if (history_ready(si_a) || history_ready(si_b)) { test_phase = 2; goto done; }
    printf("  PASS\n");

    // ===================================================================
    // Phase 5: PUSH not-at-tail. Use db_sync_instance_add (normal, with auto-sync)
    // so PUSH fires. Insert early timestamp record → PUSH → cascade_from.
    // ===================================================================
    printf("Phase 5: PUSH not-at-tail\n");
    reset_done_flags();
    remove_si(&si_a); remove_si(&si_b);
    si_a = add_table(inst_a, "push_t", 40);
    si_b = add_table(inst_b, "push_t", 40);
    if (!si_a || !si_b) { test_phase = 2; goto done; }
    if (insert_many(si_a, inst_a, 0, 2) != 0) { test_phase = 2; goto done; }
    cb_target = 2;
    if (!wait_for("seed=2", _cond_cb_count, PHASE_TIMEOUT_TB)) { test_phase = 2; goto done; }
    {   char ebuf[64]; snprintf(ebuf, sizeof(ebuf), "{\"idx\":\"early\",\"val\":\"not_at_tail\"}");
        if (publish(si_a, inst_a, ebuf, strlen(ebuf), 500) != 0)
            { test_phase = 2; goto done; }
    }
    ca_target = 3; cb_target = 3;
    if (!wait_for("both=3", _cond_both_count, PHASE_TIMEOUT_TB)) { test_phase = 2; goto done; }
    if (history_ready(si_a) || history_ready(si_b)) { test_phase = 2; goto done; }
    printf("  PASS\n");

    // ===================================================================
    // Phase 5b: ссылка ответа проходит реальный PUSH и разрешается на обоих узлах.
    printf("Phase 5b: signed reply reference over PUSH\n");
    {
        sqlite3_stmt* st = NULL; char original[65], json[256];
        if (sqlite3_prepare_v2(inst_a->topo_sqlite_db,
            "SELECT lower(hex(message_id)) FROM push_t ORDER BY timestamp LIMIT 1", -1, &st, NULL) != SQLITE_OK
            || sqlite3_step(st) != SQLITE_ROW) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "reply test: original lookup failed");
            sqlite3_finalize(st); test_phase = 2; goto done;
        }
        snprintf(original, sizeof(original), "%s", sqlite3_column_text(st, 0)); sqlite3_finalize(st);
        snprintf(json, sizeof(json), "{\"ct\":\"text/plain\",\"d\":\"Answer\",\"reply_to\":\"%s\"}", original);
        uint64_t ts = db_sync_next_timestamp(si_a);
        if (publish(si_a, inst_a, json, strlen(json), ts) != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "reply test: signed submission failed"); test_phase = 2; goto done;
        }
        ca_target = cb_target = 4;
        if (!wait_for("reply PUSH", _cond_both_count, PHASE_TIMEOUT_TB)) { test_phase = 2; goto done; }
        struct UTUN_INSTANCE* peers[] = {inst_a, inst_b};
        for (unsigned i = 0; i < 2; i++) {
            const char* sql = "SELECT count(*) FROM push_t r JOIN push_t o ON "
                "lower(hex(o.message_id))=json_extract(CAST(r.data AS TEXT),'$.reply_to')";
            st = NULL;
            if (sqlite3_prepare_v2(peers[i]->topo_sqlite_db, sql, -1, &st, NULL) != SQLITE_OK
                || sqlite3_step(st) != SQLITE_ROW || sqlite3_column_int(st, 0) != 1) {
                DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "reply test: reference unresolved peer=%u", i);
                sqlite3_finalize(st); test_phase = 2; goto done;
            }
            sqlite3_finalize(st);
        }
    }
    printf("  signed reply resolves on sender and receiver PASS\n");

    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "Phase 5c: same-timestamp messages over PUSH/ACK");
    delivery_timestamp = db_sync_next_timestamp(si_a);
    for (int i = 0; i < 2; i++) {
        char json[64]; snprintf(json, sizeof(json), "{\"delivery\":%d}", i);
        if (publish(si_a, inst_a, json, strlen(json), delivery_timestamp) != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "delivery test submission failed index=%d", i); test_phase = 2; goto done;
        }
    }
    ca_target = cb_target = 6;
    if (cond_delivery() || !wait_for("delivery messages on both peers", _cond_both_count, PHASE_TIMEOUT_TB) ||
        !wait_for("two distinct delivery ACKs", cond_delivery, PHASE_TIMEOUT_TB)) { test_phase = 2; goto done; }
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS two same-timestamp PUSH records acknowledged separately");

    /* Отключённый пир пропускает две правки: Merkle принимает сразу v3 при том же количестве строк. */
    {
        sqlite3_stmt* st = NULL; uint64_t ts; struct db_message_revision revision = {.ver = 1}; uint8_t signature[64];
        if (db_sync_local_revision(si_a, &revision)) { test_phase = 2; goto done; }
        if (sqlite3_prepare_v2(inst_a->topo_sqlite_db, "SELECT timestamp,nonce FROM push_t ORDER BY timestamp LIMIT 1", -1, &st, NULL) != SQLITE_OK ||
            sqlite3_step(st) != SQLITE_ROW || sqlite3_column_bytes(st, 1) != 16) {
            sqlite3_finalize(st); test_phase = 2; goto done;
        }
        ts = (uint64_t)sqlite3_column_int64(st, 0); memcpy(revision.nonce, sqlite3_column_blob(st, 1), 16); sqlite3_finalize(st);
        db_sync_peer_set_paused(si_a, inst_b->my_ed25519_pubkey, 1); db_sync_peer_set_paused(si_b, inst_a->my_ed25519_pubkey, 1);
        for (uint64_t ver = 2; ver <= 3; ver++) {
            const char* text = ver == 2 ? "{\"ct\":\"text/plain\",\"d\":\"edit 2\"}" : "{\"ct\":\"text/plain\",\"d\":\"edit 3\"}";
            revision.ver = ver; revision.edited_at = db_sync_next_timestamp(si_a);
            if (db_sync_sign_message(inst_a->topo_sqlite_db, topo_groups_channel(inst_a->topo_groups, 40), ts, inst_a->my_ed25519_pubkey, &revision, text, strlen(text), inst_a->my_ed25519_privkey, signature) < 0 ||
                db_sync_insert_signed(si_a, text, strlen(text), signature, 64, ts, NULL, &revision) != 0) {
                test_phase = 2; goto done;
            }
        }
        expected_revision = 3; reset_done_flags();
        db_sync_reinitiate(si_a, inst_b->my_ed25519_pubkey); db_sync_reinitiate(si_b, inst_a->my_ed25519_pubkey);
        if (!wait_for("offline edits converge by Merkle", cond_revision, PHASE_TIMEOUT_TB) ||
            db_sync_count(si_a) != 6 || db_sync_count(si_b) != 6) { test_phase = 2; goto done; }
        const char* text = "{\"ct\":\"text/plain\",\"d\":\"live edit 4\"}";
        revision.ver = 4; revision.edited_at = db_sync_next_timestamp(si_a); expected_revision = 4;
        if (db_sync_sign_message(inst_a->topo_sqlite_db, topo_groups_channel(inst_a->topo_groups, 40), ts, inst_a->my_ed25519_pubkey, &revision, text, strlen(text), inst_a->my_ed25519_privkey, signature) < 0 ||
            db_sync_insert_signed(si_a, text, strlen(text), signature, 64, ts, NULL, &revision) != 0 ||
            !wait_for("live edit over PUSH", cond_revision, PHASE_TIMEOUT_TB)) { test_phase = 2; goto done; }
        DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS offline v1->v3 and live v4 converge with stable IDs and unchanged record count");
    }

    /* Одинаковые count, отличие в первой записи длинной истории и разные локальные id. */
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "Phase 5d: long history with equal counts and divergent first record");
    remove_si(&si_a); remove_si(&si_b);
    si_a = add_table(inst_a, "long_history", 90); si_b = add_table(inst_b, "long_history", 90);
    if (!si_a || !si_b || insert_many(si_a, inst_a, 0, 200) != 0) { test_phase = 2; goto done; }
    ca_target = cb_target = 200;
    if (!wait_for("long initial history", _cond_both_count, LONG_HISTORY_TIMEOUT_TB) || !same_roots(si_a, si_b, NULL)) {
        test_phase = 2; goto done;
    }
    db_sync_peer_set_paused(si_a, inst_b->my_ed25519_pubkey, 1); db_sync_peer_set_paused(si_b, inst_a->my_ed25519_pubkey, 1);
    {
        sqlite3_stmt* st = NULL;
        if (sqlite3_prepare_v2(inst_b->topo_sqlite_db,
            "SELECT timestamp FROM long_history ORDER BY timestamp,author_signature LIMIT 1", -1, &st, NULL) != SQLITE_OK ||
            sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); test_phase = 2; goto done; }
        uint64_t ts = (uint64_t)sqlite3_column_int64(st, 0); sqlite3_finalize(st);
        const char* text = "{\"case\":\"alternate first record\"}";
        uint8_t signature[64], id[32], aroot[32], broot[32]; size_t len = strlen(text);
        struct db_message_revision rev = {.nonce = {42}, .ver = 1};
        if (db_sync_local_revision(si_a, &rev)) { test_phase = 2; goto done; }
        if (db_sync_sign_message(inst_a->topo_sqlite_db, topo_groups_channel(inst_a->topo_groups, 90), ts, inst_a->my_ed25519_pubkey, &rev, text, len, inst_a->my_ed25519_privkey, signature) < 0 ||
            db_sync_message_id(ts, inst_a->my_ed25519_pubkey, &rev, id) < 0) { test_phase = 2; goto done; }
        uint64_t bucket; memcpy(&bucket, id, 8); bucket = be64toh(bucket) >> 39;
        int rc = sqlite3_prepare_v2(inst_b->topo_sqlite_db,
            "UPDATE long_history SET data=?,author_signature=?,message_id=?,bucket=?,id=id+900,nonce=?6 WHERE timestamp=?5", -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, text, (int)len, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, signature, 64, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, id, 32, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 4, (sqlite3_int64)bucket);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 5, (sqlite3_int64)ts);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 6, rev.nonce, 16, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE || db_sync_verify(si_b) || db_sync_read_root(si_a, aroot) ||
            db_sync_read_root(si_b, broot) || !memcmp(aroot, broot, 32)) { test_phase = 2; goto done; }
    }
    reset_done_flags(); ca_target = cb_target = 201;
    db_sync_reinitiate(si_a, inst_b->my_ed25519_pubkey); db_sync_reinitiate(si_b, inst_a->my_ed25519_pubkey);
    if (!wait_for("long equal-count divergence resolves", _cond_both, LONG_HISTORY_TIMEOUT_TB) || !same_roots(si_a, si_b, NULL)) {
        test_phase = 2; goto done;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS two 200-record histories diverging at the start converge to 201 with confirmed full roots");

    // Phase 6: triple star — cascade notification.
    // ===================================================================
    printf("Phase 6: triple star\n");
    reset_done_flags();
    remove_si(&si_a); remove_si(&si_b);
    inst_c = utun_instance_create(ua, config_c);
    if (inst_c) test_etcp_set_peer(inst_c, "to_a", inst_a, "srv_a");
    if (!inst_c || utun_instance_init(inst_c) != 0) { fprintf(stderr, "inst_c fail\n"); test_phase = 2; goto done; }
    if (setup_history_model(inst_c) < 0) { test_phase = 2; goto done; }
    { struct UTUN_INSTANCE* p_ab[2] = { inst_b, inst_c };
      struct UTUN_INSTANCE* p_ac[2] = { inst_a, inst_c };
      struct UTUN_INSTANCE* p_bc[2] = { inst_a, inst_b };
      seed_members(inst_a, p_ab, 2);
      seed_members(inst_b, p_ac, 2);
      seed_members(inst_c, p_bc, 2); }
    db_sync_add_done_cbk(inst_c, test_done_cb, (void*)2);
    if (!wait_for("C links up", cond_links_init, PHASE_TIMEOUT_TB)) { test_phase = 2; goto done; }
    if (!wait_for("pubkeys", cond_pubkeys_ready, PHASE_TIMEOUT_TB)) { test_phase = 2; goto done; }

    si_a = add_table(inst_a, "triple", 70);
    si_b = add_table(inst_b, "triple", 70);
    si_c = add_table(inst_c, "triple", 70);
    if (!si_a || !si_b || !si_c) { test_phase = 2; goto done; }
    if (insert_many(si_a, inst_a, 0, 2) != 0 || insert_many(si_b, inst_b, 10, 1) != 0
        || insert_many(si_c, inst_c, 20, 1) != 0) { test_phase = 2; goto done; }
    db_sync_reinitiate(si_a, inst_b->my_ed25519_pubkey);
    db_sync_reinitiate(si_a, inst_c->my_ed25519_pubkey);
    db_sync_reinitiate(si_b, inst_a->my_ed25519_pubkey);
    db_sync_reinitiate(si_c, inst_a->my_ed25519_pubkey);
    ca_target = 4; cb_target = 4; cc_target = 4;
    if (!wait_for("all=4", _cond_all_count, PHASE_TIMEOUT_TB)) { test_phase = 2; goto done; }
    if (history_ready(si_a) || history_ready(si_b) || history_ready(si_c))
        { test_phase = 2; goto done; }
    printf("  merge=4 PASS\n");

    // 6b: PUSH at tail on A → delivered to B and C via PUSH (sync_state >=1 after merge).
    if (insert_many(si_a, inst_a, 30, 1) != 0) { test_phase = 2; goto done; }
    ca_target = 5; cb_target = 5; cc_target = 5;
    if (!wait_for("all=5 (tail PUSH)", _cond_all_count, PHASE_TIMEOUT_TB)) { test_phase = 2; goto done; }
    if (history_ready(si_a) || history_ready(si_b) || history_ready(si_c))
        { test_phase = 2; goto done; }
    printf("  tail PUSH=5 PASS\n");

    // 6c: PUSH not-at-tail on A → delivered to B and C → cascade_from, synced_pos reset.
    {
        char ebuf[64]; snprintf(ebuf, sizeof(ebuf), "{\"idx\":\"mid\",\"val\":\"triple_insert\"}");
        if (publish(si_a, inst_a, ebuf, strlen(ebuf), 500) != 0)
            { test_phase = 2; goto done; }
    }
    ca_target = 6; cb_target = 6; cc_target = 6;
    if (!wait_for("all=6 (mid PUSH cascade)", _cond_all_count, PHASE_TIMEOUT_TB)) { test_phase = 2; goto done; }
    if (history_ready(si_a) || history_ready(si_b) || history_ready(si_c))
        { test_phase = 2; goto done; }
    printf("  mid PUSH cascade=6 PASS\n");

    // ===================================================================
    // Phase 7: randomized triple — cold start divergence, live PUSH,
    // disconnect/reconnect with additional random inserts.
    // ===================================================================
    printf("Phase 7: randomized triple\n");
    reset_done_flags();
    remove_si(&si_a); remove_si(&si_b); remove_si(&si_c);
    srand((unsigned)time(NULL));
    printf("seed=%u\n", (unsigned)time(NULL));

    si_a = add_table(inst_a, "rnd_triple", 80);
    si_b = add_table(inst_b, "rnd_triple", 80);
    si_c = add_table(inst_c, "rnd_triple", 80);
    if (!si_a || !si_b || !si_c) { test_phase = 2; goto done; }

    // -- Round 1: cold start divergence merge with 0..20 random records per peer --
    int r1_a = rand() % 2, r1_b = rand() % 2, r1_c = rand() % 2;
    int total = r1_a + r1_b + r1_c;
    printf("  R1: A=%d B=%d C=%d (total=%d)\n", r1_a, r1_b, r1_c, total);
    if ((r1_a > 0 && insert_many(si_a, inst_a, 0, r1_a) != 0)
        || (r1_b > 0 && insert_many(si_b, inst_b, 100, r1_b) != 0)
        || (r1_c > 0 && insert_many(si_c, inst_c, 200, r1_c) != 0)) { test_phase = 2; goto done; }
    db_sync_reinitiate(si_a, inst_b->my_ed25519_pubkey);
    db_sync_reinitiate(si_a, inst_c->my_ed25519_pubkey);
    db_sync_reinitiate(si_b, inst_a->my_ed25519_pubkey);
    db_sync_reinitiate(si_c, inst_a->my_ed25519_pubkey);
    ca_target = cb_target = cc_target = (uint32_t)total;
    if (!wait_for("R1 converge", _cond_all_count, PHASE_TIMEOUT_TB)) { test_phase = 2; goto done; }
    {
        if (!same_roots(si_a, si_b, si_c)) { test_phase = 2; goto done; }
        if (history_ready(si_a) || history_ready(si_b) || history_ready(si_c)) { test_phase = 2; goto done; }
    }
    printf("  R1 PASS (%u records)\n", ca_target);

    // -- Round 2: live PUSH — 20 records on random peer, verify convergence --
    {
        int r2_peer = rand() % 3;
        struct DB_SYNC_INSTANCE* pick = (r2_peer == 0) ? si_a : (r2_peer == 1) ? si_b : si_c;
        struct UTUN_INSTANCE* pick_inst = (r2_peer == 0) ? inst_a : (r2_peer == 1) ? inst_b : inst_c;
        const char* pick_name = (r2_peer == 0) ? "A" : (r2_peer == 1) ? "B" : "C";
        printf("  R2: +5 on peer %s\n", pick_name);
        if (insert_many(pick, pick_inst, 300, 5) != 0) { test_phase = 2; goto done; }
        ca_target = cb_target = cc_target = (uint32_t)(total + 5);
        if (!wait_for("R2 converge", _cond_all_count, PHASE_TIMEOUT_TB)) { test_phase = 2; goto done; }
        if (!same_roots(si_a, si_b, si_c)) { test_phase = 2; goto done; }
        printf("  R2 PASS (%u records)\n", ca_target);
        total += 5;
    }

    // -- Round 3: multiple disconnect/reset/sync cycles (stress test the race condition) --
    {
        int rnd_rounds = 2;  // run 2 sub-rounds to catch rare race conditions
        for (int r = 0; r < rnd_rounds && test_phase == 0; r++) {
            reset_done_flags();
            printf("  R3.%d: disconnect/reset, insert random, reinitiate\n", r);
            db_sync_peer_set_paused(si_a, inst_b->my_ed25519_pubkey, 1); db_sync_peer_set_paused(si_a, inst_c->my_ed25519_pubkey, 1);
            db_sync_peer_set_paused(si_b, inst_a->my_ed25519_pubkey, 1); db_sync_peer_set_paused(si_b, inst_c->my_ed25519_pubkey, 1);
            db_sync_peer_set_paused(si_c, inst_a->my_ed25519_pubkey, 1); db_sync_peer_set_paused(si_c, inst_b->my_ed25519_pubkey, 1);

            int ra = rand() % 3, rb = rand() % 3, rc = rand() % 3;
            total += ra + rb + rc;
            printf("  R3.%d: A=%d B=%d C=%d (total=%d)\n", r, ra, rb, rc, total);
            if ((ra > 0 && insert_many(si_a, inst_a, 400 + r*100, ra) != 0)
                || (rb > 0 && insert_many(si_b, inst_b, 500 + r*100, rb) != 0)
                || (rc > 0 && insert_many(si_c, inst_c, 600 + r*100, rc) != 0)) { test_phase = 2; goto done; }

            db_sync_reinitiate(si_a, inst_b->my_ed25519_pubkey);
            db_sync_reinitiate(si_a, inst_c->my_ed25519_pubkey);
            db_sync_reinitiate(si_b, inst_a->my_ed25519_pubkey);
            db_sync_reinitiate(si_c, inst_a->my_ed25519_pubkey);
            ca_target = cb_target = cc_target = (uint32_t)total;
            if (!wait_for("R3 converge", _cond_all_count, PHASE_TIMEOUT_TB)) { test_phase = 2; goto done; }
            {
                if (!same_roots(si_a, si_b, si_c)) { test_phase = 2; goto done; }
            }
        }
    }
    if (test_phase == 0) printf("  R3 PASS (%u records)\n", ca_target);

done:
    remove_si(&si_a); remove_si(&si_b); remove_si(&si_c);
    if (timeout_id && ua) { uasync_cancel_timeout(ua, timeout_id); timeout_id = NULL; }
    if (inst_c) { inst_c->running = 0; if (!inst_c->chat_sync) merkle_sync_destroy(inst_c); utun_instance_destroy(inst_c); inst_c = NULL; }
    if (inst_b) { inst_b->running = 0; if (!inst_b->chat_sync) merkle_sync_destroy(inst_b); utun_instance_destroy(inst_b); inst_b = NULL; }
    if (inst_a) { inst_a->running = 0; if (!inst_a->chat_sync) merkle_sync_destroy(inst_a); utun_instance_destroy(inst_a); inst_a = NULL; }
    if (ua) { uasync_destroy(ua, 0); ua = NULL; }
    cleanup_temp_configs();
    if (test_phase == 0) test_phase = 1;
    debug_disable_file_output();
    printf("=== %s ===\n", test_phase == 1 ? "PASS" : "FAIL");
    return test_phase == 1 ? 0 : 1;
}
