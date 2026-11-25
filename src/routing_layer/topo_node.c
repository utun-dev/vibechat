#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

#include "../lib/ll_queue.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/db_async.h"
#include "../lib/memory_pool.h"
#include "utun_instance.h"
#include "etcp.h"
#include "config_parser.h"
#include "topo_node.h"
#include "../db_changes.h"
#include "../chat/chat_core.h"
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include "topo_group.h"
#include "route_connectivity.h"
#include "route_lib.h"
#include "etcp_debug.h"
#include "topo_node_sqlite.h"
#include "../transport_layer/etcp_connections.h"
#include "../lib/u_async.h"
#include "../ntp_time.h"
#include "../call/call.h"

/* Инкремент refcount узла: держит TOPO_NODE живым, пока на него ссылаются группы. */
void topo_node_ref(struct TOPO_NODE* ni) {
    if (!ni) return;
    ni->group_ref_count++;
}

/* Инкремент refcount узла в глобальном реестре по node_id (взял ссылку через реестр). */
void topo_node_registry_ref(struct TOPO_GROUPS* groups, uint64_t node_id) {
    struct TOPO_NODE* ni = topo_node_registry_find(groups, node_id);
    if (ni) ni->group_ref_count++;
}

/* Декремент refcount; при обнулении освобождает имя узла и сам TOPO_NODE. */
void topo_node_unref(struct TOPO_NODE* ni) {
    if (!ni) return;
    if (--ni->group_ref_count == 0) {
        u_free(ni->node_name);
        u_free(ni);
    }
}

/* Освобождают односвязные списки структур узла (sock_meta/адреса/подсети v4,v6) из memory_pool. */
static void free_v4_sock_list(struct memory_pool* pool, struct TOPO_SOCKMETA4* head) {
    while (head) { struct TOPO_SOCKMETA4* next = head->next; memory_pool_free(pool, head); head = next; }
}
static void free_v4_addr_list(struct memory_pool* pool, struct TOPO_ADDR4* head) {
    while (head) { struct TOPO_ADDR4* next = head->next; memory_pool_free(pool, head); head = next; }
}
static void free_v6_sock_list(struct memory_pool* pool, struct TOPO_SOCKMETA6* head) {
    while (head) { struct TOPO_SOCKMETA6* next = head->next; memory_pool_free(pool, head); head = next; }
}
static void free_v6_addr_list(struct memory_pool* pool, struct TOPO_ADDR6* head) {
    while (head) { struct TOPO_ADDR6* next = head->next; memory_pool_free(pool, head); head = next; }
}
static void free_v4_sub_list(struct memory_pool* pool, struct TOPO_SUBNET4* head) {
    while (head) { struct TOPO_SUBNET4* next = head->next; memory_pool_free(pool, head); head = next; }
}
static void free_v6_sub_list(struct memory_pool* pool, struct TOPO_SUBNET6* head) {
    while (head) { struct TOPO_SUBNET6* next = head->next; memory_pool_free(pool, head); head = next; }
}
/* Освобождает список reality-сокетов узла (выделялся u_calloc). */
static void free_reality_sock_list(struct TOPO_REALITY_SOCK* head) {
    while (head) { struct TOPO_REALITY_SOCK* next = head->next; u_free(head); head = next; }
}

/* Ищет reality-параметры сокета узла по socket_id (NULL, если сокет без reality). */
const struct TOPO_REALITY_SOCK* topo_node_find_reality_sock(const struct TOPO_NODE* ni, uint8_t socket_id) {
    if (!ni) return NULL;
    for (const struct TOPO_REALITY_SOCK* r = ni->reality_socks; r; r = r->next)
        if (r->socket_id == socket_id) return r;
    return NULL;
}

/* Копирует reality-параметры узла в link->reality и поднимает флаг reality_set. */
void topo_node_reality_config(struct reality_client_config *cfg, const struct TOPO_REALITY_SOCK *rs) {
    memcpy(cfg->server_static_pubkey, rs->server_pubkey, REALITY_AUTH_KEY_SIZE);
    memcpy(cfg->short_id, rs->short_id, REALITY_SHORT_ID_SIZE);
    memcpy(cfg->version, rs->version, REALITY_VERSION_SIZE);
    snprintf(cfg->server_name, sizeof(cfg->server_name), "%s", rs->server_name);
    cfg->fingerprint = REALITY_FP_OPENSSL;
}

void topo_node_apply_reality(struct ETCP_LINK* link, const struct TOPO_REALITY_SOCK* rs) {
    if (!link || !rs) return;
    topo_node_reality_config(&link->reality, rs);
    link->reality_set = 1;
}

/* Упорядоченное сравнение списков адресов/мета (порядок обхода сокетов детерминирован,
   prepend даёт одинаковый порядок при неизменном наборе сокетов). */
static int sockmeta4_list_equal(struct TOPO_SOCKMETA4* a, struct TOPO_SOCKMETA4* b) {
    while (a && b) {
        if (a->id != b->id || a->config_type != b->config_type ||
            a->address_profile.source != b->address_profile.source ||
            a->address_profile.mapping != b->address_profile.mapping || a->address_profile.inbound != b->address_profile.inbound)
            return 0;
        a = a->next; b = b->next;
    }
    return a == NULL && b == NULL;
}
static int addr4_list_equal(struct TOPO_ADDR4* a, struct TOPO_ADDR4* b) {
    while (a && b) {
        if (a->port != b->port || a->type != b->type || a->socket_id != b->socket_id ||
            a->protocol != b->protocol || memcmp(a->addr, b->addr, 4) != 0) return 0;
        a = a->next; b = b->next;
    }
    return a == NULL && b == NULL;
}
static int sockmeta6_list_equal(struct TOPO_SOCKMETA6* a, struct TOPO_SOCKMETA6* b) {
    while (a && b) {
        if (a->id != b->id || a->config_type != b->config_type ||
            a->address_profile.source != b->address_profile.source ||
            a->address_profile.mapping != b->address_profile.mapping || a->address_profile.inbound != b->address_profile.inbound)
            return 0;
        a = a->next; b = b->next;
    }
    return a == NULL && b == NULL;
}
static int addr6_list_equal(struct TOPO_ADDR6* a, struct TOPO_ADDR6* b) {
    while (a && b) {
        if (a->port != b->port || a->type != b->type || a->socket_id != b->socket_id ||
            a->protocol != b->protocol || memcmp(a->addr, b->addr, 16) != 0) return 0;
        a = a->next; b = b->next;
    }
    return a == NULL && b == NULL;
}
static int reality_sock_list_equal(struct TOPO_REALITY_SOCK* a, struct TOPO_REALITY_SOCK* b) {
    while (a && b) {
        if (a->socket_id != b->socket_id ||
            memcmp(a->short_id, b->short_id, 8) != 0 ||
            memcmp(a->server_pubkey, b->server_pubkey, 32) != 0 ||
            memcmp(a->version, b->version, 3) != 0 ||
            strncmp(a->server_name, b->server_name, TOPO_REALITY_SNAME_MAX) != 0) return 0;
        a = a->next; b = b->next;
    }
    return a == NULL && b == NULL;
}

/* Один выбор для пересылки данных и NODEINFO. Порядок доставки не влияет на равные пути. */
struct TOPO_NODEPATH* topo_node_best_path(const struct TOPO_GROUP_NODE* nq) {
    struct TOPO_NODEPATH* best = NULL;
    for (struct ll_entry* e = nq && nq->paths ? nq->paths->head : NULL; e; e = e->next) {
        struct TOPO_NODEPATH* p = (struct TOPO_NODEPATH*)e;
        if (!p->conn || !p->conn->links_up || p->conn->close_requested) continue;
        if (!best || p->hop_count < best->hop_count || (p->hop_count == best->hop_count &&
            memcmp(p->conn->peer_ed25519_pubkey, best->conn->peer_ed25519_pubkey, SC_PUBKEY_SIZE) < 0)) best = p;
    }
    return best;
}

/* Цепочка выбранного живого пути; память принадлежит paths, не освобождать. */
const struct topo_hop* topo_node_best_hop_list(struct TOPO_GROUP_NODE* nq, uint8_t* out_count, uint16_t* out_rtt) {
    struct TOPO_NODEPATH* best = topo_node_best_path(nq);
    *out_count = best ? best->hop_count : 0;
    if (out_rtt) *out_rtt = best ? best->cumulative_rtt : 0;
    return best ? best->hops : NULL;
}

/* Освобождает списки адресов (sock_meta + addrs + reality) узла в глобальном реестре.
 * Адреса — глобальная идентичность узла, шарится между группами через node_registry,
 * поэтому освобождаются только при последнем unref (topo_node_registry_unref). */
static void topo_node_identity_free(struct TOPO_GROUPS* groups, struct TOPO_NODE* ni) {
    if (!groups || !ni) return;
    free_v4_sock_list(groups->v4_sock_meta_pool, ni->v4_sock_meta);
    free_v4_addr_list(groups->v4_addr_pool, ni->v4_addrs);
    free_v6_sock_list(groups->v6_sock_meta_pool, ni->v6_sock_meta);
    free_v6_addr_list(groups->v6_addr_pool, ni->v6_addrs);
    free_reality_sock_list(ni->reality_socks);
    ni->v4_sock_meta = NULL; ni->v4_addrs = NULL;
    ni->v6_sock_meta = NULL; ni->v6_addrs = NULL;
    ni->reality_socks = NULL;
}

/* Полностью освобождает TOPO_NODE со всеми списками и именем (без учёта refcount). */
void topo_node_free_raw(struct TOPO_GROUPS* groups, struct TOPO_NODE* ni) {
    if (!ni) return;
    if (groups) {
        free_v4_sock_list(groups->v4_sock_meta_pool, ni->v4_sock_meta);
        free_v4_addr_list(groups->v4_addr_pool, ni->v4_addrs);
        free_v6_sock_list(groups->v6_sock_meta_pool, ni->v6_sock_meta);
        free_v6_addr_list(groups->v6_addr_pool, ni->v6_addrs);
    }
    free_reality_sock_list(ni->reality_socks);
    u_free(ni->node_name);
    u_free(ni);
}

/* Публичная обёртка free_raw — уничтожение узла глобального реестра. */
void topo_node_destroy(struct TOPO_GROUPS* groups, struct TOPO_NODE* ni) {
    topo_node_free_raw(groups, ni);
}

/* Освобождает per-group поля TOPO_GROUP_NODE: unref идентичности в реестре + подсети, сброс handle. */
void topo_nodeq_free_group_fields(struct TOPO_GROUPS* groups, struct TOPO_GROUP_NODE* nq) {
    if (!groups || !nq) return;
    topo_node_registry_release(groups, nq->identity); nq->identity = NULL;
    if (nq->subnets) {
        free_v4_sub_list(groups->v4_subnet_pool, nq->subnets->v4_subnets);
        free_v6_sub_list(groups->v6_subnet_pool, nq->subnets->v6_subnets);
        u_free(nq->subnets);
        nq->subnets = NULL;
    }
    nq->handle = NULL;
}

void topo_nodeq_clear_paths(struct TOPO_GROUP_NODE* nq) {
    if (!nq) return;
    memset(&nq->rtt_average, 0, sizeof(nq->rtt_average));
    if (!nq->paths) return;
    int count = queue_entry_count(nq->paths);
    struct ll_entry* entry;
    while ((entry = queue_data_get(nq->paths))) queue_entry_free(entry);
    queue_free(nq->paths); nq->paths = NULL;
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "node paths released node=%016llx count=%d", (unsigned long long)nq->node_id, count);
}

int topo_address_short(const uint8_t ed[SC_PUBKEY_SIZE], uint32_t* short31) {
    uint8_t hash[SHA256_DIGEST_LENGTH];
    if (!ed || !short31 || !SHA256(ed, SC_PUBKEY_SIZE, hash)) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "cannot derive short BGP address"); return -1;
    }
    *short31 = (uint32_t)(hash[0] & 0x7f) << 24 | (uint32_t)hash[1] << 16 | (uint32_t)hash[2] << 8 | hash[3];
    return 0;
}

struct TOPO_GROUP_NODE* topo_address_find(const struct TOPO_GROUP* group, uint32_t short31, const uint8_t* ed) {
    if (!group) return NULL;
    for (struct TOPO_GROUP_NODE* n = group->address_buckets[short31 % BGP_NODES_HASH_SIZE]; n; n = n->address_next) {
        if (n->short31 == short31 && (ed ? !memcmp(n->ed25519, ed, SC_PUBKEY_SIZE) : !n->short_ambiguous)) return n;
    }
    return NULL;
}

struct TOPO_GROUP_NODE* topo_node_find_by_ed(const struct TOPO_GROUP* group, const uint8_t ed[SC_PUBKEY_SIZE]) {
    uint32_t address;
    return group && ed && !topo_address_short(ed, &address) ? topo_address_find(group, address, ed) : NULL;
}

int topo_address_write(uint8_t* buf, size_t size, const struct TOPO_GROUP* group, const uint8_t ed[SC_PUBKEY_SIZE],
                       enum topo_address_format format) {
    if (!buf || !ed || format < TOPO_ADDRESS_FIXED || format > TOPO_ADDRESS_COMPACT) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "invalid BGP address output format=%d", format); return -1;
    }
    if (format == TOPO_ADDRESS_COMPACT && group) {
        uint32_t short31;
        if (topo_address_short(ed, &short31)) return -1;
        struct TOPO_GROUP_NODE* node = topo_address_find(group, short31, NULL);
        if (node && !memcmp(node->ed25519, ed, SC_PUBKEY_SIZE)) {
            if (size < 4) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "short BGP address output buffer size=%zu", size); return -1; }
            buf[0] = short31 >> 24; buf[1] = short31 >> 16; buf[2] = short31 >> 8; buf[3] = short31;
            return 4;
        }
    }
    size_t tag = format != TOPO_ADDRESS_FIXED;
    if (size < tag + SC_PUBKEY_SIZE) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "full BGP address output buffer size=%zu", size); return -1; }
    if (tag) buf[0] = 0x80;
    memcpy(buf + tag, ed, SC_PUBKEY_SIZE);
    return (int)(tag + SC_PUBKEY_SIZE);
}

int topo_address_read(const uint8_t* buf, size_t size, enum topo_address_format format, struct topo_wire_address* out) {
    if (!buf || !out || !size || format < TOPO_ADDRESS_FIXED || format > TOPO_ADDRESS_COMPACT) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "invalid BGP address input format=%d size=%zu", format, size); return -1;
    }
    struct topo_wire_address address = {0};
    if (format == TOPO_ADDRESS_COMPACT && !(buf[0] & 0x80)) {
        if (size < 4) { DEBUG_WARN(DEBUG_CATEGORY_BGP, "truncated short BGP address size=%zu", size); return -1; }
        address.short31 = (uint32_t)buf[0] << 24 | (uint32_t)buf[1] << 16 | (uint32_t)buf[2] << 8 | buf[3];
        *out = address; return 4;
    }
    size_t tag = format != TOPO_ADDRESS_FIXED;
    if ((tag && buf[0] != 0x80) || size < tag + SC_PUBKEY_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "invalid full BGP address format=%d first=%02x size=%zu", format, buf[0], size); return -1;
    }
    uint8_t x[SC_PUBKEY_SIZE];
    address.ed = buf + tag;
    if (sc_identity_public_x("BGP address", address.ed, x) != SC_OK || topo_address_short(address.ed, &address.short31)) return -1;
    *out = address;
    return (int)(tag + SC_PUBKEY_SIZE);
}

void topo_address_attach(struct TOPO_GROUP* group, struct TOPO_GROUP_NODE* node, const uint8_t ed[SC_PUBKEY_SIZE], uint32_t short31) {
    if (node->address_indexed) return;
    struct TOPO_GROUP_NODE** head = &group->address_buckets[short31 % BGP_NODES_HASH_SIZE];
    memcpy(node->ed25519, ed, SC_PUBKEY_SIZE);
    node->short31 = short31;
    for (struct TOPO_GROUP_NODE* n = *head; n; n = n->address_next) {
        if (n->short31 != short31) continue;
        n->short_ambiguous = node->short_ambiguous = 1;
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "BGP short address collision group=%016llx short=%08x peers=%s,%s",
                   (unsigned long long)group->group_id, short31, sc_public_key_hex(n->ed25519, (char[65]){0}), sc_public_key_hex(ed, (char[65]){0}));
    }
    node->address_next = *head; *head = node; node->address_indexed = 1;
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "BGP address added group=%016llx short=%08x node=%016llx",
                (unsigned long long)group->group_id, short31, (unsigned long long)node->node_id);
}

