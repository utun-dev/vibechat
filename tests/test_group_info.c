/* Настоящие подписи/SQLite/Merkle: JSON-опции, rename, rollback и сходимость независимых администраторов. */
#include "../src/chat/member_sync.c"
#include "test_member_staging.h"
#include "test_utils.h"
#include "test_membership.h"
#include <assert.h>

static unsigned updates;
static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int size) {
    (void)data; (void)size;
    if (type == CHAT_EVT_CHANNEL_UPDATED) { assert(sqlite3_get_autocommit(inst->topo_sqlite_db)); updates++; }
}
static void hex(const uint8_t* key, char out[65]) {
    for (unsigned i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", key[i]);
}
static void execute(sqlite3* db, const char* sql) {
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "test group info SQL: %s rc=%d %s", sql, rc, sqlite3_errmsg(db));
    assert(rc == SQLITE_OK);
}
static int veto_commit(void* arg) { (*(unsigned*)arg)++; return 1; }
static void command(struct UTUN_INSTANCE* inst, const char* channel, const char* name, const char* json,
    const char* expected, int status) {
    char* result = NULL;
    assert(group_info_command(inst, channel, name, json, -1, expected, &result) == status && result);
    assert(!strchr(result, '\n') && !strchr(result, '\r'));
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "test group info command result: %s", result);
    assert(strstr(result, status ? "\"ok\":false" : "\"ok\":true")); u_free(result);
}
static void snapshot(struct UTUN_INSTANCE* inst, const char* channel, struct group_info_record* info, uint8_t hash[32]) {
    assert(!group_info_get(inst->topo_sqlite_db, channel, info));
    assert(!merkle_sync_read_hash(inst, MT_DATA_MEMBERS, channel, 0, 0, hash));
}
static void equal(struct UTUN_INSTANCE* a, struct UTUN_INSTANCE* b, const char* channel) {
    struct group_info_record x, y; uint8_t hx[32], hy[32]; snapshot(a, channel, &x, hx); snapshot(b, channel, &y, hy);
    assert(!strcmp(x.json, y.json) && !memcmp(x.signature, y.signature, 64) && !memcmp(hx, hy, 32));
}
/* Передаём страницы существующего MEMBERS-протокола, включая лист0 без участников. */
static void copy(struct UTUN_INSTANCE* src, struct UTUN_INSTANCE* dst, const char* channel) {
    void* pull = NULL; assert(!member_begin_pull(dst, channel, src->my_ed25519_pubkey, 1, &pull));
    char tree[70]; snprintf(tree, sizeof(tree), "0:%s", channel);
    sqlite3_stmt* leaves = NULL;
    assert(!sqlite3_prepare_v2(src->topo_sqlite_db, "SELECT prefix64 FROM merkle_tree_hash WHERE namespace=? AND level=5", -1, &leaves, NULL));
    assert(!sqlite3_bind_text(leaves, 1, tree, -1, SQLITE_STATIC));
    int rc;
    while ((rc = sqlite3_step(leaves)) == SQLITE_ROW) {
        uint64_t prefix = (uint64_t)sqlite3_column_int64(leaves, 0);
        uint8_t data[MT_PAGE_SIZE]; size_t size = sizeof(data); merkle_key_t after = {{0}}, next; int more;
        assert(!_member_get_page(src, channel, prefix, 0, &after, data, &size, &next, &more) && !more);
        assert(!member_stage_page(dst, pull, prefix, 0, &after, &next, more, data, size));
    }
    assert(rc == SQLITE_DONE && !sqlite3_finalize(leaves));
    assert(!member_commit_pull(dst, pull)); member_publish_pull(dst, pull); member_abort_pull(dst, pull);
}
static void parser(sqlite3* db) {
    struct group_info_profile p, old; memset(&old, 0xa5, sizeof(old));
    assert(!group_info_parse(db, "{\"name\":\"x\",\"ver\":1}", &p) && !p.group_type[0]);
    char initial[GROUP_INFO_MAX + 1];
    assert(!group_info_initial_typed("Board", "kanban", initial));
    assert(!group_info_parse(db, initial, &p) && !strcmp(p.group_type, "kanban"));
    const char* bad_types[] = {"null", "0", "2", "true", "[]", "{}", "\"\"", "\"unknown\"", "\"CHAT\""};
    for (unsigned i = 0; i < sizeof(bad_types) / sizeof(bad_types[0]); i++) {
        snprintf(initial, sizeof(initial), "{\"name\":\"x\",\"ver\":1,\"group_type\":%s}", bad_types[i]);
        p = old; assert(group_info_parse(db, initial, &p) < 0 && !memcmp(&p, &old, sizeof(p)));
    }
    assert(!group_info_parse(db, "{\"name\":\"clipboard\",\"ver\":1,\"clipboard_members\":true}", &p) && p.clipboard_members);
    assert(group_info_parse(db, "{\"name\":\"clipboard\",\"ver\":1,\"clipboard_members\":1}", &p) < 0);
    const char* invalid[] = {"{}", "[]", "{\"name\":\"x\",\"ver\":0}", "{\"name\":\"x\",\"ver\":1.0}",
        "{\"name\":\"x\",\"ver\":\"1\"}", "{\"name\":\"x\",\"ver\":9223372036854775808}",
        "{\"name\":\"x\",\"ver\":1,\"ver\":2}", "{\"name\":\"x\",\"ver\":1,\"o\":{\"a\":1,\"a\":2}}",
        "{\"name\":\"x\\u0000hidden\",\"ver\":1}",
        "{\"name\":\"x\",\"ver\":1,\"a\":\"\\u0000hidden\"}", "{\"name\":\"x\",\"ver\":1}tail",
        "{\"name\":\"\xc0\x80\",\"ver\":1}"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        p = old; assert(group_info_parse(db, invalid[i], &p) < 0 && !memcmp(&p, &old, sizeof(p)));
    }
    assert(!group_info_parse(db, "{\"name\":\"Группа 😀\",\"ver\":9223372036854775807,\"options\":[true,null,3,{\"x\":\"ok\"}]}", &p));
    assert(p.version == INT64_MAX && !strcmp(p.name, "Группа 😀"));
    assert(!group_info_parse(db, "{\"name\":\"quote\\\"\\n\\\\\",\"ver\":1}", &p) && !strcmp(p.name, "quote\"\n\\"));
}
/* Переход unknown → kanban атомарен: rollback не фиксирует тип. */
static int deny_group_delete(void* unused, int action, const char* table, const char* col, const char* db, const char* caller) {
    (void)unused; (void)col; (void)db; (void)caller;
    return action == SQLITE_DELETE && table && !strcmp(table, "group_policy") ? SQLITE_DENY : SQLITE_OK;
}
static void fix_unknown(struct UTUN_INSTANCE* inst, const uint8_t owner[32], const uint8_t x[32], const uint8_t ed[32],
    const uint8_t private[32]) {
    sqlite3* db = inst->topo_sqlite_db;
    char channel[65]; sc_public_key_hex(ed, channel);
    uint8_t body[GROUP_INFO_MESSAGE_MAX];
    struct group_info_record record = { .json = "{\"name\":\"Pending\",\"ver\":1}" }, saved;
    int size = group_info_message(channel, record.json, owner, x, ed, body, sizeof(body));
    assert(size > 0 && !sc_ed25519_sign(private, body, (size_t)size, record.signature));
    assert(!topo_node_sqlite_channel_put_description(db, channel, record.json, owner, x, NULL, ed, NULL, record.signature));
    strcpy(record.json, "{\"name\":\"Board\",\"ver\":2,\"group_type\":\"kanban\"}");
    size = group_info_message(channel, record.json, owner, x, ed, body, sizeof(body));
    assert(size > 0 && !sc_ed25519_sign(private, body, (size_t)size, record.signature));
    execute(db, "BEGIN"); assert(group_info_apply(db, channel, &record) == 1); execute(db, "ROLLBACK");
    struct group_info_profile profile;
    assert(!group_info_get(db, channel, &saved) && !group_info_parse(db, saved.json, &profile) && !profile.group_type[0]);
    execute(db, "BEGIN"); assert(group_info_apply(db, channel, &record) == 1); execute(db, "COMMIT");
    assert(!group_info_get(db, channel, &saved) && !group_info_parse(db, saved.json, &profile));
    assert(!strcmp(profile.group_type, "kanban") && !group_info_verify(db, channel, &saved));
    assert(group_info_check_type(db, channel, "{\"name\":\"Reset\",\"ver\":3}") < 0);
    assert(group_info_check_type(db, channel, "{\"name\":\"Switch\",\"ver\":3,\"group_type\":\"chat\"}") < 0);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT group_type FROM channels WHERE channel_id=?", -1, &st, NULL) == SQLITE_OK);
    assert(!sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC));
    assert(sqlite3_step(st) == SQLITE_ROW && !strcmp((const char*)sqlite3_column_text(st, 0), "kanban"));
    assert(sqlite3_finalize(st) == SQLITE_OK);
    assert(topo_node_sqlite_channel_delete_write(db, channel) == SQLITE_MISUSE);
    execute(db, "BEGIN IMMEDIATE"); assert(!sqlite3_set_authorizer(db, deny_group_delete, NULL));
    assert(topo_node_sqlite_channel_delete_write(db, channel) == SQLITE_AUTH && !sqlite3_get_autocommit(db));
    assert(!sqlite3_set_authorizer(db, NULL, NULL)); execute(db, "ROLLBACK");
    assert(!group_info_get(db, channel, &saved));
    execute(db, "BEGIN IMMEDIATE"); assert(!topo_node_sqlite_channel_delete_write(db, channel) && !sqlite3_get_autocommit(db));
    execute(db, "ROLLBACK"); assert(!group_info_get(db, channel, &saved));
    assert(!topo_node_sqlite_channel_delete(db, channel));
}

