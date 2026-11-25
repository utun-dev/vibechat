#define TEST_CHANNEL "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951"

#define _GNU_SOURCE
/* Настоящие подписанные ACK и файловый RW worker: commit/rollback, отсутствие SQL в loop, stop и ошибки. */
#include "../src/dm/dm_core.c"
static int test_mb_route(struct UTUN_INSTANCE*, uint64_t, uint64_t, uint64_t*);
static int test_mb_send(struct UTUN_INSTANCE*, uint64_t, uint64_t, struct ll_entry*, int, int);
static struct ETCP_ROUTER_CONN* test_mb_channel(struct UTUN_INSTANCE*, uint64_t, uint64_t, uint8_t);
static int test_mb_room(const struct ETCP_ROUTER_CONN*);
static time_t test_mb_time(struct UTUN_INSTANCE*);
static int test_mb_allowed(struct UTUN_INSTANCE*, uint64_t, uint64_t);
static int test_mb_role(struct UTUN_INSTANCE*, uint64_t, const char*);
static int test_mb_role_key(struct UTUN_INSTANCE*, const uint8_t[32], const char*);
static int test_mb_route_key(struct UTUN_INSTANCE*, const uint8_t[32], uint64_t, uint64_t*, uint8_t);
static int test_mb_send_key(struct UTUN_INSTANCE*, uint64_t, const uint8_t[32], struct ll_entry*, int, int);
static struct ETCP_ROUTER_CONN* test_mb_channel_key(struct UTUN_INSTANCE*, uint64_t, const uint8_t[32], uint8_t);
static int test_mb_allowed_key(struct UTUN_INSTANCE*, uint64_t, const uint8_t[32]);
static int test_media_send(struct UTUN_INSTANCE*, uint64_t, uint64_t, uint8_t, const uint8_t*, size_t);
static int test_media_send_key(struct UTUN_INSTANCE*, uint64_t, const uint8_t[32], uint8_t, const uint8_t*, size_t);
#define dm_route_group test_mb_route
#define etcp_route_send test_mb_send
#define etcp_router_conn_get test_mb_channel
#define etcp_router_conn_send_q_has_room test_mb_room
#define ntp_time_get_seconds test_mb_time
#define topo_group_chat_allowed test_mb_allowed
#define dm_peer_role test_mb_role
#define dm_peer_role_key test_mb_role_key
#define dm_route_group_key test_mb_route_key
#define etcp_route_send_key test_mb_send_key
#define etcp_router_conn_get_key test_mb_channel_key
#define topo_group_chat_key_allowed test_mb_allowed_key
#include "../src/dm/dm_mailbox.c"
#define dm_media_send_key test_media_send_key
#include "../src/dm/dm_mailbox_media.c"
#undef dm_media_send_key
#undef dm_route_group
#undef etcp_route_send
#undef etcp_router_conn_get
#undef etcp_router_conn_send_q_has_room
#undef ntp_time_get_seconds
#undef topo_group_chat_allowed
#undef dm_peer_role
#undef dm_peer_role_key
#undef dm_route_group_key
#undef etcp_route_send_key
#undef etcp_router_conn_get_key
#undef topo_group_chat_key_allowed
#include "../src/db_schema.h"
#include <assert.h>
#include <pthread.h>

static pthread_t loop_thread;
static int forbid_sql, commits, rollbacks, fault_point, fault_code;
static int fault_skips, body_oom, cycle_oom;
static __thread int fail_job;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int pause_write, entered, released;
static const char* delete_prefix = "DELETE FROM dm_outbox";
static const char* pause_prefix = "SELECT body FROM dm_outbox";
static size_t allocation_min = sizeof(struct dm_receipt_job);
static int target_sql(const char* sql) { return sql && !strncmp(sql, delete_prefix, strlen(delete_prefix)); }
static int take(const char* sql, int point) {
    if ((point != 5 && !target_sql(sql)) || __atomic_load_n(&fault_point, __ATOMIC_SEQ_CST) != point) return 0;
    if (__atomic_load_n(&fault_skips, __ATOMIC_SEQ_CST) > 0) { __atomic_sub_fetch(&fault_skips, 1, __ATOMIC_SEQ_CST); return 0; }
    int expected = point;
    return __atomic_compare_exchange_n(&fault_point, &expected, 0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ?
        __atomic_load_n(&fault_code, __ATOMIC_SEQ_CST) : 0;
}
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self()));
    int error = take(sql, 1); if (error) { *st = NULL; return error; }
    if (sql && !strncmp(sql, pause_prefix, strlen(pause_prefix))) {
        pthread_mutex_lock(&lock);
        if (pause_write) {
            entered = 1; pthread_cond_broadcast(&cond);
            while (!released) pthread_cond_wait(&cond, &lock);
            pause_write = 0;
        }
        pthread_mutex_unlock(&lock);
    }
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
int __real_sqlite3_bind_int64(sqlite3_stmt*, int, sqlite3_int64);
int __wrap_sqlite3_bind_int64(sqlite3_stmt* st, int column, sqlite3_int64 value) {
    int error = take(sqlite3_sql(st), 2); return error ? error : __real_sqlite3_bind_int64(st, column, value);
}
int __real_sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, void (*)(void*));
int __wrap_sqlite3_bind_blob(sqlite3_stmt* st, int column, const void* data, int size, void (*cleanup)(void*)) {
    const char* sql = sqlite3_sql(st);
    int inject = sql && (!strncmp(sql, "UPDATE dm_media_jobs SET holder=", strlen("UPDATE dm_media_jobs SET holder=")) ||
                         !strncmp(sql, "INSERT INTO dm_file_targets", strlen("INSERT INTO dm_file_targets")) ||
                         !strncmp(sql, "UPDATE dm_file_targets SET received", strlen("UPDATE dm_file_targets SET received")) ||
                         !strncmp(sql, "SELECT ed25519_pubkey FROM nodes", strlen("SELECT ed25519_pubkey FROM nodes")));
    int error = inject ? take(sql, 2) : 0;
    return error ? error : __real_sqlite3_bind_blob(st, column, data, size, cleanup);
}
int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) { int error = take(sqlite3_sql(st), 3); return error ? error : __real_sqlite3_step(st); }
int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int error = st ? take(sqlite3_sql(st), 4) : 0, rc = __real_sqlite3_finalize(st); return error ? error : rc;
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*fn)(void*, int, char**, char**), void* arg, char** error) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self()));
    int commit = !strcmp(sql, "COMMIT"), injected = take(sql, commit ? 5 : 6);
    if (injected) return injected;
    int rc = __real_sqlite3_exec(db, sql, fn, arg, error);
    if (!rc && commit) __atomic_add_fetch(&commits, 1, __ATOMIC_SEQ_CST);
    if (!rc && !strcmp(sql, "ROLLBACK")) __atomic_add_fetch(&rollbacks, 1, __ATOMIC_SEQ_CST);
    return rc;
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t bytes, const char* where) {
    if (fail_job && (strstr(where, "dm_core.c:") || strstr(where, "dm_mailbox.c:") || strstr(where, "dm_mailbox_media.c:")) &&
        count * bytes >= allocation_min) {
        fail_job = 0; return NULL;
    }
    return __real_u_calloc_impl(count, bytes, where);
}
void* __real_u_malloc_impl(uint32_t, const char*);
void* __wrap_u_malloc_impl(uint32_t bytes, const char* where) {
    if ((strstr(where, "dm_mailbox_store.c:") || strstr(where, "dm_mailbox_media.c:")) &&
        __atomic_load_n(&body_oom, __ATOMIC_SEQ_CST) > 0 &&
        __atomic_sub_fetch(&body_oom, 1, __ATOMIC_SEQ_CST) == 0) return NULL;
    return __real_u_malloc_impl(bytes, where);
}
void* __real_u_realloc_impl(void*, uint32_t, const char*);
void* __wrap_u_realloc_impl(void* ptr, uint32_t bytes, const char* where) {
    if (strstr(where, "dm_mailbox_cycle.c:") && __atomic_exchange_n(&cycle_oom, 0, __ATOMIC_SEQ_CST)) return NULL;
    return __real_u_realloc_impl(ptr, bytes, where);
}

enum { PACKETS = 7, DELIVERY_PACKETS = 40 };
static struct { uint8_t body[512], receipt[DM_RECEIPT_SIZE]; size_t size; } packets[DELIVERY_PACKETS];
static struct dm_store_identity identity;
static const uint8_t self_ed_private[32] = {18}, peer_ed_private[32] = {19};
static uint8_t peer_x[32], peer_ed[32];
static uint64_t peer, conversation;
static char conv[21];
static int mailbox_phase, forwarded, stop_on_forward, relay_before;
static int channel_failed, room = 1;
static int delivery_phase, delivered, delivered_seq[DELIVERY_PACKETS], route_available = 1;
static int put_phase, put_allowed = 1, put_supernode = 1, put_stored, put_receipts;
static int stored_phase, stored_allowed = 1, stored_supernode = 1;
static int media_phase, media_allowed = 1, media_receipts, media_deletes;
static int cycle_phase, cycle_supernode = 1, cycle_packets, cycle_offers, cycle_stores, cycle_notices;
static const uint8_t cycle_holder_ed[2][32] = {{100}, {101}};
static uint64_t cycle_location, cycle_expiry, cycle_storage = 100;
static uint8_t cycle_assignment[16];
static int fanout_phase, fanout_count, fanout_revoke;
static uint64_t fanout_seen[3];
static uint8_t* fanout_input;
static uint8_t media_receipt[DM_MEDIA_RECEIPT_SIZE], assignment_id[16] = {63};
static uint8_t file_body[512], file_id[16] = {42};
static size_t file_size;
static time_t delivery_now = 100;
static struct ETCP_ROUTER_CONN test_channel;

static time_t test_mb_time(struct UTUN_INSTANCE* inst) { return delivery_phase || media_phase ? delivery_now : ntp_time_get_seconds(inst); }
static int test_mb_allowed(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t node) {
    if (fanout_phase) return topo_group_chat_allowed(inst, group, node);
    if (media_phase) { assert(pthread_equal(loop_thread, pthread_self()) && group == 7 && node); return media_allowed; }
    if (stored_phase) { assert(group == 7 && (node == 99 || node == 100)); return stored_allowed; }
    if (!put_phase) return topo_group_chat_allowed(inst, group, node);
    assert(pthread_equal(loop_thread, pthread_self()) && group == 7 && node == identity.node); return put_allowed;
}
static int test_mb_role(struct UTUN_INSTANCE* inst, uint64_t node, const char* role) {
    if (cycle_phase) { assert(node == 99 && !strcmp(role, "supernode")); return cycle_supernode; }
    if (stored_phase) { assert((node == 99 || node == 100) && !strcmp(role, "supernode")); return stored_supernode; }
    if (!put_phase) return dm_peer_role(inst, node, role);
    assert(pthread_equal(loop_thread, pthread_self()) && node == inst->node_id && !strcmp(role, "supernode")); return put_supernode;
}

static int test_mb_role_key(struct UTUN_INSTANCE* inst, const uint8_t ed[32], const char* role) {
    if (put_phase) return test_mb_role(inst, inst->node_id, role);
    if (stored_phase || cycle_phase) return test_mb_role(inst, ed[0], role);
    return dm_peer_role_key(inst, ed, role);
}
static struct ETCP_ROUTER_CONN* test_mb_channel(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t node, uint8_t service) {
    if (!mailbox_phase) return etcp_router_conn_get(inst, group, node, service);
    assert(pthread_equal(loop_thread, pthread_self()) && group == 7 &&
        node == (delivery_phase ? peer : identity.node) && service == ETCP_RT_ID_DM_MAILBOX);
    return channel_failed ? NULL : &test_channel;
}
static int test_mb_room(const struct ETCP_ROUTER_CONN* conn) {
    if (!mailbox_phase) return etcp_router_conn_send_q_has_room(conn);
    assert(pthread_equal(loop_thread, pthread_self()) && conn == &test_channel);
    return room;
}

static int test_mb_route(struct UTUN_INSTANCE* inst, uint64_t node, uint64_t preferred, uint64_t* group) {
    if (cycle_phase) { assert(node == 100 || node == 101); *group = 7; return route_available; }
    if (!mailbox_phase) return dm_route_group(inst, node, preferred, group);
    assert(pthread_equal(loop_thread, pthread_self()) && node == (delivery_phase ? peer : identity.node));
    *group = 7; return route_available;
}
static int test_mb_send(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t node, struct ll_entry* entry, int force, int mode) {
    if (!mailbox_phase) return etcp_route_send(inst, group, node, entry, force, mode);
    assert(pthread_equal(loop_thread, pthread_self()) && group == 7 &&
        node == (delivery_phase ? peer : identity.node) && force == 1 && mode == ROUTER_FLAG_SIGNED);
    if (put_phase) {
        uint64_t seq; assert(entry->len >= 26);
        memcpy(&seq, entry->dgram + (entry->dgram[1] == MB_STORED ? 10 : 18), 8);
        assert(seq && seq <= PACKETS && entry->dgram[0] == ETCP_RT_ID_DM_MAILBOX);
        if (entry->dgram[1] == MB_STORED) {
            uint8_t stored[MB_STORED_SIZE]; memcpy(stored, packets[seq - 1].body, 16);
            memcpy(stored + MB_STORED_RECIPIENT_OFF, peer_ed, MB_RECIPIENT_SIZE);
            SHA256(packets[seq - 1].body, packets[seq - 1].size, stored + MB_STORED_HASH_OFF);
            assert(entry->len == MB_STORED_SIZE + 2 && !memcmp(entry->dgram + 2, stored, sizeof(stored))); put_stored++;
        } else {
            assert(entry->dgram[1] == MB_RECEIPT && entry->len == DM_RECEIPT_SIZE + 2);
            assert(!memcmp(entry->dgram + 2, packets[seq - 1].receipt, DM_RECEIPT_SIZE)); put_receipts++;
        }
        assert(__atomic_load_n(&commits, __ATOMIC_SEQ_CST) > relay_before);
        queue_dgram_free(entry); queue_entry_free(entry);
        if (stop_on_forward) { stop_on_forward = 0; dm_mailbox_destroy(inst); }
        return 0;
    }
    if (delivery_phase) {
        uint64_t seq; memcpy(&seq, entry->dgram + 10, 8); assert(seq && seq <= DELIVERY_PACKETS);
        assert(entry->dgram[0] == ETCP_RT_ID_DM_MAILBOX && entry->dgram[1] == MB_DELIVER);
        assert(entry->len == packets[seq - 1].size + 2 && !memcmp(entry->dgram + 2, packets[seq - 1].body, entry->len - 2));
        assert(__atomic_load_n(&commits, __ATOMIC_SEQ_CST) > relay_before); delivered++; delivered_seq[seq - 1]++;
        queue_dgram_free(entry); queue_entry_free(entry);
        if (stop_on_forward) { stop_on_forward = 0; dm_mailbox_destroy(inst); }
        return 0;
    }
    assert(entry->len == DM_RECEIPT_SIZE + 2 && entry->dgram[0] == ETCP_RT_ID_DM_MAILBOX && entry->dgram[1] == MB_RECEIPT);
    uint64_t seq; memcpy(&seq, entry->dgram + 18, 8); assert(seq && seq <= PACKETS);
    assert(!memcmp(entry->dgram + 2, packets[seq - 1].receipt, DM_RECEIPT_SIZE));
    assert(__atomic_load_n(&commits, __ATOMIC_SEQ_CST) > relay_before); forwarded++;
    queue_dgram_free(entry); queue_entry_free(entry);
    if (stop_on_forward) { stop_on_forward = 0; dm_mailbox_destroy(inst); }
    return 0;
}