void topo_address_detach(struct TOPO_GROUP* group, struct TOPO_GROUP_NODE* node) {
    if (!node->address_indexed) return;
    struct TOPO_GROUP_NODE** head = &group->address_buckets[node->short31 % BGP_NODES_HASH_SIZE];
    struct TOPO_GROUP_NODE** link = head;
    while (*link && *link != node) link = &(*link)->address_next;
    if (!*link) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "missing BGP address index entry short=%08x", node->short31); return;
    }
    *link = node->address_next; node->address_next = NULL; node->address_indexed = 0;
    if (!node->short_ambiguous) return;
    node->short_ambiguous = 0;
    struct TOPO_GROUP_NODE* remaining = NULL;
    for (struct TOPO_GROUP_NODE* n = *head; n; n = n->address_next) {
        if (n->short31 != node->short31) continue;
        if (remaining) return;
        remaining = n;
    }
    if (remaining) {
        remaining->short_ambiguous = 0;
        DEBUG_INFO(DEBUG_CATEGORY_BGP, "BGP short address unique again group=%016llx short=%08x node=%016llx",
                   (unsigned long long)group->group_id, remaining->short31, (unsigned long long)remaining->node_id);
    }
}

/* Полное удаление узла из группы: отмена влётных проб, освобождение paths/subnets,
 * извлечение из group->nodes и освобождение самого nq. Единственный корректный способ удаления. */
void topo_nodeq_remove_node(struct TOPO_GROUP* group, struct TOPO_GROUP_NODE* nq) {
    if (!group || !nq) return;
    topo_address_detach(group, nq);
    route_connectivity_cancel_node(nq);
    if (group->instance) route_delete(group->instance->rt, nq);
    topo_nodeq_clear_paths(nq);
    queue_remove_data(group->nodes, &nq->ll);
    topo_nodeq_free_group_fields(group->instance ? group->instance->topo_groups : NULL, nq);
    queue_entry_free(&nq->ll);
}

// ===== глобальный реестр TOPO_NODE =====

/* Старший уровень хранит хеш X25519 как node_id. Совпадение хеша выбирает корзину,
 * а равенство идентичностей определяется всеми байтами ключа. */
static struct ll_entry* registry_entry(struct TOPO_GROUPS* groups, uint64_t id, const uint8_t* x) {
    struct ll_queue* q = groups ? groups->node_registry : NULL;
    struct ll_entry* e = queue_find_data_by_index(q, &id);
    if (!x) {
        if (e && queue_find_next_by_index(q, &id, e)) {
            DEBUG_WARN(DEBUG_CATEGORY_BGP, "ambiguous topology node_id=%016llx, full key required", (unsigned long long)id);
            return NULL;
        }
        return e;
    }
    while (e) {
        struct TOPO_NODE* node;
        memcpy(&node, e->data + sizeof(id), sizeof(node));
        if (!memcmp(node->public_key, x, SC_PUBKEY_SIZE)) break;
        e = queue_find_next_by_index(q, &id, e);
    }
    return e;
}

struct TOPO_NODE* topo_node_registry_lookup(struct TOPO_GROUPS* groups, uint64_t id, const uint8_t* x) {
    struct ll_entry* e = registry_entry(groups, id, x);
    struct TOPO_NODE* node = NULL;
    if (e) memcpy(&node, e->data + sizeof(id), sizeof(node));
    return node;
}

struct TOPO_NODE* topo_node_registry_find(struct TOPO_GROUPS* groups, uint64_t id) {
    return topo_node_registry_lookup(groups, id, NULL);
}

/* Является ли входящий узел полной идентичностью (BGP NODEINFO / локальный узел).
 * Partial-узлы из invite/DB-load не несут ed25519 — по ним идентичность не перезаписываем. */
static int topo_node_is_full_identity(const struct TOPO_NODE* ni) {
    for (int i = 0; i < SC_PUBKEY_SIZE; i++) if (ni->ed25519_public_key[i]) return 1;
    return 0;
}

/* Кладёт узел в реестр; если он уже есть — обновляет идентичность/адреса и возвращает существующий. */
struct TOPO_NODE* topo_node_registry_put(struct TOPO_GROUPS* groups, struct TOPO_NODE* ni) {
    if (!groups || !groups->node_registry || !ni) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "registry put: missing groups/registry/node"); return NULL;
    }
    struct TOPO_NODE* existing = topo_node_registry_lookup(groups, ni->node_id, ni->public_key);
    if (existing == ni) { topo_node_ref(existing); return existing; }
    if (ni->timestamp && topo_node_verify(ni) < 0) return NULL;
    if (existing) {
        if (topo_node_is_full_identity(existing) && topo_node_is_full_identity(ni) &&
            memcmp(existing->ed25519_public_key, ni->ed25519_public_key, SC_PUBKEY_SIZE)) {
            DEBUG_WARN(DEBUG_CATEGORY_BGP, "registry Ed25519 mismatch for the same X25519 node=%016llx", (unsigned long long)ni->node_id); return NULL;
        }
        topo_node_ref(existing);

        if (existing->timestamp || ni->timestamp) {
            if (ni->timestamp <= existing->timestamp) {
                if (ni->timestamp == existing->timestamp && memcmp(ni->x25519_self_sig, existing->x25519_self_sig, 64))
                    DEBUG_WARN(DEBUG_CATEGORY_BGP, "node timestamp conflict: node=%016llx timestamp=%llu",
                               (unsigned long long)ni->node_id, (unsigned long long)ni->timestamp);
                else DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "node unchanged/stale: node=%016llx current=%llu incoming=%llu",
                                 (unsigned long long)ni->node_id, (unsigned long long)existing->timestamp,
                                 (unsigned long long)ni->timestamp);
                topo_node_free_raw(groups, ni);
                return existing;
            }
            /* Сохраняем адрес объекта для заёмщиков, заменяем всё подписанное содержимое сразу. */
            topo_node_identity_free(groups, existing);
            u_free(existing->node_name);
            struct ll_entry ll = existing->ll;
            uint32_t refs = existing->group_ref_count;
            *existing = *ni;
            existing->ll = ll;
            existing->group_ref_count = refs;
            u_free(ni);
            DEBUG_INFO(DEBUG_CATEGORY_BGP, "node snapshot updated: node=%016llx timestamp=%llu",
                       (unsigned long long)existing->node_id, (unsigned long long)existing->timestamp);
            return existing;
        }

        /* Идентичность (ver/имя/pubkeys/подпись) — авторитетна у полного NODEINFO,
         * иначе реестр держит устаревший ver/имя и форвард nodeinfo ломает подпись. */
        if (topo_node_is_full_identity(ni)) {
            existing->ver = ni->ver;
            existing->client_type = ni->client_type;
            existing->client_activity = ni->client_activity;
            memcpy(existing->public_key, ni->public_key, SC_PUBKEY_SIZE);
            memcpy(existing->ed25519_public_key, ni->ed25519_public_key, SC_PUBKEY_SIZE);
            memcpy(existing->x25519_self_sig, ni->x25519_self_sig, 64);
            if (ni->node_name) {
                if (existing->node_name) u_free(existing->node_name);
                existing->node_name = ni->node_name; ni->node_name = NULL;
            }
        }

        /* Адреса — глобальная идентичность. Обновляем каждый список, который входящий узел несёт
         * (частичные узлы заполняют только то, что знают, не затирая остальное). */
        if (ni->v4_sock_meta) { free_v4_sock_list(groups->v4_sock_meta_pool, existing->v4_sock_meta); existing->v4_sock_meta = ni->v4_sock_meta; ni->v4_sock_meta = NULL; }
        if (ni->v4_addrs)     { free_v4_addr_list(groups->v4_addr_pool, existing->v4_addrs); existing->v4_addrs = ni->v4_addrs; ni->v4_addrs = NULL; }
        if (ni->v6_sock_meta) { free_v6_sock_list(groups->v6_sock_meta_pool, existing->v6_sock_meta); existing->v6_sock_meta = ni->v6_sock_meta; ni->v6_sock_meta = NULL; }
        if (ni->v6_addrs)     { free_v6_addr_list(groups->v6_addr_pool, existing->v6_addrs); existing->v6_addrs = ni->v6_addrs; ni->v6_addrs = NULL; }
        if (ni->reality_socks) { free_reality_sock_list(existing->reality_socks); existing->reality_socks = ni->reality_socks; ni->reality_socks = NULL; }

        topo_node_free_raw(groups, ni);
        return existing;
    }
    struct ll_entry* qe = queue_entry_new(8 + sizeof(struct TOPO_NODE*));
    if (!qe) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "registry_acquire: queue_entry_new failed for node=%016llx", (unsigned long long)ni->node_id);
        return NULL;
    }
    uint32_t previous_refs = ni->group_ref_count;
    topo_node_ref(ni);
    memcpy(qe->data,     &ni->node_id, 8);
    memcpy(qe->data + 8, &ni,          sizeof(ni));
    if (queue_data_put_with_index(groups->node_registry, qe) < 0) {
        ni->group_ref_count = previous_refs;
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "registry put: insertion failed node=%016llx", (unsigned long long)ni->node_id); return NULL;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "registry_store: node=%016llx NEW v4=%d v6=%d",
                (unsigned long long)ni->node_id,
                topo_list_count((struct _topo_head*)ni->v4_addrs),
                topo_list_count((struct _topo_head*)ni->v6_addrs));
    return ni;
}

/* Прежний синхронный путь: сравнить с диском, затем применить тот же чистый registry update. */
struct TOPO_NODE* topo_node_registry_store(struct TOPO_GROUPS* groups, struct TOPO_NODE* ni) {
    if (!groups || !ni || !groups->instance || !groups->instance->topo_sqlite_db ||
        db_async_can_write(groups->instance->db_worker) ||
        topo_node_registry_lookup(groups, ni->node_id, ni->public_key)) return topo_node_registry_put(groups, ni);
    if (ni->timestamp && topo_node_verify(ni) < 0) return NULL;
    struct topo_node_record* record = NULL;
    if (topo_node_sqlite_record_lookup(groups->instance->topo_sqlite_db, ni->node_id, ni->public_key, TOPO_NODE_RECORD_SNAPSHOT, &record) != SQLITE_OK)
        return NULL;
    struct TOPO_NODE* saved = topo_node_sqlite_record_build(groups, record);
    int invalid = record && !saved;
    topo_node_sqlite_record_free(record);
    if (invalid) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "registry store: saved snapshot unavailable node=%016llx", (unsigned long long)ni->node_id); return NULL; }
    if (saved && topo_node_is_full_identity(ni) && memcmp(saved->ed25519_public_key, ni->ed25519_public_key, SC_PUBKEY_SIZE)) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "persisted snapshot key mismatch node=%016llx", (unsigned long long)ni->node_id);
        topo_node_free_raw(groups, saved); return NULL;
    }
    if (!saved || saved->timestamp < ni->timestamp) {
        topo_node_free_raw(groups, saved); return topo_node_registry_put(groups, ni);
    }
    if (saved->timestamp == ni->timestamp && memcmp(saved->x25519_self_sig, ni->x25519_self_sig, 64))
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "persisted node timestamp conflict node=%016llx", (unsigned long long)ni->node_id);
    struct TOPO_NODE* result = topo_node_registry_put(groups, saved);
    if (result) topo_node_free_raw(groups, ni);
    else topo_node_free_raw(groups, saved);
    return result;
}

/* Снимает ссылку группы на узел; при последней — чистит идентичность и удаляет запись из реестра. */
void topo_node_registry_release(struct TOPO_GROUPS* groups, struct TOPO_NODE* node) {
    if (!node) return;
    if (node->group_ref_count == 1) {
        struct ll_entry* entry = registry_entry(groups, node->node_id, node->public_key);
        if (!entry) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "release of unregistered node=%016llx", (unsigned long long)node->node_id); return; }
        queue_remove_data(groups->node_registry, entry); queue_entry_free(entry);
        topo_node_identity_free(groups, node);
    }
    topo_node_unref(node);
}

void topo_node_registry_unref(struct TOPO_GROUPS* groups, uint64_t id) {
    topo_node_registry_release(groups, topo_node_registry_find(groups, id));
}

// ===== serialization =====

static void sockmeta_wire_write(uint8_t *out, uint8_t id, uint8_t config, const struct address_profile *profile) {
    out[0] = id; out[1] = config; out[2] = profile->mapping; out[3] = profile->inbound;
    out[4] = profile->source;
}

static int sockmeta_wire_read(const uint8_t *in, struct address_profile *profile) {
    memset(profile, 0, sizeof(*profile)); profile->mapping = in[2]; profile->inbound = in[3];
    profile->source = in[4];
    if (!address_profile_valid(profile)) { DEBUG_WARN(DEBUG_CATEGORY_BGP, "invalid address profile in NODEINFO"); return -1; }
    return 0;
}

/* Собирает канонический буфер для Ed25519-подписи узла (id, ver, pubkey, имя, тип, адреса). */
int topo_node_build_sig_msg(struct TOPO_NODE* ni, uint8_t* buf, size_t buf_size) {
    if (!ni || !buf || buf_size < 64 + 9) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "build_sig_msg: invalid args"); return -1; }
    size_t off = 0;

    memcpy(buf + off, &ni->node_id, 8); off += 8;
    buf[off++] = ni->ver;
    if (off + 8 + SC_PUBKEY_SIZE > buf_size) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "signature buffer too small"); return -1; }
    for (int i = 7; i >= 0; i--) buf[off++] = (uint8_t)(ni->timestamp >> (i * 8));
    memcpy(buf + off, ni->public_key, SC_PUBKEY_SIZE); off += SC_PUBKEY_SIZE;

    size_t order_len = strlen(ni->connection_order);
    if ((order_len && connection_order_parse(ni->connection_order, NULL) < 0) || off + 1 + order_len > buf_size) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "invalid/oversized connection policy"); return -1;
    }
    buf[off++] = (uint8_t)order_len;
    memcpy(buf + off, ni->connection_order, order_len); off += order_len;
    size_t name_len = ni->node_name ? strlen(ni->node_name) : 0;
    if (name_len > 63) name_len = 63;
    if (off + name_len + 1 > buf_size) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "build_sig_msg: buf too small for name"); return -1; }
    if (name_len) { memcpy(buf + off, ni->node_name, name_len); off += name_len; }
    buf[off++] = '\0';

    if (off + 7 > buf_size) return -1;
    buf[off++] = ni->client_type;
    buf[off++] = ni->client_activity;
    /* Границы списков тоже подписаны: нельзя переразбить то же тело на другие секции. */
    int counts[] = {topo_list_count((struct _topo_head *)ni->v4_sock_meta), topo_list_count((struct _topo_head *)ni->v4_addrs),
        topo_list_count((struct _topo_head *)ni->v6_sock_meta), topo_list_count((struct _topo_head *)ni->v6_addrs),
        topo_list_count((struct _topo_head *)ni->reality_socks)};
    for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
        if (counts[i] > UINT8_MAX) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "signed node section exceeds wire limits"); return -1; }
        buf[off++] = (uint8_t)counts[i];
    }

    { struct TOPO_SOCKMETA4* e = ni->v4_sock_meta; while (e) {
        if (off + sizeof(struct TOPOMSG_SOCKMETA4) > buf_size || !address_profile_valid(&e->address_profile)) {
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "invalid/oversized v4 socket profile"); return -1;
        }
        sockmeta_wire_write(buf + off, e->id, e->config_type, &e->address_profile);
        off += sizeof(struct TOPOMSG_SOCKMETA4); e = e->next;
    }}
    { struct TOPO_ADDR4* e = ni->v4_addrs; while (e) {
        if (off + 9 > buf_size) return -1;
        memcpy(buf + off, e->addr, 4); off += 4;
        buf[off++] = (uint8_t)(e->port & 0xFF); buf[off++] = (uint8_t)((e->port >> 8) & 0xFF);
        buf[off++] = e->type; buf[off++] = e->socket_id; buf[off++] = e->protocol; e = e->next;
    }}
    { struct TOPO_SOCKMETA6* e = ni->v6_sock_meta; while (e) {
        if (off + sizeof(struct TOPOMSG_SOCKMETA6) > buf_size || !address_profile_valid(&e->address_profile)) {
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "invalid/oversized v6 socket profile"); return -1;
        }
        sockmeta_wire_write(buf + off, e->id, e->config_type, &e->address_profile);
        off += sizeof(struct TOPOMSG_SOCKMETA6); e = e->next;
    }}
    { struct TOPO_ADDR6* e = ni->v6_addrs; while (e) {
        if (off + 21 > buf_size) return -1;
        memcpy(buf + off, e->addr, 16); off += 16;
        buf[off++] = (uint8_t)(e->port & 0xFF); buf[off++] = (uint8_t)((e->port >> 8) & 0xFF);
        buf[off++] = e->type; buf[off++] = e->socket_id; buf[off++] = e->protocol; e = e->next;
    }}
    { struct TOPO_REALITY_SOCK* e = ni->reality_socks; while (e) {
        size_t nlen = strnlen(e->server_name, TOPO_REALITY_SNAME_MAX);
        if (off + 1 + 8 + 32 + 3 + nlen + 1 > buf_size) return -1;
        buf[off++] = e->socket_id;
        memcpy(buf + off, e->short_id, 8); off += 8;
        memcpy(buf + off, e->server_pubkey, 32); off += 32;
        memcpy(buf + off, e->version, 3); off += 3;
        if (nlen) { memcpy(buf + off, e->server_name, nlen); off += nlen; }
        buf[off++] = '\0';
        e = e->next;
    }}

    uint8_t dg[DEVGROUP_WIRE_MAX];
    int bytes = devgroup_encode(&ni->devgroup, dg, sizeof(dg));
    if (bytes < 0 || off + 2 + (size_t)bytes > buf_size) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "devgroup: signature buffer too small"); return -1;
    }
    buf[off++] = (uint8_t)bytes; buf[off++] = (uint8_t)(bytes >> 8);
    memcpy(buf + off, dg, (size_t)bytes); off += (size_t)bytes;
    return (int)off;
}

