#include "topo_node_sqlite.h"
#include "../db_changes.h"
#include "../db_schema.h"
#include "../chat/group_info.h"
#include "../chat/chat_core.h"
#include "../chat/member_sync.h"
#include "topo_node.h"
#include "topo_group.h"
#include "etcp_connections.h"
#include "../config_parser.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/memory_pool.h"
#include "../lib/json_flat.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <openssl/crypto.h>

#define PEERS_JOIN_SIG_SIZE 64
#define PEERS_JOIN_TS_SIZE   8

static void sanitize_ch_id(const char* ch_id, char* out, size_t out_sz) {
    size_t i = 0;
    while (*ch_id && i < out_sz - 1) {
        char c = *ch_id++;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_')
            out[i++] = c;
        else
            out[i++] = '_';
    }
    out[i] = '\0';
}

static void peers_table_name(const char* ch_id, char* buf, size_t sz) {
    char san[65]; sanitize_ch_id(ch_id, san, sizeof(san));
    snprintf(buf, sz, "peers_%s", san);
}

struct node_owner_flags { int deleted, storage, supernode; };
static int node_owner_flag(const char* key, const char* value, void* data) {
    struct node_owner_flags* flags = data;
    int yes = !strcmp(value, "yes");
    if (!strcmp(key, "deleted")) flags->deleted = yes;
    else if (!strcmp(key, "storage")) flags->storage = yes;
    else if (!strcmp(key, "supernode")) flags->supernode = yes;
    return 0;
}

/* ── Классификация адресов (локально, по IP) ── */

static int addr_type_from_ip(int family, const uint8_t* addr) {
    if (!addr) return ADDR_TYPE_NETIF;
    if (family == 4) {
        uint8_t b0 = addr[0], b1 = addr[1];
        if (b0 == 0 || b0 == 10 || b0 == 127) return ADDR_TYPE_NETIF;
        if (b0 == 169 && b1 == 254) return ADDR_TYPE_NETIF;
        if (b0 >= 224 && b0 <= 239) return ADDR_TYPE_NETIF;
        if (b0 == 172 && b1 >= 16 && b1 <= 31) return ADDR_TYPE_NETIF;
        if (b0 == 100 && b1 >= 64 && b1 <= 127) return ADDR_TYPE_NETIF; /* 100.64.0.0/10 (CGNAT) */
        if (b0 == 192 && b1 == 168) return ADDR_TYPE_NETIF;
        { uint32_t ip; memcpy(&ip, addr, 4); if (ip == 0 || ip == 0xffffffff) return ADDR_TYPE_NETIF; }
        return ADDR_TYPE_DIRECT;
    } else {
        if (addr[0] == 0xfe && (addr[1] & 0xc0) == 0x80) return ADDR_TYPE_NETIF;
        if (addr[0] == 0xfc || addr[0] == 0xfd) return ADDR_TYPE_NETIF;
        { static const uint8_t z[16]; if (!memcmp(addr, z, 16)) return ADDR_TYPE_NETIF; }
        { static const uint8_t lb[16]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1}; if (!memcmp(addr,lb,16)) return ADDR_TYPE_NETIF; }
        return ADDR_TYPE_DIRECT;
    }
}

/* Итоговая классификация: конфиг + автоопределение + IP (всё локально). */
static int addr_type_classify(int kind, int protocol, int config_type, int global, const struct address_profile *profile) {
    if (config_type == CFG_SERVER_TYPE_PRIVATE || config_type == CFG_SERVER_TYPE_LOCAL) return ADDR_TYPE_NETIF;
    if (!profile) return ADDR_TYPE_NAT;
    if ((protocol & TOPO_PROTO_TCP) && profile->source == ADDRESS_SOURCE_ETCP && profile->mapping == ADDRESS_MAP_UNKNOWN)
        return ADDR_TYPE_NAT;
    if (profile->mapping == ADDRESS_MAP_EIM)
        return profile->inbound == ADDRESS_IN_UNKNOWN ? ADDR_TYPE_NAT_EIM_UNVERIFIED : ADDR_TYPE_NAT_EIM;
    if (profile->mapping == ADDRESS_MAP_DEPENDENT) return ADDR_TYPE_NAT_STRICT;
    if (kind == TOPO_ADDR_NAT) return ADDR_TYPE_NAT;
    /* Публичный адрес интерфейса известен без определения входящих и не становится NAT из-за UNKNOWN. */
    if (global && kind == TOPO_ADDR_INTERFACE && (profile->mapping == ADDRESS_MAP_UNKNOWN ||
        (profile->mapping == ADDRESS_MAP_REAL_IP && profile->inbound == ADDRESS_IN_UNKNOWN))) return ADDR_TYPE_NETIF;
    if (!global) return config_type == CFG_SERVER_TYPE_NAT ? ADDR_TYPE_NAT : ADDR_TYPE_NETIF;
    if (profile->mapping == ADDRESS_MAP_REAL_IP && profile->inbound == ADDRESS_IN_OPEN) return ADDR_TYPE_DIRECT;
    if (profile->mapping == ADDRESS_MAP_REAL_IP && profile->inbound == ADDRESS_IN_CONTACT) return ADDR_TYPE_CONTACT;
    return ADDR_TYPE_NAT;
}

static const struct address_profile *smeta4_lookup(struct TOPO_SOCKMETA4 *head, uint8_t sid, uint8_t *config) {
    *config = CFG_SERVER_TYPE_UNKNOWN;
    for (const struct TOPO_SOCKMETA4 *meta = head; meta; meta = meta->next)
        if (meta->id == sid) { *config = meta->config_type; return &meta->address_profile; }
    return NULL;
}

static const struct address_profile *smeta6_lookup(struct TOPO_SOCKMETA6 *head, uint8_t sid, uint8_t *config) {
    *config = CFG_SERVER_TYPE_UNKNOWN;
    for (const struct TOPO_SOCKMETA6 *meta = head; meta; meta = meta->next)
        if (meta->id == sid) { *config = meta->config_type; return &meta->address_profile; }
    return NULL;
}

unsigned topo_node_best_confirmed_type(const struct TOPO_NODE *node) {
    if (!node) return 0;
    unsigned best = 0;
    for (unsigned family = 4; family <= 6; family += 2) {
        const struct TOPO_ADDR4 *a4 = node->v4_addrs;
        const struct TOPO_ADDR6 *a6 = node->v6_addrs;
        while (family == 4 ? a4 != NULL : a6 != NULL) {
            uint8_t config;
            unsigned sid = family == 4 ? a4->socket_id : a6->socket_id;
            const struct address_profile *profile = family == 4 ? smeta4_lookup(node->v4_sock_meta, sid, &config)
                : smeta6_lookup(node->v6_sock_meta, sid, &config);
            const uint8_t *address = family == 4 ? a4->addr : a6->addr;
            unsigned type = addr_type_classify(family == 4 ? a4->type : a6->type,
                family == 4 ? a4->protocol : a6->protocol, config, addr_type_from_ip(family, address) == ADDR_TYPE_DIRECT, profile);
            unsigned port = family == 4 ? a4->port : a6->port;
            if (port && type >= ADDR_TYPE_DIRECT && type <= ADDR_TYPE_NAT_STRICT && (!best || type < best)) best = type;
            if (family == 4) a4 = a4->next; else a6 = a6->next;
        }
    }
    return best;
}

/* reality-параметры сокета → фиксированный blob для node_addresses.options.
   Возвращает 1 и заполняет opts, если у сокета есть reality; иначе 0. */
static int reality_opts_build(struct TOPO_NODE* ni, uint8_t sid, struct TOPO_ADDR_REALITY_OPTS* opts) {
    if (!ni || !opts) return 0;
    for (const struct TOPO_REALITY_SOCK* r = ni->reality_socks; r; r = r->next) {
        if (r->socket_id != sid) continue;
        memcpy(opts->short_id, r->short_id, 8);
        memcpy(opts->server_pubkey, r->server_pubkey, 32);
        memcpy(opts->version, r->version, 3);
        memcpy(opts->server_name, r->server_name, TOPO_REALITY_SNAME_MAX);
        return 1;
    }
    return 0;
}

/* ── Персистенция адресов узла (источник истины — BGP/topo_group) ── */

static int address_row_put(sqlite3_stmt *statement, struct TOPO_NODE *node, uint64_t node_id,
                            int family, const uint8_t *address, uint16_t port, uint8_t kind, uint8_t socket_id,
                            uint8_t protocol, uint8_t config, const struct address_profile *profile) {
    static const struct address_profile unknown = {0};
    if (!profile) profile = &unknown;
    int summary = addr_type_classify(kind, protocol, config, addr_type_from_ip(family, address) == ADDR_TYPE_DIRECT, profile);
    struct TOPO_ADDR_REALITY_OPTS reality;
    int has_reality = (protocol & TOPO_PROTO_REALITY) && reality_opts_build(node, socket_id, &reality);
    int result = sqlite3_bind_blob(statement, 1, node->public_key, SC_PUBKEY_SIZE, SQLITE_STATIC);
    if (!result) result = sqlite3_bind_int(statement, 2, family);
    if (!result) result = sqlite3_bind_int(statement, 3, protocol);
    if (!result) result = sqlite3_bind_blob(statement, 4, address, family == 4 ? 4 : 16, SQLITE_STATIC);
    if (!result) result = sqlite3_bind_int(statement, 5, port);
    if (!result) result = sqlite3_bind_int(statement, 6, summary);
    if (!result) result = sqlite3_bind_int(statement, 7, config);
    if (!result) result = sqlite3_bind_int(statement, 8, profile->mapping);
    if (!result) result = sqlite3_bind_int(statement, 9, socket_id);
    if (!result) result = has_reality ? sqlite3_bind_blob(statement, 10, &reality, sizeof(reality), SQLITE_STATIC) :
        sqlite3_bind_null(statement, 10);
    if (!result) result = sqlite3_bind_int(statement, 11, kind);
    if (!result) result = sqlite3_bind_int(statement, 12, profile->inbound);
    if (!result) result = sqlite3_bind_int(statement, 13, profile->source);
    if (!result) result = sqlite3_step(statement);
    if (result != SQLITE_DONE) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "address projection insert failed node=%016llx family=%d socket=%u kind=%u"
                    " mapping=%s inbound=%s: %s", (unsigned long long)node_id, family, socket_id, kind,
                    address_mapping_name(profile->mapping), address_inbound_name(profile->inbound),
                    sqlite3_errmsg(sqlite3_db_handle(statement)));
        return result;
    }
    return sqlite3_reset(statement);
}

int topo_node_sqlite_addrs_write(sqlite3 *db, uint64_t node_id, struct TOPO_NODE *node) {
    if (!db || !node) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "address projection: invalid arguments"); return SQLITE_MISUSE; }
    int rc = sqlite3_exec(db, "SAVEPOINT node_address_projection", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "address projection savepoint failed rc=%d: %s", rc, sqlite3_errmsg(db)); return rc;
    }
    sqlite3_stmt *statement = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT 1 FROM nodes WHERE x25519_pubkey=?", -1, &statement, NULL);
    if (!rc) rc = sqlite3_bind_blob(statement, 1, node->public_key, SC_PUBKEY_SIZE, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(statement);
    if (rc != SQLITE_ROW) {
        if (rc == SQLITE_DONE) rc = SQLITE_CONSTRAINT;
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "address projection has no matching owner node=%016llx rc=%d", (unsigned long long)node_id, rc);
        goto fail;
    }
    rc = sqlite3_finalize(statement); statement = NULL; if (rc) goto fail;
    rc = sqlite3_prepare_v2(db, "CREATE TEMP TABLE node_address_rtt AS SELECT node_x,family,protocol,address,port,"
        "socket_id,address_kind,rtt FROM node_addresses WHERE node_x=?", -1, &statement, NULL);
    if (!rc) rc = sqlite3_bind_blob(statement, 1, node->public_key, SC_PUBKEY_SIZE, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(statement);
    if (rc != SQLITE_DONE) goto fail;
    rc = sqlite3_finalize(statement); statement = NULL; if (rc) goto fail;
    rc = sqlite3_prepare_v2(db, "DELETE FROM node_addresses WHERE node_x=?", -1, &statement, NULL);
    if (!rc) rc = sqlite3_bind_blob(statement, 1, node->public_key, SC_PUBKEY_SIZE, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(statement);
    if (rc != SQLITE_DONE) goto fail;
    rc = sqlite3_finalize(statement); statement = NULL; if (rc) goto fail;
    rc = sqlite3_prepare_v2(db, "INSERT INTO node_addresses(node_x,family,protocol,address,port,addr_type,config_type,"
        "nat_mapping,socket_id,options,address_kind,nat_inbound,nat_source,rtt)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,(SELECT rtt FROM temp.node_address_rtt WHERE node_x=?1 AND family=?2"
        " AND protocol=?3 AND address=?4 AND port=?5 AND socket_id=?9 AND address_kind=?11))", -1, &statement, NULL);
    if (rc) goto fail;
    int written = 0;
    for (const struct TOPO_ADDR4 *address = node->v4_addrs; address; address = address->next) {
        uint8_t config;
        const struct address_profile *profile = smeta4_lookup(node->v4_sock_meta, address->socket_id, &config);
        rc = address_row_put(statement, node, node_id, 4, address->addr, address->port, address->type,
            address->socket_id, address->protocol, config, profile);
        if (rc) goto fail;
        written++;
    }
    for (const struct TOPO_ADDR6 *address = node->v6_addrs; address; address = address->next) {
        uint8_t config;
        const struct address_profile *profile = smeta6_lookup(node->v6_sock_meta, address->socket_id, &config);
        rc = address_row_put(statement, node, node_id, 6, address->addr, address->port, address->type,
            address->socket_id, address->protocol, config, profile);
        if (rc) goto fail;
        written++;
    }
    rc = sqlite3_finalize(statement); statement = NULL; if (rc) goto fail;
    rc = sqlite3_exec(db, "DROP TABLE temp.node_address_rtt; RELEASE node_address_projection", NULL, NULL, NULL); if (rc) goto fail;
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "address projection stored node=%016llx endpoints=%d",
                (unsigned long long)node_id, written);
    return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_BGP, "address projection failed node=%016llx rc=%d: %s", (unsigned long long)node_id, rc, sqlite3_errmsg(db));
    sqlite3_finalize(statement);
    if (sqlite3_exec(db, "ROLLBACK TO node_address_projection; RELEASE node_address_projection", NULL, NULL, NULL) != SQLITE_OK)
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "address projection rollback failed: %s", sqlite3_errmsg(db));
    return rc;
}