static int test_mb_allowed_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32]) {
    if (fanout_phase) return !memcmp(ed, inst->my_ed25519_pubkey, 32) && topo_group_chat_allowed(inst, group, inst->node_id);
    if (media_phase) return test_mb_allowed(inst, group, ed[0]);
    if (stored_phase) return test_mb_allowed(inst, group, 99);
    if (put_phase) { assert(!memcmp(ed, identity.ed, 32)); return test_mb_allowed(inst, group, identity.node); }
    return topo_group_chat_key_allowed(inst, group, ed);
}
static int test_mb_route_key(struct UTUN_INSTANCE* inst, const uint8_t ed[32], uint64_t preferred, uint64_t* group, uint8_t service) {
    if (cycle_phase && service == ETCP_RT_ID_DM_MEDIA) return test_mb_route(inst, ed[0], preferred, group);
    if (!mailbox_phase) return dm_route_group_key(inst, ed, preferred, group, service);
    assert(service == ETCP_RT_ID_DM_MAILBOX);
    return test_mb_route(inst, dm_wire_node_id(ed), preferred, group);
}
static struct ETCP_ROUTER_CONN* test_mb_channel_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32], uint8_t service) {
    if (!mailbox_phase) return etcp_router_conn_get_key(inst, group, ed, service);
    return test_mb_channel(inst, group, dm_wire_node_id(ed), service);
}
static int test_mb_send_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32], struct ll_entry* packet, int force, int mode) {
    if (!mailbox_phase) return etcp_route_send_key(inst, group, ed, packet, force, mode);
    return test_mb_send(inst, group, dm_wire_node_id(ed), packet, force, mode);
}

/* Любая пересылка должна видеть durable commit; остановка внутри send проверяет отсоединение handle. */
static int test_media_send(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t node, uint8_t cmd, const uint8_t* body, size_t size) {
    assert(pthread_equal(loop_thread, pthread_self()) && group == (cycle_phase || fanout_phase ? 0 : 7) &&
        __atomic_load_n(&commits, __ATOMIC_SEQ_CST) > relay_before);
    if (fanout_phase) {
        assert(cmd == DM_MEDIA_RECEIPT && size == sizeof(media_receipt) && !memcmp(body, media_receipt, size));
        assert(fanout_count < 3 && node >= 99 && node <= 101); fanout_seen[fanout_count++] = node;
        if (fanout_input) { memset(fanout_input, 0, sizeof(media_receipt)); fanout_input = NULL; }
        if (fanout_revoke) { fanout_revoke = 0; group_policy_forget(inst, TEST_CHANNEL); }
        if (stop_on_forward) { stop_on_forward = 0; dm_mailbox_destroy(inst); }
        return 0;
    }
    if (cycle_phase) {
        cycle_packets++;
        if (cmd == DM_MEDIA_OFFER) {
            assert(node == peer && size == file_size + DM_MEDIA_OFFER_HEADER && !memcmp(body, peer_ed, 32));
            const uint8_t* holder_ed = cycle_location == identity.node ? identity.ed : cycle_holder_ed[cycle_location - 100];
            assert(!memcmp(body + DM_MEDIA_OFFER_HOLDER_OFF, holder_ed, 32) && !memcmp(body + DM_MEDIA_OFFER_EXPIRES_OFF, &cycle_expiry, 8));
            assert(!memcmp(body + DM_MEDIA_OFFER_HEADER, file_body, file_size)); cycle_offers++;
        } else if (cmd == DM_MEDIA_STORE) {
            assert(node == cycle_storage && size == file_size + DM_MEDIA_STORE_HEADER && !memcmp(body + DM_MEDIA_STORE_RECIPIENT_OFF, peer_ed, 32));
            assert(!memcmp(body + DM_MEDIA_STORE_HEADER, file_body, file_size)); memcpy(cycle_assignment, body, 16); cycle_stores++;
        } else if (cmd == DM_MEDIA_SOURCE_GONE) {
            uint8_t hash[32]; SHA256(file_body, file_size, hash);
            assert(node == peer && size == DM_MEDIA_GONE_SIZE && !memcmp(body, file_id, 16));
            assert(!memcmp(body + 16, hash, 32) && !memcmp(body + DM_MEDIA_GONE_HOLDER_OFF, cycle_holder_ed[0], 32)); cycle_notices++;
        } else {
            assert(cmd == DM_MEDIA_RECEIPT || cmd == DM_MEDIA_DELETE);
            assert(size == sizeof(media_receipt) + (cmd == DM_MEDIA_DELETE ? 16 : 0));
            assert(!memcmp(body + (cmd == DM_MEDIA_DELETE ? 16 : 0), media_receipt, sizeof(media_receipt)));
            if (cmd == DM_MEDIA_RECEIPT) { assert(node == identity.node); media_receipts++; }
            else { assert(node == 100 && !memcmp(body, assignment_id, 16)); media_deletes++; }
        }
        if (stop_on_forward) { stop_on_forward = 0; dm_mailbox_destroy(inst); }
        return 0;
    }
    if (cmd == DM_MEDIA_RECEIPT) {
        assert(node == identity.node && size == DM_MEDIA_RECEIPT_SIZE && !memcmp(body, media_receipt, size)); media_receipts++;
    } else {
        assert(cmd == DM_MEDIA_DELETE && node == 100 && size == 16 + DM_MEDIA_RECEIPT_SIZE);
        assert(!memcmp(body, assignment_id, 16) && !memcmp(body + 16, media_receipt, DM_MEDIA_RECEIPT_SIZE)); media_deletes++;
    }
    if (stop_on_forward) { stop_on_forward = 0; dm_mailbox_destroy(inst); }
    return 0;
}

/* Шифротекст, подпись автора и подпись ACK настоящие; после enqueue worker не использует identity/runtime. */
static int test_media_send_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32],
                               uint8_t cmd, const uint8_t* body, size_t size) {
    if (!media_phase) return dm_media_send_key(inst, group, ed, cmd, body, size);
    const uint8_t fixture_supernode[32] = {99};
    uint64_t node = !memcmp(ed, fixture_supernode, 32) ? 99 : !memcmp(ed, cycle_holder_ed[0], 32) ? 100 :
        !memcmp(ed, cycle_holder_ed[1], 32) ? 101 : dm_wire_node_id(ed);
    return test_media_send(inst, group, node, cmd, body, size);
}

static void make_packets(void) {
    uint8_t peer_x_private[32];
    assert(!sc_identity_from_seed(self_ed_private, identity.x, identity.private_key, identity.ed));
    assert(!sc_identity_from_seed(peer_ed_private, peer_x, peer_x_private, peer_ed));
    identity.node = sc_derive_node_id_from_pubkey(identity.x); peer = sc_derive_node_id_from_pubkey(peer_x);
    conversation = dm_derive_conv_id(identity.ed, peer_ed); snprintf(conv, sizeof(conv), "%llu", (unsigned long long)conversation);
    uint8_t key[32]; assert(!dm_derive_content_key(identity.private_key, peer_x, key));
    for (unsigned i = 0; i < DELIVERY_PACKETS; i++) {
        uint64_t seq = i + 1, timestamp = 100; uint8_t* body = packets[i].body;
        memcpy(body, &conversation, 8); memcpy(body + 8, &seq, 8); memcpy(body + 16, &timestamp, 8); memcpy(body + DM_MSG_AUTHOR_OFF, identity.ed, 32);
        body[DM_MSG_CT_LEN_OFF] = 4; memcpy(body + DM_MSG_FIXED_HDR, "text", 4); uint8_t nonce[DM_NONCE_SIZE]; size_t size = 0;
        dm_build_nonce(conversation, identity.ed, seq, nonce);
        assert(!media_ccm_encrypt(key, nonce, (const uint8_t*)"receipt body", 12, body + DM_MSG_FIXED_HDR + 6, &size));
        uint16_t encrypted_size = (uint16_t)size; memcpy(body + DM_MSG_FIXED_HDR + 4, &encrypted_size, 2); packets[i].size = DM_MSG_FIXED_HDR + 6 + size + 64;
        assert(!sc_ed25519_sign(self_ed_private, body, packets[i].size - 64, body + packets[i].size - 64));
        uint8_t* receipt = packets[i].receipt; memcpy(receipt, DM_ACK_DEVICE, 8); memcpy(receipt + 8, body, 16);
        memcpy(receipt + DM_FACT_AUTHOR_OFF, identity.ed, 32); memcpy(receipt + DM_FACT_RECIPIENT_OFF, peer_ed, 32);
        SHA256(body, packets[i].size, receipt + DM_ACK_HASH_OFF);
        assert(!sc_ed25519_sign(peer_ed_private, receipt, DM_ACK_SIG_OFF, receipt + DM_ACK_SIG_OFF));
    }
    memcpy(file_body, packets[0].body, DM_MSG_FIXED_HDR + 6); memcpy(file_body + DM_MSG_FIXED_HDR, "file", 4); uint64_t seq = 100;
    memcpy(file_body + 8, &seq, 8); uint8_t nonce[DM_NONCE_SIZE]; size_t encrypted = 0;
    dm_build_nonce(conversation, identity.ed, seq, nonce);
    assert(!media_ccm_encrypt(key, nonce, (const uint8_t*)"receipt body", 12, file_body + DM_MSG_FIXED_HDR + 6, &encrypted));
    uint16_t encrypted_size = (uint16_t)encrypted; memcpy(file_body + DM_MSG_FIXED_HDR + 4, &encrypted_size, 2);
    uint8_t* desc = file_body + DM_MSG_FIXED_HDR + 6 + encrypted; memcpy(desc, file_id, 16); uint64_t bytes = 17;
    memcpy(desc + 16, &bytes, 8); SHA256((const uint8_t*)"opaque file", 11, desc + 24); file_size = DM_MSG_FIXED_HDR + 6 + encrypted + 56 + 64;
    assert(!sc_ed25519_sign(self_ed_private, file_body, file_size - 64, file_body + file_size - 64));
    OPENSSL_cleanse(key, sizeof(key));
}

/* Самостоятельная SQL-фикстура, включая историю, outbox и точный снимок получателей. */
static int seed(sqlite3* db, void* unused) {
    (void)unused; assert(!db_schema_init(db)); sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(db, "INSERT INTO nodes(node_id,name,x25519_pubkey,ed25519_pubkey) VALUES(?,'peer',?,?)", -1, &st, NULL));
    assert(!sqlite3_bind_int64(st, 1, (sqlite3_int64)peer) && !sqlite3_bind_blob(st, 2, peer_x, 32, SQLITE_STATIC));
    assert(!sqlite3_bind_blob(st, 3, peer_ed, 32, SQLITE_STATIC) && sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    assert(!sqlite3_prepare_v2(db, "INSERT INTO dm_conversations(conv_id,peer_x25519,peer_ed25519,last_out_seq)"
        " VALUES(?,?,?,7)", -1, &st, NULL));
    assert(!sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC));
    assert(!sqlite3_bind_blob(st, 2, peer_x, 32, SQLITE_STATIC) && !sqlite3_bind_blob(st, 3, peer_ed, 32, SQLITE_STATIC));
    assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    for (unsigned i = 0; i < PACKETS; i++) {
        assert(!sqlite3_prepare_v2(db, "INSERT INTO dm_outbox(conv_id,seq,body) VALUES(?,?,?)", -1, &st, NULL));
        assert(!sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC) && !sqlite3_bind_int(st, 2, (int)i + 1));
        assert(!sqlite3_bind_blob(st, 3, packets[i].body, (int)packets[i].size, SQLITE_STATIC));
        assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
        assert(!sqlite3_prepare_v2(db, "INSERT INTO dm_delivery(conv_id,author,seq,recipient) VALUES(?,?,?,?)", -1, &st, NULL));
        assert(!sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC) && !sqlite3_bind_blob(st, 2, identity.ed, 32, SQLITE_STATIC));
        assert(!sqlite3_bind_int(st, 3, (int)i + 1) && !sqlite3_bind_blob(st, 4, peer_ed, 32, SQLITE_STATIC));
        assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
        assert(!sqlite3_prepare_v2(db, "INSERT INTO dm_messages(conv_id,dir,seq,ts,author,ct,data,sig,body)"
            " VALUES(?,1,?,100,?,'text',?,?,?)", -1, &st, NULL));
        assert(!sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC) && !sqlite3_bind_int(st, 2, (int)i + 1));
        assert(!sqlite3_bind_blob(st, 3, identity.ed, 32, SQLITE_STATIC));
        assert(!sqlite3_bind_blob(st, 4, packets[i].body + DM_MSG_FIXED_HDR + 6, (int)packets[i].size - 39 - 64, SQLITE_STATIC));
        assert(!sqlite3_bind_blob(st, 5, packets[i].body + packets[i].size - 64, 64, SQLITE_STATIC));
        assert(!sqlite3_bind_blob(st, 6, packets[i].body, (int)packets[i].size, SQLITE_STATIC));
        assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    }
    return SQLITE_OK;
}

