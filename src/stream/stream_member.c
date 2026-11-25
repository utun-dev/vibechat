#include <stdio.h>
#include <string.h>

#include "../../lib/debug_config.h"
#include "../../lib/json_flat.h"
#include "../../lib/mem.h"
#include "../utun_instance.h"
#include "../chat/group_policy.h"
#include "../routing_layer/topo_group.h"
#include "stream_block.h"
#include "stream_member.h"

/* Канонические IDs не проходят через JSON number и не теряют старшие биты. */
static int member_hex(const char **text, uint64_t *value) {
    const char *p = *text;
    uint64_t result = 0;
    for (unsigned i = 0; i < 16; i++) {
        unsigned digit;
        if (p[i] >= '0' && p[i] <= '9') digit = (unsigned)(p[i] - '0');
        else if (p[i] >= 'a' && p[i] <= 'f') digit = (unsigned)(p[i] - 'a') + 10;
        else return STREAM_ERR_FORMAT;
        result = (result << 4) | digit;
    }
    if (!result) return STREAM_ERR_FORMAT;
    *text = p + 16;
    *value = result;
    return STREAM_OK;
}

static int member_address(const char **text, struct stream_address *address) {
    const char *p = *text;
    if (strspn(p, "0123456789abcdef") != 64) return STREAM_ERR_FORMAT;
    char hex[65] = {0};
    memcpy(hex, p, 64);
    if (sc_hex_to_binary(hex, address->ed, SC_PUBKEY_SIZE) || !stream_address_set(*address)) return STREAM_ERR_FORMAT;
    *text += 64;
    return STREAM_OK;
}

/* Десятичные параметры имеют ограничение ещё до сужения до uint8/uint16. */
static int member_decimal(const char **text, unsigned maximum, unsigned *value) {
    const char *p = *text;
    unsigned result = 0;
    if (*p < '0' || *p > '9' || (*p == '0' && p[1] >= '0' && p[1] <= '9')) return STREAM_ERR_FORMAT;
    while (*p >= '0' && *p <= '9') {
        unsigned digit = (unsigned)(*p++ - '0');
        if (result > (maximum - digit) / 10) return STREAM_ERR_FORMAT;
        result = result * 10 + digit;
    }
    if (result > maximum) return STREAM_ERR_FORMAT;
    *text = p;
    *value = result;
    return STREAM_OK;
}

struct member_parse {
    struct stream_member_state state;
    unsigned seen;
};

/* Полностью проверить обе опции, в том числе повторы ключей и порядок списка. */
static int member_option(const char *key, const char *value, void *arg) {
    struct member_parse *parse = arg;
    if (!strcmp(key, STREAM_MEMBER_SOURCE)) {
        if (parse->seen & 1) return -1;
        parse->seen |= 1;
        if (!value[0]) return 0;
        const char *p = value;
        unsigned n, r, size, audio;
        if (member_hex(&p, &parse->state.source.source_epoch) != STREAM_OK || *p++ != ':'
            || member_decimal(&p, 255, &n) != STREAM_OK || *p++ != ':'
            || member_decimal(&p, 254, &r) != STREAM_OK || *p++ != ':'
            || member_decimal(&p, RS_MAX_BLOCK_SIZE, &size) != STREAM_OK || *p++ != ':'
            || member_decimal(&p, 1, &audio) != STREAM_OK || *p) return -1;
        parse->state.source.n = (uint8_t)n;
        parse->state.source.r = (uint8_t)r;
        parse->state.source.block_size = (uint16_t)size;
        if (stream_descriptor_validate(&parse->state.source) != STREAM_OK) return -1;
        parse->state.published = 1;
        parse->state.audio_enabled = (int)audio;
    } else if (!strcmp(key, STREAM_MEMBER_SUBSCRIPTIONS)) {
        if (parse->seen & 2) return -1;
        parse->seen |= 2;
        const char *p = value;
        while (*p) {
            if (parse->state.count == STREAM_MEMBER_MAX_SUBSCRIPTIONS) return -1;
            struct stream_subscription_key entry = {0};
            if (member_address(&p, &entry.source) != STREAM_OK || *p++ != ':'
                || member_hex(&p, &entry.source_epoch) != STREAM_OK) return -1;
            if (*p == ':') { p++; if (*p++ != 'r') return -1; entry.relay_only = 1; }
            if (parse->state.count) {
                const struct stream_subscription_key *previous = &parse->state.subscriptions[parse->state.count - 1];
                if (stream_address_cmp(entry.source, previous->source) < 0
                    || (stream_address_equal(entry.source, previous->source) && entry.source_epoch <= previous->source_epoch)) return -1;
            }
            parse->state.subscriptions[parse->state.count++] = entry;
            if (*p && (*p++ != ',' || !*p)) return -1;
        }
    }
    return 0;
}