int topo_node_sqlite_addrs_put(sqlite3 *db, uint64_t node_id, struct TOPO_NODE *node) {
    return topo_node_sqlite_addrs_write(db, node_id, node) ? -1 : 0;
}

int topo_node_sqlite_member_exists(sqlite3* db, const uint8_t ed[32]) {
    if (!db || !ed) return 0;
    sqlite3_stmt* s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'peers\\_%' ESCAPE '\\'",
            -1, &s, NULL) != SQLITE_OK) return 0;
    char sql[256]; int found = 0;
    while (!found && sqlite3_step(s) == SQLITE_ROW) {
        const char* tbl = (const char*)sqlite3_column_text(s, 0);
        if (!tbl) continue;
        snprintf(sql, sizeof(sql), "SELECT 1 FROM \"%s\" WHERE ed25519_pubkey=? AND record_state=0", tbl);
        sqlite3_stmt* m = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &m, NULL) == SQLITE_OK) {
            sqlite3_bind_blob(m, 1, ed, 32, SQLITE_STATIC);
            if (sqlite3_step(m) == SQLITE_ROW) found = 1;
            sqlite3_finalize(m);
        }
    }
    sqlite3_finalize(s);
    return found;
}

int topo_node_sqlite_member_channels_read(sqlite3* db, const uint8_t ed[32],
                                         uint8_t (**out_ch_ids)[32], int* out_count) {
    if (out_ch_ids) *out_ch_ids = NULL;
    if (out_count) *out_count = 0;
    if (!db || !ed || !out_ch_ids || !out_count) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "member channels: invalid arguments"); return SQLITE_MISUSE; }

    sqlite3_stmt* s = NULL;
    int rc = sqlite3_prepare_v2(db,
            "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'peers\\_%' ESCAPE '\\'",
            -1, &s, NULL);
    if (rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "member channels: enumerate tables rc=%d: %s", rc, sqlite3_errmsg(db)); return rc;
    }

    uint8_t (*ids)[32] = NULL; int count = 0, cap = 0;
    char sql[256];
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        const char* tbl = (const char*)sqlite3_column_text(s, 0);
        if (!tbl) { rc = SQLITE_NOMEM; break; }
        if (strncmp(tbl, "peers_", 6) || !chat_channel_valid(tbl + 6)) continue;
        snprintf(sql, sizeof(sql), "SELECT 1 FROM \"%s\" WHERE ed25519_pubkey=? AND record_state=0", tbl);
        sqlite3_stmt* m = NULL;
        int member_rc = sqlite3_prepare_v2(db, sql, -1, &m, NULL);
        if (member_rc == SQLITE_OK) member_rc = sqlite3_bind_blob(m, 1, ed, 32, SQLITE_STATIC);
        if (member_rc == SQLITE_OK) member_rc = sqlite3_step(m);
        if (member_rc != SQLITE_ROW && member_rc != SQLITE_DONE) {
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "member channels: table=%s member=%s rc=%d: %s",
                        tbl, sc_public_key_hex(ed, (char[65]){0}), member_rc, sqlite3_errmsg(db));
            sqlite3_finalize(m); u_free(ids); sqlite3_finalize(s); return member_rc;
        }
        {
            int is_member = member_rc == SQLITE_ROW;
            member_rc = sqlite3_finalize(m);
            if (member_rc) {
                DEBUG_ERROR(DEBUG_CATEGORY_BGP, "member channels finalize rc=%d", member_rc);
                u_free(ids); sqlite3_finalize(s); return member_rc;
            }
            if (is_member) {
                {
                    if (count == cap) {
                        cap = cap ? cap * 2 : 8;
                        uint8_t (*tmp)[32] = u_realloc(ids, (size_t)cap * sizeof(*ids));
                        if (!tmp) {
                            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "member channels: allocation failed count=%d", count);
                            u_free(ids); sqlite3_finalize(s); return SQLITE_NOMEM;
                        }
                        ids = tmp;
                    }
                    sc_hex_to_binary(tbl + 6, ids[count++], sizeof(*ids));
                }
            }
        }
    }
    if (rc != SQLITE_DONE) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "member channels: enumeration failed member=%s rc=%d: %s",
                    sc_public_key_hex(ed, (char[65]){0}), rc, sqlite3_errmsg(db));
        u_free(ids); sqlite3_finalize(s); return rc;
    }
    rc = sqlite3_finalize(s);
    if (rc) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "member channels scan finalize rc=%d", rc); u_free(ids); return rc; }
    *out_ch_ids = ids; *out_count = count;
    return 0;
}

int topo_node_sqlite_get_member_channels(sqlite3* db, const uint8_t ed[32], uint8_t (**out)[32], int* count) {
    return topo_node_sqlite_member_channels_read(db, ed, out, count) ? -1 : 0;
}

int topo_node_sqlite_init(sqlite3* db) {
    if (db_schema_init(db)) return -1;
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "topo_node_sqlite tables initialized without migrations");
    return 0;
}

static int member_local_init_result(sqlite3* db, const char* channel_id) {
    if (!db || !channel_id || !channel_id[0]) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "invalid local member schema arguments"); return SQLITE_MISUSE;
    }
    char table[80]; peers_table_name(channel_id, table, sizeof(table));
    return db_schema_table_ex(db, table, "peers");
}

int topo_node_sqlite_member_local_init_write(sqlite3* db, const char* channel_id) {
    if (!db || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "local member schema requires caller WRITE"); return SQLITE_MISUSE;
    }
    return member_local_init_result(db, channel_id);
}

int topo_node_sqlite_member_local_init(sqlite3* db, const char* channel_id) {
    return member_local_init_result(db, channel_id) == SQLITE_OK ? 0 : -1;
}

int topo_node_sqlite_channel_put(sqlite3* db, const char* channel_id,
    const char* name, const uint8_t owner_node_id[32],
    const uint8_t* x25519_pub, const uint8_t* x25519_priv,
    const uint8_t* ed25519_pub, const uint8_t* ed25519_priv,
    const uint8_t* signature) {
    char json[GROUP_INFO_MAX + 1];
    if (group_info_initial(name, json)) return -1;
    return topo_node_sqlite_channel_put_description(db, channel_id, json, owner_node_id,
        x25519_pub, x25519_priv, ed25519_pub, ed25519_priv, signature);
}

static int channel_put_description_result(sqlite3* db, const char* channel_id,
    const char* json, const uint8_t owner_node_id[32],
    const uint8_t* x25519_pub, const uint8_t* x25519_priv,
    const uint8_t* ed25519_pub, const uint8_t* ed25519_priv,
    const uint8_t* signature) {
    struct group_info_profile profile;
    if (!db || !channel_id || !json || !owner_node_id || !x25519_pub || !ed25519_pub || !signature) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "channel put: invalid database/description arguments"); return SQLITE_MISUSE;
    }
    int parsed = group_info_parse(db, json, &profile); if (parsed) return -parsed;
    int rc = sqlite3_exec(db, "SAVEPOINT channel_put", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "channel put: begin channel=%s rc=%d: %s", channel_id, rc, sqlite3_errmsg(db)); return rc;
    }
    sqlite3_stmt* stmt = NULL;
    const char* stage = "allocate peers schema";
    int newer = 1;
    rc = group_info_import_order(db, channel_id, json, signature, &newer); if (rc) goto error;

    char peers_tbl[80]; peers_table_name(channel_id, peers_tbl, sizeof(peers_tbl));

    stage = "member schema";
    rc = db_schema_table_ex(db, peers_tbl, "peers"); if (rc) goto error;

    stage = "prepare channel";
    rc = sqlite3_prepare_v2(db,
            "INSERT INTO channels(channel_id, description_json, owner_node_id,"
            " x25519_pubkey, x25519_privkey, ed25519_pubkey, ed25519_privkey, signature)"
            " VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(channel_id) DO UPDATE SET"
            " description_json=CASE WHEN ?9 THEN excluded.description_json ELSE channels.description_json END,"
            " signature=CASE WHEN ?9 THEN excluded.signature ELSE channels.signature END,"
            " x25519_privkey=COALESCE(excluded.x25519_privkey,channels.x25519_privkey),"
            " ed25519_privkey=COALESCE(excluded.ed25519_privkey,channels.ed25519_privkey)"
            " WHERE channels.owner_node_id=excluded.owner_node_id AND channels.x25519_pubkey=excluded.x25519_pubkey"
            " AND channels.ed25519_pubkey=excluded.ed25519_pubkey",
            -1, &stmt, NULL);
    if (rc == SQLITE_OK) { stage = "bind channel"; rc = sqlite3_bind_text(stmt, 1, channel_id, -1, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "bind description"; rc = sqlite3_bind_text(stmt, 2, json, -1, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "bind owner"; rc = sqlite3_bind_blob(stmt, 3, owner_node_id, 32, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "bind X25519 public"; rc = sqlite3_bind_blob(stmt, 4, x25519_pub, 32, SQLITE_STATIC); }
    if (rc == SQLITE_OK) {
        stage = "bind X25519 private";
        rc = x25519_priv ? sqlite3_bind_blob(stmt, 5, x25519_priv, 32, SQLITE_STATIC) : sqlite3_bind_null(stmt, 5);
    }
    if (rc == SQLITE_OK) { stage = "bind Ed25519 public"; rc = sqlite3_bind_blob(stmt, 6, ed25519_pub, 32, SQLITE_STATIC); }
    if (rc == SQLITE_OK) {
        stage = "bind Ed25519 private";
        rc = ed25519_priv ? sqlite3_bind_blob(stmt, 7, ed25519_priv, 32, SQLITE_STATIC) : sqlite3_bind_null(stmt, 7);
    }
    if (rc == SQLITE_OK) { stage = "bind signature"; rc = sqlite3_bind_blob(stmt, 8, signature, 64, SQLITE_STATIC); }
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 9, newer);
    if (rc == SQLITE_OK) { stage = "write channel"; rc = sqlite3_step(stmt); }
    if (rc != SQLITE_DONE) {
        if ((rc & 255) == SQLITE_CONSTRAINT && sqlite3_extended_errcode(db) == SQLITE_CONSTRAINT_UNIQUE) {
            stage = "public key already belongs to another channel"; rc = SQLITE_CONSTRAINT_UNIQUE;
        }
        goto error;
    }
    stage = "channel row count";
    if (sqlite3_changes(db) != 1) { rc = SQLITE_CONSTRAINT; goto error; }
    stage = "finalize channel"; rc = sqlite3_finalize(stmt); stmt = NULL;
    if (rc != SQLITE_OK) goto error;
    if (newer) {
        stage = "index description";
        int indexed = member_sync_reindex(db, channel_id, NULL);
        if (indexed < 0) { rc = -indexed; goto error; }
    }
    stage = "release channel"; rc = sqlite3_exec(db, "RELEASE channel_put", NULL, NULL, NULL);
    if (rc == SQLITE_OK) return 0;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "channel put: channel=%s stage=%s rc=%d (%s) db=%s",
        channel_id, stage, rc, sqlite3_errstr(rc), sqlite3_errmsg(db));
    {
        int final = sqlite3_finalize(stmt);
        if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "channel put: cleanup finalize channel=%s rc=%d", channel_id, final);
    }
    if (sqlite3_get_autocommit(db)) return rc;
    int rollback = sqlite3_exec(db, "ROLLBACK TO channel_put", NULL, NULL, NULL);
    if (rollback != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "channel put: rollback channel=%s rc=%d primary=%d; caller must rollback/close", channel_id, rollback, rc);
        return rc;
    }
    rollback = sqlite3_exec(db, "RELEASE channel_put", NULL, NULL, NULL);
    if (rollback != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "channel put: rollback release channel=%s rc=%d primary=%d", channel_id, rollback, rc);
    return rc;
}

int topo_node_sqlite_channel_put_description_write(sqlite3* db, const char* channel_id,
    const char* json, const uint8_t owner_node_id[32], const uint8_t* x25519_pub, const uint8_t* x25519_priv,
    const uint8_t* ed25519_pub, const uint8_t* ed25519_priv, const uint8_t* signature) {
    if (!db || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "channel description write requires caller WRITE"); return SQLITE_MISUSE;
    }
    return channel_put_description_result(db, channel_id, json, owner_node_id, x25519_pub, x25519_priv, ed25519_pub, ed25519_priv, signature);
}

int topo_node_sqlite_channel_put_description(sqlite3* db, const char* channel_id,
    const char* json, const uint8_t owner_node_id[32], const uint8_t* x25519_pub, const uint8_t* x25519_priv,
    const uint8_t* ed25519_pub, const uint8_t* ed25519_priv, const uint8_t* signature) {
    return channel_put_description_result(db, channel_id, json, owner_node_id, x25519_pub, x25519_priv,
        ed25519_pub, ed25519_priv, signature) == SQLITE_OK ? 0 : -1;
}