static void ready(void* arg, const struct db_async_result* result) {
    assert(pthread_equal(loop_thread, pthread_self()) && !result->status && !(*(int*)arg)++);
}
static void idle(struct UTUN_INSTANCE* inst) {
    unsigned quiet = 0; uint64_t end = get_time_us() + 5000000;
    while (quiet < 2 && get_time_us() < end) { uasync_poll(inst->ua, 1); quiet = db_async_pending(inst->db_worker) ? 0 : quiet + 1; }
    assert(quiet == 2);
}
static void new_owner(struct UTUN_INSTANCE* inst) {
    assert(!inst->dm); struct dm_state* dm = u_calloc(1, sizeof(*dm)); assert(dm);
    memcpy(inst->my_ed25519_pubkey, identity.ed, 32);
    dm->inst = inst; dm->initialized = 1; inst->dm = dm;
}
struct reply { int calls, status, before; };
static void done(void* arg, int status, const uint8_t* receipt) {
    struct reply* reply = arg; assert(pthread_equal(loop_thread, pthread_self())); reply->calls++; reply->status = status;
    if (status) { assert(!receipt); return; }
    assert(receipt && !memcmp(receipt, DM_ACK_DEVICE, 8) && __atomic_load_n(&commits, __ATOMIC_SEQ_CST) > reply->before);
}
static int events, stop_on_event;
static void event(struct UTUN_INSTANCE* inst, int kind, const uint8_t* data, int size) {
    if (kind != CHAT_EVT_DM_MSG_RECEIVED) return;
    assert(size == (int)(strlen(conv) + 33) && data[0] == strlen(conv) && !memcmp(data + 1, conv, strlen(conv)));
    assert(__atomic_load_n(&commits, __ATOMIC_SEQ_CST)); events++;
    if (stop_on_event) { stop_on_event = 0; dm_core_destroy(inst); }
}
struct state { int seq, pending, confirmed, messages, done; };
static int read_state(sqlite3* db, void* arg) {
    struct state* state = arg; sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT (SELECT count(*) FROM dm_outbox WHERE conv_id=?1 AND seq=?2),"
        "(SELECT count(*) FROM dm_delivery WHERE conv_id=?1 AND seq=?2 AND receipt IS NOT NULL),"
        "(SELECT count(*) FROM dm_messages WHERE conv_id=?1)", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int(st, 2, state->seq);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        state->pending = sqlite3_column_int(st, 0); state->confirmed = sqlite3_column_int(st, 1); state->messages = sqlite3_column_int(st, 2);
    }
    int final = sqlite3_finalize(st); return rc == SQLITE_ROW ? final : rc;
}
static void state_done(void* arg, const struct db_async_result* result) { assert(!result->status); ((struct state*)arg)->done++; }
static void check_state(struct UTUN_INSTANCE* inst, int seq, int pending) {
    struct state state = {.seq=seq}; db_async_run(inst->db_worker, &(struct db_async_task){.name="receipt-state", .mode=DB_ASYNC_READ,
        .work=read_state, .data=&state, .done=state_done}); idle(inst);
    assert(state.done == 1 && state.pending == pending && state.confirmed == !pending && state.messages == PACKETS);
}
static void submit_once(struct UTUN_INSTANCE* inst, unsigned packet, int status) {
    struct reply reply = {.before=__atomic_load_n(&commits, __ATOMIC_SEQ_CST)};
    dm_receipt_submit(inst, packets[packet].receipt, done, &reply); idle(inst);
    assert(reply.calls == 1 && reply.status == status && !inst->dm->receipts);
}
static void pause_job(void) { pthread_mutex_lock(&lock); pause_write = 1; entered = released = 0; pthread_mutex_unlock(&lock); }
static void paused(struct UTUN_INSTANCE* inst) {
    int seen = 0; uint64_t end = get_time_us() + 5000000;
    while (!seen && get_time_us() < end) { uasync_poll(inst->ua, 1); pthread_mutex_lock(&lock); seen = entered; pthread_mutex_unlock(&lock); }
    assert(seen);
}
static void unpause(void) { pthread_mutex_lock(&lock); released = 1; pthread_cond_broadcast(&cond); pthread_mutex_unlock(&lock); }
static void tick(void* arg) { *(int*)arg = 1; }
static void fault(int point, int code) {
    __atomic_store_n(&fault_skips, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&fault_code, code, __ATOMIC_SEQ_CST); __atomic_store_n(&fault_point, point, __ATOMIC_SEQ_CST);
}

static void new_mailbox(struct UTUN_INSTANCE* inst) {
    assert(!inst->dm_mailbox); struct dm_mb_state* mb = u_calloc(1, sizeof(*mb)); assert(mb);
    mb->inst = inst; inst->dm_mailbox = mb;
}
/* Квитанция7 приходит без pending: подписанный факт всё равно должен пережить перезапуск. */
static int seed_mailbox(sqlite3* db, void* unused) {
    (void)unused; assert(!db_schema_init(db)); sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(db, "INSERT INTO nodes(node_id,x25519_pubkey,ed25519_pubkey) VALUES(?,?,?)", -1, &st, NULL));
    assert(!sqlite3_bind_int64(st, 1, (sqlite3_int64)peer) && !sqlite3_bind_blob(st, 2, peer_x, 32, SQLITE_STATIC));
    assert(!sqlite3_bind_blob(st, 3, peer_ed, 32, SQLITE_STATIC) && sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    for (unsigned i = 0; i + 1 < PACKETS; i++) {
        assert(!sqlite3_prepare_v2(db, "INSERT INTO dm_pending_messages(recipient,sender,seq,msg) VALUES(?,?,?,?)", -1, &st, NULL));
        assert(!sqlite3_bind_blob(st, 1, peer_ed, 32, SQLITE_STATIC) && !sqlite3_bind_blob(st, 2, identity.ed, 32, SQLITE_STATIC));
        assert(!sqlite3_bind_int(st, 3, (int)i + 1) && !sqlite3_bind_blob(st, 4, packets[i].body, (int)packets[i].size, SQLITE_STATIC));
        assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    }
    return SQLITE_OK;
}
static int read_mail_state(sqlite3* db, void* arg) {
    struct state* state = arg; sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT (SELECT count(*) FROM dm_pending_messages WHERE recipient=?1 AND sender=?2 AND seq=?3),"
        "(SELECT count(*) FROM dm_mail_receipts WHERE recipient=?1 AND sender=?2 AND seq=?3),"
        "(SELECT count(*) FROM dm_mail_receipts WHERE recipient=?1 AND sender=?2 AND seq=?3 AND receipt=?4)", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, peer_ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, identity.ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int(st, 3, state->seq);
    if (!rc) rc = sqlite3_bind_blob(st, 4, packets[state->seq - 1].receipt, DM_RECEIPT_SIZE, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        state->pending = sqlite3_column_int(st, 0); state->confirmed = sqlite3_column_int(st, 1); state->messages = sqlite3_column_int(st, 2);
    }
    int final = sqlite3_finalize(st); return rc == SQLITE_ROW ? final : rc;
}
static void check_mail_state(struct UTUN_INSTANCE* inst, int seq, int pending, int tombstone) {
    struct state state = {.seq=seq}; db_async_run(inst->db_worker, &(struct db_async_task){.name="mail-receipt-state", .mode=DB_ASYNC_READ,
        .work=read_mail_state, .data=&state, .done=state_done}); idle(inst);
    assert(state.done == 1 && state.pending == pending && state.confirmed == tombstone && state.messages == tombstone);
}
static void relay(struct UTUN_INSTANCE* inst, unsigned packet) {
    relay_before = __atomic_load_n(&commits, __ATOMIC_SEQ_CST); mb_receipt_received(inst->dm_mailbox, 7, peer, peer_ed, packets[packet].receipt);
}
/* Та же FILE/RW проверка на S: ответ на проводе наблюдается только после успешного commit. */
static void check_mailbox(struct UASYNC* ua) {
    struct SC_MYKEYS keys; assert(!sc_generate_keypair(&keys));
    struct UTUN_INSTANCE inst = {.ua=ua, .node_id=sc_derive_node_id_from_pubkey(keys.public_key), .chat_content_started=1}; new_mailbox(&inst);
    assert(inst.node_id != peer && inst.node_id != identity.node); mailbox_phase = 1;
    delete_prefix = "DELETE FROM dm_pending_messages"; pause_prefix = "SELECT msg FROM dm_pending_messages";
    allocation_min = sizeof(struct mb_receipt_job); char path[] = "/tmp/utun-mail-receipt-XXXXXX"; int fd = mkstemp(path);
    assert(fd >= 0); close(fd); int opened = 0; inst.db_worker = db_async_create(ua, path, 2, ready, &opened); assert(inst.db_worker);
    idle(&inst); assert(opened); int seeded = 0;
    db_async_run(inst.db_worker, &(struct db_async_task){.name="mail-receipt-fixture", .mode=DB_ASYNC_DIRECT,
        .work=seed_mailbox, .data=&seeded, .done=ready}); idle(&inst); assert(seeded && !inst.topo_sqlite_db && !inst.dm_mailbox->db);
    uint8_t input[DM_RECEIPT_SIZE]; memcpy(input, packets[0].receipt, sizeof(input));
    relay_before = __atomic_load_n(&commits, __ATOMIC_SEQ_CST);
    mb_receipt_received(inst.dm_mailbox, 7, peer, peer_ed, input); memset(input, 0, sizeof(input)); idle(&inst);
    assert(forwarded == 1); check_mail_state(&inst, 1, 0, 1); relay(&inst, 0); idle(&inst); assert(forwarded == 2);
    room = 0; relay(&inst, 0); idle(&inst); assert(forwarded == 2 && !inst.dm_mailbox->receipts); room = 1;
    channel_failed = 1; relay(&inst, 0); idle(&inst); assert(forwarded == 2 && !inst.dm_mailbox->receipts); channel_failed = 0;
    check_mail_state(&inst, 1, 0, 1);
    memcpy(input, packets[0].receipt, sizeof(input)); input[DM_ACK_HASH_OFF] ^= 1;
    assert(!sc_ed25519_sign(peer_ed_private, input, DM_ACK_SIG_OFF, input + DM_ACK_SIG_OFF));
    mb_receipt_received(inst.dm_mailbox, 7, peer, peer_ed, input); idle(&inst); assert(forwarded == 2); check_mail_state(&inst, 1, 0, 1);
    for (int point = 1; point <= 5; point++) {
        fault(point, SQLITE_BUSY); relay(&inst, 1); idle(&inst); assert(!__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST) && forwarded == 2);
        check_mail_state(&inst, 2, 1, 0);
    }
    relay(&inst, 1); idle(&inst); assert(forwarded == 3); check_mail_state(&inst, 2, 0, 1);
    fail_job = 1; relay(&inst, 2); idle(&inst); assert(!fail_job && forwarded == 3); check_mail_state(&inst, 3, 1, 0);
    pause_job(); relay(&inst, 2); paused(&inst); relay(&inst, 3);
    for (unsigned i = 0; i < 96; i++) relay(&inst, 4);
    assert(db_async_pending(inst.db_worker) == 2 && forwarded == 3); int alive = 0;
    assert(uasync_set_timeout(ua, 10, &alive, tick, "mailbox WRITE loop alive")); uint64_t end = get_time_us() + 5000000;
    while (!alive && get_time_us() < end) uasync_poll(ua, 1);
    assert(alive && forwarded == 3); unpause(); idle(&inst); assert(forwarded == 5);
    check_mail_state(&inst, 3, 0, 1); check_mail_state(&inst, 4, 0, 1);
    pause_job(); relay(&inst, 4); paused(&inst); relay(&inst, 5); dm_mailbox_destroy(&inst); new_mailbox(&inst); unpause(); idle(&inst);
    assert(forwarded == 5); check_mail_state(&inst, 5, 0, 1); check_mail_state(&inst, 6, 1, 0);
    stop_on_forward = 1; relay(&inst, 5); idle(&inst); assert(forwarded == 6 && !inst.dm_mailbox && !stop_on_forward); new_mailbox(&inst);
    check_mail_state(&inst, 6, 0, 1); fault(1, SQLITE_IOERR); relay(&inst, 6); idle(&inst); relay(&inst, 6); idle(&inst); assert(forwarded == 6);
    assert(!db_async_destroy(inst.db_worker)); inst.db_worker = NULL; opened = 0;
    inst.db_worker = db_async_create(ua, path, 2, ready, &opened); assert(inst.db_worker); idle(&inst); assert(opened);
    check_mail_state(&inst, 7, 0, 0); relay(&inst, 6); idle(&inst); assert(forwarded == 7); check_mail_state(&inst, 7, 0, 1);
    dm_mailbox_destroy(&inst); assert(!db_async_destroy(inst.db_worker)); mailbox_phase = 0; assert(!remove(path));
    OPENSSL_cleanse(&keys, sizeof(keys));
    puts("PASS mailbox receipt WRITE: commit before wire reply, immutable tombstone, late rollback, FULL/OOM, stop/restart and fatal reopen");
}

