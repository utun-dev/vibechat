#include "devgroup.h"
#include "topo_node.h"
#include "../utun_instance.h"
#include "../transport_layer/secure_channel.h"
#include "../../lib/json_flat.h"
#include "../../lib/strbuf.h"
#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include "../../lib/db_async.h"
#include <sqlite3.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/crypto.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

static int dg_error(const char* stage) {
    DEBUG_WARN(DEBUG_CATEGORY_BGP, "devgroup: rejected stage=%s", stage); return -1;
}
static int dg_utf8(const char* text) {
    const unsigned char* p = (const unsigned char*)text;
    while (*p) {
        uint32_t c = *p++; unsigned n; uint32_t min;
        if (c < 128) { if (c < 32) return 0; continue; }
        if (c >= 0xc2 && c <= 0xdf) { n = 1; min = 0x80; c &= 31; }
        else if (c >= 0xe0 && c <= 0xef) { n = 2; min = 0x800; c &= 15; }
        else if (c >= 0xf0 && c <= 0xf4) { n = 3; min = 0x10000; c &= 7; }
        else return 0;
        while (n--) { if ((*p & 0xc0) != 0x80) return 0; c = (c << 6) | (*p++ & 63); }
        if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return 0;
    }
    return 1;
}
/* Строгий каркас плоского JSON; escapes и Unicode проверяет json_flat_parse. */
static void dg_ws(const char** p) { while (**p && strchr(" \t\r\n", **p)) ++*p; }
static int dg_string(const char** p) {
    if (*(*p)++ != '"') return -1;
    while (**p && **p != '"') {
        if (**p == '\\') { ++*p; if (!**p) return -1; }
        ++*p;
    }
    if (!**p) return -1;
    ++*p; return 0;
}
static int dg_syntax(const char* p) {
    dg_ws(&p); if (*p++ != '{') return -1;
    dg_ws(&p);
    if (*p != '}') while (1) {
        if (dg_string(&p)) return -1;
        dg_ws(&p); if (*p++ != ':') return -1;
        dg_ws(&p); if (dg_string(&p)) return -1;
        dg_ws(&p); if (*p == '}') break;
        if (*p++ != ',') return -1;
        dg_ws(&p);
    }
    p++; dg_ws(&p); return *p ? -1 : 0;
}
static int dg_number(const char* text, uint64_t* out) {
    if (*text < '1' || *text > '9') return -1;
    uint64_t value = 0;
    for (const char* p = text; *p; p++) {
        if (*p < '0' || *p > '9' || value > ((uint64_t)INT64_MAX - (*p - '0')) / 10) return -1;
        value = value * 10 + (*p - '0');
    }
    *out = value; return 0;
}
struct dg_parser { struct devgroup_profile p; uint8_t x[32]; unsigned fields, count; char keys[32][64]; };
static int dg_field(const char* key, const char* value, void* arg) {
    struct dg_parser* p = arg;
    if (!*key || strlen(key) > 63 || p->count == 32) return -1;
    for (unsigned i = 0; i < p->count; i++) if (!strcmp(p->keys[i], key)) return -1;
    strcpy(p->keys[p->count++], key);
    if (!strcmp(key, "name")) {
        if (!*value || strlen(value) > 63 || !dg_utf8(value)) return -1;
        strcpy(p->p.name, value); p->fields |= 1;
    } else if (!strcmp(key, "ver")) { if (dg_number(value, &p->p.version)) return -1; p->fields |= 2;
    } else if (!strcmp(key, "timestamp")) { if (dg_number(value, &p->p.timestamp)) return -1; p->fields |= 4;
    } else if (!strcmp(key, "x25519")) {
        if (strlen(value) != 64 || strspn(value, "0123456789abcdef") != 64 || sc_hex_to_binary(value, p->x, 32)) return -1;
        p->fields |= 8;
    }
    return 0;
}
static size_t dg_profile_message(const char* json, uint8_t* out) {
    const char domain[] = "utun-devgroup-profile-v1";
    memcpy(out, domain, sizeof(domain)); size_t n = strlen(json);
    memcpy(out + sizeof(domain), json, n); return sizeof(domain) + n;
}
static size_t dg_member_message(const struct devgroup_record* r, const uint8_t x[32], const uint8_t ed[32], uint8_t* out) {
    const char domain[] = "utun-devgroup-member-v2"; size_t n = sizeof(domain);
    memcpy(out, domain, n); memcpy(out + n, r->ed, 32); n += 32; memcpy(out + n, r->x, 32); n += 32;
    memcpy(out + n, x, 32); n += 32; memcpy(out + n, ed, 32); return n + 32;
}
int devgroup_profile_read(const struct devgroup_record* r, struct devgroup_profile* out) {
    if (!r || !out || !r->json[0] || strnlen(r->json, sizeof(r->json)) > DEVGROUP_PROFILE_MAX) return dg_error("profile length");
    struct dg_parser p = {0}; uint8_t body[DEVGROUP_PROFILE_MAX + 32];
    if (dg_syntax(r->json) || json_flat_parse(r->json, dg_field, &p) || p.fields != 15 || memcmp(p.x, r->x, 32))
        return dg_error("profile JSON");
    size_t n = dg_profile_message(r->json, body);
    if (sc_ed25519_verify(r->ed, body, n, r->profile_sig) != SC_OK) return dg_error("profile signature");
    SHA256((const uint8_t*)r->json, strlen(r->json), p.p.hash); *out = p.p; return 0;
}
int devgroup_verify(const struct devgroup_record* r, const uint8_t x[32], const uint8_t ed[32]) {
    if (!r || !x || !ed) return dg_error("membership arguments");
    if (!r->json[0]) {
        const struct devgroup_record zero = {0};
        return memcmp(r, &zero, sizeof(zero)) ? dg_error("partial membership") : 0;
    }
    struct devgroup_profile p;
    if (devgroup_profile_read(r, &p)) return -1;
    return devgroup_verify_binding(r, x, ed);
}
int devgroup_verify_binding(const struct devgroup_record* r, const uint8_t x[32], const uint8_t ed[32]) {
    if (!r || !x || !ed) return dg_error("binding arguments");
    uint8_t body[192];
    size_t n = dg_member_message(r, x, ed, body);
    if (sc_ed25519_verify(r->ed, body, n, r->member_sig) != SC_OK) return dg_error("membership signature");
    return 0;
}
int devgroup_encode(const struct devgroup_record* r, uint8_t* out, size_t capacity) {
    if (!r || !out) return dg_error("encode arguments");
    size_t n = strnlen(r->json, sizeof(r->json)); if (!n) return 0;
    if (n > DEVGROUP_PROFILE_MAX || capacity < 192 + n) return dg_error("encode capacity");
    memcpy(out, r->ed, 32); memcpy(out + 32, r->x, 32); memcpy(out + 64, r->member_sig, 64);
    memcpy(out + 128, r->profile_sig, 64); memcpy(out + 192, r->json, n); return (int)(192 + n);
}
int devgroup_decode(struct devgroup_record* out, const uint8_t* data, size_t size) {
    if (!out || (size && (!data || size <= 192 || size > DEVGROUP_WIRE_MAX || memchr(data + 192, 0, size - 192))))
        return dg_error("decode length");
    struct devgroup_record r = {0};
    if (size) {
        memcpy(r.ed, data, 32); memcpy(r.x, data + 32, 32); memcpy(r.member_sig, data + 64, 64);
        memcpy(r.profile_sig, data + 128, 64); memcpy(r.json, data + 192, size - 192);
    }
    *out = r; return 0;
}
int devgroup_profile_compare(const struct devgroup_profile* a, const struct devgroup_profile* b) {
    if (a->version != b->version) return a->version > b->version ? 1 : -1;
    if (a->timestamp != b->timestamp) return a->timestamp > b->timestamp ? 1 : -1;
    return memcmp(a->hash, b->hash, 32);
}
int devgroup_bind(struct devgroup_record* r, const uint8_t priv[32], const uint8_t x[32], const uint8_t ed[32]) {
    if (!r || !priv || !x || !ed) return dg_error("bind arguments");
    uint8_t body[192]; size_t n = dg_member_message(r, x, ed, body);
    if (sc_ed25519_sign(priv, body, n, r->member_sig) != SC_OK) return dg_error("bind signature");
    return devgroup_verify(r, x, ed);
}
/* При локальном переименовании сохраняем неизвестные строковые поля расширений. */
static int dg_copy_extension(const char* key, const char* value, void* arg) {
    if (!strcmp(key, "name") || !strcmp(key, "ver") || !strcmp(key, "timestamp") || !strcmp(key, "x25519")) return 0;
    struct strbuf* json = arg;
    return strbuf_addf(json, ",") < 0 || strbuf_add_json_string(json, key) || strbuf_addf(json, ":") < 0 ||
        strbuf_add_json_string(json, value) ? -1 : 0;
}
int devgroup_sign(struct devgroup_record* r, const uint8_t priv[32], const char* name,
    uint64_t version, uint64_t timestamp, const uint8_t x[32], const uint8_t ed[32]) {
    if (!r || !priv || !name || !*name || strlen(name) > 63 || !dg_utf8(name) || !version || version > INT64_MAX ||
        !timestamp || timestamp > INT64_MAX || !x || !ed) return dg_error("sign arguments");
    struct devgroup_record next = *r; struct strbuf json = {0}; char hex[65];
    for (unsigned i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", r->x[i]);
    int rc = strbuf_addf(&json, "{\"ver\":\"%llu\",\"timestamp\":\"%llu\",\"x25519\":\"%s\",\"name\":",
        (unsigned long long)version, (unsigned long long)timestamp, hex);
    if (rc < 0 || strbuf_add_json_string(&json, name) ||
        (r->json[0] && json_flat_parse(r->json, dg_copy_extension, &json)) || strbuf_addf(&json, "}") < 0 || json.len > DEVGROUP_PROFILE_MAX) {
        strbuf_free(&json); return dg_error("JSON allocation/size");
    }
    memset(next.json, 0, sizeof(next.json)); memcpy(next.json, json.buf, json.len); strbuf_free(&json);
    uint8_t body[DEVGROUP_PROFILE_MAX + 32]; size_t n = dg_profile_message(next.json, body);
    if (sc_ed25519_sign(priv, body, n, next.profile_sig) != SC_OK) return dg_error("profile signing");
    n = dg_member_message(&next, x, ed, body);
    if (sc_ed25519_sign(priv, body, n, next.member_sig) != SC_OK || devgroup_verify(&next, x, ed))
        return dg_error("membership signing");
    *r = next; return 0;
}
int devgroup_public_keys(const uint8_t ep[32], const uint8_t xp[32], uint8_t ed[32], uint8_t x[32]) {
    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, ep, 32); size_t n = 32;
    int ok = key && EVP_PKEY_get_raw_public_key(key, ed, &n) == 1 && n == 32;
    EVP_PKEY_free(key);
    if (!ok || sc_compute_public_key_from_private(xp, x) != SC_OK) return dg_error("key derivation");
    return 0;
}
int devgroup_project(sqlite3* db, const struct TOPO_NODE* node) {
    if (!db || !node) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "devgroup projection: invalid database/node"); return -SQLITE_MISUSE; }
    const struct devgroup_record* r = &node->devgroup; sqlite3_stmt* st = NULL;
    /* NODEINFO не создаёт идентичность: nodes заполняют собственные ключи и проверенные MEMBERS. */
    const char* sql = "UPDATE nodes SET devgroup_ed=?4,devgroup_x=?5,devgroup_member_sig=?6,devgroup_json=?7,devgroup_profile_sig=?8"
        " WHERE node_id=?1 AND x25519_pubkey=?2 AND ed25519_pubkey=?3";
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 1, (sqlite3_int64)node->node_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, node->public_key, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, node->ed25519_public_key, 32, SQLITE_STATIC);
    const void* blobs[] = {r->ed, r->x, r->member_sig, r->profile_sig}; const int sizes[] = {32,32,64,64};
    for (unsigned i = 0; i < 4 && rc == SQLITE_OK; i++) {
        int col = i == 3 ? 8 : 4 + (int)i;
        rc = r->json[0] ? sqlite3_bind_blob(st, col, blobs[i], sizes[i], SQLITE_STATIC) : sqlite3_bind_null(st, col);
    }
    if (rc == SQLITE_OK) rc = r->json[0] ? sqlite3_bind_text(st, 7, r->json, -1, SQLITE_STATIC) : sqlite3_bind_null(st, 7);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_DONE && !sqlite3_changes(db)) DEBUG_DEBUG(DEBUG_CATEGORY_BGP,
        "devgroup projection deferred: no matching verified identity node=%016llx", (unsigned long long)node->node_id);
    int final = sqlite3_finalize(st), error = rc == SQLITE_DONE ? final : rc;
    if (error) DEBUG_ERROR(DEBUG_CATEGORY_BGP, "devgroup projection node=%016llx rc=%d finalize=%d: %s",
        (unsigned long long)node->node_id, rc, final,
        (sqlite3_errcode(db) & 255) == (error & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(error));
    return -error;
}
/* Секреты и запись выдаются только после полного SQL/crypto успеха; runtime не используется. */
int devgroup_local_read(sqlite3* db, const uint8_t device_x[32], const uint8_t device_ed[32],
    struct devgroup_record* out, uint8_t ep[32], uint8_t xp[32]) {
    if (!db || !device_x || !device_ed || !out || !ep || !xp) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "devgroup local read: invalid database/identity/output"); return -SQLITE_MISUSE;
    }
    sqlite3_stmt* st = NULL; struct devgroup_record r = {0}; uint8_t e[32] = {0}, x[32] = {0}; int found = 0;
    const char* stage = "prepare";
    int rc = sqlite3_prepare_v2(db, "SELECT ed_priv,x_priv,record FROM devgroup_local WHERE device_ed=?", -1, &st, NULL);
    if (rc != SQLITE_OK) goto done;
    stage = "bind"; rc = sqlite3_bind_blob(st, 1, device_ed, 32, SQLITE_STATIC); if (rc != SQLITE_OK) goto done;
    stage = "step"; rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        stage = "stored key/record types"; rc = SQLITE_CORRUPT;
        for (int i = 0; i < 3; i++) if (sqlite3_column_type(st, i) != SQLITE_BLOB) goto done;
        if (sqlite3_column_bytes(st, 0) != 32 || sqlite3_column_bytes(st, 1) != 32 ||
            sqlite3_column_bytes(st, 2) <= 0 || sqlite3_column_bytes(st, 2) > DEVGROUP_WIRE_MAX) goto done;
        const void* values[3];
        for (int i = 0; i < 3; i++) {
            values[i] = sqlite3_column_blob(st, i); if (!values[i]) { rc = SQLITE_NOMEM; goto done; }
        }
        stage = "stored record";
        if (devgroup_decode(&r, values[2], (size_t)sqlite3_column_bytes(st, 2)) || !r.json[0]) goto done;
        memcpy(e, values[0], 32); memcpy(x, values[1], 32); found = 1; rc = SQLITE_DONE;
    }