/* Удаляет канал и производные таблицы целиком; savepoint сохраняет внешнюю транзакцию. */
int topo_node_sqlite_channel_delete_write(sqlite3* db, const char* channel_id) {
    if (!db || !chat_channel_valid(channel_id) || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "channel delete: requires valid arguments and caller WRITE"); return SQLITE_MISUSE;
    }
    struct db_changes_origin source = db_changes_source(db, NULL, 0, "group removal");
    int rc;
    sqlite3_stmt* st = NULL;
    char san[65], sql[1024]; sanitize_ch_id(channel_id, san, sizeof(san));
    static const char* tables[] = {"peers", "msg", "archive_msg", "feedback"};
    for (size_t i = 0; i < sizeof(tables)/sizeof(*tables); ++i) {
        char name[96]; snprintf(name, sizeof(name), "%s_%s", tables[i], san);
        db_changes_before_drop(db, name);
    }
    snprintf(sql, sizeof(sql), "DROP VIEW IF EXISTS \"history_msg_%s\";"
             "DROP TABLE IF EXISTS \"peers_%s\"; DROP TABLE IF EXISTS \"msg_%s\";"
             "DROP TABLE IF EXISTS \"archive_msg_%s\"; DROP TABLE IF EXISTS \"archive_feedback_%s\";"
             "DROP TABLE IF EXISTS \"feedback_%s\"", san, san, san, san, san, san);
    if ((rc = sqlite3_exec(db, sql, NULL, NULL, NULL)) != SQLITE_OK) goto fail;
    rc = sqlite3_prepare_v2(db, "DELETE FROM group_policy WHERE channel_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?)", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, channel_id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) goto fail;
    rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto fail;
    const char* dependent[] = {
        "DELETE FROM member_invitations WHERE channel=?",
        "DELETE FROM member_sync_items WHERE channel=?",
        "DELETE FROM chat_join_recovery WHERE channel=?"
    };
    for (size_t i = 0; i < sizeof(dependent) / sizeof(dependent[0]); i++) {
        rc = sqlite3_prepare_v2(db, dependent[i], -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, channel_id, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) goto fail;
        rc = sqlite3_finalize(st); st = NULL;
        if (rc != SQLITE_OK) goto fail;
    }
    rc = sqlite3_prepare_v2(db, "DELETE FROM channels WHERE channel_id=?", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, channel_id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) goto fail;
    rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto fail;
    /* IGNORE может вернуть DONE, сохранив канал: это не успешное удаление. */
    rc = sqlite3_prepare_v2(db, "SELECT 1 FROM channels WHERE channel_id=?", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, channel_id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) goto fail;
    rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto fail;
    rc = sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='merkle_tree_hash'", -1, &st, NULL);
    if (rc != SQLITE_OK) goto fail;
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) goto fail;
    int has_merkle = rc == SQLITE_ROW;
    rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto fail;
    if (has_merkle) {
        rc = sqlite3_prepare_v2(db, "DELETE FROM merkle_tree_hash WHERE substr(namespace,instr(namespace,':')+1)=?", -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, channel_id, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) goto fail;
        rc = sqlite3_finalize(st); st = NULL;
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_prepare_v2(db, "SELECT 1 FROM merkle_tree_hash WHERE substr(namespace,instr(namespace,':')+1)=? LIMIT 1",
            -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, channel_id, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) goto fail;
        rc = sqlite3_finalize(st); st = NULL;
        if (rc != SQLITE_OK) goto fail;
    }
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "channel deleted: channel=%s tables=peers/msg/feedback merkle=%d", channel_id, has_merkle);
    db_changes_restore(db, source);
    return 0;
fail:
    db_changes_restore(db, source);
    if (rc == SQLITE_ROW || rc == SQLITE_DONE || rc == SQLITE_OK) rc = SQLITE_CONSTRAINT;
    DEBUG_ERROR(DEBUG_CATEGORY_BGP, "channel delete failed: channel=%s rc=%d: %s", channel_id, rc, sqlite3_errmsg(db));
    int final = sqlite3_finalize(st);
    if (final) DEBUG_ERROR(DEBUG_CATEGORY_BGP, "channel delete cleanup: channel=%s rc=%d", channel_id, final);
    return rc;
}

int topo_node_sqlite_channel_delete(sqlite3* db, const char* channel_id) {
    if (!db || !chat_channel_valid(channel_id)) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "channel delete: invalid arguments"); return -1;
    }
    int rc = sqlite3_exec(db, "SAVEPOINT delete_topo_channel", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "channel delete savepoint: channel=%s rc=%d: %s", channel_id, rc, sqlite3_errmsg(db)); return -1;
    }
    rc = topo_node_sqlite_channel_delete_write(db, channel_id);
    if (!rc) rc = sqlite3_exec(db, "RELEASE delete_topo_channel", NULL, NULL, NULL);
    if (!rc) return 0;
    if (!sqlite3_get_autocommit(db) && sqlite3_exec(db, "ROLLBACK TO delete_topo_channel; RELEASE delete_topo_channel", NULL, NULL, NULL) != SQLITE_OK)
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "channel delete rollback failed: channel=%s: %s", channel_id, sqlite3_errmsg(db));
    return -1;
}

/* Упрощённый плейсхолдер мембера от topo_group: только node_id + pubkeys, source=1.
 * Не участвует в merkle-хеше. Если запись уже есть (полная merkle) — не трогает её.
 * Позже полная merkle-запись дополняет/перезаписывает блоки и сбрасывает source=0. */
int topo_node_sqlite_member_placeholder_write(sqlite3* db, const char* channel_id, uint64_t node_id,
                                            const uint8_t* x25519_pubkey, const uint8_t* ed25519_pubkey) {
    if (!db || !channel_id || !x25519_pubkey || !ed25519_pubkey) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "member placeholder: invalid arguments"); return SQLITE_MISUSE;
    }
    char peers_tbl[80]; peers_table_name(channel_id, peers_tbl, sizeof(peers_tbl));
    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT INTO \"%s\"(node_id, x25519_pubkey, ed25519_pubkey, join_sig, join_ts, source)"
        " VALUES(?,?,?,?,0,1)"
        " ON CONFLICT(ed25519_pubkey,record_state) DO NOTHING", peers_tbl);
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (!rc) rc = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)node_id);
    if (!rc) rc = sqlite3_bind_blob(stmt, 2, x25519_pubkey, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(stmt, 3, ed25519_pubkey, 32, SQLITE_STATIC);
    static const unsigned char zsig[PEERS_JOIN_SIG_SIZE] = {0};
    if (!rc) rc = sqlite3_bind_blob(stmt, 4, zsig, PEERS_JOIN_SIG_SIZE, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(stmt);
    int final = sqlite3_finalize(stmt); if (rc == SQLITE_DONE) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_BGP, "member placeholder channel=%s node=%016llx rc=%d: %s",
        channel_id, (unsigned long long)node_id, rc, sqlite3_errmsg(db));
    return rc;
}

int topo_node_sqlite_member_placeholder_put(sqlite3* db, const char* channel_id, uint64_t node_id,
    const uint8_t* x25519, const uint8_t* ed25519) {
    return topo_node_sqlite_member_placeholder_write(db, channel_id, node_id, x25519, ed25519) ? -1 : 0;
}

/* Обновляет только блок мембера (keys/join/update_sig/update_ts/userinfo),
 * НЕ трогая adm_tags/adm_tags_sig/storage (блок владельца) и локальные поля. */
int topo_node_sqlite_member_block_put(sqlite3* db, const char* channel_id, uint64_t node_id,
                                       const uint8_t* join_sig, uint64_t join_ts,
                                       const uint8_t* update_sig, uint64_t update_ts,
                                       const uint8_t* x25519_pubkey, const uint8_t* ed25519_pubkey,
                                       const char* userinfo,
                                       const uint8_t* signed_by, const uint8_t* signature) {
    if (!db || !channel_id || !x25519_pubkey || !ed25519_pubkey) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member block: invalid database/channel/key arguments"); return -1;
    }
    char peers_tbl[80]; peers_table_name(channel_id, peers_tbl, sizeof(peers_tbl));
    char sql[768];
    snprintf(sql, sizeof(sql),
        "INSERT INTO \"%s\"(node_id, x25519_pubkey, ed25519_pubkey,"
        " join_sig, join_ts, update_sig, update_ts, userinfo, signed_by, signature)"
        " VALUES(?,?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(ed25519_pubkey,record_state) DO UPDATE SET"
        "  node_id=excluded.node_id,"
        "  x25519_pubkey=excluded.x25519_pubkey,"
        "  join_sig=excluded.join_sig,"
        "  join_ts=excluded.join_ts,"
        "  update_sig=excluded.update_sig,"
        "  update_ts=excluded.update_ts,"
        "  userinfo=excluded.userinfo,"
        "  signed_by=excluded.signed_by,"
        "  signature=excluded.signature,"
        "  source=0", peers_tbl);
    sqlite3_stmt* stmt = NULL;
    const char* stage = "prepare";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) { stage = "bind node"; rc = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)node_id); }
    if (rc == SQLITE_OK) { stage = "bind X25519"; rc = sqlite3_bind_blob(stmt, 2, x25519_pubkey, 32, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "bind Ed25519"; rc = sqlite3_bind_blob(stmt, 3, ed25519_pubkey, 32, SQLITE_STATIC); }
    if (rc == SQLITE_OK) {
        static const uint8_t zero_signature[PEERS_JOIN_SIG_SIZE] = {0}; stage = "bind JOIN signature";
        rc = sqlite3_bind_blob(stmt, 4, join_sig ? join_sig : zero_signature, PEERS_JOIN_SIG_SIZE, SQLITE_STATIC);
    }
    if (rc == SQLITE_OK) { stage = "bind JOIN timestamp"; rc = sqlite3_bind_int64(stmt, 5, (sqlite3_int64)join_ts); }
    if (rc == SQLITE_OK) {
        stage = "bind profile signature";
        rc = update_sig ? sqlite3_bind_blob(stmt, 6, update_sig, PEERS_UPDATE_SIG_SIZE, SQLITE_STATIC) : sqlite3_bind_null(stmt, 6);
    }
    if (rc == SQLITE_OK) { stage = "bind profile timestamp"; rc = sqlite3_bind_int64(stmt, 7, (sqlite3_int64)update_ts); }
    if (rc == SQLITE_OK) { stage = "bind profile"; rc = sqlite3_bind_text(stmt, 8, userinfo ? userinfo : "", -1, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "bind inviter"; rc = sqlite3_bind_blob(stmt, 9, signed_by, 32, SQLITE_STATIC); }
    if (rc == SQLITE_OK) {
        stage = "bind invitation signature";
        rc = signature ? sqlite3_bind_blob(stmt, 10, signature, 64, SQLITE_STATIC) : sqlite3_bind_null(stmt, 10);
    }
    if (rc == SQLITE_OK) { stage = "write"; rc = sqlite3_step(stmt); }
    int changed = rc == SQLITE_DONE ? sqlite3_changes(db) : 0;
    if (rc != SQLITE_DONE || changed != 1) DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC,
        "member block: channel=%s node=%016llx stage=%s rc=%d (%s) changed=%d db=%s",
        channel_id, (unsigned long long)node_id, stage, rc, sqlite3_errstr(rc), changed, sqlite3_errmsg(db));
    int final = sqlite3_finalize(stmt);
    if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member block: finalize channel=%s node=%016llx rc=%d",
        channel_id, (unsigned long long)node_id, final);
    return rc == SQLITE_DONE && changed == 1 && final == SQLITE_OK ? 0 : -1;
}

/* Обновляет только блок владельца (adm_tags/adm_tags_sig/storage),
 * НЕ трогая блок мембера. */
int topo_node_sqlite_member_owner_put(sqlite3* db, const char* channel_id, const uint8_t ed[32],
                                       const char* adm_tags, const uint8_t* adm_tags_sig, int storage) {
    if (!db || !channel_id || !ed) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member owner: missing database/channel/Ed25519"); return -1; }
    char peers_tbl[80]; peers_table_name(channel_id, peers_tbl, sizeof(peers_tbl));
    char sql[256];
    snprintf(sql, sizeof(sql),
        "UPDATE \"%s\" SET adm_tags=?, adm_tags_sig=?, storage=?, deleted=? WHERE ed25519_pubkey=? AND record_state=0", peers_tbl);
    struct node_owner_flags flags = {0};
    if (adm_tags && adm_tags[0] && json_flat_parse(adm_tags, node_owner_flag, &flags)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member owner flags unavailable ch=%s ed=%s", channel_id, sc_public_key_hex(ed, (char[65]){0}));
        return -1;
    }
    sqlite3_stmt* u = NULL;
    const char* stage = "prepare"; int rc = sqlite3_prepare_v2(db, sql, -1, &u, NULL);
    if (rc == SQLITE_OK) {
        stage = "bind tags"; rc = adm_tags && adm_tags[0] ? sqlite3_bind_text(u, 1, adm_tags, -1, SQLITE_STATIC) : sqlite3_bind_null(u, 1);
    }
    if (rc == SQLITE_OK) { stage = "bind signature"; rc = adm_tags_sig ? sqlite3_bind_blob(u, 2, adm_tags_sig, 64, SQLITE_STATIC) : sqlite3_bind_null(u, 2); }
    if (rc == SQLITE_OK) { stage = "bind storage"; rc = sqlite3_bind_int(u, 3, storage); }
    if (rc == SQLITE_OK) { stage = "bind deleted"; rc = sqlite3_bind_int(u, 4, flags.deleted); }
    if (rc == SQLITE_OK) { stage = "bind member"; rc = sqlite3_bind_blob(u, 5, ed, 32, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "step"; rc = sqlite3_step(u); }
    if (rc == SQLITE_DONE && sqlite3_changes(db) != 1) { stage = "conflicting node key or suppressed write"; rc = SQLITE_CONSTRAINT; }
    int final = sqlite3_finalize(u);
    if (rc != SQLITE_DONE || final != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member owner: ch=%s ed=%s stage=%s rc=%d finalize=%d: %s",
            channel_id, sc_public_key_hex(ed, (char[65]){0}), stage, rc, final, sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

/* Записывает уже проверенную вызывающим идентичность; любой отказ SQL возвращается вызывающему. */
int topo_node_sqlite_node_write(sqlite3* db, uint64_t node_id,
    const char* name, const uint8_t* x25519, const uint8_t* ed25519, uint64_t update_ts, time_t created_at) {
    if (!db || !x25519 || !ed25519) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "verified node write: invalid arguments node=%016llx", (unsigned long long)node_id);
        return SQLITE_MISUSE;
    }
    sqlite3_stmt* stmt = NULL;
    const char* stage = "prepare";
    int rc = sqlite3_prepare_v2(db,
            "INSERT INTO nodes(node_id, name, x25519_pubkey, ed25519_pubkey, created_at, update_ts, online)"
            " VALUES(?,?,?,?,?,?,1)"
            " ON CONFLICT(x25519_pubkey) DO UPDATE SET"
            "  name=CASE WHEN excluded.name IS NOT NULL AND excluded.name != ''"
            "            THEN excluded.name ELSE nodes.name END,"
            "  ed25519_pubkey=excluded.ed25519_pubkey,"
            "  created_at=COALESCE(nodes.created_at, excluded.created_at),"
            "  update_ts=CASE WHEN nodes.update_ts IS NULL OR excluded.update_ts > nodes.update_ts"
            "                THEN excluded.update_ts ELSE nodes.update_ts END"
            " WHERE nodes.ed25519_pubkey IS NULL OR nodes.ed25519_pubkey=zeroblob(32) OR nodes.ed25519_pubkey=excluded.ed25519_pubkey",
            -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        stage = "bind";
        rc = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)node_id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, name && name[0] ? name : "", -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(stmt, 3, x25519, 32, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(stmt, 4, ed25519, 32, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 5, (sqlite3_int64)created_at);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 6, (sqlite3_int64)update_ts);
    }
    if (rc == SQLITE_OK) { stage = "step"; rc = sqlite3_step(stmt); }
    if (rc == SQLITE_DONE && sqlite3_changes(db) != 1) { stage = "changes != 1"; rc = SQLITE_CONSTRAINT; }
    if (rc != SQLITE_DONE) DEBUG_ERROR(DEBUG_CATEGORY_BGP, "verified node write: node=%016llx stage=%s rc=%d: %s",
        (unsigned long long)node_id, stage, rc, sqlite3_errmsg(db));
    int final_rc = sqlite3_finalize(stmt);
    if (final_rc != SQLITE_OK && rc == SQLITE_DONE) DEBUG_ERROR(DEBUG_CATEGORY_BGP,
        "verified node write: node=%016llx stage=finalize rc=%d: %s", (unsigned long long)node_id, final_rc, sqlite3_errmsg(db));
    if (rc != SQLITE_DONE || final_rc != SQLITE_OK) return rc != SQLITE_DONE ? rc : final_rc;
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "verified node stored: node=%016llx timestamp=%llu changed=%d",
                (unsigned long long)node_id, (unsigned long long)update_ts, sqlite3_changes(db));
    return 0;
}

