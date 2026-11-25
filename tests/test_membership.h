/* Проверяемый участник сетевого теста: JOIN эпохи 1, профиль версии 1 и корневая подпись группы. */
#ifndef TEST_MEMBERSHIP_H
#define TEST_MEMBERSHIP_H
#include "test_join_id.h"
#include <openssl/sha.h>
#include <openssl/rand.h>
#include "../lib/debug_config.h"
#include "chat/member_sync.h"
#include "chat/chat_channel.h"
#include "chat/group_info.h"
#include "chat/group_policy.h"
#include "utun_instance.h"
#include "routing_layer/topo_node_sqlite.h"
#include "transport_layer/secure_channel.h"

/* Одна группа сетевого теста имеет свои ключи. Все её экземпляры получают одну копию публичных ключей. */
struct test_group_keys { struct SC_MYKEYS x; uint8_t ed[32], ed_private[32]; };
static inline int test_group_keys_generate(struct test_group_keys* keys) {
    return chat_channel_generate_keys(keys->x.public_key, keys->x.private_key, keys->ed, keys->ed_private);
}

/* После прямой SQL-инъекции теста опубликовать готовую проекцию без revalidate callbacks.
 * Production меняет состав через project/commit/publish; здесь намеренно проверяется
 * доступ при старой READY-сессии, отложенной передаче или искусственно испорченной строке. */
static inline int test_member_publish_access(struct UTUN_INSTANCE* inst, const char* channel) {
    struct group_policy_cache* snapshot = NULL;
    int rc = group_policy_snapshot_read(inst->topo_sqlite_db, channel, &snapshot);
    if (rc != SQLITE_OK) return -1;
    group_policy_snapshot_publish(inst, snapshot); return 0;
}

/* Подписать исходный JSON группы тем же форматом, который используется в JOIN и MEMBERS. */
static inline int test_group_signature(const char* channel, const char* name, const uint8_t owner[32], const uint8_t x[32],
    const uint8_t ed[32], const uint8_t private[32], uint8_t signature[64]) {
    char json[GROUP_INFO_MAX + 1]; uint8_t body[GROUP_INFO_MESSAGE_MAX];
    if (group_info_initial(name, json)) return -1;
    int size = group_info_message(channel, json, owner, x, ed, body, sizeof(body));
    if (size < 0 || sc_ed25519_sign(private, body, (size_t)size, signature) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "test group description signing failed channel=%s", channel); return -1;
    }
    return 0;
}

/* Подписать профиль тем же production codec, что использует автор;0/-1. */
static int test_member_profile(const uint8_t private[32], const uint8_t join_id[32], uint64_t version,
    const char* info, uint8_t signature[64]) {
    uint8_t body[MS_USERINFO_MAX + 73];
    int size = member_sync_build_update_msg(join_id, version, info, body, sizeof(body));
    if (size < 0 || sc_ed25519_sign(private, body, (size_t)size, signature) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "test membership: profile signing failed version=%llu", (unsigned long long)version);
        return -1;
    }
    return 0;
}

static int test_member_seed(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t x[32], const uint8_t ed[32],
    const uint8_t private[32], const uint8_t group_private[32], const char* info, const char* tags, const uint8_t* tags_sig) {
    uint8_t gx[32], ged[32], body[MS_USERINFO_MAX + 128], join[64], update[64], tree[64];
    uint64_t id = sc_derive_node_id_from_pubkey(x);
    if (topo_node_sqlite_channel_get(inst->topo_sqlite_db, channel, NULL, 0, NULL, gx, ged, NULL)) return -1;
    int size = member_sync_build_join_msg(gx, ged, ed, x, body, sizeof(body));
    if (size < 0 || sc_ed25519_sign(private, body, size, join) != SC_OK ||
        member_sync_sign_join(group_private, channel, NULL, x, ed, TEST_JOIN_ID(gx, ged, ed, x), 1, tree)) return -1;
    size = member_sync_build_update_msg(TEST_JOIN_ID(gx, ged, ed, x), 1, info, body, sizeof(body));
    if (size < 0 || sc_ed25519_sign(private, body, size, update) != SC_OK) return -1;
    return member_sync_put(inst, channel, x, ed, join, 1, update, 1, info, tags, tags_sig, 0, 0, tree);
}
#endif