/* Больше одной пачки: все тела подписаны автором, число/время повторов проверяются через другой READ. */
static int seed_delivery(sqlite3* db, void* unused) {
    (void)unused; assert(!db_schema_init(db)); sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(db, "INSERT INTO dm_pending_messages(recipient,sender,seq,msg) VALUES(?,?,?,?)", -1, &st, NULL));
    for (unsigned i = 0; i < DELIVERY_PACKETS; i++) {
        assert(!sqlite3_bind_blob(st, 1, peer_ed, 32, SQLITE_STATIC) && !sqlite3_bind_blob(st, 2, identity.ed, 32, SQLITE_STATIC));
        assert(!sqlite3_bind_int64(st, 3, i + 1) && !sqlite3_bind_blob(st, 4, packets[i].body, (int)packets[i].size, SQLITE_STATIC));
        assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_reset(st));
    }
    assert(!sqlite3_finalize(st)); return SQLITE_OK;
}
static int read_delivery(sqlite3* db, void* arg) {
    struct state* state = arg; sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT count(*),sum(retry_at=?) FROM dm_pending_messages", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_int64(st, 1, state->seq);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) { state->messages = sqlite3_column_int(st, 0); state->confirmed = sqlite3_column_int(st, 1); }
    int final = sqlite3_finalize(st); return rc == SQLITE_ROW ? final : rc;
}
static void check_delivery_state(struct UTUN_INSTANCE* inst, int at, int count) {
    struct state state = {.seq=at}; db_async_run(inst->db_worker, &(struct db_async_task){.name="mail-delivery-state",
        .mode=DB_ASYNC_READ, .work=read_delivery, .data=&state, .done=state_done}); idle(inst);
    assert(state.done == 1 && state.messages == DELIVERY_PACKETS && state.confirmed == count);
}
static void delivery_submit(struct UTUN_INSTANCE* inst) {
    struct dm_mb_state* mb = inst->dm_mailbox; assert(mb);
    if (mb->timer) uasync_cancel_timeout(inst->ua, mb->timer);
    mb->timer = NULL; relay_before = __atomic_load_n(&commits, __ATOMIC_SEQ_CST); mb_tick(mb);
}
static int delivery_read_blocker(sqlite3* db, void* unused) {
    (void)unused; sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT recipient,sender,seq,msg FROM dm_pending_messages LIMIT 1", -1, &st, NULL);
    if (!rc) rc = sqlite3_step(st);
    int final = sqlite3_finalize(st); return rc == SQLITE_ROW || rc == SQLITE_DONE ? final : rc;
}
static void check_delivery(struct UASYNC* ua) {
    struct UTUN_INSTANCE inst = {.ua=ua, .node_id=99, .chat_content_started=1}; new_mailbox(&inst); mailbox_phase = delivery_phase = 1;
    delete_prefix = "UPDATE dm_pending_messages SET retry_at"; pause_prefix = "SELECT recipient,sender,seq,msg";
    allocation_min = sizeof(struct mb_delivery_job); char path[] = "/tmp/utun-mail-delivery-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0); close(fd); int opened = 0;
    inst.db_worker = db_async_create(ua, path, 2, ready, &opened); assert(inst.db_worker); idle(&inst); assert(opened);
    int seeded = 0; db_async_run(inst.db_worker, &(struct db_async_task){.name="delivery-fixture", .mode=DB_ASYNC_DIRECT,
        .work=seed_delivery, .data=&seeded, .done=ready}); idle(&inst); assert(seeded && !inst.dm_mailbox->db);
    struct state blocked[2] = {{0}, {0}}; pause_job();
    db_async_run(inst.db_worker, &(struct db_async_task){.name="delivery-busy", .mode=DB_ASYNC_READ,
        .work=delivery_read_blocker, .data=&blocked[0], .done=state_done}); paused(&inst);
    db_async_run(inst.db_worker, &(struct db_async_task){.name="delivery-queued", .mode=DB_ASYNC_READ,
        .work=delivery_read_blocker, .data=&blocked[1], .done=state_done});
    for (unsigned i = 0; i < 3; i++) {
        delivery_submit(&inst); assert(!inst.dm_mailbox->delivery && inst.dm_mailbox->timer && db_async_pending(inst.db_worker) == 2);
    }
    unpause(); idle(&inst); assert(blocked[0].done == 1 && blocked[1].done == 1 && !delivered);
    check_delivery_state(&inst, 0, DELIVERY_PACKETS);
    for (int point = 1; point <= 5; point++) {
        fault(point, SQLITE_BUSY); delivery_submit(&inst); idle(&inst);
        assert(!__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST) && !delivered && !inst.dm_mailbox->delivery);
        check_delivery_state(&inst, 0, DELIVERY_PACKETS);
    }
    fault(3, SQLITE_BUSY); __atomic_store_n(&fault_skips, 1, __ATOMIC_SEQ_CST); delivery_submit(&inst); idle(&inst);
    assert(!delivered && !__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST)); check_delivery_state(&inst, 0, DELIVERY_PACKETS);
    __atomic_store_n(&body_oom, 2, __ATOMIC_SEQ_CST); delivery_submit(&inst); idle(&inst);
    assert(!delivered && !__atomic_load_n(&body_oom, __ATOMIC_SEQ_CST)); check_delivery_state(&inst, 0, DELIVERY_PACKETS);
    fail_job = 1; delivery_submit(&inst); idle(&inst); assert(!fail_job && !delivered && !inst.dm_mailbox->delivery);
    pause_job(); delivery_submit(&inst); paused(&inst);
    for (unsigned i = 0; i < 96; i++) delivery_submit(&inst);
    assert(db_async_pending(inst.db_worker) == 1); int alive = 0;
    assert(uasync_set_timeout(ua, 10, &alive, tick, "mailbox delivery loop alive")); uint64_t end = get_time_us() + 5000000;
    while (!alive && get_time_us() < end) uasync_poll(ua, 1);
    assert(alive && !delivered); unpause(); idle(&inst); assert(delivered == DM_MAIL_BATCH_MAX);
    check_delivery_state(&inst, 105, DM_MAIL_BATCH_MAX);
    delivery_submit(&inst); idle(&inst); assert(delivered == DELIVERY_PACKETS); check_delivery_state(&inst, 105, DELIVERY_PACKETS);
    delivery_submit(&inst); idle(&inst); assert(delivered == DELIVERY_PACKETS);
    for (unsigned i = 0; i < DELIVERY_PACKETS; i++) assert(delivered_seq[i] == 1);
    delivery_now = 105; room = 0; delivery_submit(&inst); idle(&inst); room = 1;
    channel_failed = 1; delivery_submit(&inst); idle(&inst); channel_failed = 0;
    assert(delivered == DELIVERY_PACKETS); check_delivery_state(&inst, 110, DELIVERY_PACKETS);
    delivery_now = 110; route_available = 0; delivery_submit(&inst); idle(&inst); delivery_submit(&inst); idle(&inst); route_available = 1;
    assert(delivered == DELIVERY_PACKETS); check_delivery_state(&inst, 115, DELIVERY_PACKETS);
    delivery_now = 115; pause_job(); delivery_submit(&inst); paused(&inst); dm_mailbox_destroy(&inst); new_mailbox(&inst);
    delivery_submit(&inst); dm_mailbox_destroy(&inst); new_mailbox(&inst); unpause(); idle(&inst);
    assert(delivered == DELIVERY_PACKETS && !inst.dm_mailbox->delivery); check_delivery_state(&inst, 120, DM_MAIL_BATCH_MAX);
    check_delivery_state(&inst, 115, DELIVERY_PACKETS - DM_MAIL_BATCH_MAX);
    delivery_now = 120; stop_on_forward = 1; delivery_submit(&inst); idle(&inst);
    assert(delivered == DELIVERY_PACKETS + 1 && !inst.dm_mailbox && !stop_on_forward); new_mailbox(&inst);
    delivery_now = 125; fault(1, SQLITE_IOERR); delivery_submit(&inst); idle(&inst);
    delivery_submit(&inst); idle(&inst); assert(delivered == DELIVERY_PACKETS + 1 && !inst.dm_mailbox->delivery);
    assert(!db_async_destroy(inst.db_worker)); inst.db_worker = NULL; opened = 0;
    inst.db_worker = db_async_create(ua, path, 2, ready, &opened); assert(inst.db_worker); idle(&inst); assert(opened);
    delivery_submit(&inst); idle(&inst); assert(delivered == DELIVERY_PACKETS + 1 + DM_MAIL_BATCH_MAX);
    delivery_now = 130; unsigned stopped_sent = delivered; pause_job(); delivery_submit(&inst); paused(&inst);
    inst.chat_content_started = 0; unpause(); idle(&inst); assert(delivered == stopped_sent && !inst.dm_mailbox->delivery);
    inst.chat_content_started = 1;
    dm_mailbox_destroy(&inst); assert(!db_async_destroy(inst.db_worker)); mailbox_phase = delivery_phase = 0; assert(!remove(path));
    puts("PASS mailbox delivery WRITE: bounded batches, commit before send, rollback, copied bodies, timer/coalescing, OOM, stop and reopen");
}

/* PUT начинается с пустой очереди; S знает ключи автора и получателя, но не имеет legacy connection. */
static int seed_put(sqlite3* db, void* unused) {
    (void)unused; assert(!db_schema_init(db)); sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(db, "INSERT INTO nodes(node_id,x25519_pubkey,ed25519_pubkey) VALUES(?,?,?)", -1, &st, NULL));
    for (unsigned i = 0; i < 2; i++) {
        assert(!sqlite3_bind_int64(st, 1, (sqlite3_int64)(i ? peer : identity.node)));
        assert(!sqlite3_bind_blob(st, 2, i ? peer_x : identity.x, 32, SQLITE_STATIC));
        assert(!sqlite3_bind_blob(st, 3, i ? peer_ed : identity.ed, 32, SQLITE_STATIC));
        assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_reset(st));
    }
    assert(!sqlite3_finalize(st)); return SQLITE_OK;
}
struct put_state { int seq, pending, exact, tombstone, media, targets, recipient, done; };
static int read_put_state(sqlite3* db, void* arg) {
    struct put_state* state = arg; sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT (SELECT count(*) FROM dm_pending_messages WHERE recipient=?1 AND sender=?2 AND seq=?3),"
        "(SELECT count(*) FROM dm_pending_messages WHERE recipient=?1 AND sender=?2 AND seq=?3 AND msg=?4),"
        "(SELECT count(*) FROM dm_mail_receipts WHERE recipient=?1 AND sender=?2 AND seq=?3),"
        "(SELECT count(*) FROM dm_media_jobs WHERE id=?5 AND body=?6),"
        "(SELECT count(*) FROM dm_file_targets WHERE id=?5),"
        "(SELECT count(*) FROM dm_file_targets WHERE id=?5 AND node=?7 AND side=1)", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, peer_ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, identity.ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int(st, 3, state->seq);
    if (!rc) rc = sqlite3_bind_blob(st, 4, state->seq == 100 ? file_body : packets[state->seq - 1].body,
        (int)(state->seq == 100 ? file_size : packets[state->seq - 1].size), SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 5, file_id, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 6, file_body, (int)file_size, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 7, peer_ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        state->pending = sqlite3_column_int(st, 0); state->exact = sqlite3_column_int(st, 1); state->tombstone = sqlite3_column_int(st, 2);
        state->media = sqlite3_column_int(st, 3); state->targets = sqlite3_column_int(st, 4); state->recipient = sqlite3_column_int(st, 5);
    }
    int final = sqlite3_finalize(st); return rc == SQLITE_ROW ? final : rc;
}
static void put_state_done(void* arg, const struct db_async_result* result) { assert(!result->status); ((struct put_state*)arg)->done++; }
static void check_put_state(struct UTUN_INSTANCE* inst, int seq, int pending, int tombstone, int media) {
    struct put_state state = {.seq=seq}; db_async_run(inst->db_worker, &(struct db_async_task){.name="put-state", .mode=DB_ASYNC_READ,
        .work=read_put_state, .data=&state, .done=put_state_done}); idle(inst);
    assert(state.done == 1 && state.pending == pending && state.exact == pending && state.tombstone == tombstone);
    assert(state.media == media && state.targets == media && state.recipient == media);
}
static void put_frame(struct UTUN_INSTANCE* inst, unsigned packet, int media) {
    uint8_t input[MB_RECIPIENT_SIZE + 512]; size_t size = media ? file_size : packets[packet].size;
    memcpy(input, peer_ed, MB_RECIPIENT_SIZE); memcpy(input + MB_RECIPIENT_SIZE, media ? file_body : packets[packet].body, size);
    relay_before = __atomic_load_n(&commits, __ATOMIC_SEQ_CST);
    mb_put_received(inst->dm_mailbox, 7, identity.node, identity.ed, input, size + MB_RECIPIENT_SIZE, media); memset(input, 0, sizeof(input));
}
static void check_put(struct UASYNC* ua) {
    struct UTUN_INSTANCE inst = {.ua=ua, .node_id=99, .chat_content_started=1}; new_mailbox(&inst); mailbox_phase = put_phase = 1;
    allocation_min = sizeof(struct mb_put_job); delete_prefix = pause_prefix = "SELECT msg FROM dm_pending_messages";
    char path[] = "/tmp/utun-mail-put-XXXXXX"; int fd = mkstemp(path); assert(fd >= 0); close(fd); int opened = 0;
    inst.db_worker = db_async_create(ua, path, 2, ready, &opened); assert(inst.db_worker); idle(&inst); assert(opened); int seeded = 0;
    db_async_run(inst.db_worker, &(struct db_async_task){.name="put-fixture", .mode=DB_ASYNC_DIRECT,
        .work=seed_put, .data=&seeded, .done=ready}); idle(&inst); assert(seeded && !inst.topo_sqlite_db && !inst.dm_mailbox->db);
    put_frame(&inst, 0, 0); idle(&inst); assert(put_stored == 1); check_put_state(&inst, 1, 1, 0, 0);
    put_frame(&inst, 0, 0); idle(&inst); assert(put_stored == 2); check_put_state(&inst, 1, 1, 0, 0);
    uint8_t conflict[MB_RECIPIENT_SIZE + 512]; memcpy(conflict, peer_ed, MB_RECIPIENT_SIZE); memcpy(conflict + MB_RECIPIENT_SIZE, packets[0].body, packets[0].size); conflict[MB_RECIPIENT_SIZE + 16] ^= 1;
    assert(!sc_ed25519_sign(self_ed_private, conflict + MB_RECIPIENT_SIZE, packets[0].size - 64, conflict + MB_RECIPIENT_SIZE + packets[0].size - 64));
    mb_put_received(inst.dm_mailbox, 7, identity.node, identity.ed, conflict, packets[0].size + MB_RECIPIENT_SIZE, 0); idle(&inst);
    assert(put_stored == 2); check_put_state(&inst, 1, 1, 0, 0);
    relay(&inst, 0); idle(&inst); assert(put_receipts == 1); check_put_state(&inst, 1, 0, 1, 0);
    put_frame(&inst, 0, 0); idle(&inst); assert(put_receipts == 2 && put_stored == 2); check_put_state(&inst, 1, 0, 1, 0);
    for (int point = 1; point <= 5; point++) {
        fault(point, SQLITE_BUSY); put_frame(&inst, 1, 0); idle(&inst);
        assert(!__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST) && put_stored == 2 && !inst.dm_mailbox->puts);
        check_put_state(&inst, 2, 0, 0, 0);
    }
    fail_job = 1; put_frame(&inst, 1, 0); idle(&inst); assert(!fail_job && put_stored == 2); check_put_state(&inst, 2, 0, 0, 0);
    pause_job(); put_frame(&inst, 1, 0); paused(&inst); put_frame(&inst, 2, 0);
    for (unsigned i = 0; i < 96; i++) put_frame(&inst, 3, 0);
    assert(db_async_pending(inst.db_worker) == 2 && put_stored == 2); int alive = 0;
    assert(uasync_set_timeout(ua, 10, &alive, tick, "mailbox PUT handle loop alive")); uint64_t end = get_time_us() + 5000000;
    while (!alive && get_time_us() < end) uasync_poll(ua, 1);
    assert(alive && put_stored == 2); unpause(); idle(&inst); assert(put_stored == 4);
    check_put_state(&inst, 2, 1, 0, 0); check_put_state(&inst, 3, 1, 0, 0);
    pause_job(); put_frame(&inst, 3, 0); paused(&inst); put_frame(&inst, 4, 0);
    dm_mailbox_destroy(&inst); new_mailbox(&inst); unpause(); idle(&inst); assert(put_stored == 4 && !inst.dm_mailbox->puts);
    check_put_state(&inst, 4, 0, 0, 0); check_put_state(&inst, 5, 0, 0, 0);
    pause_job(); put_frame(&inst, 3, 0); paused(&inst); put_allowed = 0; unpause(); idle(&inst);
    assert(put_stored == 4); put_allowed = 1; check_put_state(&inst, 4, 0, 0, 0);
    put_supernode = 0; put_frame(&inst, 3, 0); idle(&inst); assert(put_stored == 4 && !inst.dm_mailbox->puts); put_supernode = 1;
    memcpy(conflict, peer_ed, MB_RECIPIENT_SIZE); memcpy(conflict + MB_RECIPIENT_SIZE, packets[3].body, packets[3].size); conflict[MB_RECIPIENT_SIZE + packets[3].size - 1] ^= 1;
    mb_put_received(inst.dm_mailbox, 7, identity.node, identity.ed, conflict, packets[3].size + MB_RECIPIENT_SIZE, 0); idle(&inst);
    assert(put_stored == 4); check_put_state(&inst, 4, 0, 0, 0);
    delete_prefix = "INSERT INTO dm_file_targets";
    for (int point = 1; point <= 5; point++) {
        fault(point, SQLITE_BUSY); put_frame(&inst, 0, 1); idle(&inst);
        assert(!__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST) && put_stored == 4 && put_receipts == 2);
        check_put_state(&inst, 100, 0, 0, 0);
    }
    put_frame(&inst, 0, 1); idle(&inst); check_put_state(&inst, 100, 0, 0, 1);
    put_frame(&inst, 0, 1); idle(&inst); check_put_state(&inst, 100, 0, 0, 1);
    memcpy(conflict, peer_ed, MB_RECIPIENT_SIZE); memcpy(conflict + MB_RECIPIENT_SIZE, file_body, file_size); conflict[MB_RECIPIENT_SIZE + file_size - 64 - 1] ^= 1;
    assert(!sc_ed25519_sign(self_ed_private, conflict + MB_RECIPIENT_SIZE, file_size - 64, conflict + MB_RECIPIENT_SIZE + file_size - 64));
    mb_put_received(inst.dm_mailbox, 7, identity.node, identity.ed, conflict, file_size + MB_RECIPIENT_SIZE, 1); idle(&inst); check_put_state(&inst, 100, 0, 0, 1);
    delete_prefix = "SELECT msg FROM dm_pending_messages"; fault(1, SQLITE_IOERR); put_frame(&inst, 6, 0); idle(&inst);
    put_frame(&inst, 6, 0); idle(&inst); assert(put_stored == 4 && !inst.dm_mailbox->puts);
    assert(!db_async_destroy(inst.db_worker)); inst.db_worker = NULL; opened = 0;
    inst.db_worker = db_async_create(ua, path, 2, ready, &opened); assert(inst.db_worker); idle(&inst); assert(opened);
    check_put_state(&inst, 7, 0, 0, 1); put_frame(&inst, 6, 0); idle(&inst); assert(put_stored == 5); check_put_state(&inst, 7, 1, 0, 1);
    stop_on_forward = 1; put_frame(&inst, 5, 0); idle(&inst); assert(put_stored == 6 && !inst.dm_mailbox && !stop_on_forward);
    check_put_state(&inst, 6, 1, 0, 1); assert(!db_async_destroy(inst.db_worker)); mailbox_phase = put_phase = 0; assert(!remove(path));
    puts("PASS mailbox PUT handles: signed input, immutable body/tombstone, media+targets rollback, FULL/OOM, live timer, stop/revoke and reopen");
}