int topo_node_sqlite_node_update_verified(sqlite3* db, uint64_t node_id,
    const char* name, const uint8_t* x25519, const uint8_t* ed25519, uint64_t update_ts, time_t created_at) {
    return topo_node_sqlite_node_write(db, node_id, name, x25519, ed25519, update_ts, created_at) ? -1 : 0;
}

int topo_node_sqlite_member_join_read(sqlite3* db, const char* channel_id, const uint8_t ed[32],
    uint8_t signature[PEERS_JOIN_SIG_SIZE], uint64_t* epoch, int* found) {
    if (!db || !channel_id || !*channel_id || !ed || !signature || !epoch || !found) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "member JOIN read: invalid database/channel/node/output"); return SQLITE_MISUSE;
    }
    *found = 0;
    char peers_tbl[80], sql[256]; peers_table_name(channel_id, peers_tbl, sizeof(peers_tbl));
    snprintf(sql, sizeof(sql), "SELECT join_sig, join_ts FROM \"%s\" WHERE ed25519_pubkey=? AND record_state=0 AND source=0", peers_tbl);
    sqlite3_stmt* stmt = NULL; uint8_t saved[PEERS_JOIN_SIG_SIZE]; uint64_t timestamp = 0;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (!rc) rc = sqlite3_bind_blob(stmt, 1, ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    else if (rc == SQLITE_ROW) {
        if (sqlite3_column_type(stmt, 0) != SQLITE_BLOB || sqlite3_column_bytes(stmt, 0) != sizeof(saved) ||
            sqlite3_column_type(stmt, 1) != SQLITE_INTEGER || sqlite3_column_int64(stmt, 1) < 0) rc = SQLITE_CORRUPT;
        else {
            const void* blob = sqlite3_column_blob(stmt, 0);
            if (!blob) rc = SQLITE_NOMEM;
            else {
                memcpy(saved, blob, sizeof(saved)); timestamp = (uint64_t)sqlite3_column_int64(stmt, 1);
                int has_signature = memcmp(saved, (uint8_t[PEERS_JOIN_SIG_SIZE]){0}, sizeof(saved)) != 0;
                rc = has_signature || !timestamp ? SQLITE_OK : SQLITE_CORRUPT;
            }
        }
        if (!rc) {
            int next = sqlite3_step(stmt);
            if (next != SQLITE_DONE) rc = next == SQLITE_ROW ? SQLITE_CORRUPT : next;
        }
    }
    int final = sqlite3_finalize(stmt); if (!rc) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_BGP, "member JOIN read ch=%s ed=%s rc=%d finalize=%d (%s)",
        channel_id, sc_public_key_hex(ed, (char[65]){0}), rc, final, sqlite3_errstr(rc));
    else if (timestamp) { memcpy(signature, saved, sizeof(saved)); *epoch = timestamp; *found = 1; }
    return rc;
}

int topo_node_sqlite_member_get_join(sqlite3* db, const char* channel_id, const uint8_t ed[32], uint8_t* signature, uint64_t* epoch) {
    int found; int rc = topo_node_sqlite_member_join_read(db, channel_id, ed, signature, epoch, &found);
    return rc ? -rc : found ? 0 : -SQLITE_NOTFOUND;
}

int topo_node_sqlite_read_optional_key(sqlite3_stmt* st, int column, uint8_t out[32]) {
    if (sqlite3_column_type(st, column) == SQLITE_NULL) { memset(out, 0, 32); return SQLITE_OK; }
    if (sqlite3_column_type(st, column) == SQLITE_BLOB && sqlite3_column_bytes(st, column) == 32) {
        const void* key = sqlite3_column_blob(st, column);
        if (!key) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "public key allocation failed column=%d", column); return SQLITE_NOMEM; }
        if (memcmp(key, (uint8_t[32]){0}, 32)) { memcpy(out, key, 32); return SQLITE_OK; }
    }
    DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "malformed public key column=%d", column);
    return SQLITE_CORRUPT;
}

int topo_node_sqlite_member_get_sign(sqlite3* db, const char* channel_id, const uint8_t ed[32],
    uint8_t signed_by_out[32], uint8_t signature_out[64]) {
    if (!db || !channel_id || !ed || !signed_by_out || !signature_out) return -1;
    char peers_tbl[80]; peers_table_name(channel_id, peers_tbl, sizeof(peers_tbl));
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT signed_by, signature FROM \"%s\" WHERE ed25519_pubkey=? AND record_state=0", peers_tbl);
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_blob(stmt, 1, ed, 32, SQLITE_STATIC);
    int rc = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int code = topo_node_sqlite_read_optional_key(stmt, 0, signed_by_out);
        const uint8_t* sig = (const uint8_t*)sqlite3_column_blob(stmt, 1);
        if (!code && sig && sqlite3_column_bytes(stmt, 1) == 64) { memcpy(signature_out, sig, 64); rc = 0; }
    }
    sqlite3_finalize(stmt);
    return rc;
}

int topo_node_sqlite_member_in_channel(sqlite3* db, const char* channel_id, uint64_t node_id) {
    if (!db || !channel_id || node_id == 0) return 0;
    char peers_tbl[80]; peers_table_name(channel_id, peers_tbl, sizeof(peers_tbl));
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT 1 FROM \"%s\" WHERE node_id=? AND record_state=0", peers_tbl);
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)node_id);
    int found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}

static int member_access(sqlite3* db, const char* channel_id, uint64_t node_id, int admitted) {
    if (!db || !channel_id || !channel_id[0] || !node_id) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "member admission: invalid channel/node"); return 0;
    }
    char table[80], sql[448]; peers_table_name(channel_id, table, sizeof(table));
    snprintf(sql, sizeof(sql), "SELECT 1 FROM \"%s\" WHERE node_id=?1 AND record_state=0 AND source=0 AND deleted=0%s"
        " AND 1=(SELECT count(*) FROM \"%s\" WHERE node_id=?1 AND record_state=0)",
        table, admitted ? " AND admitted=1" : "", table);
    sqlite3_stmt* statement = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(statement, 1, (sqlite3_int64)node_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    int allowed = rc == SQLITE_ROW;
    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "member admission read failed channel=%s node=%016llx rc=%d: %s",
                    channel_id, (unsigned long long)node_id, rc, sqlite3_errmsg(db));
    int final = sqlite3_finalize(statement);
    if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_BGP, "member admission finalize failed channel=%s rc=%d", channel_id, final);
    return allowed && final == SQLITE_OK;
}

int topo_node_sqlite_member_allowed(sqlite3* db, const char* channel_id, uint64_t node_id) {
    return member_access(db, channel_id, node_id, 1);
}

/* Проекция доверия пересчитывается с MEMBERS; BGP не может заполнить этот результат. */
static int member_keys(sqlite3* db, const char* channel, uint64_t node, const uint8_t author[32], const uint8_t* peer_x, int active,
    uint8_t x[32], uint8_t ed[32], uint64_t* local_id, int* error) {
    if (error) *error = SQLITE_OK;
    if (!db || !chat_channel_valid(channel) || (!node && !author && !peer_x) || (!x && !ed && !local_id) || (active != 0 && active != 1)) {
        if (error) *error = SQLITE_MISUSE;
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member keys: invalid arguments"); return TOPO_SQLITE_LOOKUP_ERROR;
    }
    char sql[448];
    const char* identity = author ? "ed25519_pubkey" : peer_x ? "x25519_pubkey" : "node_id";
    const uint8_t* key = author ? author : peer_x;
    snprintf(sql, sizeof(sql), "SELECT x25519_pubkey,ed25519_pubkey,node_id FROM peers_%s"
        " WHERE %s=?1 AND record_state=0 AND source=0 AND identity_verified=1%s"
        " AND 1=(SELECT count(*) FROM peers_%s WHERE %s=?1 AND record_state=0)",
        channel, identity, active ? " AND admitted=1 AND deleted=0" : "", channel, identity);
    sqlite3_stmt* st = NULL; uint8_t keys[64];
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL), result = TOPO_SQLITE_LOOKUP_ERROR;
    if (rc == SQLITE_OK) rc = key ? sqlite3_bind_blob(st, 1, key, 32, SQLITE_STATIC) : sqlite3_bind_int64(st, 1, (sqlite3_int64)node);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) result = TOPO_SQLITE_LOOKUP_MISSING;
    else if (rc == SQLITE_ROW) {
        const void* xp = sqlite3_column_blob(st, 0); const void* ep = sqlite3_column_blob(st, 1);
        if (xp && ep && sqlite3_column_bytes(st, 0) == 32 && sqlite3_column_bytes(st, 1) == 32) {
            memcpy(keys, xp, 32); memcpy(keys + 32, ep, 32);
            node = (uint64_t)sqlite3_column_int64(st, 2);
            result = 0; /* Привязка ID и подписи уже проверены при публикации identity_verified. */
        } else rc = sqlite3_errcode(db) == SQLITE_NOMEM ? SQLITE_NOMEM : SQLITE_CORRUPT;
    }
    int final = sqlite3_finalize(st);
    if (final != SQLITE_OK) result = TOPO_SQLITE_LOOKUP_ERROR;
    if (result == TOPO_SQLITE_LOOKUP_ERROR) {
        int code = rc != SQLITE_OK && rc != SQLITE_ROW && rc != SQLITE_DONE ? rc : final != SQLITE_OK ? final : SQLITE_ERROR;
        int extended = sqlite3_extended_errcode(db);
        if ((extended & 255) == (code & 255)) code = extended;
        if (error) *error = code;
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member keys failed ch=%s field=%s key=%s node=%016llx active=%d rc=%d finalize=%d error=%d: %s",
            channel, identity, sc_public_key_hex(key, (char[65]){0}), (unsigned long long)node, active, rc, final, code, sqlite3_errmsg(db));
    }
    else if (result == TOPO_SQLITE_LOOKUP_MISSING) DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC,
        "member keys unavailable ch=%s field=%s key=%s node=%016llx active=%d",
        channel, identity, sc_public_key_hex(key, (char[65]){0}), (unsigned long long)node, active);
    else { if (x) memcpy(x, keys, 32); if (ed) memcpy(ed, keys + 32, 32); if (local_id) *local_id = node; }
    return result;
}
int topo_node_sqlite_member_keys_ex(sqlite3* db, const char* channel, uint64_t node, int active,
    uint8_t x[32], uint8_t ed[32], int* error) {
    return member_keys(db, channel, node, NULL, NULL, active, x, ed, NULL, error);
}
int topo_node_sqlite_member_local_id(sqlite3* db, const char* channel, const uint8_t author[32], int active, uint64_t* id, int* error) {
    return member_keys(db, channel, 0, author, NULL, active, NULL, NULL, id, error);
}
int topo_node_sqlite_member_ed_by_x(sqlite3* db, const char* channel, const uint8_t peer_x[32], uint8_t ed[32]) {
    return member_keys(db, channel, 0, NULL, peer_x, 1, NULL, ed, NULL, NULL);
}
int topo_node_sqlite_member_keys(sqlite3* db, const char* channel, uint64_t node, int active, uint8_t x[32], uint8_t ed[32]) {
    return topo_node_sqlite_member_keys_ex(db, channel, node, active, x, ed, NULL);
}

int topo_node_sqlite_member_transport_allowed(sqlite3* db, const char* channel_id, uint64_t node_id) {
    return member_access(db, channel_id, node_id, 0);
}