struct topo_write_job {
    struct TOPO_NODE node;          /* Самостоятельные списки/имя; ни одного указателя на пулы или registry. */
    char name[64], channel[65];
    uint64_t now;
    int snapshot, own;
    struct db_changes_origin source; /* Отправитель NODEINFO для записи обнаруженного участника. */
    struct UTUN_INSTANCE* inst;    /* Только guard/done в uasync; instance живёт до drain DB. */
    int* submit_result;            /* Только синхронный отказ до возврата db_async_run. */
    int* submit_finished;          /* Не обращаться к job после синхронного callback/cleanup, в том числе успешного no-op. */
    struct TOPO_NODE* runtime;     /* Только uasync: заранее выделенная pool-копия для postcommit registry update. */
    int superseded;
    int status_update;               /* Смена флагов требует новой версии даже при прежней идентичности. */
    uint8_t phone_status;           /* Значение, выбранное в uasync перед dispatch. */
};

static int topo_identity_equal(const struct TOPO_NODE* a, const struct TOPO_NODE* b) {
    return !strcmp(a->node_name ? a->node_name : "", b->node_name ? b->node_name : "") &&
        !strcmp(a->connection_order, b->connection_order) && a->client_type == b->client_type && a->client_activity == b->client_activity &&
        !memcmp(a->public_key, b->public_key, 32) && !memcmp(a->ed25519_public_key, b->ed25519_public_key, 32) &&
        !memcmp(&a->devgroup, &b->devgroup, sizeof(a->devgroup)) && sockmeta4_list_equal(a->v4_sock_meta, b->v4_sock_meta) &&
        addr4_list_equal(a->v4_addrs, b->v4_addrs) && sockmeta6_list_equal(a->v6_sock_meta, b->v6_sock_meta) &&
        addr6_list_equal(a->v6_addrs, b->v6_addrs) && reality_sock_list_equal(a->reality_socks, b->reality_socks);
}

struct TOPO_NODE* topo_node_clone(struct TOPO_GROUPS* groups, const struct TOPO_NODE* node) {
    if (!groups || !groups->instance || !node) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node clone requires topology and signed node"); return NULL; }
    uint8_t wire[TOPO_NODE_WIRE_MAX_SIZE]; struct TOPO_GROUP_NODE route = {0};
    int size = topo_node_serialize((struct TOPO_NODE*)node, &route, 0, wire, sizeof(wire), 0);
    struct TOPO_NODE* copy = NULL; struct TOPO_GROUP group = {.instance=groups->instance};
    if (size < 0 || topo_node_deserialize(&group, wire, (size_t)size, &copy, NULL, NULL, NULL, NULL) || topo_node_verify(copy)) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "cannot prepare signed node runtime copy node=%016llx", (unsigned long long)node->node_id);
        topo_node_destroy(groups, copy); return NULL;
    }
    return copy;
}

static void* topo_copy_list(const void* source, size_t bytes) {
    struct _topo_head* first = NULL; struct _topo_head** tail = &first; unsigned count = 0;
    while (source) {
        if (++count > UINT8_MAX) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "frozen NODEINFO list exceeds wire limit"); break; }
        struct _topo_head* item = u_malloc(bytes);
        if (!item) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "frozen NODEINFO list allocation failed bytes=%zu", bytes); break; }
        memcpy(item, source, bytes); source = item->next; item->next = NULL; *tail = item; tail = &item->next;
    }
    if (!source) return first;
    while (first) { struct _topo_head* next = first->next; u_free(first); first = next; }
    return NULL;
}

void topo_node_frozen_free(void* data) {
    struct topo_write_job* job = data;
    if (!job) return;
    if (job->runtime) topo_node_destroy(job->inst->topo_groups, job->runtime);
    struct _topo_head* lists[] = {(void*)job->node.v4_sock_meta, (void*)job->node.v4_addrs,
        (void*)job->node.v6_sock_meta, (void*)job->node.v6_addrs, (void*)job->node.reality_socks};
    for (unsigned i = 0; i < sizeof(lists) / sizeof(*lists); i++)
        while (lists[i]) { struct _topo_head* next = lists[i]->next; u_free(lists[i]); lists[i] = next; }
    u_free(job);
}

/* Заполнить пустой job собственной копией; частично созданные списки освобождает cleanup. */
static int topo_freeze_into(struct topo_write_job* job, const struct TOPO_NODE* node) {
    job->node = *node; memset(&job->node.ll, 0, sizeof(job->node.ll)); job->node.group_ref_count = 0;
    job->node.v4_sock_meta = NULL; job->node.v4_addrs = NULL; job->node.v6_sock_meta = NULL;
    job->node.v6_addrs = NULL; job->node.reality_socks = NULL;
    snprintf(job->name, sizeof(job->name), "%s", node->node_name ? node->node_name : ""); job->node.node_name = job->name;
    if (node->v4_sock_meta && !(job->node.v4_sock_meta = topo_copy_list(node->v4_sock_meta, sizeof(*node->v4_sock_meta)))) goto failed;
    if (node->v4_addrs && !(job->node.v4_addrs = topo_copy_list(node->v4_addrs, sizeof(*node->v4_addrs)))) goto failed;
    if (node->v6_sock_meta && !(job->node.v6_sock_meta = topo_copy_list(node->v6_sock_meta, sizeof(*node->v6_sock_meta)))) goto failed;
    if (node->v6_addrs && !(job->node.v6_addrs = topo_copy_list(node->v6_addrs, sizeof(*node->v6_addrs)))) goto failed;
    if (node->reality_socks && !(job->node.reality_socks = topo_copy_list(node->reality_socks, sizeof(*node->reality_socks)))) goto failed;
    return 0;
failed:
    DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node persistence freeze failed node=%016llx", (unsigned long long)node->node_id);
    return -1;
}

/* Телефон общий для instance, подписка рации независима в каждой CHAT-группе. */
static int topo_chat_status_changed(struct UTUN_INSTANCE* inst, uint8_t phone_status) {
    if (phone_status != inst->published_phone_status) return 1;
    struct ll_queue* groups = inst->topo_groups->group_list;
    for (struct ll_entry* e = groups ? groups->head : NULL; e; e = e->next) {
        struct TOPO_GROUP* group = (struct TOPO_GROUP*)e;
        if (!group->stopping && group->local_node && group->group_type == TOPO_GROUP_TYPE_CHAT &&
            group->radio_active != group->local_node->radio) return 1;
    }
    return 0;
}

static int topo_write_guard(void* data) {
    struct topo_write_job* job = data;
    struct UTUN_INSTANCE* inst = job->inst;
    if (inst->destroying) return 0;
    struct TOPO_NODE* current = job->own && job->snapshot ? topo_node_registry_lookup(inst->topo_groups, inst->node_id, inst->my_keys.public_key) : NULL;
    if (job->status_update) {
        if (!current) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "CHAT status publication requires local identity"); return 0; }
        job->phone_status = call_phone_status(inst);
        if (!topo_chat_status_changed(inst, job->phone_status)) { job->superseded = 1; return 0; }
        if (topo_freeze_into(job, current)) return 0;
    }
    if (!current) return 1; /* Самостоятельный SQL fixture; production self уже установлен bootstrap. */
    struct TOPO_NODE* node = &job->node;
    int renamed = strcmp(job->name, inst->name);
    snprintf(job->name, sizeof(job->name), "%s", inst->name);
    node->client_type = inst->client_type; node->client_activity = inst->client_activity;
    if (inst->local_devgroup.ready) node->devgroup = inst->local_devgroup.record;
    snprintf(node->connection_order, sizeof(node->connection_order), "%s", inst->config && inst->config->global.connection_order[0] ?
        inst->config->global.connection_order : CONNECTION_ORDER_DEFAULT);
    if (!job->status_update && topo_identity_equal(current, node)) { job->superseded = 1; return 0; }
    if (renamed || node->timestamp <= inst->local_snapshot_committed) {
        uint64_t previous = inst->local_snapshot_reserved > current->timestamp ? inst->local_snapshot_reserved : current->timestamp;
        if (previous >= INT64_MAX) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "local node dispatch timestamp exhausted"); return 0; }
        int64_t now = ntp_time_get_us(inst); node->timestamp = now > 0 && (uint64_t)now > previous ? (uint64_t)now : previous + 1;
    }
    node->ver = (current->ver % 255) + 1;
    uint8_t message[TOPO_SIG_MSG_MAX_SIZE]; int bytes = topo_node_build_sig_msg(node, message, sizeof(message));
    if (bytes <= 0 || sc_ed25519_sign(inst->my_ed25519_privkey, message, (size_t)bytes, node->x25519_self_sig) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "local node dispatch signature failed"); return 0;
    }
    if (inst->local_snapshot_reserved < node->timestamp) inst->local_snapshot_reserved = node->timestamp;
    job->runtime = topo_node_clone(inst->topo_groups, node);
    return job->runtime != NULL;
}

static int topo_write_work(sqlite3* db, void* data) {
    struct topo_write_job* job = data; struct TOPO_NODE* node = &job->node; int rc = SQLITE_OK;
    if (job->snapshot) {
        if (job->own) rc = topo_node_sqlite_node_write(db, node->node_id, node->node_name,
            node->public_key, node->ed25519_public_key, job->now, (time_t)job->now);
        if (!rc) rc = topo_node_sqlite_snapshot_write(db, node);
        if (!rc && job->channel[0]) {
            struct db_changes_origin source = db_changes_source(db, job->source.ed, job->source.node, "BGP NODEINFO");
            rc = topo_node_sqlite_member_placeholder_write(db, job->channel, node->node_id, node->public_key, node->ed25519_public_key);
            db_changes_restore(db, source);
        }
        uint8_t (*channels)[32] = NULL; int count = 0;
        if (!rc && !job->own) rc = topo_node_sqlite_member_channels_read(db, node->ed25519_public_key, &channels, &count);
        u_free(channels);
        if (!rc && (job->own || count)) rc = topo_node_sqlite_addrs_write(db, node->node_id, node);
    }
    if (!rc) rc = topo_node_sqlite_nodeinfo_write(db, node->public_key);
    return rc;
}

static void topo_announce_self(struct UTUN_INSTANCE* inst, const struct TOPO_NODE* stored) {
    for (struct ll_entry* ge = inst->topo_groups->group_list ? inst->topo_groups->group_list->head : NULL; ge; ge = ge->next) {
        struct TOPO_GROUP* group = (struct TOPO_GROUP*)ge;
        if (!group->local_node || group->stopping) continue;
        int fresh = stored->timestamp > group->local_node->last_timestamp;
        group->local_node->last_ver = stored->ver; group->local_node->last_timestamp = stored->timestamp;
        if (group->group_type == TOPO_GROUP_TYPE_CHAT && fresh &&
            (group->local_node->radio != group->radio_active || group->local_node->phone_status != inst->published_phone_status)) {
            DEBUG_INFO(DEBUG_CATEGORY_BGP, "CHAT status node=%016llx group=%016llx radio=%u->%u phone=%u->%u timestamp=%llu",
                (unsigned long long)inst->node_id, (unsigned long long)group->group_id, group->local_node->radio, group->radio_active,
                group->local_node->phone_status, inst->published_phone_status, (unsigned long long)stored->timestamp);
            group->local_node->radio = group->radio_active;
            group->local_node->phone_status = inst->published_phone_status;
            for (struct topo_node_cbk_entry* cb = group->node_cbks; cb; cb = cb->next)
                cb->fn(group, inst->my_ed25519_pubkey, TOPO_NODE_EVENT_UPDATE, cb->arg);
        }
        for (struct ll_entry* se = group->senders_list ? group->senders_list->head : NULL; se; se = se->next) {
            struct TOPO_GROUP_CONN_ITEM* peer = (struct TOPO_GROUP_CONN_ITEM*)se->data;
            if (peer->conn) topo_group_send_nodeinfo(group, group->local_node, peer->conn);
        }
    }
}

int topo_node_publish_self(struct UTUN_INSTANCE* inst, struct TOPO_NODE* prepared) {
    if (!inst || inst->destroying || !inst->topo_groups || !prepared || prepared->node_id != inst->node_id ||
        !topo_node_registry_lookup(inst->topo_groups, inst->node_id, inst->my_keys.public_key)) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "self publication requires live core and prepared own node"); return -1;
    }
    struct TOPO_NODE* stored = topo_node_registry_put(inst->topo_groups, prepared);
    if (!stored) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "cannot publish prepared local snapshot after disk commit"); return -1; }
    if (inst->local_snapshot_committed < stored->timestamp) inst->local_snapshot_committed = stored->timestamp;
    if (inst->local_snapshot_reserved < stored->timestamp) inst->local_snapshot_reserved = stored->timestamp;
    topo_node_registry_release(inst->topo_groups, stored); /* Отпустить временную ссылку registry_put, core ref остаётся. */
    topo_announce_self(inst, stored);
    return 0;
}