int stream_member_has_subscription(const struct stream_member_state *state, struct stream_address source, uint64_t epoch) {
    if (!state || state->count > STREAM_MEMBER_MAX_SUBSCRIPTIONS) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "member subscription lookup invalid state"); return 0;
    }
    for (size_t i = 0; i < state->count; i++)
        if (stream_address_equal(state->subscriptions[i].source, source) && state->subscriptions[i].source_epoch == epoch) return 1;
    return 0;
}

/* Отсутствие stream-ключей — обычный мембер, оно не создаёт экземпляр или буфер. */
int stream_member_parse(uint64_t group_id, struct stream_address member, const char *userinfo, struct stream_member_state *state) {
    if (!group_id || !stream_address_set(member) || !userinfo || !state || strlen(userinfo) > MS_USERINFO_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "member parse invalid arguments group=%llx member=%s",
                    (unsigned long long)group_id, sc_public_key_hex(member.ed, (char[65]){0}));
        return STREAM_ERR_ARGUMENT;
    }
    struct member_parse parse = { .state.source = { .group_id = group_id, .source = member } };
    if (json_flat_parse(userinfo, member_option, &parse) < 0
        || (parse.state.published && !stream_member_has_subscription(&parse.state, member, parse.state.source.source_epoch))) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "member stream options malformed/inconsistent group=%llx member=%s",
                   (unsigned long long)group_id, sc_public_key_hex(member.ed, (char[65]){0}));
        return STREAM_ERR_FORMAT;
    }
    *state = parse.state;
    return STREAM_OK;
}

/* Сериализовать канонические строки до обновления подписанного JSON-профиля. */
int stream_member_format(const struct stream_member_state *state, char source[MS_USERINFO_MAX + 1],
                         char subscriptions[MS_USERINFO_MAX + 1]) {
    if (!state || !source || !subscriptions || state->count > STREAM_MEMBER_MAX_SUBSCRIPTIONS
        || (state->audio_enabled != 0 && state->audio_enabled != 1) || (!state->published && state->audio_enabled)
        || (state->published && (stream_descriptor_validate(&state->source) != STREAM_OK
            || !stream_member_has_subscription(state, state->source.source, state->source.source_epoch)))) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "member format invalid state"); return STREAM_ERR_ARGUMENT;
    }
    char encoded_source[MS_USERINFO_MAX + 1] = "", encoded_subscriptions[MS_USERINFO_MAX + 1] = "";
    if (state->published) snprintf(encoded_source, sizeof(encoded_source), "%016llx:%u:%u:%u:%d",
        (unsigned long long)state->source.source_epoch, state->source.n, state->source.r, state->source.block_size, state->audio_enabled);
    size_t used = 0;
    for (size_t i = 0; i < state->count; i++) {
        const struct stream_subscription_key *entry = &state->subscriptions[i];
        if (!stream_address_set(entry->source) || !entry->source_epoch || (i &&
            (stream_address_cmp(entry->source, state->subscriptions[i - 1].source) < 0 ||
             (stream_address_equal(entry->source, state->subscriptions[i - 1].source)
              && entry->source_epoch <= state->subscriptions[i - 1].source_epoch)))) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "member format invalid subscription index=%zu", i); return STREAM_ERR_ARGUMENT;
        }
        int length = snprintf(encoded_subscriptions + used, sizeof(encoded_subscriptions) - used, "%s%s:%016llx%s",
                              i ? "," : "", sc_public_key_hex(entry->source.ed, (char[65]){0}), (unsigned long long)entry->source_epoch,
                              entry->relay_only ? ":r" : "");
        if (length < 0 || (size_t)length >= sizeof(encoded_subscriptions) - used) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "member subscriptions exceed limit count=%zu", state->count); return STREAM_ERR_ARGUMENT;
        }
        used += (size_t)length;
    }
    memcpy(source, encoded_source, strlen(encoded_source) + 1);
    memcpy(subscriptions, encoded_subscriptions, used + 1);
    return STREAM_OK;
}