int topo_node_sqlite_member_key_allowed(sqlite3* db, const char* channel_id, const uint8_t key[32], const uint8_t ed[32], int transport) {
    if (!db || !channel_id || !channel_id[0] || !key || (transport && !ed)) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "member key admission: invalid database/channel/key"); return 0;
    }
    char table[80], sql[320]; peers_table_name(channel_id, table, sizeof(table));
    snprintf(sql, sizeof(sql), "SELECT 1 FROM \"%s\" WHERE record_state=0"
        " AND x25519_pubkey=?1 AND (?2 IS NULL OR ed25519_pubkey=?2) AND source=0 AND deleted=0%s",
        table, transport ? "" : " AND identity_verified=1 AND admitted=1");
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, key, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, ed, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int allowed = rc == SQLITE_ROW;
    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "member key admission failed channel=%s x25519=%s rc=%d: %s",
                    channel_id, sc_public_key_hex(key, (char[65]){0}), rc, sqlite3_errmsg(db));
    if (sqlite3_finalize(st) != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "member key admission finalize failed channel=%s", channel_id); return 0;
    }
    return allowed;
}

int topo_node_sqlite_member_del(sqlite3* db, const char* channel_id, uint64_t node_id) {
    if (!db || !channel_id) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "member delete: invalid arguments"); return -1; }
    char peers_tbl[80]; peers_table_name(channel_id, peers_tbl, sizeof(peers_tbl));
    char sql[384];
    snprintf(sql, sizeof(sql), "DELETE FROM \"%s\" WHERE node_id=?1 AND record_state=0"
        " AND 1=(SELECT count(*) FROM \"%s\" WHERE node_id=?1 AND record_state=0)", peers_tbl, peers_tbl);
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)node_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) DEBUG_ERROR(DEBUG_CATEGORY_BGP, "member delete: channel=%s node=%016llx rc=%d: %s",
        channel_id, (unsigned long long)node_id, rc, sqlite3_errmsg(db));
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int topo_node_sqlite_channel_get(sqlite3* db, const char* channel_id,
    char* name_out, int name_sz, uint8_t owner_node_id[32],
    uint8_t* x25519_pub, uint8_t* ed25519_pub, uint8_t* signature) {
    if (!db || !channel_id) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "channel lookup: invalid arguments"); return TOPO_SQLITE_LOOKUP_ERROR;
    }
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT name, owner_node_id, x25519_pubkey, ed25519_pubkey, signature"
        " FROM channels WHERE channel_id=?", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, channel_id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    int result = TOPO_SQLITE_LOOKUP_ERROR;
    if (rc == SQLITE_DONE) { result = TOPO_SQLITE_LOOKUP_MISSING; goto done; }
    if (rc != SQLITE_ROW) goto done;
    for (int col = 1; col <= 4; col++) {
        if (!(col == 1 ? owner_node_id != NULL : col == 2 ? x25519_pub != NULL : col == 3 ? ed25519_pub != NULL : signature != NULL)) continue;
        int size = sqlite3_column_bytes(stmt, col), expected = col == 4 ? 64 : 32;
        if (size == expected && sqlite3_column_type(stmt, col) == SQLITE_BLOB && sqlite3_column_blob(stmt, col)) continue;
        if (size == 0 && sqlite3_errcode(db) != SQLITE_NOMEM) result = TOPO_SQLITE_LOOKUP_MISSING;
        else DEBUG_ERROR(DEBUG_CATEGORY_BGP, "channel lookup: invalid field channel=%s column=%d bytes=%d expected=%d",
                         channel_id, col, size, expected);
        goto done;
    }
    if (name_out && name_sz > 0) {
        const unsigned char* txt = sqlite3_column_text(stmt, 0);
        if (txt) snprintf(name_out, name_sz, "%s", txt); else name_out[0] = '\0';
    }
    if (owner_node_id) memcpy(owner_node_id, sqlite3_column_blob(stmt, 1), 32);
    if (x25519_pub) memcpy(x25519_pub, sqlite3_column_blob(stmt, 2), 32);
    if (ed25519_pub) memcpy(ed25519_pub, sqlite3_column_blob(stmt, 3), 32);
    if (signature) memcpy(signature, sqlite3_column_blob(stmt, 4), 64);
    result = 0;
done:
    {
        int final_rc = sqlite3_finalize(stmt);
        if (final_rc != SQLITE_OK) result = TOPO_SQLITE_LOOKUP_ERROR;
        if (result == TOPO_SQLITE_LOOKUP_ERROR)
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "channel lookup failed channel=%s rc=%d finalize=%d: %s",
                        channel_id, rc, final_rc, sqlite3_errmsg(db));
        else if (result == TOPO_SQLITE_LOOKUP_MISSING)
            DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "channel lookup: dependency unavailable channel=%s", channel_id);
    }
    return result;
}

/* Выходные ключи публикуются только после успешного завершения SELECT. */
static int channel_private_keys(sqlite3* db, const char* channel, uint8_t* x25519, uint8_t* ed25519) {
    if (!db || !channel || !ed25519) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "channel private keys: missing argument"); return -1;
    }
    const char* sql = x25519 ? "SELECT x25519_privkey, ed25519_privkey FROM channels WHERE channel_id=?" :
        "SELECT ed25519_privkey FROM channels WHERE channel_id=?";
    sqlite3_stmt* st = NULL; uint8_t keys[64] = {0}; int columns = x25519 ? 2 : 1, result = -1;
    const char* stage = "prepare"; int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) goto done;
    stage = "bind"; rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC); if (rc != SQLITE_OK) goto done;
    stage = "step"; rc = sqlite3_step(st); if (rc != SQLITE_ROW) goto done;
    for (int i = 0; i < columns; i++) {
        if (sqlite3_column_type(st, i) == SQLITE_NULL) { rc = SQLITE_DONE; goto done; }
        stage = "key size/type"; rc = SQLITE_CORRUPT;
        if (sqlite3_column_type(st, i) != SQLITE_BLOB || sqlite3_column_bytes(st, i) != 32) goto done;
        const void* key = sqlite3_column_blob(st, i); if (!key) { rc = SQLITE_NOMEM; goto done; }
        memcpy(keys + i * 32, key, 32);
    }
    rc = SQLITE_OK;
done:
    {
        int final = sqlite3_finalize(st);
        if (rc == SQLITE_OK && final == SQLITE_OK) {
            if (x25519) memcpy(x25519, keys, 32);
            memcpy(ed25519, keys + (columns - 1) * 32, 32); result = 0;
        } else if (rc == SQLITE_DONE && final == SQLITE_OK) {
            DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "channel private keys unavailable channel=%s", channel);
        } else DEBUG_ERROR(DEBUG_CATEGORY_BGP, "channel private keys channel=%s stage=%s rc=%d finalize=%d: %s",
            channel, stage, rc, final, sqlite3_errmsg(db));
    }
    OPENSSL_cleanse(keys, sizeof(keys)); return result;
}

int topo_node_sqlite_channel_get_priv(sqlite3* db, const char* channel_id, uint8_t* ed25519_priv_out) {
    return channel_private_keys(db, channel_id, NULL, ed25519_priv_out);
}

int topo_node_sqlite_channel_get_privs(sqlite3* db, const char* channel_id,
    uint8_t* x25519_priv_out, uint8_t* ed25519_priv_out) {
    if (!x25519_priv_out) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "channel private keys: X25519 output missing"); return -1;
    }
    return channel_private_keys(db, channel_id, x25519_priv_out, ed25519_priv_out);
}

/* Импорт пары: savepoint охватывает UPDATE и его завершение, внешний commit остаётся вызывающему. */
int topo_node_sqlite_channel_set_privs(sqlite3* db, const char* channel_id,
    const uint8_t* x25519_priv, const uint8_t* ed25519_priv) {
    if (!db || !channel_id || !channel_id[0] || !x25519_priv || !ed25519_priv) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "channel private keys: invalid arguments"); return -1;
    }
    int own = sqlite3_get_autocommit(db);
    int rc = sqlite3_exec(db, "SAVEPOINT channel_private_keys", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "channel private keys: channel=%s stage=savepoint rc=%d: %s",
            channel_id, rc, sqlite3_errmsg(db)); return -1;
    }
    sqlite3_stmt* stmt = NULL;
    const char* stage = "prepare";
    rc = sqlite3_prepare_v2(db, "UPDATE channels SET x25519_privkey=?, ed25519_privkey=? WHERE channel_id=?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind x25519"; rc = sqlite3_bind_blob(stmt, 1, x25519_priv, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind ed25519"; rc = sqlite3_bind_blob(stmt, 2, ed25519_priv, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind channel"; rc = sqlite3_bind_text(stmt, 3, channel_id, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "update"; rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) goto fail;
    int changed = sqlite3_changes(db);
    stage = "finalize"; rc = sqlite3_finalize(stmt); stmt = NULL;
    if (rc != SQLITE_OK) goto fail;
    if (changed != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "channel private keys: channel=%s changed=%d expected=1", channel_id, changed);
        stage = "row count"; rc = SQLITE_NOTFOUND; goto fail;
    }
    stage = "release"; rc = sqlite3_exec(db, "RELEASE channel_private_keys", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "channel private keys stored: channel=%s committed=%d", channel_id, own);
    return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_BGP, "channel private keys: channel=%s stage=%s rc=%d: %s",
        channel_id, stage, rc, sqlite3_errmsg(db));
    int final_rc = sqlite3_finalize(stmt);
    if (final_rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_BGP,
        "channel private keys: cleanup finalize channel=%s rc=%d: %s", channel_id, final_rc, sqlite3_errmsg(db));
    if (!sqlite3_get_autocommit(db) &&
        sqlite3_exec(db, "ROLLBACK TO channel_private_keys; RELEASE channel_private_keys", NULL, NULL, NULL) != SQLITE_OK)
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "channel private keys: rollback failed channel=%s: %s", channel_id, sqlite3_errmsg(db));
    return -1;
}

int topo_node_sqlite_node_set_online(sqlite3* db, uint64_t node_id, int online) {
    if (!db) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node online: database is NULL"); return -1; }
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "UPDATE nodes SET online=? WHERE node_id=?", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 1, online ? 1 : 0);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, (sqlite3_int64)node_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node online: node=%016llx online=%d rc=%d: %s",
        (unsigned long long)node_id, online, rc, sqlite3_errmsg(db));
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int topo_node_sqlite_node_get_online(sqlite3* db, uint64_t node_id) {
    if (!db) return 0;
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT online FROM nodes WHERE node_id=?", -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)node_id);
    int online = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) online = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return online;
}

int topo_node_sqlite_get_ed25519_pubkey(sqlite3* db, uint64_t node_id, uint8_t pubkey_out[32])
{
    if (!db || !pubkey_out) {
        DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "get_ed25519_pubkey: invalid arguments node=%016llx", (unsigned long long)node_id);
        return TOPO_SQLITE_LOOKUP_ERROR;
    }
    sqlite3_stmt* stmt = NULL;
    int sql_rc = sqlite3_prepare_v2(db, "SELECT ed25519_pubkey FROM nodes WHERE node_id=?", -1, &stmt, NULL);
    if (sql_rc == SQLITE_OK) sql_rc = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)node_id);
    if (sql_rc == SQLITE_OK) sql_rc = sqlite3_step(stmt);
    int rc = sql_rc == SQLITE_DONE ? TOPO_SQLITE_LOOKUP_MISSING : TOPO_SQLITE_LOOKUP_ERROR;
    uint8_t key[32];
    if (sql_rc == SQLITE_ROW) {
        const void* b = sqlite3_column_blob(stmt, 0);
        int bytes = sqlite3_column_bytes(stmt, 0);
        if (b && bytes == 32) {
            memcpy(key, b, 32);
            sql_rc = sqlite3_step(stmt);
            if (sql_rc == SQLITE_DONE) rc = 0;
            else if (sql_rc == SQLITE_ROW) {
                DEBUG_WARN(DEBUG_CATEGORY_ROUTING, "ambiguous route alias node=%016llx", (unsigned long long)node_id);
                sql_rc = SQLITE_CONSTRAINT;
            }
        }
        else if (bytes == 0 && sqlite3_errcode(db) != SQLITE_NOMEM) rc = TOPO_SQLITE_LOOKUP_MISSING;
    }
    int final_rc = sqlite3_finalize(stmt);
    if (final_rc != SQLITE_OK) rc = TOPO_SQLITE_LOOKUP_ERROR;
    if (rc == 0) memcpy(pubkey_out, key, 32);
    if (rc == TOPO_SQLITE_LOOKUP_MISSING)
        DEBUG_DEBUG(DEBUG_CATEGORY_ROUTING, "get_ed25519_pubkey: unknown node=%016llx", (unsigned long long)node_id);
    else if (rc) DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "get_ed25519_pubkey: unavailable node=%016llx sql=%d finalize=%d: %s",
                           (unsigned long long)node_id, sql_rc, final_rc, sqlite3_errmsg(db));
    return rc;
}