static void topo_write_done(void* data, const struct db_async_result* result) {
    struct topo_write_job* job = data; struct UTUN_INSTANCE* inst = job->inst;
    if (job->submit_result) *job->submit_result = result->status ? -1 : 0;
    if (job->submit_finished) *job->submit_finished = 1;
    if (job->status_update) inst->status_publish_pending = 0;
    if (job->superseded && result->status == DB_ASYNC_CANCELLED) {
        if (job->submit_result) *job->submit_result = 0;
        DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "node snapshot already committed; queued duplicate skipped"); return;
    }
    if (result->status) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node persistence failed node=%016llx snapshot=%d timestamp=%llu status=%d rc=%d: %s",
            (unsigned long long)job->node.node_id, job->snapshot, (unsigned long long)job->node.timestamp,
            result->status, result->sqlite_code, result->message); return;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "node persistence committed node=%016llx snapshot=%d timestamp=%llu",
        (unsigned long long)job->node.node_id, job->snapshot, (unsigned long long)job->node.timestamp);
    if (!job->snapshot || !job->own || inst->destroying || !inst->topo_groups) return;
    if (job->runtime) {
        uint8_t previous = inst->published_phone_status;
        if (job->status_update) inst->published_phone_status = job->phone_status;
        if (topo_node_publish_self(inst, job->runtime)) { inst->published_phone_status = previous; return; }
        job->runtime = NULL;
        if (job->status_update) topo_node_update_chat_status(inst);
        return;
    }
    if (inst->local_snapshot_committed < job->node.timestamp) inst->local_snapshot_committed = job->node.timestamp;
}

static int topo_write_submit(struct topo_write_job* job) {
    struct UTUN_INSTANCE* inst = job->inst;
    if (!db_async_can_write(inst->db_worker)) {
        sqlite3* db = inst->topo_sqlite_db;
        const char* file = db ? sqlite3_db_filename(db, "main") : NULL;
        if (inst->db_worker || !db || (file && *file)) {
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node persistence requires writable database worker node=%016llx",
                (unsigned long long)job->node.node_id); topo_node_frozen_free(job); return -1;
        }
        struct db_async_result result = {.sqlite_code=topo_write_work(db, job)};
        result.status = result.sqlite_code ? DB_ASYNC_SQL : DB_ASYNC_OK;
        topo_write_done(job, &result); topo_node_frozen_free(job); return result.status ? -1 : 0;
    }
    int result = 0, finished = 0; job->submit_result = &result; job->submit_finished = &finished;
    db_async_run(inst->db_worker, &(struct db_async_task){.name="persist-node", .mode=DB_ASYNC_WRITE,
        .work=topo_write_work, .done=topo_write_done, .guard=topo_write_guard, .cleanup=topo_node_frozen_free, .data=job});
    if (!finished) { job->submit_result = NULL; job->submit_finished = NULL; }
    return result;
}

struct TOPO_NODE* topo_node_freeze(const struct TOPO_NODE* node) {
    if (!node || !node->node_id || (node->node_name && strlen(node->node_name) > 63)) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node freeze: invalid node/name"); return NULL;
    }
    struct topo_write_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node persistence: cannot reserve snapshot"); return NULL; }
    if (topo_freeze_into(job, node)) { topo_node_frozen_free(job); return NULL; }
    return &job->node;
}

int topo_node_persist(struct UTUN_INSTANCE* inst, const struct TOPO_NODE* node, const char* channel, const struct ETCP_CONN* from) {
    if (!inst || inst->destroying || (channel && !chat_channel_valid(channel))) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node persistence: invalid instance/channel"); return -1;
    }
    struct topo_write_job* job = (void*)topo_node_freeze(node); if (!job) return -1;
    job->inst = inst; job->snapshot = 1; job->own = !memcmp(node->public_key, inst->my_keys.public_key, 32); job->now = ntp_time_get_seconds(inst);
    if (from) { job->source.node = from->peer_node_id; memcpy(job->source.ed, from->peer_ed25519_pubkey, 32); }
    snprintf(job->channel, sizeof(job->channel), "%s", channel ? channel : ""); return topo_write_submit(job);
}

int topo_node_mark_dirty(struct UTUN_INSTANCE* inst, const uint8_t x[32]) {
    if (!inst || inst->destroying || !x) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node classification: invalid instance/node"); return -1; }
    struct topo_write_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node classification allocation failed node_x=%s", sc_public_key_hex(x, (char[65]){0})); return -1; }
    job->inst = inst; job->node.node_id = sc_derive_node_id_from_pubkey(x); memcpy(job->node.public_key, x, 32); return topo_write_submit(job);
}

/* Подписывает свой узел Ed25519: build_sig_msg + sc_ed25519_sign → ni->x25519_self_sig. */
int topo_node_sign_self(struct UTUN_INSTANCE* instance, struct TOPO_NODE* ni) {
    if (!instance || !ni) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "invalid signing arguments"); return -1; }
    char old_order[CONNECTION_ORDER_MAX]; memcpy(old_order, ni->connection_order, sizeof(old_order));
    uint64_t old_timestamp = ni->timestamp;
    uint8_t old_sig[64]; memcpy(old_sig, ni->x25519_self_sig, sizeof(old_sig));
    uint64_t previous = ni->timestamp;
    int asynchronous = db_async_can_write(instance->db_worker);
    if (asynchronous && topo_node_registry_find(instance->topo_groups, ni->node_id) == ni) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "RW self signing requires a detached candidate, not the live registry node"); return -1;
    }
    if (asynchronous && instance->local_snapshot_reserved > previous) previous = instance->local_snapshot_reserved;
    if (!asynchronous && instance->topo_sqlite_db) {
        uint64_t saved;
        if (topo_node_sqlite_snapshot_time_read(instance->topo_sqlite_db, ni->node_id, ni->public_key, &saved)) return -1;
        if (saved > previous) previous = saved;
    }
    int64_t now = ntp_time_get_us(instance);
    if (previous >= INT64_MAX) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node timestamp exhausted"); return -1; }
    ni->timestamp = now > 0 && (uint64_t)now > previous ? (uint64_t)now : previous + 1;
    uint8_t sig_msg[TOPO_SIG_MSG_MAX_SIZE];
    snprintf(ni->connection_order, sizeof(ni->connection_order), "%s",
             instance->config && instance->config->global.connection_order[0] ?
             instance->config->global.connection_order : CONNECTION_ORDER_DEFAULT);
    struct TOPO_NODE* current = asynchronous ? topo_node_registry_find(instance->topo_groups, ni->node_id) : NULL;
    if (current && topo_identity_equal(current, ni)) {
        ni->timestamp = current->timestamp; ni->ver = current->ver;
        memcpy(ni->x25519_self_sig, current->x25519_self_sig, 64); return 0;
    }
    int sig_len = topo_node_build_sig_msg(ni, sig_msg, sizeof(sig_msg));
    if (sig_len <= 0 || sc_ed25519_sign(instance->my_ed25519_privkey, sig_msg, (size_t)sig_len, ni->x25519_self_sig) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "Ed25519 self-sign FAILED for node=%016llx", (unsigned long long)ni->node_id);
        memcpy(ni->connection_order, old_order, sizeof(old_order));
        ni->timestamp = old_timestamp;
        memcpy(ni->x25519_self_sig, old_sig, sizeof(old_sig));
        return -1;
    }
    if ((asynchronous && topo_node_persist(instance, ni, NULL, NULL) < 0) ||
        (!asynchronous && instance->topo_sqlite_db && topo_node_sqlite_snapshot_put(instance->topo_sqlite_db, ni) < 0)) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "cannot persist local node snapshot node=%016llx", (unsigned long long)ni->node_id);
        memcpy(ni->connection_order, old_order, sizeof(old_order));
        ni->timestamp = old_timestamp;
        memcpy(ni->x25519_self_sig, old_sig, sizeof(old_sig));
        return -1;
    }
    if (instance->local_snapshot_reserved < ni->timestamp) instance->local_snapshot_reserved = ni->timestamp;
    if (!asynchronous) instance->local_snapshot_committed = ni->timestamp;
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "node signed: node=%016llx timestamp=%llu",
                (unsigned long long)ni->node_id, (unsigned long long)ni->timestamp);
    return 0;
}

void topo_node_update_chat_status(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->topo_groups || inst->destroying || inst->status_publish_pending) return;
    uint8_t status = call_phone_status(inst);
    if (!topo_chat_status_changed(inst, status)) return;
    struct TOPO_NODE* self = topo_node_registry_lookup(inst->topo_groups, inst->node_id, inst->my_keys.public_key);
    if (!self) { DEBUG_WARN(DEBUG_CATEGORY_BGP, "CHAT status publication deferred: local identity unavailable"); return; }
    if (db_async_can_write(inst->db_worker)) {
        struct topo_write_job* job = u_calloc(1, sizeof(*job));
        if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "CHAT status publication allocation failed"); return; }
        job->inst = inst; job->snapshot = job->own = job->status_update = 1; job->node.node_id = inst->node_id;
        job->now = ntp_time_get_seconds(inst); inst->status_publish_pending = 1;
        topo_write_submit(job);  /* done снимает pending и проверяет изменения за время записи. */
        return;
    }
    struct TOPO_NODE candidate = *self;
    candidate.ver = (self->ver % 255) + 1;
    if (topo_node_sign_self(inst, &candidate)) return;
    self->timestamp = candidate.timestamp; self->ver = candidate.ver;
    memcpy(self->connection_order, candidate.connection_order, sizeof(self->connection_order));
    memcpy(self->x25519_self_sig, candidate.x25519_self_sig, sizeof(self->x25519_self_sig));
    inst->published_phone_status = status;
    topo_announce_self(inst, self);
}

int topo_node_verify(const struct TOPO_NODE* ni) {
    uint8_t msg[TOPO_SIG_MSG_MAX_SIZE];
    if (!ni || !ni->timestamp || ni->timestamp > INT64_MAX || ni->node_id != sc_derive_node_id_from_pubkey(ni->public_key)) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "invalid signed node identity or timestamp"); return -1;
    }
    if (sc_identity_check("BGP NODEINFO", ni->public_key, ni->ed25519_public_key) != SC_OK) return -1;
    if (devgroup_verify(&ni->devgroup, ni->public_key, ni->ed25519_public_key)) return -1;
    int len = topo_node_build_sig_msg((struct TOPO_NODE*)ni, msg, sizeof(msg));
    if (len <= 0 || sc_ed25519_verify(ni->ed25519_public_key, msg, (size_t)len, ni->x25519_self_sig) != SC_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "node signature rejected: node=%016llx timestamp=%llu",
                   (unsigned long long)ni->node_id, (unsigned long long)ni->timestamp); return -1;
    }
    return 0;
}

/* Вычисляет суммарный размер динамической части NODEINFO по счётчикам заголовка. */
int topo_node_dyn_size(const struct TOPOMSG_NODE* msg) {
    if (!msg) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "NODEINFO size: missing header"); return -1; }
    if (!(msg->flags & TOPO_FLAG_SEND_SUBNETS) && (msg->local_v4_subnets || msg->local_v6_subnets)) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "NODEINFO size: subnets without flag node=%s flags=%02x v4=%u v6=%u",
                   sc_public_key_hex(msg->ed25519_public_key, (char[65]){0}), msg->flags, msg->local_v4_subnets, msg->local_v6_subnets);
        return -1;
    }
    if (msg->devgroup_size && (msg->devgroup_size <= 192 || msg->devgroup_size > DEVGROUP_WIRE_MAX)) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "devgroup: invalid NODEINFO block size=%u", msg->devgroup_size); return -1;
    }
    return   msg->connection_order_len + msg->devgroup_size + msg->node_name_len
           + msg->local_v4_sockets * sizeof(struct TOPOMSG_SOCKMETA4)
           + msg->local_v4_addrs   * sizeof(struct TOPOMSG_ADDR4)
           + msg->local_v6_sockets * sizeof(struct TOPOMSG_SOCKMETA6)
           + msg->local_v6_addrs   * sizeof(struct TOPOMSG_ADDR6)
           + msg->reality_size
           + msg->local_v4_subnets * sizeof(struct TOPOMSG_SUBNET4)
           + msg->local_v6_subnets * sizeof(struct TOPOMSG_SUBNET6)
           + msg->hop_count * SC_PUBKEY_SIZE;
}

/* Сериализует узел в wire-формат NODEINFO (заголовок + имя + адреса + подсети + hop_list). */
int topo_node_serialize(struct TOPO_NODE* ni, struct TOPO_GROUP_NODE* nq, uint8_t flags,
                        uint8_t* out, size_t out_max, uint16_t cumulative_rtt) {
    if (!ni || !nq || !out) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "NODEINFO encode: missing node/group/output"); return -1; }

    uint8_t dg[DEVGROUP_WIRE_MAX];
    int dg_size = devgroup_encode(&ni->devgroup, dg, sizeof(dg));
    if (dg_size < 0) return -1;
    struct TOPOMSG_NODE msg;
    memset(&msg, 0, sizeof(msg));
    msg.connection_order_len = (uint8_t)strlen(ni->connection_order);
    msg.devgroup_size = (uint16_t)dg_size;
    msg.flags           = flags;
    msg.ver             = ni->ver;
    msg.timestamp       = ni->timestamp;
    memcpy(msg.public_key, ni->public_key, SC_PUBKEY_SIZE);
    memcpy(msg.ed25519_public_key, ni->ed25519_public_key, SC_PUBKEY_SIZE);
    memcpy(msg.x25519_self_sig, ni->x25519_self_sig, 64);
    msg.node_name_len   = ni->node_name ? (uint8_t)strlen(ni->node_name) : 0;
    if ((ni->node_name && strlen(ni->node_name) > 63) ||
        topo_list_count((struct _topo_head*)ni->v4_sock_meta) > UINT8_MAX ||
        topo_list_count((struct _topo_head*)ni->v4_addrs) > UINT8_MAX ||
        topo_list_count((struct _topo_head*)ni->v6_sock_meta) > UINT8_MAX ||
        topo_list_count((struct _topo_head*)ni->v6_addrs) > UINT8_MAX ||
        topo_list_count((struct _topo_head*)ni->reality_socks) > UINT8_MAX ||
        ((flags & TOPO_FLAG_SEND_SUBNETS) && nq->subnets &&
                        (topo_list_count((struct _topo_head*)nq->subnets->v4_subnets) > UINT8_MAX ||
                         topo_list_count((struct _topo_head*)nq->subnets->v6_subnets) > UINT8_MAX))) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node exceeds wire format limits"); return -1;
    }
    msg.local_v4_sockets = topo_list_count((struct _topo_head*)ni->v4_sock_meta);
    msg.local_v4_addrs   = topo_list_count((struct _topo_head*)ni->v4_addrs);
    msg.local_v6_sockets = topo_list_count((struct _topo_head*)ni->v6_sock_meta);
    msg.local_v6_addrs   = topo_list_count((struct _topo_head*)ni->v6_addrs);
    msg.local_reality_socks = topo_list_count((struct _topo_head*)ni->reality_socks);
    size_t reality_size = 0;
    for (struct TOPO_REALITY_SOCK* e = ni->reality_socks; e; e = e->next) {
        size_t n = strnlen(e->server_name, sizeof(e->server_name));
        if (n >= sizeof(e->server_name)) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "REALITY name unterminated"); return -1; }
        reality_size += sizeof(struct TOPOMSG_REALITY_SOCK) + n;
    }
    if (reality_size > UINT16_MAX) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "REALITY section too large"); return -1; }
    msg.reality_size = (uint16_t)reality_size;
    if ((flags & TOPO_FLAG_SEND_SUBNETS) && nq->subnets) {
        msg.local_v4_subnets = topo_list_count((struct _topo_head*)nq->subnets->v4_subnets);
        msg.local_v6_subnets = topo_list_count((struct _topo_head*)nq->subnets->v6_subnets);
    }
    const struct topo_hop* hop_list = topo_node_best_hop_list(nq, &msg.hop_count, NULL);
    msg.client_type     = ni->client_type;
    msg.client_activity = ni->client_activity;
    msg.cumulative_rtt   = cumulative_rtt;
    if (nq->radio) msg.flags |= TOPO_FLAG_RADIO;
    msg.flags |= nq->phone_status;

    int dyn = topo_node_dyn_size(&msg);
    if (dyn < 0) return -1; // Причина уже записана в topo_node_dyn_size.
    size_t total = TOPOMSG_NODE_HDR_SIZE + (size_t)dyn;
    if (total > out_max) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "NODEINFO encode: output too small node=%016llx need=%zu capacity=%zu",
                   (unsigned long long)ni->node_id, total, out_max);
        return -1;
    }

    memcpy(out, &msg, TOPOMSG_NODE_HDR_SIZE);
    uint8_t* dp = out + TOPOMSG_NODE_HDR_SIZE;

    if (msg.node_name_len) { memcpy(dp, ni->node_name, msg.node_name_len); dp += msg.node_name_len; }
    memcpy(dp, ni->connection_order, msg.connection_order_len); dp += msg.connection_order_len;
    memcpy(dp, dg, (size_t)dg_size); dp += dg_size;

    { struct TOPO_SOCKMETA4* e = ni->v4_sock_meta; while (e) {
        sockmeta_wire_write(dp, e->id, e->config_type, &e->address_profile);
        dp += sizeof(struct TOPOMSG_SOCKMETA4); e = e->next;
    }}
    { struct TOPO_ADDR4* e = ni->v4_addrs; while (e) {
        struct TOPOMSG_ADDR4 w = { .port=e->port, .type=e->type, .socket_id=e->socket_id, .protocol=e->protocol };
        memcpy(w.addr, e->addr, 4); memcpy(dp, &w, sizeof(w)); dp += sizeof(w); e = e->next;
    }}
    { struct TOPO_SOCKMETA6* e = ni->v6_sock_meta; while (e) {
        sockmeta_wire_write(dp, e->id, e->config_type, &e->address_profile);
        dp += sizeof(struct TOPOMSG_SOCKMETA6); e = e->next;
    }}
    { struct TOPO_ADDR6* e = ni->v6_addrs; while (e) {
        struct TOPOMSG_ADDR6 w = { .port=e->port, .type=e->type, .socket_id=e->socket_id, .protocol=e->protocol };
        memcpy(w.addr, e->addr, 16); memcpy(dp, &w, sizeof(w)); dp += sizeof(w); e = e->next;
    }}
    { struct TOPO_REALITY_SOCK* e = ni->reality_socks; while (e) {
        struct TOPOMSG_REALITY_SOCK w; memset(&w, 0, sizeof(w));
        w.socket_id = e->socket_id;
        memcpy(w.short_id, e->short_id, 8);
        memcpy(w.server_pubkey, e->server_pubkey, 32);
        memcpy(w.version, e->version, 3);
        w.server_name_len = (uint8_t)strlen(e->server_name);
        memcpy(dp, &w, sizeof(w)); dp += sizeof(w);
        memcpy(dp, e->server_name, w.server_name_len); dp += w.server_name_len; e = e->next;
    }}
    if (msg.flags & TOPO_FLAG_SEND_SUBNETS) {
        if (nq->subnets) {
            { struct TOPO_SUBNET4* e = nq->subnets->v4_subnets; while (e) {
                struct TOPOMSG_SUBNET4 w = { .prefix_length=e->prefix_length };
                memcpy(w.addr, e->addr, 4); memcpy(dp, &w, sizeof(w)); dp += sizeof(w); e = e->next;
            }}
            { struct TOPO_SUBNET6* e = nq->subnets->v6_subnets; while (e) {
                struct TOPOMSG_SUBNET6 w = { .prefix_length=e->prefix_length };
                memcpy(w.addr, e->addr, 16); memcpy(dp, &w, sizeof(w)); dp += sizeof(w); e = e->next;
            }}
        }
    }
    for (uint8_t i = 0; i < msg.hop_count; i++, dp += SC_PUBKEY_SIZE) memcpy(dp, hop_list[i].ed, SC_PUBKEY_SIZE);
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "NODEINFO encoded: node=%s bytes=%zu flags=%02x subnets=%u/%u hops=%u",
                sc_public_key_hex(ni->ed25519_public_key, (char[65]){0}),
                total, msg.flags, msg.local_v4_subnets, msg.local_v6_subnets, msg.hop_count);
    return (int)(dp - out);
}

