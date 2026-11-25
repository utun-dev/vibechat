#define _GNU_SOURCE
/* FILE/RW: настоящий автомат двух PIN, copied SQL continuations и доставка уже проверенного router payload. */
#include "test_join_id.h"
#include "../src/chat/chat_devgroup_invite.c"
#include "../src/chat/group_info.h"
#include "../src/db_storage.h"
#include "test_utils.h"
#include <assert.h>
#include <pthread.h>

static struct UASYNC* ua;
static struct UTUN_INSTANCE* instances[2];
static sqlite3* legacy[2];
static pthread_t owner;
static int forbid_sql, fail_import, drop_import_ack, deliveries;
static char databases[2][1200];
static uint8_t repeated[64 + DEVGROUP_WIRE_MAX + 18];
static size_t repeated_size;
static uint64_t channel_id;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static int entered, released;
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int size, sqlite3_stmt** stmt, const char** tail) {
    if (forbid_sql && pthread_equal(owner, pthread_self())) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "forbidden devgroup loop SQL: %.150s", sql);
    assert(!forbid_sql || !pthread_equal(owner, pthread_self()));
    return __real_sqlite3_prepare_v2(db, sql, size, stmt, tail);
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*callback)(void*, int, char**, char**), void* arg, char** error) {
    assert(!forbid_sql || !pthread_equal(owner, pthread_self()));
    if (fail_import && !strcmp(sql, "COMMIT") && !strcmp(sqlite3_db_filename(db, "main"), databases[1])) {
        fail_import = 0; return SQLITE_BUSY;
    }
    return __real_sqlite3_exec(db, sql, callback, arg, error);
}
struct delivery { struct UTUN_INSTANCE* destination; struct ll_entry* packet; };
static void deliver(void* data) {
    struct delivery* delivery = data; struct ETCP_CONN connection = {.instance=delivery->destination}; deliveries--;
    dg_receive(&connection, delivery->packet); u_free(delivery);
}
int __real_etcp_route_send_key(struct UTUN_INSTANCE*, uint64_t, const uint8_t*, struct ll_entry*, int, int);
int __wrap_etcp_route_send_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], struct ll_entry* packet, int ttl, int crypto) {
    if (packet->len < 18 || packet->dgram[0] != ETCP_RT_ID_DEVGROUP)
        return __real_etcp_route_send_key(inst, group, peer, packet, ttl, crypto);
    assert(crypto == (ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT));
    if (packet->dgram[1] == DG_KEYS) {
        assert(packet->len <= sizeof(repeated)); memcpy(repeated, packet->dgram, packet->len); repeated_size = packet->len;
    }
    if (drop_import_ack && packet->dgram[1] == DG_RESULT && packet->len == 21 &&
        packet->dgram[18] == DG_PHASE_IMPORT && packet->dgram[20] == 0) {
        drop_import_ack = 0; queue_dgram_free(packet); queue_entry_free(packet); return 0;
    }
    struct UTUN_INSTANCE* destination = !memcmp(instances[0]->my_ed25519_pubkey, peer, 32) ? instances[0] : instances[1];
    assert(!memcmp(destination->my_ed25519_pubkey, peer, 32));
    struct delivery* delivery = u_calloc(1, sizeof(*delivery)); assert(delivery);
    delivery->destination = destination; delivery->packet = ll_alloc_lldgram(ROUTER_SVC_HDR_SIZE + packet->len - 1); assert(delivery->packet);
    struct ll_entry* incoming = delivery->packet; incoming->len = ROUTER_SVC_HDR_SIZE + packet->len - 1;
    memset(incoming->dgram, 0, ROUTER_SVC_HDR_SIZE); memcpy(incoming->dgram + ROUTER_SVC_SRC_ED_OFF, inst->my_ed25519_pubkey, 32);
    memcpy(incoming->dgram + ROUTER_SVC_GROUP_OFF, &group, 8); incoming->dgram[ROUTER_SVC_FLAGS_OFF] = ROUTER_FLAG_ENCRYPTED | ROUTER_FLAG_SIGNED;
    memcpy(incoming->dgram + ROUTER_SVC_HDR_SIZE, packet->dgram + 1, packet->len - 1);
    queue_dgram_free(packet); queue_entry_free(packet); deliveries++; assert(uasync_call_soon(ua, delivery, deliver)); return 0;
}
static void ready(void* data, const struct db_async_result* result) { assert(!result->status); *(int*)data = 1; }
static void wait_idle(void) {
    uint64_t until = get_time_us() + 5000000;
    while ((deliveries || db_async_pending(instances[0]->db_worker) || db_async_pending(instances[1]->db_worker)) && get_time_us() < until)
        uasync_poll(ua, 1);
    assert(!deliveries && !db_async_pending(instances[0]->db_worker) && !db_async_pending(instances[1]->db_worker));
}
static struct dg_invite* invitation(unsigned node, const char* id) {
    uint8_t bytes[16]; assert(!dg_id(id, bytes)); return dg_find(dg_ctx(instances[node]), bytes);
}
static void start_invitation(char id[33]) {
    char pin[5]; assert(!chat_devgroup_invite(instances[0], channel_id, instances[1]->my_ed25519_pubkey, id, pin)); wait_idle();
    struct dg_invite* receiver = invitation(1, id); assert(receiver && receiver->state == DG_WAIT_PIN);
    assert(!chat_devgroup_accept(instances[1], id, pin)); wait_idle();
    assert(receiver->state == DG_WAIT_RETURN && invitation(0, id)->state == DG_WAIT_RETURN);
    memcpy(pin, receiver->pin, sizeof(pin)); assert(!chat_devgroup_accept(instances[0], id, pin));
}
static void device_done(void* data, const struct chat_devgroup_result* result) { assert(!result->status); *(int*)data = 1; }
static int block(sqlite3* db, void* data) {
    (void)db; (void)data; pthread_mutex_lock(&gate); entered = 1; pthread_cond_signal(&wake);
    while (!released) pthread_cond_wait(&wake, &gate);
    pthread_mutex_unlock(&gate); return SQLITE_OK;
}
static void block_done(void* data, const struct db_async_result* result) { (void)data; assert(!result->status); }
static void seed(const char* channel, const uint8_t x[32], const uint8_t ed[32], const uint8_t key[32]) {
    for (unsigned destination = 0; destination < 2; destination++) for (unsigned author = 0; author < 2; author++) {
        uint8_t body[MEMBER_JOIN_MSG_SIZE], join[64], signature[64]; struct UTUN_INSTANCE* node = instances[author];
        int length = member_sync_build_join_msg(x, ed, node->my_ed25519_pubkey, node->my_keys.public_key, body, sizeof(body));
        assert(length > 0 && sc_ed25519_sign(node->my_ed25519_privkey, body, (size_t)length, join) == SC_OK);
        assert(!member_sync_sign_join(key, channel, NULL, node->my_keys.public_key, node->my_ed25519_pubkey,
            TEST_JOIN_ID(x, ed, node->my_ed25519_pubkey, node->my_keys.public_key), 1, signature));
        assert(member_sync_put(instances[destination], channel, node->my_keys.public_key, node->my_ed25519_pubkey,
            join, 1, NULL, 0, "", NULL, NULL, 0, 0, signature) >= 0);
    }
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_CHAT, DEBUG_LEVEL_DEBUG);
    owner = pthread_self(); size_t baseline = u_get_allocated_count(); utun_instance_set_tun_init_enabled(0); ua = uasync_create(); assert(ua);
    char directories[2][1024], config[1600], channel[65]; const char* tmp = getenv("TMPDIR");
    for (unsigned i = 0; i < 2; i++) {
        snprintf(directories[i], sizeof(directories[i]), "%s/utun-devgroup-async-XXXXXX", tmp && *tmp ? tmp : "/tmp");
        assert(!test_mkdtemp(directories[i]));
        snprintf(config, sizeof(config), "[global]\nmy_node_name=device%u\ndb_path=%s\n[ntp]\nenabled=no\n"
            "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", i, directories[i]);
        instances[i] = utun_instance_create_from_str(ua, config); assert(instances[i]);
        assert(!utun_core_start(instances[i]) && !chat_service_start(instances[i])); legacy[i] = instances[i]->topo_sqlite_db;
        snprintf(databases[i], sizeof(databases[i]), "%s", sqlite3_db_filename(legacy[i], "main"));
    }
    chat_core_create_channel_auto(instances[0], "Devices"); sqlite3_stmt* statement = NULL;
    assert(sqlite3_prepare_v2(legacy[0], "SELECT channel_id FROM channels", -1, &statement, NULL) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW); snprintf(channel, sizeof(channel), "%s", sqlite3_column_text(statement, 0));
    assert(!sqlite3_finalize(statement)); channel_id = topo_groups_channel_slot(instances[0]->topo_groups, channel);
    struct group_info_record description; uint8_t x[32], ed[32], private[32];
    assert(!group_info_get(legacy[0], channel, &description));
    assert(!topo_node_sqlite_channel_get(legacy[0], channel, NULL, 0, NULL, x, ed, NULL));
    assert(!topo_node_sqlite_channel_get_priv(legacy[0], channel, private));
    assert(!topo_node_sqlite_channel_put_description(legacy[1], channel, description.json, instances[0]->my_ed25519_pubkey,
        x, NULL, ed, NULL, description.signature)); assert(!chat_core_ensure_channel_ready(instances[1], channel)); seed(channel, x, ed, private);
    wait_idle();
    for (unsigned i = 0; i < 2; i++) {
        struct devgroup_local_identity identity = {.ready=1};
        identity.present = devgroup_local_read(legacy[i], instances[i]->my_keys.public_key,
            instances[i]->my_ed25519_pubkey, &identity.record, identity.ed_private, identity.x_private);
        assert(identity.present >= 0 && !devgroup_local_activate(instances[i], &identity)); OPENSSL_cleanse(&identity, sizeof(identity));
        assert(!db_async_destroy(instances[i]->db_worker)); instances[i]->db_worker = NULL;
        instances[i]->topo_sqlite_db = CC(instances[i])->db = NULL; int opened = 0;
        instances[i]->db_worker = db_async_create(ua, databases[i], 16, ready, &opened); assert(instances[i]->db_worker);
        while (!opened) uasync_poll(ua, 1);
    }
    forbid_sql = 1; char id[33]; fail_import = 1; start_invitation(id); wait_idle();
    assert(!fail_import && invitation(1, id)->state == DG_REJECTED && !instances[1]->local_devgroup.present);
    assert(invitation(0, id)->state == DG_UNKNOWN && instances[0]->local_devgroup.present);
    drop_import_ack = 1; start_invitation(id); wait_idle();
    assert(invitation(1, id)->state == DG_JOINED && invitation(0, id)->state == DG_KEYS_SENT && !drop_import_ack);
    assert(instances[1]->local_devgroup.present && !memcmp(instances[0]->local_devgroup.record.ed, instances[1]->local_devgroup.record.ed, 32));
    assert(!dg_keys(dg_ctx(instances[0]), invitation(0, id))); wait_idle(); assert(invitation(0, id)->state == DG_JOINED);
    int finished = 0; chat_devgroup_leave_submit(instances[1], device_done, &finished); wait_idle(); assert(finished && !instances[1]->local_devgroup.present);
    struct ll_entry* replay = ll_alloc_lldgram(repeated_size); assert(replay); memcpy(replay->dgram, repeated, repeated_size); replay->len = repeated_size;
    assert(!__wrap_etcp_route_send_key(instances[0], channel_id, instances[1]->my_ed25519_pubkey, replay, 0, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT));
    wait_idle(); assert(!instances[1]->local_devgroup.present);
    pthread_mutex_lock(&gate); entered = released = 0; pthread_mutex_unlock(&gate);
    db_async_submit(instances[1]->db_worker, "devgroup-cancel-block", DB_ASYNC_READ, block, NULL, block_done);
    uint64_t until = get_time_us() + 5000000; int observed = 0;
    while (!observed && get_time_us() < until) { uasync_poll(ua, 1); pthread_mutex_lock(&gate); observed = entered; pthread_mutex_unlock(&gate); }
    assert(observed); char pin[5]; assert(!chat_devgroup_invite(instances[0], channel_id, instances[1]->my_ed25519_pubkey, id, pin));
    while (deliveries) uasync_poll(ua, 1); assert(!chat_devgroup_accept(instances[1], id, pin)); while (deliveries) uasync_poll(ua, 1);
    memcpy(pin, invitation(1, id)->pin, 5); assert(!chat_devgroup_accept(instances[0], id, pin));
    until = get_time_us() + 5000000;
    while (!invitation(1, id)->busy && get_time_us() < until) uasync_poll(ua, 1);
    assert(invitation(1, id)->busy); chat_devgroup_cancel_pending(instances[1]);
    pthread_mutex_lock(&gate); released = 1; pthread_cond_signal(&wake); pthread_mutex_unlock(&gate);
    wait_idle(); assert(!instances[1]->local_devgroup.present && invitation(1, id)->state == DG_REJECTED);
    forbid_sql = 0;
    for (unsigned i = 0; i < 2; i++) {
        assert(!db_async_destroy(instances[i]->db_worker)); instances[i]->db_worker = NULL;
        instances[i]->topo_sqlite_db = CC(instances[i])->db = legacy[i]; utun_instance_destroy(instances[i]);
        assert(!db_storage_clear(directories[i])); const char* subdirs[] = {"member_files/.uploads", "member_files", "dm_media", "dm_pending"};
        for (unsigned j = 0; j < sizeof(subdirs) / sizeof(subdirs[0]); j++) {
            snprintf(config, sizeof(config), "%s/%s", directories[i], subdirs[j]); assert(!test_rmdir(config));
        }
        assert(!test_rmdir(directories[i]));
    }
    uasync_destroy(ua, 0); assert(u_get_allocated_count() == baseline);
    assert(!pthread_cond_destroy(&wake) && !pthread_mutex_destroy(&gate));
    puts("PASS devgroup FILE/RW: two PINs, atomic import rollback, ACK after commit, receipt replay after leave, queued cancel, no loop SQL"); return 0;
}