static const uint8_t stored_senders[2][32] = {{99}, {100}};
struct stored_state { int supernode, submitted, hash, outbox, received, messages, done; };
static int read_stored_state(sqlite3* db, void* arg) {
    struct stored_state* state = arg; sqlite3_stmt* st = NULL; uint8_t hash[32]; SHA256(packets[0].body, packets[0].size, hash);
    int rc = sqlite3_prepare_v2(db, "SELECT COALESCE(d.supernode=?3,0),d.submitted_to=?3,d.submitted_hash=?2,"
        "(SELECT count(*) FROM dm_outbox WHERE conv_id=?1 AND seq=1),d.receipt IS NOT NULL,"
        "(SELECT count(*) FROM dm_messages WHERE conv_id=?1) FROM dm_delivery d WHERE d.conv_id=?1 AND d.seq=1", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, hash, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 3, stored_senders[0], 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        state->supernode = sqlite3_column_int(st, 0); state->submitted = sqlite3_column_int(st, 1); state->hash = sqlite3_column_int(st, 2);
        state->outbox = sqlite3_column_int(st, 3); state->received = sqlite3_column_int(st, 4); state->messages = sqlite3_column_int(st, 5);
    }
    int final = sqlite3_finalize(st); return rc == SQLITE_ROW ? final : rc;
}
static void stored_state_done(void* arg, const struct db_async_result* result) { assert(!result->status); ((struct stored_state*)arg)->done++; }
static void check_stored_state(struct UTUN_INSTANCE* inst, int supernode, int received) {
    struct stored_state state = {0}; db_async_run(inst->db_worker, &(struct db_async_task){.name="stored-state", .mode=DB_ASYNC_READ,
        .work=read_stored_state, .data=&state, .done=stored_state_done}); idle(inst);
    assert(state.done == 1 && state.supernode == (supernode != 0) && state.submitted == 1 && state.hash == 1);
    assert(state.outbox == !received && state.received == received && state.messages == PACKETS);
}
static void stored_frame(struct UTUN_INSTANCE* inst, uint64_t source, const uint8_t fact[MB_STORED_SIZE], int signed_packet) {
    struct ETCP_CONN conn = {.instance=inst};
    struct ll_entry* entry = ll_alloc_lldgram(ROUTER_SVC_PAYLOAD_OFF + 1 + MB_STORED_SIZE); assert(entry);
    entry->len = ROUTER_SVC_PAYLOAD_OFF + 1 + MB_STORED_SIZE; memset(entry->dgram, 0, entry->len);
    uint64_t group = 7; entry->dgram[0] = ETCP_RT_ID_DM_MAILBOX;
    memcpy(entry->dgram + ROUTER_SVC_GROUP_OFF, &group, 8);
    memcpy(entry->dgram + ROUTER_SVC_SRC_ED_OFF, stored_senders[source == 100], 32);
    entry->dgram[ROUTER_SVC_FLAGS_OFF] = signed_packet ? ROUTER_FLAG_SIGNED : 0;
    entry->dgram[ROUTER_SVC_PAYLOAD_OFF] = MB_STORED; memcpy(entry->dgram + ROUTER_SVC_PAYLOAD_OFF + 1, fact, MB_STORED_SIZE); mb_recv(&conn, entry);
}
static int reset_stored(sqlite3* db, void* unused) {
    (void)unused; return sqlite3_exec(db, "UPDATE dm_delivery SET supernode=NULL WHERE seq=1", NULL, NULL, NULL);
}
static void check_stored(struct UASYNC* ua) {
    struct UTUN_INSTANCE inst = {.ua=ua, .node_id=identity.node, .chat_content_started=1}; new_mailbox(&inst); new_owner(&inst); mailbox_phase = stored_phase = 1;
    allocation_min = sizeof(struct mb_store_write); delete_prefix = "UPDATE dm_delivery SET supernode";
    pause_prefix = "SELECT recipient,sender,seq,msg"; char path[] = "/tmp/utun-mail-stored-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0); close(fd); int opened = 0;
    inst.db_worker = db_async_create(ua, path, 2, ready, &opened); assert(inst.db_worker); idle(&inst); assert(opened); int seeded = 0;
    db_async_run(inst.db_worker, &(struct db_async_task){.name="stored-fixture", .mode=DB_ASYNC_DIRECT,
        .work=seed, .data=&seeded, .done=ready}); idle(&inst); assert(seeded && !inst.topo_sqlite_db && !inst.dm_mailbox->db);
    uint8_t fact[MB_STORED_SIZE], temporary[MB_STORED_SIZE]; memcpy(fact, packets[0].body, 16); memcpy(fact + MB_STORED_RECIPIENT_OFF, peer_ed, MB_RECIPIENT_SIZE);
    SHA256(packets[0].body, packets[0].size, fact + MB_STORED_HASH_OFF); memcpy(temporary, fact, MB_STORED_SIZE);
    assert(!mb_store_queue(inst.dm_mailbox, stored_senders[0], temporary, 1)); memset(temporary, 0, sizeof(temporary));
    memcpy(temporary, fact, MB_STORED_SIZE); temporary[MB_STORED_HASH_OFF] ^= 1; stored_frame(&inst, 99, temporary, 1); idle(&inst); check_stored_state(&inst, 0, 0);
    stored_frame(&inst, 100, fact, 1); idle(&inst); check_stored_state(&inst, 0, 0);
    stored_frame(&inst, 99, fact, 0); idle(&inst); check_stored_state(&inst, 0, 0);
    stored_allowed = 0; stored_frame(&inst, 99, fact, 1); idle(&inst); stored_allowed = 1; check_stored_state(&inst, 0, 0);
    stored_supernode = 0; stored_frame(&inst, 99, fact, 1); idle(&inst); stored_supernode = 1; check_stored_state(&inst, 0, 0);
    for (int point = 1; point <= 5; point++) {
        fault(point, SQLITE_BUSY); stored_frame(&inst, 99, fact, 1); idle(&inst);
        if (__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST))
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "STORED fixture: SQL fault was not exercised point=%d", point);
        assert(!__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST)); check_stored_state(&inst, 0, 0);
    }
    fail_job = 1; stored_frame(&inst, 99, fact, 1); idle(&inst); assert(!fail_job); check_stored_state(&inst, 0, 0);
    struct state blocked[2] = {{0}, {0}}; pause_job();
    db_async_run(inst.db_worker, &(struct db_async_task){.name="stored-busy", .mode=DB_ASYNC_READ,
        .work=delivery_read_blocker, .data=&blocked[0], .done=state_done}); paused(&inst);
    db_async_run(inst.db_worker, &(struct db_async_task){.name="stored-queued", .mode=DB_ASYNC_READ,
        .work=delivery_read_blocker, .data=&blocked[1], .done=state_done}); stored_frame(&inst, 99, fact, 1);
    assert(db_async_pending(inst.db_worker) == 2); unpause(); idle(&inst); check_stored_state(&inst, 0, 0);
    stored_frame(&inst, 99, fact, 1); dm_mailbox_destroy(&inst); idle(&inst); check_stored_state(&inst, 99, 0); new_mailbox(&inst);
    int reset = 0; db_async_run(inst.db_worker, &(struct db_async_task){.name="reset-stored", .mode=DB_ASYNC_WRITE,
        .work=reset_stored, .data=&reset, .done=ready}); idle(&inst); assert(reset); check_stored_state(&inst, 0, 0);
    fault(1, SQLITE_IOERR); stored_frame(&inst, 99, fact, 1); idle(&inst); stored_frame(&inst, 99, fact, 1); idle(&inst);
    assert(!db_async_destroy(inst.db_worker)); inst.db_worker = NULL; opened = 0;
    inst.db_worker = db_async_create(ua, path, 2, ready, &opened); assert(inst.db_worker); idle(&inst); assert(opened);
    check_stored_state(&inst, 0, 0); stored_frame(&inst, 99, fact, 1); idle(&inst); check_stored_state(&inst, 99, 0);
    struct reply delivered = {0}; dm_receipt_submit(&inst, packets[0].receipt, done, &delivered); idle(&inst);
    assert(delivered.calls == 1 && !delivered.status); check_stored_state(&inst, 99, 1);
    stored_frame(&inst, 100, fact, 1); idle(&inst); check_stored_state(&inst, 99, 1);
    dm_core_destroy(&inst); dm_mailbox_destroy(&inst); assert(!db_async_destroy(inst.db_worker)); mailbox_phase = stored_phase = 0;
    assert(!remove(path)); puts("PASS mailbox STORED/tracking: copied FIFO facts, signed scoped source/hash, rollback/FULL/OOM, stop, recipient ACK and reopen");
}