/* Пересчитывает канальные роли/хранение/удаление; при отказе откатывает всю проекцию узла. */
int topo_node_sqlite_nodeinfo_write(sqlite3* db, const uint8_t x[32]) {
    if (!db || !x) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node classification: missing database or X25519"); return SQLITE_MISUSE; }
    int rc = sqlite3_exec(db, "SAVEPOINT node_classification", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node classification savepoint: rc=%d: %s", rc, sqlite3_errmsg(db)); return rc;
    }
    sqlite3_stmt *scan = NULL, *read = NULL, *write = NULL;
    const char* stage = "addresses";
    int best_at = -1;
    rc = sqlite3_prepare_v2(db, "SELECT addr_type FROM node_addresses WHERE node_x=?", -1, &read, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(read, 1, x, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    while ((rc = sqlite3_step(read)) == SQLITE_ROW) {
        int at = sqlite3_column_int(read, 0);
        if (at == ADDR_TYPE_DIRECT) { best_at = ADDR_TYPE_DIRECT; break; }
        if (best_at < 0 || at < best_at) best_at = at;
    }
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) goto fail;
    rc = sqlite3_finalize(read); read = NULL; if (rc) goto fail;
    static const int at2type[] = { [ADDR_TYPE_NETIF]=1, [ADDR_TYPE_DIRECT]=1, [ADDR_TYPE_NAT_EIM]=2,
        [ADDR_TYPE_NAT_STRICT]=1, [ADDR_TYPE_NAT]=2, [ADDR_TYPE_CONTACT]=2, [ADDR_TYPE_NAT_EIM_UNVERIFIED]=2 };
    int base_type = best_at < 0 || (unsigned)best_at >= sizeof(at2type) / sizeof(at2type[0]) ? 0 : at2type[best_at];
    if (best_at >= 0 && (unsigned)best_at >= sizeof(at2type) / sizeof(at2type[0]))
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "invalid projected address type=%d node_x=%s", best_at, sc_public_key_hex(x, (char[65]){0}));
    stage = "membership tables";
    rc = sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'peers\\_%' ESCAPE '\\'",
                          -1, &scan, NULL);
    if (rc != SQLITE_OK) goto fail;
    while ((rc = sqlite3_step(scan)) == SQLITE_ROW) {
        const char* table = (const char*)sqlite3_column_text(scan, 0);
        if (!table) { rc = SQLITE_NOMEM; goto fail; }
        int node_type = base_type, storage = 0, deleted = 0;
        char sql[512]; snprintf(sql, sizeof(sql), "SELECT adm_tags FROM \"%s\" WHERE x25519_pubkey=? AND record_state=0", table);
        stage = "member tags";
        rc = sqlite3_prepare_v2(db, sql, -1, &read, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(read, 1, x, 32, SQLITE_STATIC);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_step(read);
        if (rc == SQLITE_ROW) {
            const char* tags = (const char*)sqlite3_column_text(read, 0);
            if (!tags && sqlite3_column_type(read, 0) != SQLITE_NULL) { rc = SQLITE_NOMEM; goto fail; }
            struct node_owner_flags flags = {0};
            if (tags && tags[0] && json_flat_parse(tags, node_owner_flag, &flags)) { rc = SQLITE_ERROR; goto fail; }
            if (flags.supernode) node_type = 4;
            storage = best_at == ADDR_TYPE_DIRECT && flags.storage;
            deleted = flags.deleted;
        } else if (rc != SQLITE_DONE) goto fail;
        rc = sqlite3_finalize(read); read = NULL; if (rc) goto fail;
        snprintf(sql, sizeof(sql), "UPDATE \"%s\" SET node_type=?,storage=?,deleted=? WHERE x25519_pubkey=? AND record_state=0", table);
        stage = "member projection";
        rc = sqlite3_prepare_v2(db, sql, -1, &write, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int(write, 1, node_type);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int(write, 2, storage);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int(write, 3, deleted);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(write, 4, x, 32, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(write);
        if (rc != SQLITE_DONE) goto fail;
        rc = sqlite3_finalize(write); write = NULL; if (rc) goto fail;
        stage = "membership tables";
    }
    if (rc != SQLITE_DONE) goto fail;
    rc = sqlite3_finalize(scan); scan = NULL; if (rc) goto fail;
    stage = "release";
    if ((rc = sqlite3_exec(db, "RELEASE node_classification", NULL, NULL, NULL)) != SQLITE_OK) goto fail;
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "node classification stored: node_x=%s best_at=%d base_type=%d",
                sc_public_key_hex(x, (char[65]){0}), best_at, base_type);
    return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node classification failed: node_x=%s stage=%s rc=%d: %s",
                sc_public_key_hex(x, (char[65]){0}), stage, rc, sqlite3_errmsg(db));
    sqlite3_finalize(write); sqlite3_finalize(read); sqlite3_finalize(scan);
    if (sqlite3_exec(db, "ROLLBACK TO node_classification; RELEASE node_classification", NULL, NULL, NULL) != SQLITE_OK)
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node classification rollback failed: node_x=%s: %s",
                    sc_public_key_hex(x, (char[65]){0}), sqlite3_errmsg(db));
    return rc;
}

int topo_node_sqlite_nodeinfo_updated(sqlite3* db, const uint8_t x[32]) {
    return topo_node_sqlite_nodeinfo_write(db, x) ? -1 : 0;
}

int topo_node_sqlite_update_rtt(sqlite3* db, const uint8_t x[32], uint16_t rtt) {
    if (!db || !x || !rtt || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "RTT write requires sample and caller transaction node_x=%s", sc_public_key_hex(x, (char[65]){0}));
        return SQLITE_MISUSE;
    }
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "UPDATE nodes SET rtt=? WHERE x25519_pubkey=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_int(st, 1, rtt);
    if (!rc) rc = sqlite3_bind_blob(st, 2, x, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int changed = rc == SQLITE_DONE ? sqlite3_changes(db) : 0;
    int final = sqlite3_finalize(st);
    if (rc == SQLITE_DONE) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node RTT write failed node_x=%s rtt=%u rc=%d finalize=%d: %s",
        sc_public_key_hex(x, (char[65]){0}), rtt, rc, final, sqlite3_errmsg(db));
    else DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "node RTT staged node_x=%s rtt=%u rows=%d", sc_public_key_hex(x, (char[65]){0}), rtt, changed);
    return rc;
}

int topo_node_sqlite_update_address_rtt(sqlite3* db, const uint8_t x[32], const struct topo_rtt_endpoint* endpoint, uint16_t rtt) {
    if (!db || !x || !endpoint || !rtt || (endpoint->family != 4 && endpoint->family != 6) ||
        !endpoint->port || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "address RTT write requires endpoint and caller transaction node_x=%s",
            sc_public_key_hex(x, (char[65]){0})); return SQLITE_MISUSE;
    }
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "UPDATE node_addresses SET rtt=? WHERE node_x=? AND family=? AND protocol=? AND address=? AND port=?",
        -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_int(st, 1, rtt);
    if (!rc) rc = sqlite3_bind_blob(st, 2, x, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int(st, 3, endpoint->family);
    if (!rc) rc = sqlite3_bind_int(st, 4, endpoint->protocol);
    if (!rc) rc = sqlite3_bind_blob(st, 5, endpoint->address, endpoint->family == 4 ? 4 : 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int(st, 6, endpoint->port);
    if (!rc) rc = sqlite3_step(st);
    int changed = rc == SQLITE_DONE ? sqlite3_changes(db) : 0;
    int final = sqlite3_finalize(st);
    if (rc == SQLITE_DONE) rc = final;
    char ip[INET6_ADDRSTRLEN] = "?";
    inet_ntop(endpoint->family == 4 ? AF_INET : AF_INET6, endpoint->address, ip, sizeof(ip));
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_BGP, "address RTT write failed node_x=%s endpoint=%s:%u proto=%u rtt=%u rc=%d: %s",
        sc_public_key_hex(x, (char[65]){0}), ip, endpoint->port, endpoint->protocol, rtt, rc, sqlite3_errmsg(db));
    else DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "address RTT staged node_x=%s endpoint=%s:%u proto=%u rtt=%u rows=%d",
        sc_public_key_hex(x, (char[65]){0}), ip, endpoint->port, endpoint->protocol, rtt, changed);
    return rc;
}

int topo_node_sqlite_snapshot_time_read(sqlite3* db, uint64_t node_id, const uint8_t* x, uint64_t* out) {
    if (!db || !node_id || !out) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "snapshot timestamp: invalid arguments"); return SQLITE_MISUSE; }
    sqlite3_stmt* stmt = NULL;
    uint64_t timestamp = 0;
    int rc = sqlite3_prepare_v2(db, "SELECT timestamp FROM node_snapshots WHERE node_id=?1 AND (?2 IS NULL OR x25519=?2)", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)node_id);
    if (rc == SQLITE_OK && x) rc = sqlite3_bind_blob(stmt, 2, x, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        if (sqlite3_column_type(stmt, 0) != SQLITE_INTEGER || sqlite3_column_int64(stmt, 0) <= 0) rc = SQLITE_CORRUPT;
        else {
            timestamp = (uint64_t)sqlite3_column_int64(stmt, 0);
            rc = sqlite3_step(stmt);
            if (rc == SQLITE_ROW) rc = SQLITE_CONSTRAINT; /* Неоднозначный числовой запрос. */
        }
    }
    int final = sqlite3_finalize(stmt);
    if ((rc != SQLITE_ROW && rc != SQLITE_DONE) || final != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "snapshot timestamp: node=%016llx rc=%d finalize=%d: %s",
            (unsigned long long)node_id, rc, final, sqlite3_errmsg(db));
        return rc != SQLITE_ROW && rc != SQLITE_DONE ? rc : final;
    }
    *out = timestamp; return 0;
}

int topo_node_sqlite_snapshot_timestamp(sqlite3* db, uint64_t node_id, uint64_t* out) {
    return topo_node_sqlite_snapshot_time_read(db, node_id, NULL, out) ? -1 : 0;
}

int topo_node_sqlite_snapshot_write(sqlite3* db, struct TOPO_NODE* ni) {
    if (!db || topo_node_verify(ni) < 0) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "invalid node snapshot"); return SQLITE_AUTH; }
    uint8_t record[TOPO_NODE_WIRE_MAX_SIZE];
    struct TOPO_GROUP_NODE nq = {0};
    int len = topo_node_serialize(ni, &nq, 0, record, sizeof(record), 0);
    if (len < 0) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "snapshot serialization failed"); return SQLITE_TOOBIG; }
    int own = sqlite3_get_autocommit(db);
    int rc = sqlite3_exec(db, "SAVEPOINT node_snapshot", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "snapshot begin rc=%d: %s", rc, sqlite3_errmsg(db)); return rc; }
    sqlite3_stmt* stmt = NULL;
    const char* sql = "INSERT INTO node_snapshots(node_id,timestamp,record,format,x25519) VALUES(?,?,?,3,?) "
        "ON CONFLICT(x25519) DO UPDATE SET timestamp=excluded.timestamp,record=excluded.record,format=excluded.format "
        "WHERE excluded.timestamp > node_snapshots.timestamp OR "
        "(excluded.timestamp = node_snapshots.timestamp AND (length(node_snapshots.record)=0 OR node_snapshots.format<>excluded.format))";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)ni->node_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, (sqlite3_int64)ni->timestamp);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(stmt, 3, record, len, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(stmt, 4, ni->public_key, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    int changed = rc == SQLITE_DONE ? sqlite3_changes(db) : 0;
    int final = sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE && final == SQLITE_OK && changed) {
        int projected = devgroup_project(db, ni);
        if (projected) rc = -projected;
    }
    if (rc == SQLITE_DONE && final == SQLITE_OK) {
        rc = sqlite3_exec(db, "RELEASE node_snapshot", NULL, NULL, NULL);
        if (rc == SQLITE_OK) return 0;
    }
    DEBUG_ERROR(DEBUG_CATEGORY_BGP, "snapshot write: node=%016llx rc=%d finalize=%d: %s",
        (unsigned long long)ni->node_id, rc, final, sqlite3_errmsg(db));
    if (rc == SQLITE_DONE) rc = final;
    if (!sqlite3_get_autocommit(db)) {
        int rollback = sqlite3_exec(db, "ROLLBACK TO node_snapshot", NULL, NULL, NULL);
        if (rollback == SQLITE_OK) rollback = sqlite3_exec(db, "RELEASE node_snapshot", NULL, NULL, NULL);
        if (rollback != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_BGP, "snapshot rollback rc=%d own=%d: %s", rollback, own, sqlite3_errmsg(db));
        if (own && !sqlite3_get_autocommit(db)) {
            rollback = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            if (rollback != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_BGP, "snapshot full rollback rc=%d: %s", rollback, sqlite3_errmsg(db));
        }
    }
    return rc;
}

int topo_node_sqlite_snapshot_put(sqlite3* db, struct TOPO_NODE* ni) {
    return topo_node_sqlite_snapshot_write(db, ni) ? -1 : 0;
}

int topo_node_sqlite_connection_order_read(sqlite3* db, const uint8_t x[32], char out[256]) {
    if (!db || !x || !out) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "connection order: missing database/output"); return SQLITE_MISUSE; }
    out[0] = 0;
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT record FROM node_snapshots WHERE x25519=? AND format=3", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, x, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int result = SQLITE_OK;
    if (rc == SQLITE_ROW && sqlite3_column_bytes(st, 0)) {
        const uint8_t* record = sqlite3_column_blob(st, 0);
        size_t size = (size_t)sqlite3_column_bytes(st, 0);
        struct TOPOMSG_NODE h;
        if (!record || size < sizeof(h)) goto invalid;
        memcpy(&h, record, sizeof(h));
        size_t offset = sizeof(h) + h.node_name_len;
        if (memcmp(h.public_key, x, 32) || h.node_name_len > 63 || offset + h.connection_order_len > size) goto invalid;
        if (h.connection_order_len) {
            if (memchr(record + offset, 0, h.connection_order_len)) goto invalid;
            memcpy(out, record + offset, h.connection_order_len); out[h.connection_order_len] = 0;
            if (connection_order_parse(out, NULL) < 0) goto invalid;
        }
    } else if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "connection order read: node_x=%s rc=%d: %s",
                    sc_public_key_hex(x, (char[65]){0}), rc, sqlite3_errmsg(db)); result = rc;
    }
    if (rc == SQLITE_ROW) {
        rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) goto invalid;
        if (rc != SQLITE_DONE) { result = rc; DEBUG_ERROR(DEBUG_CATEGORY_BGP, "connection order lookup rc=%d", rc); }
    }
    goto done;
invalid:
    DEBUG_ERROR(DEBUG_CATEGORY_BGP, "connection order: invalid snapshot node_x=%s", sc_public_key_hex(x, (char[65]){0}));
    result = SQLITE_CORRUPT;
done:
    rc = sqlite3_finalize(st);
    if (rc != SQLITE_OK) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "connection order finalize: rc=%d", rc); if (!result) result = rc; }
    if (result) out[0] = 0;
    else DEBUG_DEBUG(DEBUG_CATEGORY_CONNECTION, "member connection order: node_x=%s order=%s",
                     sc_public_key_hex(x, (char[65]){0}), out[0] ? out : "unknown");
    return result;
}

int topo_node_sqlite_get_connection_order(sqlite3* db, const uint8_t x[32], char out[256]) {
    return topo_node_sqlite_connection_order_read(db, x, out) ? -1 : 0;
}