done:
    { int final = sqlite3_finalize(st); if (rc == SQLITE_DONE) { rc = final; stage = "finalize"; }
      else if (final) DEBUG_ERROR(DEBUG_CATEGORY_BGP, "devgroup local cleanup rc=%d", final); }
    if (!rc && found) {
        uint8_t ed[32], pub[32];
        stage = "stored identity/secret verification";
        if (devgroup_public_keys(e, x, ed, pub) || memcmp(ed, r.ed, 32) || memcmp(pub, r.x, 32) ||
            devgroup_verify(&r, device_x, device_ed)) rc = SQLITE_AUTH;
    }
    if (!rc) { *out = r; memcpy(ep, e, 32); memcpy(xp, x, 32); }
    else DEBUG_ERROR(DEBUG_CATEGORY_BGP, "devgroup local read ed=%s stage=%s rc=%d: %s",
        sc_public_key_hex(device_ed, (char[65]){0}), stage, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    OPENSSL_cleanse(e, 32); OPENSSL_cleanse(x, 32); return rc ? -rc : found;
}
int devgroup_load_local(struct UTUN_INSTANCE* inst, struct devgroup_record* out, uint8_t ep[32], uint8_t xp[32]) {
    if (!inst || !out || !ep || !xp) return dg_error("load arguments");
    if (db_async_can_write(inst->db_worker)) {
        const struct devgroup_local_identity* cached = &inst->local_devgroup;
        if (!cached->ready) return dg_error("local identity not activated");
        *out = cached->record; memcpy(ep, cached->ed_private, 32); memcpy(xp, cached->x_private, 32); return cached->present;
    }
    int rc = devgroup_local_read(inst->topo_sqlite_db, inst->my_keys.public_key,
        inst->my_ed25519_pubkey, out, ep, xp); return rc < 0 ? -1 : rc;
}
int devgroup_local_activate(struct UTUN_INSTANCE* inst, const struct devgroup_local_identity* prepared) {
    if (!inst || !prepared || prepared->ready != 1 || (prepared->present != 0 && prepared->present != 1))
        return dg_error("local activation arguments/readiness");
    if (!prepared->present) {
        const struct devgroup_record empty = {0}; const uint8_t zero[32] = {0};
        if (memcmp(&prepared->record, &empty, sizeof(empty)) || memcmp(prepared->ed_private, zero, 32) ||
            memcmp(prepared->x_private, zero, 32)) return dg_error("absent local identity has data");
    } else {
        uint8_t ed[32], x[32];
        if (devgroup_public_keys(prepared->ed_private, prepared->x_private, ed, x) ||
            memcmp(ed, prepared->record.ed, 32) || memcmp(x, prepared->record.x, 32) ||
            devgroup_verify(&prepared->record, inst->my_keys.public_key, inst->my_ed25519_pubkey))
            return dg_error("local activation identity/secret verification");
    }
    if (prepared != &inst->local_devgroup) {
        OPENSSL_cleanse(&inst->local_devgroup, sizeof(inst->local_devgroup)); inst->local_devgroup = *prepared;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "devgroup local identity activated node=%016llx present=%d",
        (unsigned long long)inst->node_id, prepared->present); return 0;
}
static int dg_runtime_member(struct UTUN_INSTANCE* inst, uint64_t node, const uint8_t ed[32], struct devgroup_record* out) {
    if (!inst || !node || !out) return dg_error("runtime member arguments");
    if ((ed ? !memcmp(ed, inst->my_ed25519_pubkey, 32) : node == inst->node_id) && inst->local_devgroup.ready) { *out = inst->local_devgroup.record; return inst->local_devgroup.present; }
    struct TOPO_NODE* current = inst->topo_groups ? topo_node_registry_find(inst->topo_groups, node) : NULL;
    if (!current || (ed && memcmp(ed, current->ed25519_public_key, 32)) || !current->timestamp || !current->devgroup.json[0]) { memset(out, 0, sizeof(*out)); return 0; }
    if (devgroup_verify(&current->devgroup, current->public_key, current->ed25519_public_key))
        return dg_error("runtime member certificate");
    *out = current->devgroup; return 1;
}
int devgroup_runtime_member(struct UTUN_INSTANCE* inst, uint64_t node, struct devgroup_record* out) {
    return dg_runtime_member(inst, node, NULL, out);
}
int devgroup_latest(sqlite3* db, const uint8_t id[32], struct devgroup_record* out, struct devgroup_profile* profile) {
    if (!db || !id || !out || !profile) { dg_error("latest arguments"); return -SQLITE_MISUSE; }
    sqlite3_stmt* st = NULL; struct devgroup_record best = {0}; struct devgroup_profile bp = {0}; int found = 0;
    int rc = sqlite3_prepare_v2(db, "SELECT devgroup_x,devgroup_member_sig,devgroup_json,devgroup_profile_sig,"
        "node_id,x25519_pubkey,ed25519_pubkey FROM nodes WHERE devgroup_ed=?", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, id, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        struct devgroup_record r = {0}; struct devgroup_profile p;
        int type = sqlite3_column_type(st, 2), n = sqlite3_column_bytes(st, 2);
        const char* json = (const char*)sqlite3_column_text(st, 2);
        const void* values[7] = {0}; const int sizes[7] = {32, 64, 0, 64, 0, 32, 32};
        for (unsigned i = 0; i < 7; i++) if (sizes[i]) {
            if (sqlite3_column_type(st, (int)i) != SQLITE_BLOB || sqlite3_column_bytes(st, (int)i) != sizes[i]) {
                rc = SQLITE_CORRUPT; break;
            }
            values[i] = sqlite3_column_blob(st, (int)i);
            if (!values[i]) { rc = SQLITE_NOMEM; break; }
        }
        if (rc != SQLITE_ROW) break;
        if (type != SQLITE_TEXT || n <= 0 || n > DEVGROUP_PROFILE_MAX) { rc = SQLITE_CORRUPT; break; }
        if (!json) { rc = SQLITE_NOMEM; break; }
        if (memchr(json, 0, (size_t)n)) { rc = SQLITE_CORRUPT; break; }
        memcpy(r.ed, id, 32); memcpy(r.x, values[0], 32); memcpy(r.member_sig, values[1], 64);
        memcpy(r.json, json, (size_t)n); memcpy(r.profile_sig, values[3], 64);
        if (devgroup_verify(&r, values[5], values[6]) ||
            devgroup_profile_read(&r, &p)) { rc = SQLITE_CORRUPT; break; }
        if (!found || devgroup_profile_compare(&p, &bp) > 0) { best = r; bp = p; found = 1; }
    }
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK) {
        int error = rc == SQLITE_DONE ? final : rc;
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "devgroup latest read rc=%d finalize=%d: %s", rc, final,
            (sqlite3_errcode(db) & 255) == (error & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(error)); return -error;
    }
    *out = best; *profile = bp; return found;
}