int main(void) {
    debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_CHAT, DEBUG_LEVEL_INFO);
    debug_enable_console(1); utun_instance_set_tun_init_enabled(0);
    struct UASYNC* ua = uasync_create(); assert(ua);
    char directory[256] = "/tmp/utun-group-info-XXXXXX"; assert(!test_mkdtemp(directory));
    struct UTUN_INSTANCE* nodes[2];
    for (unsigned i = 0; i < 2; i++) {
        uint8_t device_x[32], device_seed[32]; char pub[65], priv[65], config[2048]; assert(!sc_identity_generate_config(device_x, device_seed));
        hex(device_x, pub); hex(device_seed, priv);
        snprintf(config, sizeof(config), "[global]\nmy_private_key=%s\nmy_public_key=%s\ndb_path=%s/node%u\n"
            "[server:s1]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=1\n[chatserver]\n"
            "group_autoconnect=0\nstorage_autoload=0\nmember_files_enabled=0\n", priv, pub, directory, i);
        nodes[i] = utun_instance_create_from_str(ua, config); assert(nodes[i] && !utun_instance_init(nodes[i]));
        chat_event_set_handler(nodes[i], event);
    }
    uint64_t group = chat_core_create_channel_auto(nodes[0], "Original"); assert(group);
    char channel[65]; strcpy(channel, topo_groups_channel(nodes[0]->topo_groups, group));
    sqlite3* db = nodes[0]->topo_sqlite_db; parser(db);
    uint8_t x[32], ed[32], private[32], xprivate[32], body[GROUP_INFO_MESSAGE_MAX]; uint8_t owner[32];
    struct group_info_record initial; assert(!group_info_get(db, channel, &initial));
    assert(!topo_node_sqlite_channel_get(db, channel, NULL, 0, owner, x, ed, NULL));
    assert(!topo_node_sqlite_channel_get_privs(db, channel, xprivate, private));
    struct test_group_keys pending; assert(!test_group_keys_generate(&pending));
    fix_unknown(nodes[0], owner, pending.x.public_key, pending.ed, pending.ed_private);
    assert(!topo_node_sqlite_channel_put_description(nodes[1]->topo_sqlite_db, channel, initial.json, owner,
        x, NULL, ed, private, initial.signature));
    assert(!chat_core_ensure_channel_ready(nodes[1], channel));
    uint8_t join[64], tree[64];
    int n = member_sync_build_join_msg(x, ed, nodes[1]->my_ed25519_pubkey, nodes[1]->my_keys.public_key, body, sizeof(body));
    assert(n > 0 && !sc_ed25519_sign(nodes[1]->my_ed25519_privkey, body, (size_t)n, join));
    assert(!member_sync_sign_join(private, channel, NULL, nodes[1]->my_keys.public_key, nodes[1]->my_ed25519_pubkey,
        TEST_JOIN_ID(x, ed, nodes[1]->my_ed25519_pubkey, nodes[1]->my_keys.public_key), 123, tree));
    assert(member_sync_put(nodes[0], channel, nodes[1]->my_keys.public_key, nodes[1]->my_ed25519_pubkey,
        join, 123, NULL, 0, "", NULL, NULL, 0, 0, tree) >= 0);
    copy(nodes[0], nodes[1], channel); equal(nodes[0], nodes[1], channel);
    assert(topo_node_sqlite_member_allowed(nodes[1]->topo_sqlite_db, channel, nodes[1]->node_id));
    /* Тип фиксирован даже для подписанной правки администратора и старого JOIN. */
    command(nodes[0], channel, NULL, "{\"name\":\"Wrong type\",\"ver\":999,\"group_type\":\"kanban\"}", NULL, -1);
    command(nodes[0], channel, NULL, "{\"name\":\"Missing type\",\"ver\":999}", NULL, -1);
    struct group_info_profile typed;
    assert(!group_info_parse(db, initial.json, &typed) && !strcmp(typed.group_type, "chat"));
    struct group_info_record wrong = initial;
    strcpy(wrong.json, "{\"name\":\"Old conflicting type\",\"ver\":1,\"group_type\":\"kanban\"}");
    int signed_size = group_info_message(channel, wrong.json, owner, x, ed, body, sizeof(body));
    assert(signed_size > 0 && !sc_ed25519_sign(private, body, (size_t)signed_size, wrong.signature));
    assert(topo_node_sqlite_channel_put_description(db, channel, wrong.json, owner, x, NULL, ed, NULL, wrong.signature) < 0);
    execute(db, "BEGIN"); assert(group_info_apply(db, channel, &wrong) < 0); execute(db, "ROLLBACK");
    assert(!group_info_get(db, channel, &wrong) && !strcmp(wrong.json, initial.json));
    const char* formatted = "{\n \"name\":\"Original\",\n \"ver\":2,\n \"group_type\":\"chat\",\n \"options\":{\"enabled\":true,\"list\":[1,null,\"x\"]}\n}";
    command(nodes[0], channel, NULL, formatted, NULL, 0);
    struct group_info_record raw;
    assert(!group_info_get(db, channel, &raw) && !strcmp(raw.json, formatted));
    command(nodes[0], channel, "Новое имя 😀", NULL, NULL, 0);
    struct group_info_record before, after; uint8_t hash_before[32], hash_after[32]; snapshot(nodes[0], channel, &before, hash_before);
    assert(strstr(before.json, "\"ver\":3") && strstr(before.json, "\"options\":{\"enabled\":true,\"list\":[1,null,\"x\"]}"));
    unsigned saved = updates; command(nodes[0], channel, "Новое имя 😀", NULL, NULL, 0); assert(updates == saved);
    copy(nodes[0], nodes[1], channel); equal(nodes[0], nodes[1], channel);
    assert(!group_info_verify(db, channel, &before));
    execute(db, "CREATE TRIGGER reject_group_info BEFORE UPDATE OF description_json ON channels BEGIN SELECT RAISE(ABORT,'injected'); END");
    command(nodes[0], channel, "SQL failure", NULL, NULL, -1); snapshot(nodes[0], channel, &after, hash_after);
    assert(!strcmp(before.json, after.json) && !memcmp(hash_before, hash_after, 32) && updates == saved + 1);
    execute(db, "DROP TRIGGER reject_group_info");
    unsigned vetoes = 0; saved = updates;
    sqlite3_commit_hook(db, veto_commit, &vetoes);
    command(nodes[0], channel, "Commit failure", NULL, NULL, -1);
    sqlite3_commit_hook(db, NULL, NULL);
    snapshot(nodes[0], channel, &after, hash_after);
    assert(vetoes == 1 && updates == saved && !strcmp(before.json, after.json) && !memcmp(hash_before, hash_after, 32));
    execute(db, "BEGIN"); assert(group_info_apply(db, channel, &initial) == 0); execute(db, "ROLLBACK");
    struct group_info_record forged = before; forged.signature[0] ^= 1;
    execute(db, "BEGIN"); assert(group_info_apply(db, channel, &forged) < 0); execute(db, "ROLLBACK");
    command(nodes[0], channel, "Alpha", NULL, NULL, 0); command(nodes[1], channel, "Omega", NULL, NULL, 0);
    struct group_info_record forks[2], selected;
    for (unsigned i = 0; i < 2; i++) assert(!group_info_get(nodes[i]->topo_sqlite_db, channel, &forks[i]));
    const struct group_info_record* winner = memcmp(forks[0].signature, forks[1].signature, 64) < 0 ? &forks[0] : &forks[1];
    copy(nodes[0], nodes[1], channel); copy(nodes[1], nodes[0], channel); equal(nodes[0], nodes[1], channel);
    char corrupt[256];
    snprintf(corrupt, sizeof(corrupt), "UPDATE channels SET description_json=json_set(description_json,'$.ver',999,'$.group_type','kanban'),"
        "signature=zeroblob(64) WHERE channel_id='%s'", channel);
    execute(db, "BEGIN"); execute(db, corrupt);
    assert(member_sync_reindex(db, channel, 0) > 0);
    assert(group_info_apply(db, channel, &forged) < 0);
    execute(db, "COMMIT");
    snapshot(nodes[0], channel, &before, hash_before); snapshot(nodes[1], channel, &after, hash_after);
    assert(memcmp(hash_before, hash_after, 32));
    copy(nodes[1], nodes[0], channel); equal(nodes[0], nodes[1], channel);
    assert(!group_info_get(db, channel, &selected) && !strcmp(selected.json, winner->json));
    for (unsigned i = 0; i < 2; i++) {
        assert(!topo_node_sqlite_channel_put_description(db, channel, forks[i].json, owner, x, NULL, ed, NULL, forks[i].signature));
        assert(!group_info_get(db, channel, &selected) && !memcmp(selected.signature, winner->signature, 64));
    }
    command(nodes[0], channel, "conflict", NULL, "0000000000000000000000000000000000000000000000000000000000000000", -2);
    command(nodes[0], channel, "", NULL, NULL, -1);
    char too_long[129]; memset(too_long, 'a', 128); too_long[128] = 0;
    command(nodes[0], channel, too_long, NULL, NULL, -1);
    char boundary[GROUP_INFO_MAX + 2]; const char* prefix = "{\"name\":\"boundary\",\"ver\":5,\"group_type\":\"chat\",\"extra\":\"";
    size_t head = strlen(prefix); memcpy(boundary, prefix, head); memset(boundary + head, 'a', GROUP_INFO_MAX - head - 2);
    memcpy(boundary + GROUP_INFO_MAX - 2, "\"}", 3);
    command(nodes[0], channel, NULL, boundary, NULL, 0);
    snapshot(nodes[0], channel, &before, hash_before); saved = updates;
    command(nodes[0], channel, "Name exceeds remaining JSON capacity", NULL, NULL, -1);
    snapshot(nodes[0], channel, &after, hash_after);
    assert(updates == saved && !strcmp(before.json, after.json) && !memcmp(hash_before, hash_after, 32));
    boundary[GROUP_INFO_MAX - 2] = 'a'; memcpy(boundary + GROUP_INFO_MAX - 1, "\"}", 3);
    command(nodes[0], channel, NULL, boundary, NULL, -1);
    command(nodes[0], channel, "A", NULL, NULL, 0);
    copy(nodes[0], nodes[1], channel); equal(nodes[0], nodes[1], channel);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(nodes[1]->topo_sqlite_db, "UPDATE channels SET ed25519_privkey=NULL", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
    command(nodes[1], channel, "forbidden", NULL, NULL, -1);
    command(nodes[0], channel, NULL, "{\"name\":\"Final\",\"ver\":9223372036854775807,\"group_type\":\"chat\"}", NULL, 0);
    command(nodes[0], channel, "overflow", NULL, NULL, -1);
    copy(nodes[0], nodes[1], channel); equal(nodes[0], nodes[1], channel);
    /* Старый JOIN не должен откатить JSON даже после обновления приватных ключей. */
    assert(!topo_node_sqlite_channel_put_description(db, channel, initial.json, owner, x, NULL, ed, NULL, initial.signature));
    struct group_info_profile final; assert(!group_info_get(db, channel, &after) && !group_info_parse(db, after.json, &final));
    assert(final.version == INT64_MAX && !strcmp(final.name, "Final"));
    uint64_t settings = chat_core_create_channel_auto(nodes[0], "Clipboard settings"); assert(settings);
    const char* settings_id = topo_groups_channel(nodes[0]->topo_groups, settings);
    assert(!group_info_clipboard_allowed(db, settings_id));
    char* response = NULL;
    assert(!group_info_command(nodes[0], settings_id, NULL, NULL, 1, NULL, &response)); u_free(response);
    assert(group_info_clipboard_allowed(db, settings_id));
    assert(!group_info_get(db, settings_id, &raw) && strstr(raw.json, "\"ver\":2"));
    char replace_signature[144];
    snprintf(replace_signature, sizeof(replace_signature), "UPDATE channels SET signature=zeroblob(64) WHERE channel_id='%s'", settings_id);
    execute(db, "BEGIN"); execute(db, replace_signature);
    int allowed = 1;
    assert(group_info_access_read(db, settings_id, &allowed, NULL) == SQLITE_OK && !allowed);
    execute(db, "ROLLBACK");
    assert(!group_info_command(nodes[0], settings_id, "Renamed", NULL, -1, NULL, &response)); u_free(response);
    assert(group_info_clipboard_allowed(db, settings_id));
    assert(!group_info_command(nodes[0], settings_id, NULL, NULL, 0, NULL, &response)); u_free(response);
    assert(!group_info_clipboard_allowed(db, settings_id));
    for (unsigned i = 0; i < 2; i++) utun_instance_destroy(nodes[i]);
    uasync_destroy(ua, 1); OPENSSL_cleanse(private, sizeof(private)); OPENSSL_cleanse(xprivate, sizeof(xprivate));
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "PASS group JSON: strict parser, preserved options, atomic rollback, stale JOIN, conflicts and Merkle convergence");
    return 0;
}