struct topo_node_record_address {
    uint8_t address[16], family, socket_id, protocol, config, kind, has_reality; /* Копия endpoint; family=4/6. */
    uint16_t port;                         /* Host order, 0 допустим для сведений об интерфейсе. */
    struct TOPO_ADDR_REALITY_OPTS reality; /* Самостоятельная копия, когда has_reality=1. */
};
struct topo_node_record {
    uint64_t node_id;                      /* Идентичность запроса; сверяется с подписанным wire при build. */
    enum topo_node_record_kind kind;      /* Определяет, какие поля снимка заполнены. */
    char* name;                           /* Собственная строка либо NULL, никогда не SQLite text. */
    uint8_t x25519[32], ed25519[32], client_type; /* Поля nodes для IDENTITY/BOOTSTRAP/CONNECT. */
    uint8_t* wire;                        /* Собственный подписанный wire для SNAPSHOT. */
    size_t wire_size, address_count;      /* Размер wire и число самостоятельных endpoint. */
    struct topo_node_record_address* addresses; /* Собственный массив для BOOTSTRAP, без runtime/pools. */
};

void topo_node_sqlite_record_free(struct topo_node_record* record) {
    if (!record) return;
    u_free(record->name); u_free(record->wire); u_free(record->addresses); u_free(record);
}

int topo_node_sqlite_record_read(sqlite3* db, uint64_t node_id, enum topo_node_record_kind kind, struct topo_node_record** out) {
    return topo_node_sqlite_record_lookup(db, node_id, NULL, kind, out);
}

int topo_node_sqlite_record_lookup(sqlite3* db, uint64_t node_id, const uint8_t* key, enum topo_node_record_kind kind, struct topo_node_record** out) {
    if (out) *out = NULL;
    if (!db || !node_id || !out || kind < TOPO_NODE_RECORD_SNAPSHOT || kind > TOPO_NODE_RECORD_CONNECT) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node record read: invalid arguments node=%016llx kind=%d", (unsigned long long)node_id, kind);
        return SQLITE_MISUSE;
    }
    struct topo_node_record* record = NULL;
    sqlite3_stmt* st = NULL;
    int rc, final;
    size_t capacity = 0;
    const char* stage = "prepare identity/snapshot";
    const char* sql = kind == TOPO_NODE_RECORD_SNAPSHOT ?
        "SELECT record, x25519 FROM node_snapshots WHERE node_id=?1 AND format=3 AND (?2 IS NULL OR x25519=?2)" :
        "SELECT name,x25519_pubkey,ed25519_pubkey,client_type FROM nodes WHERE node_id=?1 AND (?2 IS NULL OR x25519_pubkey=?2)";
    rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) { stage = "bind identity/snapshot"; rc = sqlite3_bind_int64(st, 1, (sqlite3_int64)node_id); }
    if (rc == SQLITE_OK && key) rc = sqlite3_bind_blob(st, 2, key, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) { stage = "read identity/snapshot"; rc = sqlite3_step(st); }
    if (rc == SQLITE_DONE) goto missing;
    if (rc != SQLITE_ROW) goto fail;
    stage = "allocate record";
    record = u_calloc(1, sizeof(*record));
    if (!record) { rc = SQLITE_NOMEM; goto fail; }
    record->kind = kind; record->node_id = node_id;
    stage = "copy identity/snapshot";
    if (kind == TOPO_NODE_RECORD_SNAPSHOT) {
        const uint8_t* indexed = sqlite3_column_blob(st, 1);
        if (!indexed || sqlite3_column_bytes(st, 1) != 32) { rc = SQLITE_CORRUPT; goto fail; }
        memcpy(record->x25519, indexed, 32);
        int size = sqlite3_column_bytes(st, 0);
        if (!size) { /* Инвалидированная запись всё ещё может иметь коллизию ID. */
            rc = sqlite3_step(st);
            if (rc == SQLITE_DONE) goto missing;
            if (rc == SQLITE_ROW) rc = SQLITE_CONSTRAINT;
            goto fail;
        }
        if (sqlite3_column_type(st, 0) != SQLITE_BLOB || size > TOPO_NODE_WIRE_MAX_SIZE) {
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node record wire invalid node=%016llx type=%d bytes=%d maximum=%d",
                (unsigned long long)node_id, sqlite3_column_type(st, 0), size, TOPO_NODE_WIRE_MAX_SIZE);
            rc = SQLITE_MISMATCH; goto fail;
        }
        record->wire = u_malloc((size_t)size);
        if (!record->wire) { rc = SQLITE_NOMEM; goto fail; }
        const void* wire = sqlite3_column_blob(st, 0);
        if (!wire) { rc = SQLITE_NOMEM; goto fail; }
        memcpy(record->wire, wire, (size_t)size); record->wire_size = (size_t)size;
    } else {
        const void* x = sqlite3_column_blob(st, 1);
        const void* ed = sqlite3_column_blob(st, 2);
        if (sqlite3_column_type(st, 1) != SQLITE_BLOB || sqlite3_column_bytes(st, 1) != 32 || !x ||
            sqlite3_column_type(st, 2) != SQLITE_BLOB || sqlite3_column_bytes(st, 2) != 32 || !ed) {
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node record keys invalid node=%016llx x_type/size=%d/%d ed_type/size=%d/%d",
                (unsigned long long)node_id, sqlite3_column_type(st, 1), sqlite3_column_bytes(st, 1),
                sqlite3_column_type(st, 2), sqlite3_column_bytes(st, 2));
            rc = SQLITE_MISMATCH; goto fail;
        }
        memcpy(record->x25519, x, 32); memcpy(record->ed25519, ed, 32);
        if ((kind == TOPO_NODE_RECORD_IDENTITY || kind == TOPO_NODE_RECORD_CONNECT) &&
            sc_derive_node_id_from_pubkey(record->x25519) != node_id) {
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node record identity mismatch requested=%016llx derived=%016llx",
                (unsigned long long)node_id, (unsigned long long)sc_derive_node_id_from_pubkey(record->x25519));
            rc = SQLITE_MISMATCH; goto fail;
        }
        const char* name = (const char*)sqlite3_column_text(st, 0);
        if (!name && sqlite3_column_type(st, 0) != SQLITE_NULL) { rc = SQLITE_NOMEM; goto fail; }
        if (name && !(record->name = u_strdup(name))) { rc = SQLITE_NOMEM; goto fail; }
        record->client_type = (uint8_t)sqlite3_column_int(st, 3);
    }
    rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) {
        if (rc == SQLITE_ROW) { stage = "ambiguous numeric node_id"; rc = SQLITE_CONSTRAINT; }
        goto fail;
    }
    rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) { stage = "finalize identity/snapshot"; goto fail; }
    if (kind != TOPO_NODE_RECORD_BOOTSTRAP && kind != TOPO_NODE_RECORD_CONNECT) goto success;
    stage = "prepare addresses";
    rc = sqlite3_prepare_v2(db, "SELECT family,address,port,socket_id,protocol,config_type,options,address_kind"
        " FROM node_addresses WHERE node_x=? AND family IN(4,6) ORDER BY family,addr_type", -1, &st, NULL);
    if (rc == SQLITE_OK) { stage = "bind addresses"; rc = sqlite3_bind_blob(st, 1, record->x25519, 32, SQLITE_STATIC); }
    if (rc != SQLITE_OK) goto fail;
    stage = "read addresses";
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        struct topo_node_record_address row = {0};
        row.family = (uint8_t)sqlite3_column_int(st, 0);
        int size = row.family == 4 ? 4 : 16;
        sqlite3_int64 port = sqlite3_column_int64(st, 2), socket_id = sqlite3_column_int64(st, 3);
        const void* address = sqlite3_column_blob(st, 1);
        if (sqlite3_column_type(st, 1) != SQLITE_BLOB || sqlite3_column_bytes(st, 1) != size || !address ||
            port < 0 || port > UINT16_MAX || socket_id < 0 || socket_id > UINT8_MAX) {
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node record address invalid node=%016llx family=%u bytes=%d port=%lld socket=%lld",
                (unsigned long long)node_id, row.family, sqlite3_column_bytes(st, 1), (long long)port, (long long)socket_id);
            rc = SQLITE_MISMATCH; goto fail;
        }
        memcpy(row.address, address, (size_t)size);
        row.port = (uint16_t)port; row.socket_id = (uint8_t)socket_id;
        row.protocol = (uint8_t)sqlite3_column_int(st, 4); row.config = (uint8_t)sqlite3_column_int(st, 5);
        row.kind = (uint8_t)sqlite3_column_int(st, 7);
        if ((row.protocol & TOPO_PROTO_REALITY) && sqlite3_column_type(st, 6) == SQLITE_BLOB &&
            sqlite3_column_bytes(st, 6) == TOPO_ADDR_REALITY_OPTS_SIZE) {
            const void* options = sqlite3_column_blob(st, 6);
            if (!options) { rc = SQLITE_NOMEM; goto fail; }
            memcpy(&row.reality, options, sizeof(row.reality)); row.has_reality = 1;
            row.reality.server_name[TOPO_REALITY_SNAME_MAX - 1] = 0;
        }
        if (record->address_count == capacity) {
            if (capacity > UINT32_MAX / 2 / sizeof(row)) { rc = SQLITE_TOOBIG; goto fail; }
            size_t next = capacity ? capacity * 2 : 8;
            void* addresses = u_realloc(record->addresses, next * sizeof(row));
            if (!addresses) { rc = SQLITE_NOMEM; goto fail; }
            record->addresses = addresses; capacity = next;
        }
        record->addresses[record->address_count++] = row;
    }
    if (rc != SQLITE_DONE) goto fail;
    rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) { stage = "finalize addresses"; goto fail; }
success:
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "node record copied node=%016llx kind=%d wire=%zu addresses=%zu",
        (unsigned long long)node_id, kind, record->wire_size, record->address_count);
    *out = record; return SQLITE_OK;
missing:
    rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) { stage = "finalize missing record"; goto fail; }
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "node record missing node=%016llx kind=%d", (unsigned long long)node_id, kind);
    topo_node_sqlite_record_free(record); return SQLITE_OK;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node record read failed node=%016llx kind=%d stage=%s rc=%d: %s",
        (unsigned long long)node_id, kind, stage, rc, sqlite3_errstr(rc));
    if (rc != SQLITE_NOMEM && rc != SQLITE_MISMATCH && rc != SQLITE_TOOBIG)
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node record SQL detail: %s", sqlite3_errmsg(db));
    final = sqlite3_finalize(st);
    if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node record cleanup finalize rc=%d: %s", final, sqlite3_errmsg(db));
    topo_node_sqlite_record_free(record); return rc;
}

struct TOPO_NODE* topo_node_sqlite_record_build(struct TOPO_GROUPS* groups, const struct topo_node_record* record) {
    if (!record) return NULL;
    if (record->kind != TOPO_NODE_RECORD_IDENTITY && (!groups || !groups->instance || groups->instance->topo_groups != groups)) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node record build: invalid groups node=%016llx", (unsigned long long)record->node_id); return NULL;
    }
    struct TOPO_NODE* ni = NULL;
    if (record->kind == TOPO_NODE_RECORD_SNAPSHOT) {
        struct TOPO_GROUP group = {.instance = groups->instance};
        if (topo_node_deserialize(&group, record->wire, record->wire_size, &ni, NULL, NULL, NULL, NULL) < 0 ||
            !ni || ni->node_id != record->node_id || memcmp(ni->public_key, record->x25519, 32) || topo_node_verify(ni) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node record build: invalid signature/wire node=%016llx", (unsigned long long)record->node_id);
            topo_node_destroy(groups, ni); return NULL;
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "node record verified node=%016llx bytes=%zu", (unsigned long long)record->node_id, record->wire_size);
        return ni;
    }
    if (record->kind == TOPO_NODE_RECORD_BOOTSTRAP && !record->address_count) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "node record build: no bootstrap addresses node=%016llx", (unsigned long long)record->node_id); return NULL;
    }
    ni = u_calloc(1, sizeof(*ni));
    if (!ni) goto allocation_failed;
    ni->node_id = record->node_id; ni->client_type = record->client_type;
    ni->ver = record->kind == TOPO_NODE_RECORD_IDENTITY ? 0 : 1;
    memcpy(ni->public_key, record->x25519, 32); memcpy(ni->ed25519_public_key, record->ed25519, 32);
    if (record->name && !(ni->node_name = u_strdup(record->name))) goto allocation_failed;
    struct TOPO_ADDR4** a4 = &ni->v4_addrs;
    struct TOPO_ADDR6** a6 = &ni->v6_addrs;
    struct TOPO_SOCKMETA4** m4 = &ni->v4_sock_meta;
    struct TOPO_SOCKMETA6** m6 = &ni->v6_sock_meta;
    struct TOPO_REALITY_SOCK** reality = &ni->reality_socks;
    for (size_t i = 0; i < record->address_count; i++) {
        const struct topo_node_record_address* row = &record->addresses[i];
        if (row->family == 4) {
            *a4 = memory_pool_alloc(groups->v4_addr_pool);
            if (!*a4) goto allocation_failed;
            memset(*a4, 0, sizeof(**a4)); memcpy((*a4)->addr, row->address, 4);
            (*a4)->port = row->port; (*a4)->socket_id = row->socket_id; (*a4)->protocol = row->protocol; (*a4)->type = row->kind;
            a4 = &(*a4)->next;
            const struct TOPO_SOCKMETA4* existing = ni->v4_sock_meta;
            while (existing && existing->id != row->socket_id) existing = existing->next;
            if (!existing) {
                *m4 = memory_pool_alloc(groups->v4_sock_meta_pool);
                if (!*m4) goto allocation_failed;
                memset(*m4, 0, sizeof(**m4)); (*m4)->id = row->socket_id; (*m4)->config_type = row->config;
                m4 = &(*m4)->next;
            }
        } else {
            *a6 = memory_pool_alloc(groups->v6_addr_pool);
            if (!*a6) goto allocation_failed;
            memset(*a6, 0, sizeof(**a6)); memcpy((*a6)->addr, row->address, 16);
            (*a6)->port = row->port; (*a6)->socket_id = row->socket_id; (*a6)->protocol = row->protocol; (*a6)->type = row->kind;
            a6 = &(*a6)->next;
            const struct TOPO_SOCKMETA6* existing = ni->v6_sock_meta;
            while (existing && existing->id != row->socket_id) existing = existing->next;
            if (!existing) {
                *m6 = memory_pool_alloc(groups->v6_sock_meta_pool);
                if (!*m6) goto allocation_failed;
                memset(*m6, 0, sizeof(**m6)); (*m6)->id = row->socket_id; (*m6)->config_type = row->config;
                m6 = &(*m6)->next;
            }
        }
        /* SQL-проекция не является доказательством NAT-профиля: metadata остаётся UNKNOWN. */
        if (row->has_reality) {
            *reality = u_calloc(1, sizeof(**reality));
            if (!*reality) goto allocation_failed;
            (*reality)->socket_id = row->socket_id;
            memcpy((*reality)->short_id, row->reality.short_id, 8); memcpy((*reality)->server_pubkey, row->reality.server_pubkey, 32);
            memcpy((*reality)->version, row->reality.version, 3);
            memcpy((*reality)->server_name, row->reality.server_name, TOPO_REALITY_SNAME_MAX);
            reality = &(*reality)->next;
        }
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "node record built node=%016llx kind=%d addresses=%zu",
        (unsigned long long)record->node_id, record->kind, record->address_count);
    return ni;