#define DG_MEMBER_COLUMNS "devgroup_ed,devgroup_x,devgroup_member_sig,devgroup_json,devgroup_profile_sig,x25519_pubkey,ed25519_pubkey"

static int dg_member_row(sqlite3_stmt* st, struct devgroup_record* out) {
    int type = sqlite3_column_type(st, 3), n = sqlite3_column_bytes(st, 3);
    const char* json = (const char*)sqlite3_column_text(st, 3);
    const void* values[7] = {0}; const int sizes[7] = {32, 32, 64, 0, 64, 32, 32};
    for (unsigned i = 0; i < 7; i++) if (sizes[i]) {
        if (sqlite3_column_type(st, (int)i) != SQLITE_BLOB || sqlite3_column_bytes(st, (int)i) != sizes[i]) return SQLITE_CORRUPT;
        values[i] = sqlite3_column_blob(st, (int)i);
        if (!values[i]) return SQLITE_NOMEM;
    }
    if (type != SQLITE_TEXT || n <= 0 || n > DEVGROUP_PROFILE_MAX) return SQLITE_CORRUPT;
    if (!json) return SQLITE_NOMEM;
    if (memchr(json, 0, (size_t)n)) return SQLITE_CORRUPT;
    struct devgroup_record r = {0};
    memcpy(r.ed, values[0], 32); memcpy(r.x, values[1], 32); memcpy(r.member_sig, values[2], 64);
    memcpy(r.json, json, (size_t)n); memcpy(r.profile_sig, values[4], 64);
    if (devgroup_verify(&r, values[5], values[6])) return SQLITE_CORRUPT;
    *out = r;
    return SQLITE_OK;
}