/* Координатор с настоящим RW worker и без legacy: M=100, S=99, подписанные A/B из make_packets. */
static void new_media(struct UTUN_INSTANCE* inst) {
    memset(inst->my_ed25519_pubkey, 0, 32); inst->my_ed25519_pubkey[0] = 99;
    new_mailbox(inst); struct dm_mb_media* media = u_calloc(1, sizeof(*media)); assert(media);
    media->inst = inst; inst->dm_mailbox->media = media;
}
static int reset_media(sqlite3* db, void* unused) {
    (void)unused; assert(!sqlite3_exec(db, "DELETE FROM dm_media_holders;DELETE FROM dm_media_jobs;DELETE FROM dm_file_targets", NULL, NULL, NULL));
    assert(!dm_store_verify_message(db, peer, file_body, file_size) && !dm_mailbox_media_register(db, peer_ed, file_body, file_size));
    assert(!sqlite3_exec(db, "UPDATE dm_media_jobs SET holder=x'6400000000000000000000000000000000000000000000000000000000000000',retry_at=99", NULL, NULL, NULL)); sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(db, "INSERT INTO dm_media_holders(id,node,assignment_id) VALUES(?,x'6400000000000000000000000000000000000000000000000000000000000000',?)", -1, &st, NULL));
    assert(!sqlite3_bind_blob(st, 1, file_id, 16, SQLITE_STATIC) && !sqlite3_bind_blob(st, 2, assignment_id, 16, SQLITE_STATIC));
    assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st)); return SQLITE_OK;
}
static void media_reset(struct UTUN_INSTANCE* inst) {
    int done = 0; db_async_run(inst->db_worker, &(struct db_async_task){.name="media-reset", .mode=DB_ASYNC_WRITE,
        .work=reset_media, .data=&done, .done=ready}); idle(inst); assert(done);
}
struct media_state { int holder, stored, policy, expires, expired, released, received, taken, retry, terminal, done; };
static int read_media(sqlite3* db, void* arg) {
    struct media_state* state = arg; sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT CASE j.holder WHEN x'6400000000000000000000000000000000000000000000000000000000000000' THEN 100 WHEN x'6500000000000000000000000000000000000000000000000000000000000000' THEN 101 ELSE 0 END,j.stored,j.delete_on_open,j.expires,j.expired,h.released,"
        "t.received IS NOT NULL,t.taken IS NOT NULL,j.retry_at,j.receipt IS NOT NULL FROM dm_media_jobs j"
        " JOIN dm_media_holders h ON h.id=j.id AND h.node=x'6400000000000000000000000000000000000000000000000000000000000000' JOIN dm_file_targets t ON t.id=j.id AND t.node=? WHERE j.id=?",
        -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, peer_ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, file_id, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        state->holder = sqlite3_column_int(st, 0); state->stored = sqlite3_column_int(st, 1); state->policy = sqlite3_column_int(st, 2);
        state->expires = sqlite3_column_int(st, 3); state->expired = sqlite3_column_int(st, 4); state->released = sqlite3_column_int(st, 5);
        state->received = sqlite3_column_int(st, 6); state->taken = sqlite3_column_int(st, 7); state->retry = sqlite3_column_int(st, 8);
        state->terminal = sqlite3_column_int(st, 9);
    }
    int final = sqlite3_finalize(st); return rc == SQLITE_ROW ? final : rc;
}
static void media_state_done(void* arg, const struct db_async_result* result) { assert(!result->status); ((struct media_state*)arg)->done++; }
static void media_expect(struct UTUN_INSTANCE* inst, struct media_state expected) {
    struct media_state state = {0}; db_async_run(inst->db_worker, &(struct db_async_task){.name="media-state", .mode=DB_ASYNC_READ,
        .work=read_media, .data=&state, .done=media_state_done}); idle(inst);
    int same = state.done == 1 && state.holder == expected.holder && state.stored == expected.stored && state.policy == expected.policy &&
        state.expires == expected.expires && state.expired == expected.expired && state.released == expected.released &&
        state.received == expected.received && state.taken == expected.taken && state.retry == expected.retry && state.terminal == expected.terminal;
    if (!same) DEBUG_ERROR(DEBUG_CATEGORY_DM,
        "media state: holder=%d stored=%d policy=%d expires=%d expired=%d released=%d received=%d taken=%d retry=%d terminal=%d",
        state.holder, state.stored, state.policy, state.expires, state.expired, state.released,
        state.received, state.taken, state.retry, state.terminal);
    assert(same);
}
static void media_input(struct UTUN_INSTANCE* inst, uint8_t cmd, int policy) {
    uint8_t input[41]; uint64_t expiry = 200;
    memcpy(input, assignment_id, 16); memcpy(input + 16, file_id, 16); memcpy(input + 32, &expiry, 8); input[40] = (uint8_t)policy;
    if (cmd == DM_MEDIA_REJECT) input[32] = 1;
    relay_before = __atomic_load_n(&commits, __ATOMIC_SEQ_CST);
    dm_mailbox_media_recv(inst->dm_mailbox->media, 7, cycle_holder_ed[0], cmd, input,
        cmd == DM_MEDIA_STORED || cmd == DM_MEDIA_RESERVED ? 41 : cmd == DM_MEDIA_REJECT ? 33 : 32);
    memset(input, 0, sizeof(input));
}
static void media_fact(struct UTUN_INSTANCE* inst, const uint8_t* receipt) {
    uint8_t input[DM_MEDIA_RECEIPT_SIZE]; memcpy(input, receipt, sizeof(input));
    relay_before = __atomic_load_n(&commits, __ATOMIC_SEQ_CST);
    dm_mailbox_media_recv(inst->dm_mailbox->media, 7, peer_ed, DM_MEDIA_RECEIPT, input, sizeof(input)); memset(input, 0, sizeof(input));
}
static void fixture_sql_done(void* arg, const struct db_sql_result* result) { assert(!result->outcome.status); (*(int*)arg)++; }
static int missing_media_key(sqlite3* db, void* unused) {
    (void)unused; uint8_t x[32] = {9}, ed[32] = {8};
    assert(dm_store_node_keys(db, peer, x, ed) == SQLITE_NOTFOUND && x[0] == 9 && ed[0] == 8); return SQLITE_OK;
}
static void check_media_controls(struct UASYNC* ua) {
    struct UTUN_INSTANCE inst = {.ua=ua, .node_id=99, .chat_content_started=1}; new_media(&inst); media_phase = 1; delivery_now = 100;
    memcpy(media_receipt, DM_FILE_RECEIVED_DEVICE, 8); memcpy(media_receipt + 8, file_body, 16); memcpy(media_receipt + DM_FACT_AUTHOR_OFF, identity.ed, 32);
    memcpy(media_receipt + DM_FACT_RECIPIENT_OFF, peer_ed, 32); memcpy(media_receipt + DM_FILE_FACT_ID_OFF, file_id, 16); SHA256(file_body, file_size, media_receipt + DM_FILE_FACT_HASH_OFF);
    assert(!sc_ed25519_sign(peer_ed_private, media_receipt, DM_FILE_FACT_SIG_OFF, media_receipt + DM_FILE_FACT_SIG_OFF));
    char path[] = "/tmp/utun-media-control-XXXXXX"; int fd = mkstemp(path); assert(fd >= 0); close(fd); int opened = 0;
    inst.db_worker = db_async_create(ua, path, 2, ready, &opened); assert(inst.db_worker); idle(&inst); assert(opened); int seeded = 0;
    db_async_run(inst.db_worker, &(struct db_async_task){.name="media-fixture", .mode=DB_ASYNC_DIRECT,
        .work=seed_put, .data=&seeded, .done=ready}); idle(&inst); assert(seeded && !inst.topo_sqlite_db && !inst.dm_mailbox->db); media_reset(&inst);
    struct media_state initial = {.holder=100, .policy=-1, .retry=99}; media_expect(&inst, initial);
    int key_changed = 0; db_exec(inst.db_worker, "UPDATE nodes SET ed25519_pubkey=NULL WHERE node_id=?",
        DB_ARGS(DB_I64(peer)), fixture_sql_done, &key_changed); idle(&inst); assert(key_changed == 1);
    media_fact(&inst, media_receipt); idle(&inst); media_expect(&inst, initial); assert(!media_receipts);
    int checked = 0; db_async_run(inst.db_worker, &(struct db_async_task){.name="media-key-unavailable", .mode=DB_ASYNC_READ,
        .work=missing_media_key, .data=&checked, .done=ready}); idle(&inst); assert(checked == 1);
    db_exec(inst.db_worker, "UPDATE nodes SET ed25519_pubkey=? WHERE node_id=?",
        DB_ARGS(DB_BLOB(peer_ed, 32), DB_I64(peer)), fixture_sql_done, &key_changed); idle(&inst); assert(key_changed == 2);
    allocation_min = sizeof(struct dm_control_job); fail_job = 1; media_input(&inst, DM_MEDIA_RESERVED, 0); idle(&inst);
    assert(!fail_job); media_expect(&inst, initial);
    media_input(&inst, DM_MEDIA_RESERVED, 0); idle(&inst);
    struct media_state reserved = {.holder=100, .expires=200}; media_expect(&inst, reserved);
    media_input(&inst, DM_MEDIA_STORED, 0); idle(&inst); reserved.stored = 1; media_expect(&inst, reserved);
    media_input(&inst, DM_MEDIA_RESERVED, 0); idle(&inst); media_expect(&inst, reserved);
    media_input(&inst, DM_MEDIA_RESERVED, 1); idle(&inst); media_expect(&inst, reserved);
    media_input(&inst, DM_MEDIA_REJECT, 0); idle(&inst); media_expect(&inst, reserved);
    media_reset(&inst); delete_prefix = "UPDATE dm_media_jobs SET holder=NULL";
    for (int point = 1; point <= 5; point++) {
        fault(point, SQLITE_BUSY); media_input(&inst, DM_MEDIA_EXPIRED, 0); idle(&inst);
        assert(!__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST)); media_expect(&inst, initial);
    }
    media_input(&inst, DM_MEDIA_REJECT, 0); idle(&inst);
    media_expect(&inst, (struct media_state){.policy=-1, .released=3, .retry=160});
    media_input(&inst, DM_MEDIA_REJECT, 0); idle(&inst); media_expect(&inst, (struct media_state){.policy=-1, .released=3, .retry=160});
    media_reset(&inst); media_input(&inst, DM_MEDIA_EXPIRED, 0); idle(&inst);
    media_expect(&inst, (struct media_state){.policy=-1, .released=2, .expired=1});
    media_reset(&inst); media_input(&inst, DM_MEDIA_DELETE_ACK, 0); idle(&inst);
    media_expect(&inst, (struct media_state){.holder=100, .policy=-1, .released=1, .retry=99});
    media_reset(&inst); delete_prefix = "SELECT ed25519_pubkey";
    const int key_faults[] = {1, 2, 3, 4};
    for (unsigned i = 0; i < sizeof(key_faults) / sizeof(key_faults[0]); i++) {
        fault(key_faults[i], SQLITE_BUSY); media_fact(&inst, media_receipt); idle(&inst);
        assert(!__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST) && !media_receipts); media_expect(&inst, initial);
    }
    delete_prefix = "UPDATE dm_file_targets SET received";
    for (int point = 1; point <= 5; point++) {
        fault(point, SQLITE_BUSY); media_fact(&inst, media_receipt); idle(&inst);
        assert(!__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST) && !media_receipts); media_expect(&inst, initial);
    }
    delete_prefix = "UPDATE dm_media_jobs SET retry_at=0";
    for (int point = 1; point <= 4; point++) {
        if (point == 2) continue; /* Этот UPDATE связывает только BLOB; int64 bind проверен на UPDATE факта выше. */
        fault(point, SQLITE_BUSY); media_fact(&inst, media_receipt); idle(&inst);
        assert(!__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST) && !media_receipts); media_expect(&inst, initial);
    }
    __atomic_store_n(&body_oom, 1, __ATOMIC_SEQ_CST); media_fact(&inst, media_receipt); idle(&inst);
    assert(!__atomic_load_n(&body_oom, __ATOMIC_SEQ_CST) && !media_receipts); media_expect(&inst, initial);
    uint8_t bad[DM_MEDIA_RECEIPT_SIZE]; memcpy(bad, media_receipt, sizeof(bad)); bad[56] ^= 1;
    media_fact(&inst, bad); idle(&inst); memcpy(bad, media_receipt, sizeof(bad)); bad[151] ^= 1; media_fact(&inst, bad); idle(&inst);
    assert(!media_receipts); media_expect(&inst, initial);
    media_fact(&inst, media_receipt); idle(&inst); assert(media_receipts == 1);
    struct media_state received = {.holder=100, .policy=-1, .received=1}; media_expect(&inst, received);
    int done = 0; db_exec(inst.db_worker, "UPDATE dm_media_jobs SET receipt=?,body=NULL WHERE id=?",
        DB_ARGS(DB_BLOB(media_receipt, sizeof(media_receipt)), DB_BLOB(file_id, 16)), fixture_sql_done, &done); idle(&inst); assert(done);
    media_input(&inst, DM_MEDIA_STORED, 0); idle(&inst); assert(media_deletes == 1);
    media_fact(&inst, media_receipt); idle(&inst); assert(media_receipts == 1);
    media_expect(&inst, (struct media_state){.holder=100, .policy=-1, .received=1, .terminal=1});
    media_reset(&inst); pause_prefix = "UPDATE dm_media_jobs SET holder=NULL"; pause_job(); media_input(&inst, DM_MEDIA_EXPIRED, 0); paused(&inst);
    media_input(&inst, DM_MEDIA_RESERVED, 0); dm_mailbox_destroy(&inst); new_media(&inst); unpause(); idle(&inst); media_expect(&inst, initial);
    pause_job(); media_input(&inst, DM_MEDIA_EXPIRED, 0); paused(&inst); media_allowed = 0; unpause(); idle(&inst);
    media_allowed = 1; media_expect(&inst, initial);
    pause_job(); media_input(&inst, DM_MEDIA_EXPIRED, 0); paused(&inst); inst.chat_content_started = 0;
    unpause(); idle(&inst); assert(!inst.dm_mailbox->media->controls); inst.chat_content_started = 1; media_expect(&inst, initial);
    pause_job(); media_input(&inst, DM_MEDIA_EXPIRED, 0); paused(&inst); media_input(&inst, DM_MEDIA_RESERVED, 0);
    for (unsigned i = 0; i < 96; i++) media_input(&inst, DM_MEDIA_RESERVED, 0);
    assert(db_async_pending(inst.db_worker) == 2); int alive = 0;
    assert(uasync_set_timeout(ua, 10, &alive, tick, "media-control loop alive")); uint64_t end = get_time_us() + 5000000;
    while (!alive && get_time_us() < end) uasync_poll(ua, 1);
    assert(alive); unpause(); idle(&inst); media_expect(&inst, (struct media_state){.policy=-1, .released=2, .expired=1});
    media_reset(&inst); delete_prefix = "SELECT ed25519_pubkey"; fault(1, SQLITE_IOERR); media_fact(&inst, media_receipt); idle(&inst);
    media_fact(&inst, media_receipt); idle(&inst); assert(media_receipts == 1 && !inst.dm_mailbox->media->controls);
    assert(!db_async_destroy(inst.db_worker)); inst.db_worker = NULL; opened = 0;
    inst.db_worker = db_async_create(ua, path, 2, ready, &opened); assert(inst.db_worker); idle(&inst); assert(opened); media_expect(&inst, initial);
    stop_on_forward = 1; media_fact(&inst, media_receipt); idle(&inst); assert(media_receipts == 2 && !inst.dm_mailbox && !stop_on_forward);
    media_expect(&inst, received); assert(!db_async_destroy(inst.db_worker)); media_phase = 0; assert(!remove(path));
    puts("PASS media-control handles: copied replies after commit, immutable policy, release/fact rollback, "
        "signatures, FULL/OOM, stop/revoke and reopen");
}

enum cycle_fixture { C_NEW, C_ASSIGNED, C_STORED, C_TTL, C_RETAIN, C_TERMINAL, C_RELEASED, C_FINISHED_TTL,
    C_QUOTA, C_OLD, C_EMPTY };