/* Подпись/допуск проверены общей проекцией; здесь только разбор собственного снимка, без SQL. */
int stream_member_load(struct UTUN_INSTANCE *inst, uint64_t group_id, struct stream_address member,
                       struct stream_member_state *state, uint8_t public_key[32]) {
    struct TOPO_GROUP *group = inst && inst->topo_groups ? topo_groups_find(inst->topo_groups, group_id) : NULL;
    if (!group || group->stopping || group->group_type != TOPO_GROUP_TYPE_CHAT || !state || !stream_address_set(member)) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "member load invalid group=%llx member=%s", (unsigned long long)group_id,
                    sc_public_key_hex(member.ed, (char[65]){0})); return STREAM_ERR_ARGUMENT;
    }
    const char* userinfo = NULL; uint64_t version = 0;
    int status = group_policy_member_profile_key(inst, group->channel_id, member.ed, &userinfo, &version);
    if (status != MS_AUTHOR_ACTIVE) {
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "member snapshot unavailable group=%llx member=%s status=%d",
            (unsigned long long)group_id, sc_public_key_hex(member.ed, (char[65]){0}), status);
        if (status == MS_AUTHOR_PENDING) return STREAM_WAIT;
        if (status == MS_AUTHOR_INACTIVE) return STREAM_ERR_NOT_MEMBER;
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "member snapshot failed group=%llx member=%s status=%d",
            (unsigned long long)group_id, sc_public_key_hex(member.ed, (char[65]){0}), status);
        return status == MS_AUTHOR_INVALID ? STREAM_ERR_SIGNATURE : STREAM_ERR_STATE;
    }
    struct stream_member_state parsed;
    int result = stream_member_parse(group_id, member, userinfo, &parsed);
    if (result == STREAM_OK) {
        const uint8_t* key = topo_group_wire_id(group);
        if (key) memcpy(parsed.source.group_key, key, SC_PUBKEY_SIZE);
        parsed.version = version; *state = parsed;
        if (public_key) memcpy(public_key, member.ed, 32);
    }
    return result;
}

/* Online относится к пути этой группы; транспорт или запись в общей БД не заменяют его. */
int stream_member_online(struct UTUN_INSTANCE *inst, uint64_t group_id, struct stream_address member) {
    struct TOPO_GROUP *group = inst && inst->topo_groups ? topo_groups_find(inst->topo_groups, group_id) : NULL;
    if (!group || group->stopping || group->group_type != TOPO_GROUP_TYPE_CHAT) return 0;
    if (!memcmp(member.ed, inst->my_ed25519_pubkey, SC_PUBKEY_SIZE)) return inst->core_started && group->local_node != NULL;
    struct TOPO_GROUP_NODE *node = topo_node_find_by_ed(group, member.ed);
    return node && topo_node_best_path(node) != NULL;
}

/* Роль подготовлена вместе с допуском: сетевой цикл не читает SQLite. */
int stream_member_supernode(struct UTUN_INSTANCE *inst, uint64_t group_id, struct stream_address member) {
    struct TOPO_GROUP *group = inst && inst->topo_groups ? topo_groups_find(inst->topo_groups, group_id) : NULL;
    if (!group || group->stopping || group->group_type != TOPO_GROUP_TYPE_CHAT) return 0;
    int result = group_policy_member_role_key(inst, group->channel_id, member.ed, GROUP_MEMBER_SUPERNODE);
    DEBUG_TRACE(DEBUG_CATEGORY_STREAM, "member role group=%llx member=%s supernode=%d",
                (unsigned long long)group_id, sc_public_key_hex(member.ed, (char[65]){0}), result);
    return result;
}