static int dg_read_member(sqlite3* db, uint64_t node, const uint8_t ed[32], struct devgroup_record* out) {
    if (!db || (!node && !ed) || !out) { dg_error("member arguments"); return -SQLITE_MISUSE; }
    sqlite3_stmt* st = NULL; struct devgroup_record r = {0};
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT " DG_MEMBER_COLUMNS " FROM nodes WHERE %s=? AND devgroup_ed IS NOT NULL",
             ed ? "ed25519_pubkey" : "node_id");
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = ed ? sqlite3_bind_blob(st, 1, ed, 32, SQLITE_STATIC) : sqlite3_bind_int64(st, 1, (sqlite3_int64)node);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int found = rc == SQLITE_ROW;
    if (found) {
        rc = dg_member_row(st, &r);
        if (!rc) rc = SQLITE_DONE;
    }
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK) {
        int error = rc == SQLITE_DONE ? final : rc;
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "devgroup member read node=%llx ed=%s rc=%d finalize=%d: %s", (unsigned long long)node,
            ed ? sc_public_key_hex(ed, (char[65]){0}) : "", rc, final,
            (sqlite3_errcode(db) & 255) == (error & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(error)); return -error;
    }
    *out = r;
    return found;
}

int devgroup_read_member(sqlite3* db, uint64_t node, struct devgroup_record* out) {
    return dg_read_member(db, node, NULL, out);
}