allocation_failed:
    DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node record build allocation failed node=%016llx kind=%d",
        (unsigned long long)record->node_id, record->kind);
    topo_node_destroy(groups, ni); return NULL;
}

struct TOPO_NODE* topo_node_sqlite_snapshot_load(sqlite3* db, struct TOPO_GROUPS* groups, uint64_t node_id) {
    struct topo_node_record* record = NULL;
    if (topo_node_sqlite_record_read(db, node_id, TOPO_NODE_RECORD_SNAPSHOT, &record) != SQLITE_OK) return NULL;
    struct TOPO_NODE* ni = topo_node_sqlite_record_build(groups, record);
    topo_node_sqlite_record_free(record); return ni;
}

struct TOPO_NODE* topo_node_sqlite_identity_load(sqlite3* db, uint64_t node_id) {
    struct topo_node_record* record = NULL;
    if (topo_node_sqlite_record_read(db, node_id, TOPO_NODE_RECORD_IDENTITY, &record) != SQLITE_OK) return NULL;
    struct TOPO_NODE* ni = topo_node_sqlite_record_build(NULL, record);
    topo_node_sqlite_record_free(record); return ni;
}

struct TOPO_NODE* topo_node_sqlite_node_load(sqlite3* db, struct TOPO_GROUPS* groups, uint64_t node_id) {
    return topo_node_sqlite_load_key(db, groups, node_id, NULL, TOPO_NODE_RECORD_BOOTSTRAP);
}

struct TOPO_NODE* topo_node_sqlite_load_key(sqlite3* db, struct TOPO_GROUPS* groups, uint64_t id,
                                           const uint8_t* x, enum topo_node_record_kind fallback) {
    if (!groups || !groups->instance || groups->instance->topo_groups != groups) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node load: invalid groups node=%016llx", (unsigned long long)id); return NULL;
    }
    struct topo_node_record* record = NULL;
    if (topo_node_sqlite_record_lookup(db, id, x, TOPO_NODE_RECORD_SNAPSHOT, &record) != SQLITE_OK) return NULL;
    if (!record && topo_node_sqlite_record_lookup(db, id, x, fallback, &record) != SQLITE_OK) return NULL;
    struct TOPO_NODE* ni = topo_node_sqlite_record_build(groups, record);
    topo_node_sqlite_record_free(record); return ni;
}

int topo_node_sqlite_set_connected(sqlite3* db, const char* channel_id, const uint8_t ed[32], int connected) {
    if (!db || !channel_id || !ed) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "set connected: invalid arguments"); return -1; }
    char peers_tbl[80]; peers_table_name(channel_id, peers_tbl, sizeof(peers_tbl));
    char sql[192]; snprintf(sql, sizeof(sql), "UPDATE \"%s\" SET connected=? WHERE ed25519_pubkey=? AND record_state=0", peers_tbl);
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(st, 1, connected ? 1 : 0);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, ed, 32, SQLITE_STATIC);
    struct db_changes_origin source = db_changes_source(db, ed, 0, "group transport state");
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    db_changes_restore(db, source);
    int final = sqlite3_finalize(st);
    if (rc == SQLITE_DONE) rc = final;
    if (rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "set connected ch=%s peer=%s value=%d rc=%d: %s",
                    channel_id, sc_public_key_hex(ed, (char[65]){0}), connected, rc, sqlite3_errmsg(db)); return -1;
    }
    return 0;
}

/* Читаем тип из уже проверенного snapshot, а не из устаревшей проекции nodes.client_type. */
int topo_node_sqlite_get_connect_peers(sqlite3* db, const char* channel_id,
                                     struct topo_connect_peer** out, size_t* out_count) {
    if (out) *out = NULL;
    if (out_count) *out_count = 0;
    if (!db || !channel_id || !out || !out_count) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "invalid group candidate query arguments"); return -SQLITE_MISUSE;
    }
    *out = NULL; *out_count = 0;
    char table[80]; peers_table_name(channel_id, table, sizeof(table));
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT p.node_id,p.adm_tags,s.timestamp,s.record,p.ed25519_pubkey FROM \"%s\" p"
        " LEFT JOIN node_snapshots s ON s.node_id=p.node_id AND s.x25519=p.x25519_pubkey AND s.format=3 AND length(s.record)>0"
        " LEFT JOIN nodes n ON n.ed25519_pubkey=p.ed25519_pubkey"
        " WHERE p.record_state=0 AND p.deleted=0 AND p.source=0 AND p.admitted=1"
        " ORDER BY p.connected DESC,n.rtt IS NULL,n.rtt,p.ed25519_pubkey", table);
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group candidate query channel=%s rc=%d: %s", channel_id, rc, sqlite3_errmsg(db)); return -rc;
    }
    struct topo_connect_peer* peers = NULL;
    size_t count = 0, capacity = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 16;
            if (next > UINT32_MAX / sizeof(*peers)) {
                DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group candidate query too large channel=%s count=%zu", channel_id, count);
                rc = SQLITE_TOOBIG; break;
            }
            struct topo_connect_peer* list = u_realloc(peers, next * sizeof(*list));
            if (!list) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group candidate query allocation failed channel=%s", channel_id); rc = SQLITE_NOMEM; break; }
            peers = list; capacity = next;
        }
        struct topo_connect_peer* peer = &peers[count++];
        *peer = (struct topo_connect_peer){ .node_id = (uint64_t)sqlite3_column_int64(stmt, 0), .client_type = UINT8_MAX };
        const void* ed = sqlite3_column_blob(stmt, 4);
        if (ed && sqlite3_column_bytes(stmt, 4) == 32) memcpy(peer->ed, ed, 32);
        const char* tags = (const char*)sqlite3_column_text(stmt, 1);
        char value[16];
        peer->supernode = tags && json_flat_get(tags, "supernode", value, sizeof(value)) == 0 && strcmp(value, "yes") == 0;
        const void* record = sqlite3_column_blob(stmt, 3);
        if (record) {
            struct TOPOMSG_NODE header;
            if (sqlite3_column_bytes(stmt, 3) < (int)sizeof(header)) {
                DEBUG_ERROR(DEBUG_CATEGORY_BGP, "short candidate snapshot channel=%s node=%016llx", channel_id,
                            (unsigned long long)peer->node_id); rc = SQLITE_CORRUPT; break;
            }
            memcpy(&header, record, sizeof(header));
            if (sc_derive_node_id_from_pubkey(header.public_key) != peer->node_id || header.timestamp != (uint64_t)sqlite3_column_int64(stmt, 2) ||
                header.client_type > CLIENT_TYPE_MOBILE) {
                DEBUG_ERROR(DEBUG_CATEGORY_BGP, "invalid candidate snapshot channel=%s node=%016llx type=%u", channel_id,
                            (unsigned long long)peer->node_id, header.client_type); rc = SQLITE_CORRUPT; break;
            }
            peer->client_type = header.client_type;
        }
    }
    int final = sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) rc = final;
    if (rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group candidate read failed channel=%s rows=%zu rc=%d (%s) finalize=%d: %s",
            channel_id, count, rc, sqlite3_errstr(rc), final, sqlite3_errmsg(db));
        u_free(peers); return -rc;
    }
    *out = peers; *out_count = count; return 0;
}

/* Один проход SQL: частичный массив никогда не публикуется при ошибке чтения/памяти. */
static int node_ids_query(sqlite3* db, const char* sql, int exclude_self, uint64_t self,
                          uint64_t** out_ids, int* out_count) {
    *out_ids = NULL; *out_count = 0;
    sqlite3_stmt* st = NULL;
    uint64_t* ids = NULL;
    size_t count = 0, capacity = 0;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK && exclude_self) rc = sqlite3_bind_int64(st, 1, (sqlite3_int64)self);
    if (rc != SQLITE_OK) goto fail;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 16;
            if (next > UINT32_MAX / sizeof(*ids)) {
                DEBUG_ERROR(DEBUG_CATEGORY_BGP, "peer ID list too large: count=%zu", count); goto fail;
            }
            uint64_t* resized = u_realloc(ids, next * sizeof(*ids));
            if (!resized) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "peer ID list allocation failed: count=%zu", count); goto fail; }
            ids = resized; capacity = next;
        }
        ids[count++] = (uint64_t)sqlite3_column_int64(st, 0);
    }
    if (rc != SQLITE_DONE) goto fail;
    sqlite3_finalize(st);
    *out_ids = ids; *out_count = (int)count;
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "peer ID list read: count=%zu exclude_self=%d", count, exclude_self);
    return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_BGP, "peer ID query failed: rc=%d rows=%zu sql=%s: %s", rc, count, sql, sqlite3_errmsg(db));
    sqlite3_finalize(st); u_free(ids); return -1;
}

int topo_node_sqlite_get_connected_peers(sqlite3* db, const char* channel_id, uint64_t** out_ids, int* out_count) {
    if (out_ids) *out_ids = NULL;
    if (out_count) *out_count = 0;
    if (!db || !channel_id || !out_ids || !out_count) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "connected peers: invalid arguments"); return -1;
    }
    char table[80], sql[192]; peers_table_name(channel_id, table, sizeof(table));
    snprintf(sql, sizeof(sql), "SELECT node_id FROM \"%s\" WHERE record_state=0 AND connected=1 AND deleted=0", table);
    return node_ids_query(db, sql, 0, 0, out_ids, out_count);
}

int topo_node_sqlite_get_public_peers(sqlite3* db, const char* channel_id,
                                      uint64_t** out_ids, int* out_count, uint64_t self_node_id) {
    if (out_ids) *out_ids = NULL;
    if (out_count) *out_count = 0;
    if (!db || !channel_id || !out_ids || !out_count) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "public peers: invalid arguments"); return -1;
    }
    char table[80], sql[512]; peers_table_name(channel_id, table, sizeof(table));
    snprintf(sql, sizeof(sql),
        "SELECT DISTINCT n.node_id FROM node_addresses na JOIN nodes n ON n.x25519_pubkey=na.node_x"
        " WHERE n.node_id!=?1 AND na.family IN (4,6) AND na.addr_type IN (1,2)"
        " AND NOT EXISTS (SELECT 1 FROM \"%s\" p WHERE p.ed25519_pubkey=n.ed25519_pubkey AND p.record_state=0 AND p.deleted=1)", table);
    return node_ids_query(db, sql, 1, self_node_id, out_ids, out_count);
}

int topo_node_sqlite_get_supernode_peers(sqlite3* db, const char* channel_id, uint64_t** out_ids, int* out_count) {
    if (out_ids) *out_ids = NULL;
    if (out_count) *out_count = 0;
    if (!db || !channel_id || !out_ids || !out_count) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "supernode peers: invalid arguments"); return -1;
    }
    char table[80], sql[192]; peers_table_name(channel_id, table, sizeof(table));
    snprintf(sql, sizeof(sql), "SELECT node_id FROM \"%s\" WHERE record_state=0 AND node_type=4 AND deleted=0", table);
    return node_ids_query(db, sql, 0, 0, out_ids, out_count);
}

int topo_node_sqlite_get_local_peers(sqlite3* db, const char* channel_id,
                                     uint64_t** out_ids, int* out_count, uint64_t self_node_id) {
    if (out_ids) *out_ids = NULL;
    if (out_count) *out_count = 0;
    if (!db || !channel_id || !out_ids || !out_count) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "local peers: invalid arguments"); return -1;
    }
    char table[80], sql[512]; peers_table_name(channel_id, table, sizeof(table));
    snprintf(sql, sizeof(sql),
        "SELECT DISTINCT n.node_id FROM node_addresses na JOIN nodes n ON n.x25519_pubkey=na.node_x"
        " WHERE n.node_id!=?1 AND na.family IN (4,6) AND na.addr_type IN (0,3)"
        " AND NOT EXISTS (SELECT 1 FROM \"%s\" p WHERE p.ed25519_pubkey=n.ed25519_pubkey AND p.record_state=0 AND p.deleted=1)", table);
    return node_ids_query(db, sql, 1, self_node_id, out_ids, out_count);
}

int topo_node_sqlite_get_node_type(sqlite3* db, const char* channel_id, uint64_t node_id) {
    if (!db || !channel_id || !node_id) return -1;
    char peers_tbl[80]; peers_table_name(channel_id, peers_tbl, sizeof(peers_tbl));
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT node_type FROM \"%s\" WHERE node_id=? AND record_state=0", peers_tbl);
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)node_id);
    int node_type = -1;
    if (sqlite3_step(st) == SQLITE_ROW) node_type = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return node_type;
}