struct cycle_reset { int mode, done; };
/* Только SQL worker: реальные подписанные A/B и таблица группы, два подходящих хранилища. */
static int seed_cycle(sqlite3* db, void* unused) {
    seed_put(db, unused); assert(!db_schema_table(db, "peers_" TEST_CHANNEL "", "peers"));
    sqlite3_stmt* holder = NULL;
    assert(!sqlite3_prepare_v2(db, "INSERT INTO nodes(node_id,x25519_pubkey,ed25519_pubkey) VALUES(?,?,?)", -1, &holder, NULL));
    for (unsigned n = 0; n < 2; n++) {
        const uint8_t x[32] = {(uint8_t)(n + 1)};
        assert(!sqlite3_bind_int(holder, 1, (int)(100 + n)) && !sqlite3_bind_blob(holder, 2, x, 32, SQLITE_TRANSIENT));
        assert(!sqlite3_bind_blob(holder, 3, cycle_holder_ed[n], 32, SQLITE_STATIC) && sqlite3_step(holder) == SQLITE_DONE);
        assert(!sqlite3_reset(holder));
    }
    assert(!sqlite3_finalize(holder));
    assert(!sqlite3_exec(db, "INSERT INTO peers_" TEST_CHANNEL "(node_id,x25519_pubkey,ed25519_pubkey,join_sig,join_ts,admitted,adm_tags)"
        " VALUES(99,zeroblob(32),x'6300000000000000000000000000000000000000000000000000000000000000',zeroblob(64),1,1,'{}'),"
        "(100,zeroblob(32),x'6400000000000000000000000000000000000000000000000000000000000000',zeroblob(64),1,1,'{\"storage\":\"yes\"}'),"
        "(101,zeroblob(32),x'6500000000000000000000000000000000000000000000000000000000000000',zeroblob(64),1,1,'{\"storage\":\"yes\"}')", NULL, NULL, NULL));
    return SQLITE_OK;
}
static int reset_cycle(sqlite3* db, void* arg) {
    int mode = ((struct cycle_reset*)arg)->mode; reset_media(db, NULL);
    assert(!sqlite3_exec(db, "UPDATE peers_" TEST_CHANNEL " SET admitted=1", NULL, NULL, NULL));
    if (mode == C_NEW || mode == C_QUOTA || mode == C_OLD) {
        assert(!sqlite3_exec(db, "UPDATE dm_media_jobs SET holder=NULL", NULL, NULL, NULL));
        assert(!sqlite3_exec(db, mode == C_NEW ? "DELETE FROM dm_media_holders" :
            "UPDATE dm_media_holders SET released=3", NULL, NULL, NULL));
    }
    if (mode >= C_STORED && mode <= C_FINISHED_TTL)
        assert(!sqlite3_exec(db, "UPDATE dm_media_jobs SET stored=1,expires=200,delete_on_open=0", NULL, NULL, NULL));
    if (mode == C_TTL || mode == C_FINISHED_TTL)
        assert(!sqlite3_exec(db, "UPDATE dm_media_jobs SET expires=99", NULL, NULL, NULL));
    if (mode >= C_RETAIN && mode <= C_FINISHED_TTL) {
        sqlite3_stmt* st = NULL;
        assert(!sqlite3_prepare_v2(db, "UPDATE dm_file_targets SET received=?", -1, &st, NULL));
        assert(!sqlite3_bind_blob(st, 1, media_receipt, sizeof(media_receipt), SQLITE_STATIC));
        assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
        if (mode != C_RETAIN)
            assert(!sqlite3_exec(db, "UPDATE dm_media_jobs SET receipt=(SELECT received FROM dm_file_targets)", NULL, NULL, NULL));
    }
    if (mode == C_RELEASED) assert(!sqlite3_exec(db, "UPDATE dm_media_holders SET released=1", NULL, NULL, NULL));
    if (mode == C_OLD) assert(!sqlite3_exec(db, "UPDATE peers_" TEST_CHANNEL " SET admitted=0 WHERE node_id=101", NULL, NULL, NULL));
    if (mode == C_EMPTY) assert(!sqlite3_exec(db, "DELETE FROM dm_media_holders;DELETE FROM dm_media_jobs", NULL, NULL, NULL));
    return SQLITE_OK;
}
static void cycle_reset_done(void* arg, const struct db_async_result* result) {
    assert(!result->status); ((struct cycle_reset*)arg)->done++;
}
static void cycle_reset(struct UTUN_INSTANCE* inst, int mode) {
    struct cycle_reset request = {.mode=mode};
    db_async_run(inst->db_worker, &(struct db_async_task){.name="cycle-fixture", .mode=DB_ASYNC_WRITE,
        .work=reset_cycle, .data=&request, .done=cycle_reset_done}); idle(inst); assert(request.done == 1);
    cycle_packets = cycle_offers = cycle_stores = cycle_notices = media_receipts = media_deletes = 0;
    cycle_location = mode == C_STORED ? 100 : identity.node; cycle_expiry = mode == C_STORED ? 200 : 0;
    cycle_storage = mode == C_QUOTA ? 101 : 100; memset(cycle_assignment, 0, 16);
}
struct cycle_state { int fields[10], done; };
static void cycle_state_done(void* arg, const struct db_sql_result* result) {
    struct cycle_state* state = arg;
    assert(!result->outcome.status && result->rows == 1 && result->columns == 10);
    for (unsigned i = 0; i < 10; i++) {
        const struct db_value* value = &result->values[i];
        if (value->type != SQLITE_INTEGER || value->i64 != state->fields[i])
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "cycle state column=%u type=%d actual=%lld expected=%d",
                i, value->type, (long long)value->i64, state->fields[i]);
        assert(value->type == SQLITE_INTEGER && value->i64 == state->fields[i]);
    }
    state->done++;
}
/* Состояние читается через независимый запрос: holder/stored/expiry/expired/policy/retry/terminal/body/released/active. */
static void cycle_expect(struct UTUN_INSTANCE* inst, const int expected[10]) {
    struct cycle_state state = {0}; memcpy(state.fields, expected, sizeof(state.fields));
    db_query(inst->db_worker, "SELECT CASE holder WHEN x'6400000000000000000000000000000000000000000000000000000000000000' THEN 100 WHEN x'6500000000000000000000000000000000000000000000000000000000000000' THEN 101 ELSE 0 END,stored,expires,expired,delete_on_open,retry_at,receipt IS NOT NULL,body IS NOT NULL,"
        "COALESCE((SELECT released FROM dm_media_holders h WHERE h.id=j.id AND h.node=x'6400000000000000000000000000000000000000000000000000000000000000'),-1),"
        "(SELECT count(*) FROM dm_media_holders h WHERE h.id=j.id AND h.released=0) FROM dm_media_jobs j WHERE id=?",
        DB_ARGS(DB_BLOB(file_id, 16)), cycle_state_done, &state); idle(inst); assert(state.done == 1);
}
static void cycle_submit(struct UTUN_INSTANCE* inst) {
    relay_before = __atomic_load_n(&commits, __ATOMIC_SEQ_CST); dm_cycle_start(inst->dm_mailbox->media, 1);
}
/* Один handle охватывает SQL-снимок, выбор в uasync и назначение; wire возможен только после commit. */
static void cycle_routes(struct UTUN_INSTANCE* inst, struct TOPO_GROUPS* groups, struct TOPO_GROUP* group) {
    *group = (struct TOPO_GROUP){.group_id=7, .group_type=TOPO_GROUP_TYPE_CHAT, .instance=inst, .channel_id=TEST_CHANNEL};
    group->ll.size = sizeof(*group) - sizeof(group->ll);
    *groups = (struct TOPO_GROUPS){.instance=inst, .group_list=queue_new(inst->ua, 16, 0, 8, "cycle routes")};
    assert(groups->group_list && !queue_data_put_with_index(groups->group_list, &group->ll)); inst->topo_groups = groups;
}
static void check_media_cycle(struct UASYNC* ua) {
    struct UTUN_INSTANCE inst = {.ua=ua, .node_id=99, .chat_content_started=1}; new_media(&inst); media_phase = cycle_phase = 1; delivery_now = 100;
    struct TOPO_GROUPS groups; struct TOPO_GROUP group; cycle_routes(&inst, &groups, &group);
    allocation_min = sizeof(struct dm_cycle_job); char path[] = "/tmp/utun-media-cycle-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0); close(fd); int opened = 0;
    inst.db_worker = db_async_create(ua, path, 2, ready, &opened); assert(inst.db_worker); idle(&inst); assert(opened);
    int seeded = 0; db_async_run(inst.db_worker, &(struct db_async_task){.name="cycle-seed", .mode=DB_ASYNC_DIRECT,
        .work=seed_cycle, .data=&seeded, .done=ready}); idle(&inst); assert(seeded && !inst.topo_sqlite_db && !inst.dm_mailbox->db);
    cycle_reset(&inst, C_EMPTY); cycle_submit(&inst); idle(&inst); assert(!cycle_packets && !inst.dm_mailbox->media->cycle);
    cycle_reset(&inst, C_NEW); cycle_submit(&inst); idle(&inst); assert(cycle_offers == 1 && cycle_stores == 1);
    cycle_expect(&inst, (int[]){100,0,0,0,-1,105,0,1,0,1});
    uint8_t first_assignment[16]; memcpy(first_assignment, cycle_assignment, 16);
    delivery_now = 105; cycle_submit(&inst); idle(&inst);
    assert(cycle_stores == 2 && !memcmp(first_assignment, cycle_assignment, 16)); delivery_now = 100;
    cycle_reset(&inst, C_QUOTA); cycle_submit(&inst); idle(&inst); assert(cycle_stores == 1);
    cycle_expect(&inst, (int[]){101,0,0,0,-1,105,0,1,3,1});
    cycle_reset(&inst, C_OLD); cycle_submit(&inst); idle(&inst);
    assert(cycle_stores == 1 && memcmp(cycle_assignment, assignment_id, 16));
    cycle_expect(&inst, (int[]){100,0,0,0,-1,105,0,1,0,1});
    for (unsigned denied = 0; denied < 3; denied++) {
        cycle_reset(&inst, C_NEW); media_allowed = denied != 0; cycle_supernode = denied != 1; route_available = denied != 2;
        cycle_submit(&inst); idle(&inst); assert(cycle_offers == 1 && !cycle_stores);
        cycle_expect(&inst, (int[]){0,0,0,0,-1,105,0,1,-1,0}); media_allowed = cycle_supernode = route_available = 1;
    }
    cycle_reset(&inst, C_STORED); cycle_submit(&inst); idle(&inst); assert(cycle_offers == 1 && cycle_stores == 1);
    assert(!memcmp(cycle_assignment, assignment_id, 16)); cycle_expect(&inst, (int[]){100,1,200,0,0,105,0,1,0,1});
    cycle_reset(&inst, C_TTL); cycle_submit(&inst); idle(&inst); assert(!cycle_packets);
    cycle_expect(&inst, (int[]){0,0,99,1,0,0,0,1,2,0}); cycle_submit(&inst); idle(&inst);
    assert(cycle_notices == 1 && cycle_offers == 1 && !cycle_stores);
    cycle_reset(&inst, C_RETAIN); cycle_submit(&inst); idle(&inst); assert(!cycle_packets);
    cycle_expect(&inst, (int[]){100,1,200,0,0,0,1,1,0,1}); cycle_submit(&inst); idle(&inst);
    assert(media_deletes == 1 && media_receipts == 1); cycle_expect(&inst, (int[]){100,1,200,0,0,105,1,1,0,1});
    cycle_reset(&inst, C_RELEASED); cycle_submit(&inst); idle(&inst); assert(!media_deletes && media_receipts == 1);
    cycle_expect(&inst, (int[]){100,1,200,0,0,105,1,0,1,0});
    cycle_reset(&inst, C_FINISHED_TTL); cycle_submit(&inst); idle(&inst); assert(!media_deletes && media_receipts == 1);
    cycle_expect(&inst, (int[]){100,1,99,0,0,105,1,0,2,0});

    /* Ошибка второго UPDATE откатывает уже вставленный token вместе с retry; пакеты не публикуются. */
    delete_prefix = "UPDATE dm_media_jobs SET holder=?2";
    for (int point = 1; point <= 5; point++) {
        cycle_reset(&inst, C_NEW); fault(point, SQLITE_BUSY); cycle_submit(&inst); idle(&inst);
        assert(!__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST) && !cycle_packets && !inst.dm_mailbox->media->cycle);
        cycle_expect(&inst, (int[]){0,0,0,0,-1,99,0,1,-1,0});
    }
    delete_prefix = "UPDATE dm_media_jobs SET body=NULL";
    for (int point = 1; point <= 5; point++) {
        if (point == 2) continue;
        cycle_reset(&inst, C_RELEASED); fault(point, SQLITE_BUSY); cycle_submit(&inst); idle(&inst);
        assert(!__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST) && !cycle_packets);
        cycle_expect(&inst, (int[]){100,1,200,0,0,99,1,1,1,0});
    }
    cycle_reset(&inst, C_NEW); fail_job = 1; cycle_submit(&inst); idle(&inst); assert(!fail_job && !cycle_packets);
    cycle_expect(&inst, (int[]){0,0,0,0,-1,99,0,1,-1,0});
    __atomic_store_n(&cycle_oom, 1, __ATOMIC_SEQ_CST); cycle_submit(&inst); idle(&inst);
    assert(!__atomic_load_n(&cycle_oom, __ATOMIC_SEQ_CST) && !cycle_packets);
    cycle_expect(&inst, (int[]){0,0,0,0,-1,99,0,1,-1,0});
    cycle_reset(&inst, C_TTL); cycle_submit(&inst); idle(&inst);
    __atomic_store_n(&cycle_oom, 1, __ATOMIC_SEQ_CST); cycle_submit(&inst); idle(&inst);
    assert(!__atomic_load_n(&cycle_oom, __ATOMIC_SEQ_CST) && !cycle_packets);
    cycle_expect(&inst, (int[]){0,0,99,1,0,0,0,1,2,0});

    cycle_reset(&inst, C_NEW); pause_prefix = "UPDATE dm_media_jobs SET holder=?2"; pause_job(); cycle_submit(&inst); paused(&inst);
    for (unsigned i = 0; i < 96; i++) dm_cycle_start(inst.dm_mailbox->media, 1);
    assert(db_async_pending(inst.db_worker) == 1 && !cycle_packets); int alive = 0;
    assert(uasync_set_timeout(ua, 10, &alive, tick, "media-cycle loop alive")); uint64_t end = get_time_us() + 5000000;
    while (!alive && get_time_us() < end) uasync_poll(ua, 1);
    assert(alive); dm_mailbox_destroy(&inst); new_media(&inst); unpause(); idle(&inst); assert(!cycle_packets);
    cycle_expect(&inst, (int[]){0,0,0,0,-1,99,0,1,-1,0});
    struct state blocked[2] = {{0}, {0}}; pause_prefix = "SELECT recipient,sender,seq,msg"; pause_job();
    db_async_run(inst.db_worker, &(struct db_async_task){.name="cycle-busy", .mode=DB_ASYNC_READ,
        .work=delivery_read_blocker, .data=&blocked[0], .done=state_done}); paused(&inst);
    db_async_run(inst.db_worker, &(struct db_async_task){.name="cycle-queued", .mode=DB_ASYNC_READ,
        .work=delivery_read_blocker, .data=&blocked[1], .done=state_done}); cycle_submit(&inst);
    assert(db_async_pending(inst.db_worker) == 2 && !inst.dm_mailbox->media->cycle); unpause(); idle(&inst);
    cycle_expect(&inst, (int[]){0,0,0,0,-1,99,0,1,-1,0});
    cycle_submit(&inst); idle(&inst); assert(cycle_stores == 1);
    cycle_reset(&inst, C_NEW); stop_on_forward = 1; cycle_submit(&inst); idle(&inst);
    assert(cycle_offers == 1 && !cycle_stores && !inst.dm_mailbox && !stop_on_forward);
    cycle_expect(&inst, (int[]){100,0,0,0,-1,105,0,1,0,1}); new_media(&inst);
    cycle_reset(&inst, C_NEW); pause_prefix = "UPDATE dm_media_jobs SET holder=?2"; pause_job(); cycle_submit(&inst); paused(&inst);
    inst.chat_content_started = 0; unpause(); idle(&inst); assert(!cycle_packets && !inst.dm_mailbox->media->cycle);
    inst.chat_content_started = 1; cycle_expect(&inst, (int[]){0,0,0,0,-1,99,0,1,-1,0});
    cycle_reset(&inst, C_NEW); delete_prefix = "UPDATE dm_media_jobs SET holder=?2";
    fault(3, SQLITE_IOERR); cycle_submit(&inst); idle(&inst); cycle_submit(&inst); idle(&inst);
    assert(!cycle_packets && !inst.dm_mailbox->media->cycle); assert(!db_async_destroy(inst.db_worker)); opened = 0;
    inst.db_worker = db_async_create(ua, path, 2, ready, &opened); assert(inst.db_worker); idle(&inst); assert(opened);
    cycle_expect(&inst, (int[]){0,0,0,0,-1,99,0,1,-1,0}); cycle_submit(&inst); idle(&inst); assert(cycle_stores == 1);
    dm_mailbox_destroy(&inst); assert(!db_async_destroy(inst.db_worker)); media_phase = cycle_phase = 0; assert(!remove(path));
    assert(queue_data_get(groups.group_list) == &group.ll); queue_free(groups.group_list);
    puts("PASS media-cycle handle: selection/reuse, retention/TTL/cleanup, commit-before-wire, rollback, "
        "FULL/OOM, coalescing, loop timer, stop/restart/send-stop and fatal reopen");
}