int devgroup_read_member_key(sqlite3* db, const uint8_t ed[32], struct devgroup_record* out) {
    return dg_read_member(db, 0, ed, out);
}

int devgroup_member_by_key(struct UTUN_INSTANCE* inst, const uint8_t ed[32], struct devgroup_record* out) {
    if (!inst || !ed || !out) return dg_error("member key arguments");
    if (!db_async_can_write(inst->db_worker)) return devgroup_read_member_key(inst->topo_sqlite_db, ed, out);
    uint8_t x[32];
    if (sc_identity_public_x("devgroup member", ed, x) != SC_OK) return -1;
    return dg_runtime_member(inst, sc_derive_node_id_from_pubkey(x), ed, out);
}

/* Проверить каждую принадлежность один раз; поиск самого свежего имени адресатам не нужен. */
int devgroup_nodes_read(sqlite3* db, const uint8_t anchor[32], uint8_t (**out)[32], size_t* count) {
    if (out) *out = NULL;
    if (count) *count = 0;
    if (!db || !anchor || !out || !count) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "devgroup nodes read invalid database/anchor/output"); return SQLITE_MISUSE;
    }
    struct devgroup_record group;
    int found = devgroup_read_member_key(db, anchor, &group);
    if (found < 0) return -found;
    if (!found) { DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "devgroup anchor has no group key=%s", sc_public_key_hex(anchor, (char[65]){0})); return SQLITE_OK; }
    sqlite3_stmt* st = NULL; uint8_t (*nodes)[32] = NULL; size_t used = 0, capacity = 0; const char* stage = "prepare";
    int rc = sqlite3_prepare_v2(db, "SELECT " DG_MEMBER_COLUMNS " FROM nodes WHERE devgroup_ed=? ORDER BY ed25519_pubkey", -1, &st, NULL);
    if (rc != SQLITE_OK) goto error;
    stage = "bind group"; rc = sqlite3_bind_blob(st, 1, group.ed, 32, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    stage = "read";
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (used == 65535) { stage = "member limit"; rc = SQLITE_TOOBIG; goto error; }
        struct devgroup_record current;
        int verified = dg_member_row(st, &current);
        if (verified || memcmp(group.ed, current.ed, 32) || memcmp(group.x, current.x, 32)) {
            stage = "membership"; rc = verified ? verified : SQLITE_CORRUPT; goto error;
        }
        if (used == capacity) {
            size_t next = capacity ? capacity * 2 : 16; if (next > 65535) next = 65535;
            uint8_t (*resized)[32] = u_realloc(nodes, next * sizeof(*nodes));
            if (!resized) { stage = "reserve nodes"; rc = SQLITE_NOMEM; goto error; }
            nodes = resized; capacity = next;
        }
        memcpy(nodes[used++], sqlite3_column_blob(st, 6), 32);
    }
    if (rc != SQLITE_DONE) goto error;
    stage = "finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    *out = nodes; *count = used;
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "devgroup nodes read anchor=%s count=%zu", sc_public_key_hex(anchor, (char[65]){0}), used); return SQLITE_OK;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_BGP, "devgroup nodes read failed anchor=%s stage=%s count=%zu rc=%d: %s",
        sc_public_key_hex(anchor, (char[65]){0}), stage, used, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int final = sqlite3_finalize(st);
    if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_BGP, "devgroup nodes cleanup finalize rc=%d", final);
    u_free(nodes); return rc;
}

int devgroup_member(sqlite3* db, const uint8_t ed[32], uint8_t id[32], char name[64]) {
    if (!id || !name) { dg_error("member output arguments"); return -SQLITE_MISUSE; }
    struct devgroup_record r, best; struct devgroup_profile profile;
    int found = devgroup_read_member_key(db, ed, &r);
    if (found < 0) return found;
    if (!found) { memset(id, 0, 32); memset(name, 0, 64); return 0; }
    found = devgroup_latest(db, r.ed, &best, &profile);
    if (found < 0) return found;
    if (!found) { dg_error("member profile unavailable"); return -SQLITE_CORRUPT; }
    memcpy(id, r.ed, 32); memcpy(name, profile.name, 64); return 1;
}