// ===== deserialization =====

/* Разбирает wire-формат NODEINFO в TOPO_NODE + подсети/hop_list/cumulative_rtt (память через пулы). */
int topo_node_deserialize(struct TOPO_GROUP* group, const uint8_t* data, size_t len, struct TOPO_NODE** out_ni, struct TOPO_NODESUBNETS** out_subnets,
                          struct topo_hop** out_hop_list, uint8_t* out_hop_count,
                          uint16_t* out_cumulative_rtt) {
    if (out_ni) *out_ni = NULL;
    if (out_subnets) *out_subnets = NULL;
    if (out_hop_list) *out_hop_list = NULL;
    if (out_hop_count) *out_hop_count = 0;
    if (out_cumulative_rtt) *out_cumulative_rtt = 0;
    if (!group || !group->instance || !group->instance->topo_groups || !data || !out_ni) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "NODEINFO decode: missing group/instance/registry/data/output");
        return -1;
    }
    DEBUG_TRACE(DEBUG_CATEGORY_BGP, "NODEINFO decode: length=%zu", len);
    if (len < TOPOMSG_NODE_HDR_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "NODEINFO decode: short header length=%zu need=%zu", len, TOPOMSG_NODE_HDR_SIZE);
        return -1;
    }
    const struct TOPOMSG_NODE* msg = (const struct TOPOMSG_NODE*)data;
    int dyn = topo_node_dyn_size(msg);
    if (dyn < 0) return -1; // Причина уже записана в topo_node_dyn_size.
    if ((size_t)dyn != len - TOPOMSG_NODE_HDR_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "NODEINFO decode: size mismatch node=%s length=%zu expected=%zu flags=%02x subnets=%u/%u hops=%u",
                   sc_public_key_hex(msg->ed25519_public_key, (char[65]){0}), len, TOPOMSG_NODE_HDR_SIZE + (size_t)dyn,
                   msg->flags, msg->local_v4_subnets, msg->local_v6_subnets, msg->hop_count);
        return -1;
    }

    struct TOPO_GROUPS* groups = group->instance->topo_groups;
    struct TOPO_NODE* ni = u_calloc(1, sizeof(*ni));
    struct TOPO_NODESUBNETS* subnets = NULL;
    struct topo_hop* hops = NULL;
    const uint8_t* dp = data + TOPOMSG_NODE_HDR_SIZE;
    const char* section = "node";
    unsigned index = 0;
    if (!ni) goto no_memory;
    ni->node_id = sc_derive_node_id_from_pubkey(msg->public_key); ni->ver = msg->ver; ni->timestamp = msg->timestamp;
    memcpy(ni->public_key, msg->public_key, SC_PUBKEY_SIZE);
    memcpy(ni->ed25519_public_key, msg->ed25519_public_key, SC_PUBKEY_SIZE);
    memcpy(ni->x25519_self_sig, msg->x25519_self_sig, 64);
    ni->client_type = msg->client_type; ni->client_activity = msg->client_activity;

    section = "name";
    if (msg->node_name_len) {
        ni->node_name = u_malloc(msg->node_name_len + 1);
        if (!ni->node_name) goto no_memory;
        memcpy(ni->node_name, dp, msg->node_name_len); ni->node_name[msg->node_name_len] = 0;
    }
    dp += msg->node_name_len;
    section = "connection policy";
    memcpy(ni->connection_order, dp, msg->connection_order_len);
    if (memchr(dp, 0, msg->connection_order_len) ||
        (msg->connection_order_len && connection_order_parse(ni->connection_order, NULL) < 0)) goto invalid_field;
    dp += msg->connection_order_len;
    section = "devgroup";
    if (devgroup_decode(&ni->devgroup, dp, msg->devgroup_size)) goto invalid_field;
    dp += msg->devgroup_size;

    /* Списки сразу принадлежат ni: общий выход ошибки видит и незавершённую секцию. */
    section = "v4 socket metadata";
    {   struct TOPO_SOCKMETA4** tail = &ni->v4_sock_meta;
        for (index = 0; index < msg->local_v4_sockets; index++) {
            const struct TOPOMSG_SOCKMETA4* w = (const struct TOPOMSG_SOCKMETA4*)dp; dp += sizeof(*w);
            struct TOPO_SOCKMETA4* e = memory_pool_alloc(groups->v4_sock_meta_pool);
            if (!e) goto no_memory;
            *tail = e; tail = &e->next;
            e->id = w->id; e->config_type = w->config_type;
            if (sockmeta_wire_read((const uint8_t*)w, &e->address_profile)) goto invalid_field;
        }
    }
    section = "v4 addresses";
    {   struct TOPO_ADDR4** tail = &ni->v4_addrs;
        for (index = 0; index < msg->local_v4_addrs; index++) {
            const struct TOPOMSG_ADDR4* w = (const struct TOPOMSG_ADDR4*)dp; dp += sizeof(*w);
            struct TOPO_ADDR4* e = memory_pool_alloc(groups->v4_addr_pool);
            if (!e) goto no_memory;
            *tail = e; tail = &e->next;
            memcpy(e->addr, w->addr, 4); e->port = w->port; e->type = w->type;
            e->socket_id = w->socket_id; e->protocol = w->protocol;
        }
    }
    section = "v6 socket metadata";
    {   struct TOPO_SOCKMETA6** tail = &ni->v6_sock_meta;
        for (index = 0; index < msg->local_v6_sockets; index++) {
            const struct TOPOMSG_SOCKMETA6* w = (const struct TOPOMSG_SOCKMETA6*)dp; dp += sizeof(*w);
            struct TOPO_SOCKMETA6* e = memory_pool_alloc(groups->v6_sock_meta_pool);
            if (!e) goto no_memory;
            *tail = e; tail = &e->next;
            e->id = w->id; e->config_type = w->config_type;
            if (sockmeta_wire_read((const uint8_t*)w, &e->address_profile)) goto invalid_field;
        }
    }
    section = "v6 addresses";
    {   struct TOPO_ADDR6** tail = &ni->v6_addrs;
        for (index = 0; index < msg->local_v6_addrs; index++) {
            const struct TOPOMSG_ADDR6* w = (const struct TOPOMSG_ADDR6*)dp; dp += sizeof(*w);
            struct TOPO_ADDR6* e = memory_pool_alloc(groups->v6_addr_pool);
            if (!e) goto no_memory;
            *tail = e; tail = &e->next;
            memcpy(e->addr, w->addr, 16); e->port = w->port; e->type = w->type;
            e->socket_id = w->socket_id; e->protocol = w->protocol;
        }
    }
    section = "REALITY sockets";
    {   struct TOPO_REALITY_SOCK** tail = &ni->reality_socks;
        const uint8_t* end = dp + msg->reality_size;
        for (index = 0; index < msg->local_reality_socks; index++) {
            if ((size_t)(end - dp) < sizeof(struct TOPOMSG_REALITY_SOCK)) goto invalid_field;
            const struct TOPOMSG_REALITY_SOCK* w = (const struct TOPOMSG_REALITY_SOCK*)dp; dp += sizeof(*w);
            if ((size_t)(end - dp) < w->server_name_len || memchr(dp, 0, w->server_name_len)) goto invalid_field;
            struct TOPO_REALITY_SOCK* e = u_calloc(1, sizeof(*e));
            if (!e) goto no_memory;
            *tail = e; tail = &e->next;
            e->socket_id = w->socket_id;
            memcpy(e->short_id, w->short_id, 8); memcpy(e->server_pubkey, w->server_pubkey, 32);
            memcpy(e->version, w->version, 3); memcpy(e->server_name, dp, w->server_name_len); dp += w->server_name_len;
        }
        if (dp != end) goto invalid_field;
    }

    section = "subnets"; index = 0;
    if (out_subnets && (msg->local_v4_subnets || msg->local_v6_subnets)) {
        subnets = u_calloc(1, sizeof(*subnets));
        if (!subnets) goto no_memory;
    }
    /* Курсор проходит все присутствующие секции; out_subnets управляет только выделением памяти. */
    section = "v4 subnets";
    {   struct TOPO_SUBNET4** tail = subnets ? &subnets->v4_subnets : NULL;
        for (index = 0; index < msg->local_v4_subnets; index++) {
            const struct TOPOMSG_SUBNET4* w = (const struct TOPOMSG_SUBNET4*)dp; dp += sizeof(*w);
            if (w->prefix_length > 32) {
                DEBUG_WARN(DEBUG_CATEGORY_BGP, "NODEINFO decode: invalid IPv4 prefix node=%s index=%u prefix=%u",
                           sc_public_key_hex(msg->ed25519_public_key, (char[65]){0}), index, w->prefix_length);
                goto fail;
            }
            if (!subnets) continue;
            struct TOPO_SUBNET4* e = memory_pool_alloc(groups->v4_subnet_pool);
            if (!e) goto no_memory;
            *tail = e; tail = &e->next;
            memcpy(e->addr, w->addr, 4); e->prefix_length = w->prefix_length;
        }
    }
    section = "v6 subnets";
    {   struct TOPO_SUBNET6** tail = subnets ? &subnets->v6_subnets : NULL;
        for (index = 0; index < msg->local_v6_subnets; index++) {
            const struct TOPOMSG_SUBNET6* w = (const struct TOPOMSG_SUBNET6*)dp; dp += sizeof(*w);
            if (w->prefix_length > 128) {
                DEBUG_WARN(DEBUG_CATEGORY_BGP, "NODEINFO decode: invalid IPv6 prefix node=%s index=%u prefix=%u",
                           sc_public_key_hex(msg->ed25519_public_key, (char[65]){0}), index, w->prefix_length);
                goto fail;
            }
            if (!subnets) continue;
            struct TOPO_SUBNET6* e = memory_pool_alloc(groups->v6_subnet_pool);
            if (!e) goto no_memory;
            *tail = e; tail = &e->next;
            memcpy(e->addr, w->addr, 16); e->prefix_length = w->prefix_length;
        }
    }

    section = "hops"; index = 0;
    if (msg->hop_count > MAX_HOPS) goto invalid_field;
    if (out_hop_list && msg->hop_count) {
        hops = u_malloc(msg->hop_count * sizeof(*hops));
        if (!hops) goto no_memory;
    }
    for (index = 0; index < msg->hop_count; index++, dp += SC_PUBKEY_SIZE) {
        uint8_t x[SC_PUBKEY_SIZE];
        if (sc_identity_public_x("BGP hop", dp, x) != SC_OK) goto invalid_field;
        if (hops) {
            memcpy(hops[index].ed, dp, SC_PUBKEY_SIZE);
            hops[index].id = sc_derive_node_id_from_pubkey(x);
        }
    }
    if ((size_t)(dp - data) != len) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "NODEINFO decode: cursor mismatch node=%s consumed=%zu length=%zu",
                    sc_public_key_hex(msg->ed25519_public_key, (char[65]){0}), (size_t)(dp - data), len);
        goto fail;
    }

    *out_ni = ni;
    if (out_subnets) *out_subnets = subnets;
    if (out_hop_list) *out_hop_list = hops;
    if (out_hop_count) *out_hop_count = msg->hop_count;
    if (out_cumulative_rtt) *out_cumulative_rtt = msg->cumulative_rtt;
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "NODEINFO decoded: node=%s bytes=%zu flags=%02x subnets=%u/%u hops=%u keep_subnets=%d keep_hops=%d",
                sc_public_key_hex(msg->ed25519_public_key, (char[65]){0}), len, msg->flags, msg->local_v4_subnets, msg->local_v6_subnets,
                msg->hop_count, out_subnets != NULL, out_hop_list != NULL);
    return 0;

invalid_field:
    DEBUG_WARN(DEBUG_CATEGORY_BGP, "NODEINFO decode: invalid field node=%s section=%s index=%u",
               sc_public_key_hex(msg->ed25519_public_key, (char[65]){0}), section, index);
    goto fail;
no_memory:
    DEBUG_ERROR(DEBUG_CATEGORY_BGP, "NODEINFO decode: allocation failed node=%s section=%s index=%u offset=%zu",
                sc_public_key_hex(msg->ed25519_public_key, (char[65]){0}), section, index, (size_t)(dp - data));
fail:
    topo_node_free_raw(groups, ni);
    if (subnets) {
        free_v4_sub_list(groups->v4_subnet_pool, subnets->v4_subnets);
        free_v6_sub_list(groups->v6_subnet_pool, subnets->v6_subnets);
        u_free(subnets);
    }
    u_free(hops);
    return -1;
}