/* Настоящий init с RW worker: никакого legacy SQL и немедленного ожидания очереди. */
static void startup_owner(struct UTUN_INSTANCE* inst) {
    assert(!inst->dm_mailbox && !inst->topo_sqlite_db && !dm_mailbox_init(inst));
    struct dm_mb_state* mb = inst->dm_mailbox; assert(!mb->db && mb->media->reset_retry == 1);
    assert(mb->timer && mb->media->timer);
    uasync_cancel_timeout(inst->ua, mb->timer); mb->timer = NULL;
    uasync_cancel_timeout(inst->ua, mb->media->timer); mb->media->timer = NULL;
}
static void startup_tick(struct UTUN_INSTANCE* inst) {
    struct dm_mb_media* media = inst->dm_mailbox->media; assert(!media->timer);
    relay_before = __atomic_load_n(&commits, __ATOMIC_SEQ_CST); dm_mailbox_media_tick(media);
    assert(media->timer); uasync_cancel_timeout(inst->ua, media->timer); media->timer = NULL;
}
static void future_retry(struct UTUN_INSTANCE* inst) {
    int done = 0; db_exec(inst->db_worker, "UPDATE dm_media_jobs SET retry_at=999", DB_NO_ARGS, fixture_sql_done, &done);
    idle(inst); assert(done == 1);
}
struct fanout_roles { struct UTUN_INSTANCE* inst; struct group_policy_cache* cache; int done; };
static int fanout_roles_work(sqlite3* db, void* arg) {
    assert(!sqlite3_exec(db, "UPDATE peers_" TEST_CHANNEL " SET admitted=1,identity_verified=1,adm_tags='{\"supernode\":\"yes\"}';"
        "INSERT INTO peers_" TEST_CHANNEL "(node_id,x25519_pubkey,ed25519_pubkey,join_sig,join_ts,admitted,identity_verified,adm_tags)"
        " SELECT node_id,x25519_pubkey,ed25519_pubkey,zeroblob(64),1,1,1,'{\"supernode\":\"yes\"}' FROM nodes"
        " WHERE 1 ON CONFLICT(ed25519_pubkey,record_state) DO NOTHING", NULL, NULL, NULL));
    return group_policy_snapshot_read(db, TEST_CHANNEL, &((struct fanout_roles*)arg)->cache);
}
static void fanout_roles_done(void* arg, const struct db_async_result* result) {
    struct fanout_roles* roles = arg; assert(!result->status && roles->cache);
    group_policy_snapshot_publish(roles->inst, roles->cache); roles->cache = NULL; roles->done++;
}
static void fanout_roles(struct UTUN_INSTANCE* inst) {
    struct fanout_roles roles = {.inst=inst}; db_async_run(inst->db_worker,
        &(struct db_async_task){.name="fanout-roles", .mode=DB_ASYNC_WRITE, .work=fanout_roles_work,
            .data=&roles, .done=fanout_roles_done}); idle(inst); assert(roles.done == 1);
}
/* Подтверждённые роли читаются из RAM; автор/B исключены даже при наличии роли S. */
static void check_media_fanout(struct UTUN_INSTANCE* inst) {
    cycle_reset(inst, C_TERMINAL); fanout_roles(inst);
    struct TOPO_GROUP* group = topo_groups_find(inst->topo_groups, 7); assert(group);
    fanout_phase = 1; fanout_count = 0; relay_before = __atomic_load_n(&commits, __ATOMIC_SEQ_CST) - 1;
    uint8_t input[DM_MEDIA_RECEIPT_SIZE]; memcpy(input, media_receipt, sizeof(input)); fanout_input = input;
    dm_mailbox_media_receipt(inst, input);
    assert(fanout_count == 3 && fanout_seen[0] == 99 && fanout_seen[1] == 100 && fanout_seen[2] == 101 && !input[0]);
    fanout_count = 0; group->stopping = 1; dm_mailbox_media_receipt(inst, media_receipt); assert(!fanout_count); group->stopping = 0;
    inst->destroying = 1; dm_mailbox_media_receipt(inst, media_receipt); assert(!fanout_count); inst->destroying = 0;
    inst->chat_content_started = 0; dm_mailbox_media_receipt(inst, media_receipt); assert(!fanout_count); inst->chat_content_started = 1;
    fanout_revoke = 1; dm_mailbox_media_receipt(inst, media_receipt); assert(fanout_count == 1 && !fanout_revoke);
    fanout_roles(inst); fanout_count = 0; stop_on_forward = 1; dm_mailbox_media_receipt(inst, media_receipt);
    assert(fanout_count == 1 && !inst->dm_mailbox && !stop_on_forward);
    dm_mailbox_media_receipt(inst, media_receipt); assert(fanout_count == 1);
    group_policy_forget(inst, NULL); fanout_phase = 0;
    puts("PASS media receipt fanout: committed RAM roles without SQL, own packet copy, self included, "
        "author/recipient excluded, revoke and stop during send");
}
static void check_media_startup(struct UASYNC* ua) {
    struct UTUN_INSTANCE inst = {.ua=ua, .node_id=99, .my_ed25519_pubkey={99}, .chat_content_started=1}; media_phase = cycle_phase = 1; delivery_now = 100;
    struct TOPO_GROUPS groups; struct TOPO_GROUP group; cycle_routes(&inst, &groups, &group);
    char path[] = "/tmp/utun-media-startup-XXXXXX"; int fd = mkstemp(path); assert(fd >= 0); close(fd); int opened = 0;
    inst.db_worker = db_async_create(ua, path, 2, ready, &opened); assert(inst.db_worker); idle(&inst); assert(opened);
    int seeded = 0; db_async_run(inst.db_worker, &(struct db_async_task){.name="startup-seed", .mode=DB_ASYNC_DIRECT,
        .work=seed_cycle, .data=&seeded, .done=ready}); idle(&inst); assert(seeded);
    startup_owner(&inst); cycle_reset(&inst, C_EMPTY); startup_tick(&inst); idle(&inst);
    assert(!inst.dm_mailbox->media->reset_retry && !cycle_packets); dm_mailbox_destroy(&inst);
    startup_owner(&inst); cycle_reset(&inst, C_ASSIGNED); future_retry(&inst);
    delete_prefix = "UPDATE dm_media_jobs SET retry_at=0"; fault(6, SQLITE_BUSY); startup_tick(&inst); idle(&inst);
    assert(!__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST) && inst.dm_mailbox->media->reset_retry && !cycle_packets);
    cycle_expect(&inst, (int[]){100,0,0,0,-1,999,0,1,0,1});
    fault(5, SQLITE_BUSY); startup_tick(&inst); idle(&inst);
    assert(!__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST) && inst.dm_mailbox->media->reset_retry && !cycle_packets);
    cycle_expect(&inst, (int[]){100,0,0,0,-1,999,0,1,0,1});
    allocation_min = sizeof(struct dm_cycle_job); fail_job = 1; startup_tick(&inst); idle(&inst);
    assert(!fail_job && inst.dm_mailbox->media->reset_retry && !cycle_packets);
    startup_tick(&inst); idle(&inst); assert(!inst.dm_mailbox->media->reset_retry && cycle_offers == 1 && cycle_stores == 1);
    cycle_expect(&inst, (int[]){100,0,0,0,-1,105,0,1,0,1});
    cycle_reset(&inst, C_ASSIGNED); future_retry(&inst); startup_tick(&inst); idle(&inst); assert(!cycle_packets);
    cycle_expect(&inst, (int[]){100,0,0,0,-1,999,0,1,0,1}); dm_mailbox_destroy(&inst); startup_owner(&inst);
    pause_prefix = "SELECT j.id,j.author"; pause_job(); startup_tick(&inst); paused(&inst);
    assert(inst.dm_mailbox->media->reset_retry && !cycle_packets); int alive = 0;
    assert(uasync_set_timeout(ua, 10, &alive, tick, "startup loop alive")); uint64_t end = get_time_us() + 5000000;
    while (!alive && get_time_us() < end) uasync_poll(ua, 1);
    assert(alive); dm_mailbox_destroy(&inst); startup_owner(&inst); unpause(); idle(&inst); assert(!cycle_packets);
    cycle_expect(&inst, (int[]){100,0,0,0,-1,999,0,1,0,1});
    struct state blocked[2] = {{0}, {0}}; pause_prefix = "SELECT recipient,sender,seq,msg"; pause_job();
    db_async_run(inst.db_worker, &(struct db_async_task){.name="startup-busy", .mode=DB_ASYNC_READ,
        .work=delivery_read_blocker, .data=&blocked[0], .done=state_done}); paused(&inst);
    db_async_run(inst.db_worker, &(struct db_async_task){.name="startup-queued", .mode=DB_ASYNC_READ,
        .work=delivery_read_blocker, .data=&blocked[1], .done=state_done});
    for (unsigned i = 0; i < 8; i++) startup_tick(&inst);
    assert(db_async_pending(inst.db_worker) == 2 && !inst.dm_mailbox->media->cycle && inst.dm_mailbox->media->reset_retry);
    unpause(); idle(&inst); startup_tick(&inst); idle(&inst); assert(cycle_stores == 1 && !inst.dm_mailbox->media->reset_retry);
    dm_mailbox_destroy(&inst); startup_owner(&inst); cycle_reset(&inst, C_ASSIGNED); future_retry(&inst);
    fault(6, SQLITE_IOERR); startup_tick(&inst); idle(&inst); startup_tick(&inst); idle(&inst);
    assert(!__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST) && inst.dm_mailbox->media->reset_retry && !cycle_packets);
    assert(!db_async_destroy(inst.db_worker)); opened = 0;
    inst.db_worker = db_async_create(ua, path, 2, ready, &opened); assert(inst.db_worker); idle(&inst); assert(opened);
    cycle_expect(&inst, (int[]){100,0,0,0,-1,999,0,1,0,1}); startup_tick(&inst); idle(&inst);
    assert(cycle_stores == 1 && !inst.dm_mailbox->media->reset_retry); check_media_fanout(&inst);
    assert(!db_async_destroy(inst.db_worker)); media_phase = cycle_phase = 0; assert(!remove(path));
    assert(queue_data_get(groups.group_list) == &group.ll); queue_free(groups.group_list);
    puts("PASS mailbox/media startup: no loop SQL/legacy, first tick reset and bounded continuation, "
        "reset/commit rollback, FULL/OOM, timer, stop/restart and fatal reopen");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); loop_thread = pthread_self(); size_t before = u_get_allocated_count();
    assert(dm_media_verify_receipt(NULL, NULL, NULL, 0) == SQLITE_MISUSE && dm_media_accept_receipt(NULL, NULL) == -1);
    dm_mailbox_media_receipt(NULL, NULL); /* Входные проверки не разыменовывают отсутствующий runtime/SQL owner. */
    make_packets(); struct UTUN_INSTANCE inst = {.node_id=identity.node, .chat_content_started=1}; inst.ua = uasync_create(); assert(inst.ua); new_owner(&inst);
    chat_event_set_handler(&inst, event); char path[] = "/tmp/utun-dm-receipt-XXXXXX"; int fd = mkstemp(path); assert(fd >= 0); close(fd);
    forbid_sql = 1; int opened = 0; inst.db_worker = db_async_create(inst.ua, path, 2, ready, &opened); assert(inst.db_worker);
    idle(&inst); assert(opened); int seeded = 0;
    db_async_run(inst.db_worker, &(struct db_async_task){.name="receipt-fixture", .mode=DB_ASYNC_DIRECT, .work=seed, .data=&seeded, .done=ready});
    idle(&inst); assert(seeded && !inst.topo_sqlite_db && !inst.dm->db);
    struct reply copied = {0}; uint8_t input[DM_RECEIPT_SIZE]; memcpy(input, packets[0].receipt, sizeof(input));
    dm_receipt_submit(&inst, input, done, &copied); memset(input, 0, sizeof(input)); idle(&inst);
    assert(copied.calls == 1 && !copied.status && events == 1); check_state(&inst, 1, 0);
    submit_once(&inst, 0, 0); assert(events == 1);
    memcpy(input, packets[1].receipt, sizeof(input)); input[DM_ACK_HASH_OFF] ^= 1;
    assert(!sc_ed25519_sign(peer_ed_private, input, DM_ACK_SIG_OFF, input + DM_ACK_SIG_OFF));
    struct reply invalid = {0}; dm_receipt_submit(&inst, input, done, &invalid); idle(&inst);
    assert(invalid.calls == 1 && invalid.status == -1); check_state(&inst, 2, 1);
    for (int point = 1; point <= 5; point++) {
        int prior = __atomic_load_n(&rollbacks, __ATOMIC_SEQ_CST); fault(point, SQLITE_BUSY); submit_once(&inst, 1, -1);
        assert(!__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST) && __atomic_load_n(&rollbacks, __ATOMIC_SEQ_CST) > prior);
        check_state(&inst, 2, 1); assert(events == 1);
    }
    submit_once(&inst, 1, 0); assert(events == 2); check_state(&inst, 2, 0);
    fail_job = 1; submit_once(&inst, 2, -1); assert(!fail_job); check_state(&inst, 3, 1);
    struct reply active = {.before=__atomic_load_n(&commits, __ATOMIC_SEQ_CST)}, queued = active, full = {0}; pause_job();
    dm_receipt_submit(&inst, packets[2].receipt, done, &active); paused(&inst);
    dm_receipt_submit(&inst, packets[3].receipt, done, &queued);
    for (unsigned i = 0; i < 96; i++) dm_receipt_submit(&inst, packets[4].receipt, done, &full);
    assert(full.calls == 96 && full.status == -1); int alive = 0;
    assert(uasync_set_timeout(inst.ua, 10, &alive, tick, "DM WRITE loop alive")); uint64_t end = get_time_us() + 5000000;
    while (!alive && get_time_us() < end) uasync_poll(inst.ua, 1);
    assert(alive && !active.calls && !queued.calls); unpause(); idle(&inst);
    assert(active.calls == 1 && !active.status && queued.calls == 1 && !queued.status && events == 4);
    check_state(&inst, 3, 0); check_state(&inst, 4, 0);
    active = (struct reply){0}; queued = (struct reply){0}; pause_job(); dm_receipt_submit(&inst, packets[4].receipt, done, &active); paused(&inst);
    dm_receipt_submit(&inst, packets[5].receipt, done, &queued); dm_core_destroy(&inst); new_owner(&inst); unpause(); idle(&inst);
    assert(active.calls == 1 && active.status == -1 && queued.calls == 1 && queued.status == -1 && events == 4);
    check_state(&inst, 5, 0); check_state(&inst, 6, 1);
    stop_on_event = 1; struct reply stopping = {0}; dm_receipt_submit(&inst, packets[5].receipt, done, &stopping); idle(&inst);
    assert(stopping.calls == 1 && stopping.status == -1 && !inst.dm && !stop_on_event && events == 5); new_owner(&inst);
    check_state(&inst, 6, 0); fault(1, SQLITE_IOERR); submit_once(&inst, 6, -1); submit_once(&inst, 6, -1);
    assert(!db_async_destroy(inst.db_worker)); inst.db_worker = NULL; opened = 0;
    inst.db_worker = db_async_create(inst.ua, path, 2, ready, &opened); assert(inst.db_worker); idle(&inst); assert(opened);
    check_state(&inst, 7, 1); submit_once(&inst, 6, 0); check_state(&inst, 7, 0); assert(events == 6);
    dm_core_destroy(&inst); assert(!db_async_destroy(inst.db_worker)); inst.db_worker = NULL;
    check_mailbox(inst.ua); check_delivery(inst.ua); check_put(inst.ua); check_stored(inst.ua); check_media_controls(inst.ua);
    check_media_cycle(inst.ua);
    check_media_startup(inst.ua);
    uasync_destroy(inst.ua, 0); forbid_sql = 0;
    OPENSSL_cleanse(&identity, sizeof(identity)); assert(!remove(path) && u_get_allocated_count() == before);
    puts("PASS DM receipt WRITE: no loop SQL/legacy, real signatures, commit/rollback, timer, FULL/OOM, stop/restart, event stop and fatal reopen");
    return 0;
}
