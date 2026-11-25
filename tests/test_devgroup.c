#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <openssl/evp.h>
#include "utun_instance.h"
#include "routing_layer/topo_group.h"
#include "routing_layer/topo_node_sqlite.h"
#include "chat/chat_devgroup.h"
#include "../lib/u_async.h"
#include "../lib/mem.h"
#include "../lib/db_async.h"

/* Проверка отрицательных SQLite-кодов и целостного выхода SQL-only helpers. */
static int deny_profile(void* unused, int action, const char* table, const char* column, const char* db, const char* caller) {
    (void)unused; (void)db; (void)caller;
    return action == SQLITE_READ && table && column && !strcmp(table, "nodes") && !strcmp(column, "devgroup_json") ?
        SQLITE_DENY : SQLITE_OK;
}
static void read_failures(sqlite3* db, const uint8_t device_ed[32], const uint8_t group[32]) {
    struct devgroup_record record; struct devgroup_profile profile; uint8_t id[32]; char name[64];
    assert(sqlite3_set_authorizer(db, deny_profile, NULL) == SQLITE_OK);
    memset(&record, 0x5a, sizeof(record)); memset(&profile, 0x5a, sizeof(profile));
    assert(devgroup_latest(db, group, &record, &profile) == -SQLITE_AUTH);
    assert(record.ed[0] == 0x5a && profile.name[0] == 0x5a);
    memset(id, 0x5a, sizeof(id)); memset(name, 0x5a, sizeof(name));
    assert(devgroup_member(db, device_ed, id, name) == -SQLITE_AUTH && id[0] == 0x5a && name[0] == 0x5a);
    assert(sqlite3_set_authorizer(db, NULL, NULL) == SQLITE_OK);
    const char* invalid[] = {"UPDATE nodes SET devgroup_x=printf('%032d',0)",
        "UPDATE nodes SET devgroup_member_sig=zeroblob(63)", "UPDATE nodes SET devgroup_profile_sig=NULL",
        "UPDATE nodes SET x25519_pubkey=zeroblob(32)", "UPDATE nodes SET devgroup_json=x'7b7d'"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK && sqlite3_exec(db, invalid[i], NULL, NULL, NULL) == SQLITE_OK);
        assert(devgroup_latest(db, group, &record, &profile) == -SQLITE_CORRUPT);
        assert(record.ed[0] == 0x5a && profile.name[0] == 0x5a);
        assert(devgroup_member(db, device_ed, id, name) == -SQLITE_CORRUPT && id[0] == 0x5a && name[0] == 0x5a);
        assert(sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    }
    assert(devgroup_latest(db, group, &record, &profile) == 1 && devgroup_member(db, device_ed, id, name) == 1);
    puts("PASS devgroup SQL reads: exact AUTH/CORRUPT errors, unchanged outputs and recovery");
}