/* Ищет per-group узел TOPO_GROUP_NODE в группе по node_id (NULL, если нет). */
struct TOPO_GROUP_NODE* topo_node_find_by_id(struct TOPO_GROUP* group, uint64_t node_id) {
    if (!group || !group->nodes) return NULL;
    uint64_t key = node_id;
    struct ll_entry* e = queue_find_data_by_index(group->nodes, &key);
    if (e && queue_find_next_by_index(group->nodes, &key, e)) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "ambiguous group node_id=%016llx", (unsigned long long)key); return NULL;
    }
    return (struct TOPO_GROUP_NODE*)e;
}

struct TOPO_GROUP_NODE* topo_node_find_by_transport(struct TOPO_GROUP* group, uint64_t id, const uint8_t x[32]) {
    if (!group || !group->nodes || !x) return NULL;
    for (struct ll_entry* e = queue_find_data_by_index(group->nodes, &id); e; e = queue_find_next_by_index(group->nodes, &id, e)) {
        struct TOPO_GROUP_NODE* node = (struct TOPO_GROUP_NODE*)e;
        if (node->identity && !memcmp(node->identity->public_key, x, 32)) return node;
    }
    return NULL;
}

/* Решает, нужен ли ping узлу: да, если в любой группе last_ping старше PING_TTL_TB. */
int topo_node_ping_request_cbk(struct TOPO_GROUPS* groups, uint64_t node_id) {
    if (!groups || !groups->group_list) return 0;
    uint64_t now = get_time_tb();
    struct ll_entry* ge = groups->group_list->head;
    while (ge) {
        struct TOPO_GROUP* g = (struct TOPO_GROUP*)ge;
        struct TOPO_GROUP_NODE* nq = topo_node_find_by_id(g, node_id);
        if (nq) {
            struct TOPO_CONNECTIVITY* c = &nq->connectivity;
            if (c->last_ping_time == 0 || (now - c->last_ping_time) > PING_TTL_TB) {
                DEBUG_TRACE(DEBUG_CATEGORY_BGP, "ping_request_cbk: node=%016llx last_ping=%llu now=%llu stale=1",
                            (unsigned long long)node_id, (unsigned long long)c->last_ping_time, (unsigned long long)now);
                return 1;
            }
            DEBUG_TRACE(DEBUG_CATEGORY_BGP, "ping_request_cbk: node=%016llx last_ping=%llu now=%llu stale=0",
                        (unsigned long long)node_id, (unsigned long long)c->last_ping_time, (unsigned long long)now);
        }
        ge = ge->next;
    }
    return 0;
}

/* Обновляет только runtime проб; сбор/сохранение замеров выполняет topo_node_rtt. */
void topo_node_ping_update_rtt(struct TOPO_GROUPS* groups, const uint8_t x[32], uint16_t rtt) {
    if (!groups || !groups->group_list || !x) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "ping_update_rtt: no groups, node_x=%s rtt=%u", sc_public_key_hex(x, (char[65]){0}), (unsigned)rtt);
        return;
    }
    uint64_t node_id = sc_derive_node_id_from_pubkey(x), now = get_time_tb();
    struct ll_entry* ge = groups->group_list->head;
    while (ge) {
        struct TOPO_GROUP* g = (struct TOPO_GROUP*)ge;
        struct TOPO_GROUP_NODE* nq = topo_node_find_by_transport(g, node_id, x);
        if (nq) {
            nq->connectivity.last_ping_time = now;
            if (nq->connectivity.interface_min_rtt == 0 || rtt < nq->connectivity.interface_min_rtt)
                nq->connectivity.interface_min_rtt = rtt;
            if (nq->connectivity.nat_min_rtt == 0 || rtt < nq->connectivity.nat_min_rtt)
                nq->connectivity.nat_min_rtt = rtt;
            if (nq->connectivity.real_min_rtt == 0 || rtt < nq->connectivity.real_min_rtt)
                nq->connectivity.real_min_rtt = rtt;
            DEBUG_TRACE(DEBUG_CATEGORY_BGP, "ping_update_rtt: node_x=%s rtt=%u (%llu) group=%016llx",
                       sc_public_key_hex(x, (char[65]){0}), (unsigned)rtt, (unsigned long long)now, (unsigned long long)g->group_id);
            topo_fire_node_metrics_cbk(groups->instance, g, nq);
        }
        ge = ge->next;
    }
}

/* RTT именно выбранного пути, с насыщением вместо переполнения. */
uint16_t topo_get_chain_rtt(struct TOPO_GROUP_NODE* nq) {
    struct TOPO_NODEPATH* p = topo_node_best_path(nq);
    if (!p || !p->conn->rtt_last) return UINT16_MAX;
    uint32_t total = (uint32_t)p->conn->rtt_last + p->cumulative_rtt;
    return total > UINT16_MAX ? UINT16_MAX : (uint16_t)total;
}

// ===== dump / format =====

/* Есть ли у узла непустая диагностика связности (статусы/таймстемпы проб). */
static int is_node_connectivity_active(struct TOPO_CONNECTIVITY* c) {
    return c->probe_status != PROBE_STATUS_NONE
        || c->interface_status != PROBE_RESULT_UNKNOWN
        || c->nat_status != PROBE_RESULT_UNKNOWN
        || c->real_status != PROBE_RESULT_UNKNOWN
        || c->probe_start_time != 0
        || c->ping_req_time != 0
        || c->last_ping_time != 0;
}

/* Читаемое имя статуса/результата пробы и типа conn_mgr для дампа. */
static const char* probe_status_str(uint8_t s) {
    switch (s) { case 0: return "NONE"; case 1: return "IN_PROGRESS"; case 2: return "DONE"; default: return "?"; }
}
static const char* probe_result_str(uint8_t s) {
    switch (s) { case 0: return "UNKNOWN"; case 1: return "REACHABLE"; case 2: return "UNREACHABLE"; default: return "?"; }
}
static const char* conn_mgr_type_str(uint8_t t) {
    switch (t) { case 1: return "DIRECT"; case 2: return "REVERSE"; case 3: return "INDIRECT"; default: return "NONE"; }
}

/* Выводит в лог полный дамп узлов группы (адреса, пути, связность, conn_mgr). */
void topo_node_dump_all(struct TOPO_GROUP* group) {
    if (!group || !group->nodes) return;

    DEBUG_INFO(DEBUG_CATEGORY_BGP, "=== BGP Nodes dump: %d nodes ===", queue_entry_count(group->nodes));

    int idx = 0;
    struct ll_entry* e = group->nodes->head;
    while (e) {
        struct TOPO_GROUP_NODE* nq = (struct TOPO_GROUP_NODE*)e;
        struct TOPO_NODE* ni = topo_node_registry_find(group->instance->topo_groups, nq->node_id);
        if (!ni) { e = e->next; continue; }
        int is_local = (group->local_node == nq);
        const char* name_s = ni->node_name ? ni->node_name : "";

        DEBUG_INFO(DEBUG_CATEGORY_BGP, "--- Node #%d: id=%016llx name=\"%s\" ver=%u grp=%llu%s ---",
                   idx, (unsigned long long)ni->node_id, name_s, ni->ver,
                   (unsigned long long)group->group_id, is_local ? " [LOCAL]" : "");

        log_dump(DEBUG_LEVEL_INFO, DEBUG_CATEGORY_BGP, "  pubkey ", ni->public_key, SC_PUBKEY_SIZE);
        log_dump(DEBUG_LEVEL_INFO, DEBUG_CATEGORY_BGP, "  ed25519", ni->ed25519_public_key, SC_PUBKEY_SIZE);

        int si = 0;
        { struct TOPO_SOCKMETA4* sm = ni->v4_sock_meta; while (sm) {
            DEBUG_INFO(DEBUG_CATEGORY_BGP, "  v4sock[%d]: id=%u config=%u mapping=%s inbound=%s source=%s",
                       si++, sm->id, sm->config_type, address_mapping_name(sm->address_profile.mapping),
                       address_inbound_name(sm->address_profile.inbound), address_profile_source_name(sm->address_profile.source));
            sm = sm->next;
        }}
        { const char* atn[] = {"INTERFACE","NAT","REAL"};
          int ai = 0; struct TOPO_ADDR4* a = ni->v4_addrs; while (a) {
            const char* proto = (a->protocol & TOPO_PROTO_REALITY) ? "tcp+reality" : (a->protocol & TOPO_PROTO_TCP) ? "tcp" : (a->protocol & TOPO_PROTO_UDP) ? "udp" : "?";
            DEBUG_INFO(DEBUG_CATEGORY_BGP, "  v4addr[%d]: %s:%u %s sock=%u proto=%s", ai++,
                       ip_to_str(a->addr, AF_INET).str, a->port, (a->type<=2)?atn[a->type]:"?", a->socket_id, proto);
            a = a->next;
        }}
        { int si6 = 0; struct TOPO_SOCKMETA6* sm6 = ni->v6_sock_meta; while (sm6) {
            DEBUG_INFO(DEBUG_CATEGORY_BGP, "  v6sock[%d]: id=%u config=%u mapping=%s inbound=%s source=%s",
                       si6++, sm6->id, sm6->config_type, address_mapping_name(sm6->address_profile.mapping),
                       address_inbound_name(sm6->address_profile.inbound), address_profile_source_name(sm6->address_profile.source));
            sm6 = sm6->next;
        }}
        { int ai6 = 0; struct TOPO_ADDR6* a6 = ni->v6_addrs; while (a6) {
            const char* proto = (a6->protocol & TOPO_PROTO_REALITY) ? "tcp+reality" : (a6->protocol & TOPO_PROTO_TCP) ? "tcp" : (a6->protocol & TOPO_PROTO_UDP) ? "udp" : "?";
            DEBUG_INFO(DEBUG_CATEGORY_BGP, "  v6addr[%d]: %s:%u type=%u sock=%u proto=%s", ai6++,
                       ip_to_str(a6->addr, AF_INET6).str, a6->port, a6->type, a6->socket_id, proto);
            a6 = a6->next;
        }}
        if (nq->subnets) {
            { int ri = 0; struct TOPO_SUBNET4* sub = nq->subnets->v4_subnets; while (sub) {
                DEBUG_INFO(DEBUG_CATEGORY_BGP, "  v4sub[%d]: %s/%d", ri++, ip_to_str(sub->addr, AF_INET).str, sub->prefix_length);
                sub = sub->next;
            }}
            { int ri6 = 0; struct TOPO_SUBNET6* sub6 = nq->subnets->v6_subnets; while (sub6) {
                DEBUG_INFO(DEBUG_CATEGORY_BGP, "  v6sub[%d]: %s/%d", ri6++, ip_to_str(sub6->addr, AF_INET6).str, sub6->prefix_length);
                sub6 = sub6->next;
            }}
        }
        int path_idx = 0;
        if (nq->paths) {
            struct ll_entry* pe = nq->paths->head;
            while (pe) {
                struct TOPO_NODEPATH* path = (struct TOPO_NODEPATH*)pe;
                const struct topo_hop* hop = path->hops;
                char hop_buf[256] = ""; int hop_pos = 0;
                for (uint8_t hi = 0; hi < path->hop_count && hop_pos < (int)sizeof(hop_buf)-20; hi++)
                    hop_pos += snprintf(hop_buf + hop_pos, sizeof(hop_buf) - hop_pos, "%s%016llx", hi ? "->" : "", (unsigned long long)hop[hi].id);
                DEBUG_INFO(DEBUG_CATEGORY_BGP, "  path[%d]: conn=%s hop_count=%u hops=[%s]",
                           path_idx, path->conn ? path->conn->log_name : "NULL", path->hop_count, hop_buf);
                path_idx++; pe = pe->next;
            }
        }

        struct TOPO_CONNECTIVITY* c = &nq->connectivity;
        if (is_node_connectivity_active(c)) {
            DEBUG_INFO(DEBUG_CATEGORY_BGP, "  connectivity: probe=%s iface=%s(%u) nat=%s(%u) real=%s(%u) pend=%u iface_rtt=%u nat_rtt=%u real_rtt=%u start_tb=%llu ping_req_tb=%llu last_ping_tb=%llu",
                       probe_status_str(c->probe_status), probe_result_str(c->interface_status), c->interface_min_rtt,
                       probe_result_str(c->nat_status), c->nat_min_rtt, probe_result_str(c->real_status), c->real_min_rtt,
                       c->pending_count, c->interface_min_rtt, c->nat_min_rtt, c->real_min_rtt,
                       (unsigned long long)c->probe_start_time, (unsigned long long)c->ping_req_time, (unsigned long long)c->last_ping_time);
        }
        if (nq->conn_mgr_type != CONN_TYPE_NONE) {
            char interm_buf[CONN_MGR_MAX_INTERMEDIARIES * 65 + 1] = ""; size_t ipos = 0;
            for (uint8_t i = 0; i < nq->conn_mgr_intermediariy_count; i++) {
                char ed_hex[65];
                ipos += snprintf(interm_buf + ipos, sizeof(interm_buf) - ipos, "%s%s",
                                 i ? "," : "", sc_public_key_hex(nq->conn_mgr_intermediaries[i], ed_hex));
            }
            DEBUG_INFO(DEBUG_CATEGORY_BGP, "  conn_mgr: type=%s intermediates=[%s]", conn_mgr_type_str(nq->conn_mgr_type), interm_buf);
        }

        e = e->next; idx++;
    }
}

/* То же, что dump_all, но в строковый буфер (для control-сервера). */
int topo_node_format_all(struct TOPO_GROUP* group, char* buf, size_t buf_size) {
    if (buf && buf_size) buf[0] = '\0';
    if (!group || !group->nodes || !buf || buf_size == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node dump: invalid arguments group=%p buf=%p capacity=%zu", (void*)group, (void*)buf, buf_size);
        return 0;
    }
    if (buf_size > INT_MAX) buf_size = INT_MAX;
    size_t pos = 0;

#define FMT_ADD(...) do { \
    int added = snprintf(buf + pos, buf_size - pos, __VA_ARGS__); \
    if (added < 0) { \
        buf[pos] = '\0'; \
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node dump: formatting failed after %zu bytes", pos); \
        return -1; \
    } \
    if ((size_t)added >= buf_size - pos) { pos = buf_size - 1; goto truncated; } \
    pos += (size_t)added; \
} while (0)

    FMT_ADD("=== BGP Nodes dump: %d nodes ===\n", queue_entry_count(group->nodes));

    int idx = 0;
    struct ll_entry* e = group->nodes->head;
    while (e) {
        struct TOPO_GROUP_NODE* nq = (struct TOPO_GROUP_NODE*)e;
        struct TOPO_NODE* ni = topo_node_registry_find(group->instance->topo_groups, nq->node_id);
        if (!ni) { e = e->next; continue; }
        int is_local = (group->local_node == nq);
        const char* name_s = ni->node_name ? ni->node_name : "";

        FMT_ADD("--- Node #%d: id=%016llx name=\"%s\" ver=%u grp=%llu%s ---\n",
                idx, (unsigned long long)ni->node_id, name_s, ni->ver,
                (unsigned long long)group->group_id, is_local ? " [LOCAL]" : "");

        FMT_ADD("  pubkey: "); for (int k = 0; k < SC_PUBKEY_SIZE; k++) FMT_ADD("%02x", ni->public_key[k]); FMT_ADD("\n");
        FMT_ADD("  ed25519: "); for (int k = 0; k < SC_PUBKEY_SIZE; k++) FMT_ADD("%02x", ni->ed25519_public_key[k]); FMT_ADD("\n");

        { struct TOPO_SOCKMETA4* sm = ni->v4_sock_meta; int si = 0; while (sm) {
            FMT_ADD("  v4sock[%d]: id=%u config=%u mapping=%s inbound=%s source=%s\n", si++, sm->id, sm->config_type,
                    address_mapping_name(sm->address_profile.mapping), address_inbound_name(sm->address_profile.inbound),
                    address_profile_source_name(sm->address_profile.source)); sm = sm->next;
        }}
        { const char* atn[] = {"INTERFACE","NAT","REAL"}; struct TOPO_ADDR4* a = ni->v4_addrs; int ai = 0; while (a) {
            const char* proto = (a->protocol & TOPO_PROTO_REALITY) ? "tcp+reality" : (a->protocol & TOPO_PROTO_TCP) ? "tcp" : (a->protocol & TOPO_PROTO_UDP) ? "udp" : "?";
            FMT_ADD("  v4addr[%d]: %s:%u %s sock=%u proto=%s\n",
                    ai++, ip_to_str(a->addr, AF_INET).str, a->port, (a->type<=2)?atn[a->type]:"?", a->socket_id, proto);
            a = a->next;
        }}
        { struct TOPO_SOCKMETA6* sm6 = ni->v6_sock_meta; int s6i = 0; while (sm6) {
            FMT_ADD("  v6sock[%d]: id=%u config=%u mapping=%s inbound=%s source=%s\n", s6i++, sm6->id, sm6->config_type,
                    address_mapping_name(sm6->address_profile.mapping), address_inbound_name(sm6->address_profile.inbound),
                    address_profile_source_name(sm6->address_profile.source)); sm6 = sm6->next;
        }}
        { struct TOPO_ADDR6* a6 = ni->v6_addrs; int a6i = 0; while (a6) {
            const char* proto = (a6->protocol & TOPO_PROTO_REALITY) ? "tcp+reality" : (a6->protocol & TOPO_PROTO_TCP) ? "tcp" : (a6->protocol & TOPO_PROTO_UDP) ? "udp" : "?";
            FMT_ADD("  v6addr[%d]: %s:%u type=%u sock=%u proto=%s\n",
                    a6i++, ip_to_str(a6->addr, AF_INET6).str, a6->port, a6->type, a6->socket_id, proto);
            a6 = a6->next;
        }}
        if (nq->subnets) {
            { struct TOPO_SUBNET4* sub = nq->subnets->v4_subnets; int ri = 0; while (sub) {
                FMT_ADD("  v4sub[%d]: %s/%d\n", ri++, ip_to_str(sub->addr, AF_INET).str, sub->prefix_length); sub = sub->next;
            }}
            { struct TOPO_SUBNET6* sub6 = nq->subnets->v6_subnets; int r6i = 0; while (sub6) {
                FMT_ADD("  v6sub[%d]: %s/%d\n", r6i++, ip_to_str(sub6->addr, AF_INET6).str, sub6->prefix_length); sub6 = sub6->next;
            }}
        }
        if (nq->paths) {
            struct ll_entry* pe = nq->paths->head; int pi = 0;
            while (pe) {
                struct TOPO_NODEPATH* path = (struct TOPO_NODEPATH*)pe;
                const struct topo_hop* hop = path->hops;
                FMT_ADD("  path[%d]: conn=%s hop_count=%u hops=[", pi++, path->conn ? path->conn->log_name : "NULL", path->hop_count);
                for (uint8_t hi = 0; hi < path->hop_count; hi++)
                    FMT_ADD("%s%016llx", hi ? "->" : "", (unsigned long long)hop[hi].id);
                FMT_ADD("]\n"); pe = pe->next;
            }
        }

        struct TOPO_CONNECTIVITY* c = &nq->connectivity;
        if (is_node_connectivity_active(c)) {
            FMT_ADD("  connectivity: probe=%s iface=%s(%u) nat=%s(%u) real=%s(%u) pend=%u iface_rtt=%u nat_rtt=%u real_rtt=%u start_tb=%llu ping_req_tb=%llu last_ping_tb=%llu\n",
                    probe_status_str(c->probe_status), probe_result_str(c->interface_status), c->interface_min_rtt,
                    probe_result_str(c->nat_status), c->nat_min_rtt, probe_result_str(c->real_status), c->real_min_rtt,
                    c->pending_count, c->interface_min_rtt, c->nat_min_rtt, c->real_min_rtt,
                    (unsigned long long)c->probe_start_time, (unsigned long long)c->ping_req_time, (unsigned long long)c->last_ping_time);
        }
        if (nq->conn_mgr_type != CONN_TYPE_NONE) {
            FMT_ADD("  conn_mgr: type=%s intermediates=[", conn_mgr_type_str(nq->conn_mgr_type));
            for (uint8_t i = 0; i < nq->conn_mgr_intermediariy_count; i++) {
                char ed_hex[65];
                FMT_ADD("%s%s", i ? "," : "", sc_public_key_hex(nq->conn_mgr_intermediaries[i], ed_hex));
            }
            FMT_ADD("]\n");
        }

        e = e->next; idx++;
    }
#undef FMT_ADD

    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "node dump: group=%016llx nodes=%d written=%zu capacity=%zu",
                (unsigned long long)group->group_id, idx, pos, buf_size);
    return (int)pos;

truncated: {
        const char marker[] = "\n[TRUNCATED]\n";
        size_t marker_len = sizeof(marker) - 1;
        if (marker_len > pos) marker_len = pos;
        memcpy(buf + pos - marker_len, marker, marker_len);
        buf[pos] = '\0';
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "node dump truncated: group=%016llx written=%zu capacity=%zu",
                   (unsigned long long)group->group_id, pos, buf_size);
        return (int)pos;
    }
}

// ===== update my nodeinfo =====

/* Core owns one reference independently of all services. */
int topo_node_init_self(struct UTUN_INSTANCE* instance) {
    struct TOPO_NODE* ni = u_calloc(1, sizeof(*ni));
    if (!ni) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "local identity allocation failed"); return -1; }
    ni->node_id = instance->node_id; ni->ver = 1;
    ni->client_type = instance->client_type; ni->client_activity = instance->client_activity;
    memcpy(ni->public_key, instance->my_keys.public_key, SC_PUBKEY_SIZE);
    memcpy(ni->ed25519_public_key, instance->my_ed25519_pubkey, SC_PUBKEY_SIZE);
    ni->node_name = u_strdup(instance->name);
    uint8_t ed_private[32], x_private[32];
    int dg = instance->topo_sqlite_db ? devgroup_load_local(instance, &ni->devgroup, ed_private, x_private) : 0;
    OPENSSL_cleanse(ed_private, sizeof(ed_private)); OPENSSL_cleanse(x_private, sizeof(x_private));
    if (dg < 0) { topo_node_destroy(instance->topo_groups, ni); return -1; }
    if (instance->topo_sqlite_db && topo_node_sqlite_node_update_verified(instance->topo_sqlite_db, instance->node_id,
        instance->name, instance->my_keys.public_key, instance->my_ed25519_pubkey, 0, ntp_time_get_seconds(instance)) < 0) {
        topo_node_destroy(instance->topo_groups, ni); return -1;
    }
    if (!ni->node_name || topo_node_sign_self(instance, ni) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "cannot initialize local identity");
        topo_node_destroy(instance->topo_groups, ni); return -1;
    }
    if (!topo_node_registry_store(instance->topo_groups, ni)) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "cannot register local identity");
        topo_node_destroy(instance->topo_groups, ni); return -1;
    }
    return 0;
}

/* Готовит local_node целиком; отказ подготовки/публикации сохраняет прежний узел и маршруты. */
int topo_group_update_my_nodeinfo(struct UTUN_INSTANCE* instance, struct TOPO_GROUP* group) {
    if (!instance || !group || group->instance != instance || !instance->config || !instance->topo_groups) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "local node update: invalid instance/group/config"); return -1;
    }
    struct TOPO_GROUPS* groups = instance->topo_groups;
    struct TOPO_GROUP_NODE* old = group->local_node;
    struct TOPO_GROUP_NODE* lq = u_calloc(1, sizeof(*lq));
    if (!lq) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "local node update: allocation failed"); return -1; }
    struct TOPO_NODE* ni = NULL;
    char* name = NULL;
    int new_identity = 0, retained = 0, routes_changed = 0, changed = 1, vc = 0, vc6 = 0;
    lq->node_id = instance->node_id;
    /* Пока WRITE выполняется, перестройка маршрута сохраняет уже опубликованную подписку. */
    lq->radio = old && db_async_can_write(instance->db_worker) ? old->radio : group->radio_active;
    lq->phone_status = group->group_type == TOPO_GROUP_TYPE_CHAT ? instance->published_phone_status : 0;
    if (group->group_type != TOPO_GROUP_TYPE_CHAT) {
        struct TOPO_SUBNET4** tail4 = NULL; struct TOPO_SUBNET6** tail6 = NULL;
        for (struct CFG_ROUTE_ENTRY* s = instance->config->my_subnets; s; s = s->next) {
            if ((s->ip.family != AF_INET && s->ip.family != AF_INET6) ||
                s->netmask > (s->ip.family == AF_INET ? 32 : 128)) {
                DEBUG_ERROR(DEBUG_CATEGORY_BGP, "local node update: invalid subnet family=%d prefix=%u", s->ip.family, s->netmask);
                goto fail;
            }
            if (!lq->subnets) {
                lq->subnets = u_calloc(1, sizeof(*lq->subnets));
                if (!lq->subnets) goto allocation_failed;
                tail4 = &lq->subnets->v4_subnets; tail6 = &lq->subnets->v6_subnets;
            }
            if (s->ip.family == AF_INET) {
                struct TOPO_SUBNET4* sub = memory_pool_alloc(groups->v4_subnet_pool);
                if (!sub) goto allocation_failed;
                *sub = (struct TOPO_SUBNET4){.prefix_length = s->netmask}; memcpy(sub->addr, &s->ip.addr.v4, 4);
                *tail4 = sub; tail4 = &sub->next; vc++;
            } else {
                struct TOPO_SUBNET6* sub = memory_pool_alloc(groups->v6_subnet_pool);
                if (!sub) goto allocation_failed;
                *sub = (struct TOPO_SUBNET6){.prefix_length = s->netmask}; memcpy(sub->addr, &s->ip.addr.v6, 16);
                *tail6 = sub; tail6 = &sub->next; vc6++;
            }
        }
        if (vc > UINT8_MAX || vc6 > UINT8_MAX) {
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "local node update: subnet counters exceed wire limit v4=%d v6=%d", vc, vc6); goto fail;
        }
    }
    ni = topo_node_registry_lookup(groups, instance->node_id, instance->my_keys.public_key);
    if (!ni && db_async_can_write(instance->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "local group update requires committed self registry"); goto fail;
    }
    changed = !old || !ni || strcmp(ni->node_name ? ni->node_name : "", instance->name) ||
        memcmp(ni->public_key, instance->my_keys.public_key, SC_PUBKEY_SIZE) ||
        memcmp(ni->ed25519_public_key, instance->my_ed25519_pubkey, SC_PUBKEY_SIZE) ||
        ni->client_type != instance->client_type || ni->client_activity != instance->client_activity ||
        old->radio != lq->radio || old->phone_status != lq->phone_status;
    if (!changed) {
        struct TOPO_SUBNET4* a4 = old->subnets ? old->subnets->v4_subnets : NULL;
        struct TOPO_SUBNET4* b4 = lq->subnets ? lq->subnets->v4_subnets : NULL;
        while (a4 && b4 && a4->prefix_length == b4->prefix_length && !memcmp(a4->addr, b4->addr, 4)) { a4 = a4->next; b4 = b4->next; }
        struct TOPO_SUBNET6* a6 = old->subnets ? old->subnets->v6_subnets : NULL;
        struct TOPO_SUBNET6* b6 = lq->subnets ? lq->subnets->v6_subnets : NULL;
        while (a6 && b6 && a6->prefix_length == b6->prefix_length && !memcmp(a6->addr, b6->addr, 16)) { a6 = a6->next; b6 = b6->next; }
        changed = a4 || b4 || a6 || b6;
    }
    if (!changed) {
        old->last_ver = ni->ver; old->last_timestamp = ni->timestamp;
        DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "local node unchanged: group=%016llx ver=%u", (unsigned long long)group->group_id, ni->ver);
        goto unchanged;
    }
    uint32_t short31;
    if (topo_address_short(instance->my_ed25519_pubkey, &short31)) goto fail;
    name = u_strdup(instance->name);
    if (!name) goto allocation_failed;
    if (!ni) {
        ni = u_calloc(1, sizeof(*ni)); new_identity = 1;
        if (!ni) goto allocation_failed;
    } else { topo_node_ref(ni); retained = 1; }
    struct TOPO_NODE candidate = *ni;
    candidate.node_id = instance->node_id; candidate.ver = (ni->ver % 255) + 1;
    candidate.node_name = name; candidate.client_type = instance->client_type; candidate.client_activity = instance->client_activity;
    memcpy(candidate.public_key, instance->my_keys.public_key, SC_PUBKEY_SIZE);
    memcpy(candidate.ed25519_public_key, instance->my_ed25519_pubkey, SC_PUBKEY_SIZE);
    if (group->group_type != TOPO_GROUP_TYPE_CHAT && instance->rt) {
        routes_changed = 1;
        route_delete(instance->rt, old);
        if (vc && !route_insert(instance->rt, lq, true)) {
            DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "local node update: route conflict/allocation group=%016llx",
                        (unsigned long long)group->group_id); goto fail;
        }
    }
    if (topo_node_sign_self(instance, &candidate) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "local node update: cannot publish group=%016llx", (unsigned long long)group->group_id); goto fail;
    }
    if (db_async_can_write(instance->db_worker)) { u_free(name); name = NULL; }
    else { char* old_name = ni->node_name; *ni = candidate; name = NULL; u_free(old_name); }
    if (new_identity) {
        struct TOPO_NODE* stored = topo_node_registry_store(groups, ni);
        if (!stored) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "local node update: cannot register identity"); goto fail; }
        ni = stored; new_identity = 0; retained = 1;
    }
    lq->last_ver = ni->ver; lq->last_timestamp = ni->timestamp;
    if (old) { topo_address_detach(group, old); topo_nodeq_clear_paths(old); topo_nodeq_free_group_fields(groups, old); u_free(old); }
    lq->identity = ni; group->local_node = lq;
    topo_address_attach(group, lq, instance->my_ed25519_pubkey, short31);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "local node published: group=%016llx v4=%d v6=%d ver=%u timestamp=%llu",
                (unsigned long long)group->group_id, vc, vc6, ni->ver, (unsigned long long)ni->timestamp);
    return vc;
allocation_failed:
    DEBUG_ERROR(DEBUG_CATEGORY_BGP, "local node update: preparation allocation failed group=%016llx",
                (unsigned long long)group->group_id);
fail:
    if (routes_changed) {
        route_delete(instance->rt, lq);
        if (old && old->subnets && old->subnets->v4_subnets && !route_insert(instance->rt, old, true))
            DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "local node update: previous routes restoration failed group=%016llx",
                        (unsigned long long)group->group_id);
    }
    if (new_identity) topo_node_destroy(groups, ni);
    if (retained) topo_node_registry_release(groups, ni);
    u_free(name);
unchanged:
    if (lq->subnets) {
        free_v4_sub_list(groups->v4_subnet_pool, lq->subnets->v4_subnets);
        free_v6_sub_list(groups->v6_subnet_pool, lq->subnets->v6_subnets);
        u_free(lq->subnets);
    }
    u_free(lq);
    return changed ? -1 : vc;
}