static int deny_projection(void* unused, int action, const char* table, const char* column, const char* db, const char* caller) {
    (void)unused; (void)column; (void)db; (void)caller;
    return action == SQLITE_UPDATE && table && !strcmp(table, "nodes") ? SQLITE_DENY : SQLITE_OK;
}
static void device_ready(void* arg, const struct db_async_result* result) {
    assert(result->status == DB_ASYNC_OK); *(int*)arg = 1;
}
static void cached_identity(struct UTUN_INSTANCE* inst, const struct devgroup_record* record, const uint8_t ep[32], const uint8_t xp[32]) {
    struct devgroup_local_identity prepared = {.record = *record, .ready = 1, .present = 1};
    memcpy(prepared.ed_private, ep, 32); memcpy(prepared.x_private, xp, 32);
    assert(!devgroup_local_activate(inst, &prepared));
    prepared.ed_private[0] ^= 1;
    assert(devgroup_local_activate(inst, &prepared) < 0 && !memcmp(inst->local_devgroup.ed_private, ep, 32));
    prepared.ed_private[0] ^= 1;
    char path[512]; snprintf(path, sizeof(path), "%s/devgroup-cache-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    int fd = mkstemp(path), ready = 0; assert(fd >= 0); close(fd);
    inst->db_worker = db_async_create(inst->ua, path, 4, device_ready, &ready); assert(inst->db_worker);
    for (int i = 0; !ready && i < 5000; i++) uasync_poll(inst->ua, 1);
    assert(ready);
    sqlite3* legacy = inst->topo_sqlite_db; inst->topo_sqlite_db = NULL;
    struct devgroup_record got; uint8_t ed[32], x[32];
    assert(devgroup_runtime_member(inst, inst->node_id, &got) == 1 && !memcmp(&got, record, sizeof(got)));
    assert(devgroup_load_local(inst, &got, ed, x) == 1 && !memcmp(&got, record, sizeof(got)));
    assert(!memcmp(ed, ep, 32) && !memcmp(x, xp, 32));
    struct devgroup_local_identity empty = {.ready = 1}; assert(!devgroup_local_activate(inst, &empty));
    assert(!devgroup_runtime_member(inst, inst->node_id, &got) && !got.json[0]);
    assert(!devgroup_load_local(inst, &got, ed, x) && !got.json[0]);
    inst->local_devgroup.ready = 0; memset(&got, 0x5a, sizeof(got));
    assert(devgroup_load_local(inst, &got, ed, x) < 0 && got.ed[0] == 0x5a);
    db_async_destroy(inst->db_worker); inst->db_worker = NULL; inst->topo_sqlite_db = legacy; unlink(path);
    assert(!sqlite3_set_authorizer(legacy, deny_projection, NULL));
    struct TOPO_NODE node = {.node_id = inst->node_id, .devgroup = *record};
    memcpy(node.public_key, inst->my_keys.public_key, 32); memcpy(node.ed25519_public_key, inst->my_ed25519_pubkey, 32);
    assert(devgroup_project(legacy, &node) == -SQLITE_AUTH);
    assert(!sqlite3_set_authorizer(legacy, NULL, NULL) && !devgroup_project(legacy, &node));
    puts("PASS devgroup committed identity: NULL legacy RW cache, invalid replacement, absent/readiness, exact projection AUTH");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_DEBUG);
    struct UTUN_INSTANCE inst = {0}; struct TOPO_GROUPS groups = {.instance = &inst}; inst.topo_groups = &groups;
    inst.ua = uasync_create(); assert(inst.ua);
    groups.node_registry = queue_new(inst.ua, 16, 0, 8, "devgroup_nodes");
    groups.group_list = queue_new(inst.ua, 0, 0, 0, "devgroup_groups");
    assert(groups.node_registry && groups.group_list);
    assert(!sc_identity_generate(inst.my_keys.public_key, inst.my_keys.private_key, inst.my_ed25519_pubkey, inst.my_ed25519_privkey));
    inst.node_id = sc_derive_node_id_from_pubkey(inst.my_keys.public_key); strcpy(inst.name, "device");
    assert(sqlite3_open(":memory:", &inst.topo_sqlite_db) == SQLITE_OK);
    assert(!topo_node_sqlite_init(inst.topo_sqlite_db)); assert(!topo_node_init_self(&inst));
    assert(!chat_devgroup_rename(&inst, "Мои устройства"));
    struct devgroup_record r, loaded; uint8_t ep[32], xp[32]; struct devgroup_profile p;
    assert(devgroup_load_local(&inst, &r, ep, xp) == 1);
    assert(memcmp(ep, xp, 32) && !devgroup_profile_read(&r, &p) && p.version == 1);
    cached_identity(&inst, &r, ep, xp);
    struct TOPO_NODE* node = topo_node_registry_find(&groups, inst.node_id);
    uint64_t before = node->timestamp;
    assert(!chat_devgroup_rename(&inst, "Мои устройства") && node->timestamp == before);
    assert(!devgroup_verify(&r, inst.my_keys.public_key, inst.my_ed25519_pubkey));
    assert(devgroup_verify(&r, inst.my_keys.public_key, (uint8_t[32]){7}) < 0);
    r.json[10] ^= 1; assert(devgroup_profile_read(&r, &p) < 0); r.json[10] ^= 1;
    struct devgroup_record conflict = r; struct devgroup_profile original, changed;
    assert(!devgroup_profile_read(&r, &original));
    assert(!devgroup_sign(&conflict, ep, "Concurrent name", original.version, original.timestamp, inst.my_keys.public_key, inst.my_ed25519_pubkey));
    assert(!devgroup_profile_read(&conflict, &changed));
    assert((devgroup_profile_compare(&original, &changed) > 0) == (memcmp(original.hash, changed.hash, 32) > 0));
    changed.version++; assert(devgroup_profile_compare(&changed, &original) > 0);
    changed.version--; changed.timestamp++; assert(devgroup_profile_compare(&changed, &original) > 0);
    assert(devgroup_decode(&conflict, (const uint8_t*)"", 192) < 0);
    uint8_t wire[TOPO_NODE_WIRE_MAX_SIZE]; struct TOPO_GROUP_NODE nq = {0};
    int bytes = topo_node_serialize(node, &nq, 0, wire, sizeof(wire), 0); assert(bytes > 0);
    struct TOPO_GROUP group = {.instance = &inst}; struct TOPO_NODE* parsed = NULL;
    assert(!topo_node_deserialize(&group, wire, bytes, &parsed, NULL, NULL, NULL, NULL));
    assert(!topo_node_verify(parsed) && !memcmp(&parsed->devgroup, &r, sizeof(r))); topo_node_destroy(&groups, parsed);
    assert(devgroup_latest(inst.topo_sqlite_db, r.ed, &loaded, &p) == 1 && p.version == 1);
    read_failures(inst.topo_sqlite_db, inst.my_ed25519_pubkey, r.ed);
    assert(sqlite3_exec(inst.topo_sqlite_db, "CREATE TRIGGER deny_devgroup BEFORE UPDATE ON nodes BEGIN SELECT RAISE(ABORT,'test'); END",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(chat_devgroup_rename(&inst, "Rejected") < 0 && node->timestamp == before);
    assert(devgroup_load_local(&inst, &loaded, ep, xp) == 1 && !strcmp(loaded.json, r.json));
    assert(sqlite3_exec(inst.topo_sqlite_db, "DROP TRIGGER deny_devgroup", NULL, NULL, NULL) == SQLITE_OK);
    assert(!chat_devgroup_rename(&inst, "New name"));
    assert(devgroup_load_local(&inst, &loaded, ep, xp) == 1 && !devgroup_profile_read(&loaded, &p) && p.version == 2);
    /* Новый runtime восстанавливает обе пары и принадлежность до первой публикации. */
    topo_node_registry_unref(&groups, inst.node_id); assert(!topo_node_init_self(&inst));
    node = topo_node_registry_find(&groups, inst.node_id); assert(!strcmp(node->devgroup.json, loaded.json));
    uint8_t receipt[16] = {1};
    assert(!chat_devgroup_import(&inst, &loaded, ep, xp, receipt, (uint8_t[32]){42}));
    assert(chat_devgroup_receipt(&inst, receipt, (uint8_t[32]){42}) == 1);
    assert(chat_devgroup_receipt(&inst, receipt, (uint8_t[32]){42, [31]=1}) < 0);
    assert(!chat_devgroup_leave(&inst) && !node->devgroup.json[0]);
    assert(!chat_devgroup_import(&inst, &loaded, ep, xp, receipt, (uint8_t[32]){42}) && !node->devgroup.json[0]);
    assert(devgroup_latest(inst.topo_sqlite_db, r.ed, &loaded, &p) == 0);
    assert(!chat_devgroup_leave(&inst));
    topo_node_registry_unref(&groups, inst.node_id); assert(!topo_node_init_self(&inst));
    node = topo_node_registry_find(&groups, inst.node_id); assert(!node->devgroup.json[0]);
    topo_node_registry_unref(&groups, inst.node_id); queue_free(groups.node_registry); queue_free(groups.group_list);
    assert(sqlite3_close(inst.topo_sqlite_db) == SQLITE_OK); uasync_destroy(inst.ua, 0);
    puts("PASS devgroup: signatures, wire, transactional projection, restart, rename, leave and late import replay"); return 0;
}