/* Пересобирает списки адресов своего узла из ETCP-сокетов; при изменении — подпись, БД, рассылка. */
int topo_node_update_my_addresses(struct UTUN_INSTANCE* instance) {
    if (!instance || !instance->topo_groups) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "invalid args"); return -1; }
    struct TOPO_NODE* ni = topo_node_registry_lookup(instance->topo_groups, instance->node_id, instance->my_keys.public_key);
    if (!ni) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "my node not in registry"); return -1; }

    /* Собираем новые списки во временные головы, чтобы сравнить со старыми до их освобождения. */
    struct TOPO_SOCKMETA4* m4_head = NULL;
    struct TOPO_ADDR4*     v4_head = NULL;
    struct TOPO_SOCKMETA6* m6_head = NULL;
    struct TOPO_ADDR6*     v6_head = NULL;
    struct TOPO_REALITY_SOCK* r_head = NULL;

    /* node-global reality-параметры (одинаковы для всех reality-сокетов): рекламируем первый short_id */
    uint8_t  r_short_id[8], r_pubkey[32], r_version[3]; char r_sname[TOPO_REALITY_SNAME_MAX];
    uint8_t  r_valid = 0;
    {   const struct reality_config* rc = &instance->config->global.reality;
        if (rc->enabled && rc->has_private_key) {
            if (reality_pubkey_from_priv(rc->private_key, r_pubkey) == REALITY_OK) {
                if (rc->short_id_count > 0) memcpy(r_short_id, rc->short_ids[0], 8);
                else memcpy(r_short_id, rc->short_id, 8);
                memcpy(r_version, rc->version, 3);
                memset(r_sname, 0, sizeof(r_sname));
                strncpy(r_sname, rc->server_name, sizeof(r_sname) - 1);
                r_valid = 1;
            }
        }
    }

    struct ETCP_SOCKET* e_sock = instance->etcp_sockets;
    while (e_sock) {
        if (e_sock->is_tcp || e_sock->probe_only) { e_sock = e_sock->next; continue; }
        if (e_sock->type == CFG_SERVER_TYPE_PRIVATE) { e_sock = e_sock->next; continue; }  /* private скрыт, LOCAL рекламируем (LAN) */
        if (e_sock->local_addr.ss_family == AF_INET) {
            { struct TOPO_SOCKMETA4* sm = memory_pool_alloc(instance->topo_groups->v4_sock_meta_pool);
              if (!sm) goto allocation_failed;
              sm->id = e_sock->sock_id; sm->config_type = e_sock->type; sm->address_profile = e_sock->address_profile;
              sm->next = m4_head; m4_head = sm; }
            struct sockaddr_in* local_sin = (struct sockaddr_in*)&e_sock->local_addr;
            struct sockaddr_in* if_sin   = (struct sockaddr_in*)&e_sock->interface_addr;
            struct sockaddr_in* nat_sin  = (struct sockaddr_in*)&e_sock->nat_addr;
            /* адрес ДО NAT (локальный/интерфейсный) — рекламируем всегда */
            int use_local = (local_sin->sin_addr.s_addr != 0);
            uint32_t pre_ip   = use_local ? local_sin->sin_addr.s_addr : if_sin->sin_addr.s_addr;
            uint16_t pre_port = ntohs(use_local ? local_sin->sin_port : if_sin->sin_port);
            { struct TOPO_ADDR4* a = memory_pool_alloc(instance->topo_groups->v4_addr_pool);
              if (!a) goto allocation_failed;
              memcpy(a->addr, &pre_ip, 4); a->port = pre_port;
              a->type = TOPO_ADDR_INTERFACE; a->socket_id = e_sock->sock_id;
              a->protocol = TOPO_PROTO_UDP;
              a->next = v4_head; v4_head = a; }
            /* nat_addr получен для рабочего порта; STUN может уточнить только его предварительный тип EIM?. */
            if (e_sock->nat_addr.ss_family == AF_INET && nat_sin->sin_addr.s_addr != 0 &&
                (e_sock->address_profile.mapping == ADDRESS_MAP_UNKNOWN || e_sock->address_profile.mapping == ADDRESS_MAP_EIM) &&
                e_sock->address_profile.source != ADDRESS_SOURCE_NONE) {
                struct TOPO_ADDR4* a = memory_pool_alloc(instance->topo_groups->v4_addr_pool);
                if (!a) goto allocation_failed;
                memcpy(a->addr, &nat_sin->sin_addr.s_addr, 4); a->port = ntohs(nat_sin->sin_port);
                a->type = TOPO_ADDR_NAT; a->socket_id = e_sock->sock_id;
                a->protocol = TOPO_PROTO_UDP;
                a->next = v4_head; v4_head = a;
            }
        } else if (e_sock->local_addr.ss_family == AF_INET6) {
            { struct TOPO_SOCKMETA6* sm6 = memory_pool_alloc(instance->topo_groups->v6_sock_meta_pool);
              if (!sm6) goto allocation_failed;
              sm6->id = e_sock->sock_id; sm6->config_type = e_sock->type; sm6->address_profile = e_sock->address_profile;
              sm6->next = m6_head; m6_head = sm6; }
            struct sockaddr_in6* if_sin6 = (struct sockaddr_in6*)&e_sock->interface_addr;
            { struct TOPO_ADDR6* a6 = memory_pool_alloc(instance->topo_groups->v6_addr_pool);
              if (!a6) goto allocation_failed;
              memcpy(a6->addr, &if_sin6->sin6_addr, 16); a6->port = ntohs(if_sin6->sin6_port);
              a6->type = TOPO_ADDR_INTERFACE; a6->socket_id = e_sock->sock_id; a6->protocol = TOPO_PROTO_UDP;
              a6->next = v6_head; v6_head = a6; }
            if (e_sock->nat_addr.ss_family == AF_INET6 &&
                (e_sock->address_profile.mapping == ADDRESS_MAP_UNKNOWN || e_sock->address_profile.mapping == ADDRESS_MAP_EIM) &&
                e_sock->address_profile.source != ADDRESS_SOURCE_NONE) {
                const struct sockaddr_in6 *mapped = (const struct sockaddr_in6 *)&e_sock->nat_addr;
                struct TOPO_ADDR6 *a6 = memory_pool_alloc(instance->topo_groups->v6_addr_pool);
                if (!a6) goto allocation_failed;
                memcpy(a6->addr, &mapped->sin6_addr, 16); a6->port = ntohs(mapped->sin6_port);
                a6->type = TOPO_ADDR_NAT; a6->socket_id = e_sock->sock_id; a6->protocol = TOPO_PROTO_UDP;
                a6->next = v6_head; v6_head = a6;
            }
        }
        e_sock = e_sock->next;
    }
    { struct ETCP_SOCKET* ts = instance->etcp_sockets;
      while (ts) { if (!ts->is_tcp) { ts = ts->next; continue; }
        if (ts->type == CFG_SERVER_TYPE_PRIVATE) { ts = ts->next; continue; }  /* private скрыт, LOCAL рекламируем (LAN) */
        struct sockaddr_storage* addr = ts->interface_addr.ss_family ? &ts->interface_addr : &ts->local_addr;
        if (!addr || !addr->ss_family) { ts = ts->next; continue; }
        const int is_reality = r_valid && ts->reality_enabled;
        uint8_t proto = TOPO_PROTO_TCP | (is_reality ? TOPO_PROTO_REALITY : 0);
        if (addr->ss_family == AF_INET) {
            struct sockaddr_in* sin = (struct sockaddr_in*)addr;
            { struct TOPO_SOCKMETA4* sm = memory_pool_alloc(instance->topo_groups->v4_sock_meta_pool);
              if (!sm) goto allocation_failed;
              sm->id = ts->sock_id; sm->config_type = ts->type; sm->address_profile = ts->address_profile;
              sm->next = m4_head; m4_head = sm; }
            { struct TOPO_ADDR4* a = memory_pool_alloc(instance->topo_groups->v4_addr_pool);
              if (!a) goto allocation_failed;
              memcpy(a->addr, &sin->sin_addr.s_addr, 4); a->port = ntohs(sin->sin_port);
              a->type = TOPO_ADDR_INTERFACE; a->socket_id = ts->sock_id; a->protocol = proto;
              a->next = v4_head; v4_head = a; }
            if (ts->nat_addr.ss_family == AF_INET && ts->address_profile.source == ADDRESS_SOURCE_ETCP &&
                ts->address_profile.mapping == ADDRESS_MAP_UNKNOWN) {
                struct TOPO_ADDR4 *a = memory_pool_alloc(instance->topo_groups->v4_addr_pool);
                if (!a) goto allocation_failed;
                memcpy(a->addr, &((const struct sockaddr_in *)&ts->nat_addr)->sin_addr, 4);
                a->port = 0; a->type = TOPO_ADDR_NAT; a->socket_id = ts->sock_id; a->protocol = proto;
                a->next = v4_head; v4_head = a;
            }
        } else if (addr->ss_family == AF_INET6) {
            struct sockaddr_in6* sin6 = (struct sockaddr_in6*)addr;
            { struct TOPO_SOCKMETA6* sm6 = memory_pool_alloc(instance->topo_groups->v6_sock_meta_pool);
              if (!sm6) goto allocation_failed;
              sm6->id = ts->sock_id; sm6->config_type = ts->type; sm6->address_profile = ts->address_profile;
              sm6->next = m6_head; m6_head = sm6; }
            { struct TOPO_ADDR6* a6 = memory_pool_alloc(instance->topo_groups->v6_addr_pool);
              if (!a6) goto allocation_failed;
              memcpy(a6->addr, &sin6->sin6_addr, 16); a6->port = ntohs(sin6->sin6_port);
              a6->type = TOPO_ADDR_INTERFACE; a6->socket_id = ts->sock_id; a6->protocol = proto;
              a6->next = v6_head; v6_head = a6; }
            if (ts->nat_addr.ss_family == AF_INET6 && ts->address_profile.source == ADDRESS_SOURCE_ETCP &&
                ts->address_profile.mapping == ADDRESS_MAP_UNKNOWN) {
                struct TOPO_ADDR6 *a6 = memory_pool_alloc(instance->topo_groups->v6_addr_pool);
                if (!a6) goto allocation_failed;
                memcpy(a6->addr, &((const struct sockaddr_in6 *)&ts->nat_addr)->sin6_addr, 16);
                a6->port = 0; a6->type = TOPO_ADDR_NAT; a6->socket_id = ts->sock_id; a6->protocol = proto;
                a6->next = v6_head; v6_head = a6;
            }
        }
        if (is_reality) {
            struct TOPO_REALITY_SOCK* r = u_calloc(1, sizeof(struct TOPO_REALITY_SOCK));
            if (!r) goto allocation_failed;
            else {
                r->socket_id = ts->sock_id;
                memcpy(r->short_id, r_short_id, 8);
                memcpy(r->server_pubkey, r_pubkey, 32);
                memcpy(r->version, r_version, 3);
                memcpy(r->server_name, r_sname, TOPO_REALITY_SNAME_MAX);
                r->next = r_head; r_head = r;
            }
        }
        ts = ts->next; }
    }

    /* Без изменений — подпись/рассылку гасим, но БД всё равно синхронизируем:
       она могла быть не готова (или пуста) при предыдущем вызове. */
    int changed = !(sockmeta4_list_equal(ni->v4_sock_meta, m4_head) && addr4_list_equal(ni->v4_addrs, v4_head) &&
                    sockmeta6_list_equal(ni->v6_sock_meta, m6_head) && addr6_list_equal(ni->v6_addrs, v6_head) &&
                    reality_sock_list_equal(ni->reality_socks, r_head));
    if (changed) {
        struct TOPO_NODE candidate = *ni;
        candidate.v4_sock_meta = m4_head; candidate.v4_addrs = v4_head;
        candidate.v6_sock_meta = m6_head; candidate.v6_addrs = v6_head;
        candidate.reality_socks = r_head;
        candidate.ver = (ni->ver % 255) + 1;
        if (topo_node_sign_self(instance, &candidate) < 0) {
            free_v4_sock_list(instance->topo_groups->v4_sock_meta_pool, m4_head);
            free_v4_addr_list(instance->topo_groups->v4_addr_pool, v4_head);
            free_v6_sock_list(instance->topo_groups->v6_sock_meta_pool, m6_head);
            free_v6_addr_list(instance->topo_groups->v6_addr_pool, v6_head);
            free_reality_sock_list(r_head);
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "address snapshot publication failed"); return -1;
        }
        if (db_async_can_write(instance->db_worker)) {
            free_v4_sock_list(instance->topo_groups->v4_sock_meta_pool, m4_head);
            free_v4_addr_list(instance->topo_groups->v4_addr_pool, v4_head);
            free_v6_sock_list(instance->topo_groups->v6_sock_meta_pool, m6_head);
            free_v6_addr_list(instance->topo_groups->v6_addr_pool, v6_head); free_reality_sock_list(r_head);
            DEBUG_INFO(DEBUG_CATEGORY_BGP, "my addresses queued for disk commit");
        } else {
            free_v4_sock_list(instance->topo_groups->v4_sock_meta_pool, ni->v4_sock_meta); ni->v4_sock_meta = m4_head;
            free_v4_addr_list(instance->topo_groups->v4_addr_pool, ni->v4_addrs); ni->v4_addrs = v4_head;
            free_v6_sock_list(instance->topo_groups->v6_sock_meta_pool, ni->v6_sock_meta); ni->v6_sock_meta = m6_head;
            free_v6_addr_list(instance->topo_groups->v6_addr_pool, ni->v6_addrs); ni->v6_addrs = v6_head;
            free_reality_sock_list(ni->reality_socks); ni->reality_socks = r_head;
            ni->ver = candidate.ver; ni->timestamp = candidate.timestamp;
            memcpy(ni->x25519_self_sig, candidate.x25519_self_sig, sizeof(ni->x25519_self_sig));
            DEBUG_INFO(DEBUG_CATEGORY_BGP, "my addresses updated, new ver=%d", ni->ver);
        }
    } else {
        free_v4_sock_list(instance->topo_groups->v4_sock_meta_pool, m4_head);
        free_v4_addr_list(instance->topo_groups->v4_addr_pool, v4_head);
        free_v6_sock_list(instance->topo_groups->v6_sock_meta_pool, m6_head);
        free_v6_addr_list(instance->topo_groups->v6_addr_pool, v6_head);
        free_reality_sock_list(r_head);
        DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "my addresses unchanged (ver=%d), persist DB", ni->ver);
    }

    int database_failed = 0;
    /* всегда: адреса (addrs_put) + классификация node_type/storage (nodeinfo_updated) */
    if (!db_async_can_write(instance->db_worker) && instance->topo_sqlite_db) {
        time_t now_sec = ntp_time_get_seconds(instance);
        if (topo_node_sqlite_node_update_verified(instance->topo_sqlite_db, instance->node_id,
            instance->name, instance->my_keys.public_key, instance->my_ed25519_pubkey,
            (uint64_t)now_sec, now_sec) < 0 ||
            devgroup_project(instance->topo_sqlite_db, ni) < 0 ||
            topo_node_sqlite_addrs_put(instance->topo_sqlite_db, instance->node_id, ni) < 0 ||
            topo_node_sqlite_nodeinfo_updated(instance->topo_sqlite_db, instance->my_keys.public_key) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "local address projection update failed"); database_failed = 1;
        }
    }

    if (changed && !db_async_can_write(instance->db_worker)) {
        struct ll_entry* ge = instance->topo_groups->group_list->head;
        while (ge) {
            struct TOPO_GROUP* g = (struct TOPO_GROUP*)ge;
            if (g->local_node) {
                if (g->senders_list) {
                    struct ll_entry* se = g->senders_list->head;
                    while (se) {
                        struct TOPO_GROUP_CONN_ITEM* item = (struct TOPO_GROUP_CONN_ITEM*)se->data;
                        if (item && item->conn) topo_group_send_nodeinfo(g, g->local_node, item->conn);
                        se = se->next;
                    }
                }
            }
            ge = ge->next;
        }
    }
    return database_failed ? -1 : changed ? 1 : 0;
allocation_failed:
    DEBUG_ERROR(DEBUG_CATEGORY_BGP, "local address snapshot allocation failed");
    free_v4_sock_list(instance->topo_groups->v4_sock_meta_pool, m4_head);
    free_v4_addr_list(instance->topo_groups->v4_addr_pool, v4_head);
    free_v6_sock_list(instance->topo_groups->v6_sock_meta_pool, m6_head);
    free_v6_addr_list(instance->topo_groups->v6_addr_pool, v6_head);
    free_reality_sock_list(r_head);
    return -1;
}

/* Callback изменения сокета (адрес/статус) — инициирует пересборку адресов своего узла. */
void topo_node_on_socket_changed(struct ETCP_SOCKET* sock, int event, void* arg) {
    (void)arg;
    if (!sock || !sock->instance) return;
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "socket %s changed (event=0x%x), updating nodeinfo", sock->name, event);
    topo_node_update_my_addresses(sock->instance);
}
