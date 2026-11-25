#include "merkle_tree.h"
#include "../db_schema.h"
#include "../../lib/debug_config.h"
#include <string.h>

static int tree_error(sqlite3* db, const char* op, const char* ns) {
    DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_tree: %s ns=%s: %s", op, ns ? ns : "-", db ? sqlite3_errmsg(db) : "no DB");
    return -1;
}

uint64_t merkle_sync_level_prefix(uint64_t key, uint8_t level) {
    if (level == 0) return 0;
    if (level > MT_MAX_LEVEL) return 0;
    return key & (~UINT64_C(0) << (64 - level * 5));
}

uint8_t merkle_sync_prefix_bytes(uint8_t level) {
    return level <= MT_MAX_LEVEL ? (uint8_t)((level * 5 + 7) / 8) : 0;
}

int merkle_tree_init(sqlite3* db) {
    if (db_schema_table(db, "merkle_tree_hash", NULL)) return -1;
    if (sqlite3_exec(db, "DELETE FROM merkle_tree_hash", NULL, NULL, NULL) != SQLITE_OK) return tree_error(db, "init", NULL);
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_tree: empty derived index initialized");
    return 0;
}

int merkle_tree_get(sqlite3* db, const char* ns, uint8_t level, uint64_t prefix, uint8_t hash[MT_HASH_SIZE]) {
    if (!db || !ns || !hash || level > MT_MAX_LEVEL || prefix != merkle_sync_level_prefix(prefix, level)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_tree invalid get db=%p ns=%s level=%u prefix=%016llx output=%p",
            (void*)db, ns ? ns : "-", level, (unsigned long long)prefix, (void*)hash); return -SQLITE_MISUSE;
    }
    uint8_t value[MT_HASH_SIZE] = {0}; sqlite3_stmt* st = NULL; const char* stage = "prepare";
    int rc = sqlite3_prepare_v2(db, "SELECT hash FROM merkle_tree_hash WHERE namespace=? AND level=? AND prefix64=?", -1, &st, NULL);
    if (rc == SQLITE_OK) { stage = "bind namespace"; rc = sqlite3_bind_text(st, 1, ns, -1, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "bind level"; rc = sqlite3_bind_int(st, 2, level); }
    if (rc == SQLITE_OK) { stage = "bind prefix"; rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)prefix); }
    if (rc == SQLITE_OK) { stage = "step"; rc = sqlite3_step(st); }
    if (rc == SQLITE_ROW) {
        stage = "hash type/size";
        int type = sqlite3_column_type(st, 0), bytes_count = sqlite3_column_bytes(st, 0);
        if (type != SQLITE_BLOB || bytes_count != MT_HASH_SIZE) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_tree invalid hash ns=%s level=%u prefix=%016llx type=%d bytes=%d expected=%u",
                ns, level, (unsigned long long)prefix, type, bytes_count, (unsigned)MT_HASH_SIZE); rc = SQLITE_CORRUPT;
        }
        else {
            const void* bytes = sqlite3_column_blob(st, 0); stage = "hash allocation";
            if (!bytes) rc = SQLITE_NOMEM; else { memcpy(value, bytes, sizeof(value)); rc = SQLITE_OK; }
        }
    } else if (rc == SQLITE_DONE) rc = SQLITE_OK;
    int final = sqlite3_finalize(st);
    if (rc == SQLITE_OK && final != SQLITE_OK) stage = "finalize";
    if (rc != SQLITE_OK || final != SQLITE_OK) {
        int code = rc != SQLITE_OK ? rc : final;
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_tree get failed ns=%s level=%u prefix=%016llx stage=%s rc=%d finalize=%d: %s",
            ns, level, (unsigned long long)prefix, stage, rc, final,
            (sqlite3_errcode(db) & 255) == (code & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(code)); return -code;
    }
    memcpy(hash, value, sizeof(value)); return 0;
}

int merkle_tree_children(sqlite3* db, const char* ns, uint8_t level, uint64_t prefix,
        uint8_t hashes[MT_BUCKETS][MT_HASH_SIZE]) {
    if (!db || !ns || !hashes || level >= MT_MAX_LEVEL || prefix != merkle_sync_level_prefix(prefix, level)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_tree invalid children db=%p ns=%s level=%u prefix=%016llx output=%p",
            (void*)db, ns ? ns : "-", level, (unsigned long long)prefix, (void*)hashes); return -SQLITE_MISUSE;
    }
    uint8_t values[MT_BUCKETS][MT_HASH_SIZE] = {{0}};
    int shift = 64 - (level + 1) * 5;
    const char* sql = level == 0 ? "SELECT prefix64,hash FROM merkle_tree_hash WHERE namespace=? AND level=?"
        : "SELECT prefix64,hash FROM merkle_tree_hash WHERE namespace=? AND level=? AND prefix64>=? AND prefix64<=?";
    sqlite3_stmt* st = NULL; const char* stage = "prepare";
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) { stage = "bind namespace"; rc = sqlite3_bind_text(st, 1, ns, -1, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "bind level"; rc = sqlite3_bind_int(st, 2, level + 1); }
    if (level && rc == SQLITE_OK) { stage = "bind prefix"; rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)prefix); }
    if (level && rc == SQLITE_OK) {
        stage = "bind prefix end"; rc = sqlite3_bind_int64(st, 4, (sqlite3_int64)(prefix | (UINT64_C(31) << shift)));
    }
    if (rc == SQLITE_OK) stage = "step";
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        int prefix_type = sqlite3_column_type(st, 0), hash_type = sqlite3_column_type(st, 1), bytes_count = sqlite3_column_bytes(st, 1);
        uint64_t child = (uint64_t)sqlite3_column_int64(st, 0); stage = "child prefix/type/hash size";
        if (prefix_type != SQLITE_INTEGER || child != merkle_sync_level_prefix(child, level + 1) ||
            hash_type != SQLITE_BLOB || bytes_count != MT_HASH_SIZE) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_tree invalid child ns=%s level=%u child=%016llx"
                " prefix_type=%d hash_type=%d bytes=%d", ns, level + 1, (unsigned long long)child, prefix_type, hash_type, bytes_count);
            rc = SQLITE_CORRUPT; break;
        }
        const void* bytes = sqlite3_column_blob(st, 1); stage = "child allocation";
        if (!bytes) { rc = SQLITE_NOMEM; break; }
        memcpy(values[(child >> shift) & 31], bytes, MT_HASH_SIZE); stage = "step";
    }
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    int final = sqlite3_finalize(st);
    if (rc == SQLITE_OK && final != SQLITE_OK) stage = "finalize";
    if (rc != SQLITE_OK || final != SQLITE_OK) {
        int code = rc != SQLITE_OK ? rc : final;
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_tree children failed ns=%s level=%u prefix=%016llx stage=%s rc=%d finalize=%d: %s",
            ns, level, (unsigned long long)prefix, stage, rc, final,
            (sqlite3_errcode(db) & 255) == (code & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(code)); return -code;
    }
    memcpy(hashes, values, sizeof(values)); return 0;
}

static int tree_store(sqlite3* db, const char* ns, uint8_t level, uint64_t prefix, const uint8_t hash[MT_HASH_SIZE]) {
    static const uint8_t empty[MT_HASH_SIZE];
    uint8_t old[MT_HASH_SIZE];
    int previous = merkle_tree_get(db, ns, level, prefix, old);
    if (previous < 0) return previous;
    if (!memcmp(old, hash, MT_HASH_SIZE)) return 0;
    int is_empty = !memcmp(hash, empty, MT_HASH_SIZE);
    const char* sql = is_empty ? "DELETE FROM merkle_tree_hash WHERE namespace=? AND level=? AND prefix64=?"
        : "INSERT OR REPLACE INTO merkle_tree_hash(namespace,level,prefix64,hash) VALUES(?,?,?,?)";
    sqlite3_stmt* st = NULL; const char* stage = "prepare";
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) { stage = "bind namespace"; rc = sqlite3_bind_text(st, 1, ns, -1, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "bind level"; rc = sqlite3_bind_int(st, 2, level); }
    if (rc == SQLITE_OK) { stage = "bind prefix"; rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)prefix); }
    if (rc == SQLITE_OK && !is_empty) { stage = "bind hash"; rc = sqlite3_bind_blob(st, 4, hash, MT_HASH_SIZE, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "step"; rc = sqlite3_step(st); }
    int changes = rc == SQLITE_DONE ? sqlite3_changes(db) : 0;
    if (rc == SQLITE_DONE) { stage = "changes"; rc = changes == 1 ? SQLITE_OK : SQLITE_CONSTRAINT; }
    int final = sqlite3_finalize(st);
    if (rc == SQLITE_OK && final != SQLITE_OK) stage = "finalize";
    if (rc != SQLITE_OK || final != SQLITE_OK) {
        int code = rc != SQLITE_OK ? rc : final;
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_tree store failed ns=%s level=%u prefix=%016llx delete=%d"
            " stage=%s changes=%d rc=%d finalize=%d: %s", ns, level, (unsigned long long)prefix, is_empty,
            stage, changes, rc, final,
            (sqlite3_errcode(db) & 255) == (code & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(code)); return -code;
    }
    return 1;
}

int merkle_tree_digest_begin(EVP_MD_CTX* hash, uint8_t level) {
    uint8_t domain[2] = {0x4d, level};
    return EVP_DigestInit_ex(hash, EVP_sha256(), NULL) == 1 && EVP_DigestUpdate(hash, domain, sizeof(domain)) == 1;
}

static int tree_branch_digest(EVP_MD_CTX* sha, uint8_t level, const uint8_t* children, uint8_t hash[MT_HASH_SIZE]) {
    static const uint8_t empty[MT_BUCKETS * MT_HASH_SIZE];
    if (!memcmp(children, empty, sizeof(empty))) { memset(hash, 0, MT_HASH_SIZE); return 1; }
    return merkle_tree_digest_begin(sha, level) && EVP_DigestUpdate(sha, children, sizeof(empty)) == 1 &&
        EVP_DigestFinal_ex(sha, hash, NULL) == 1;
}

int merkle_tree_branch_hash(uint8_t level, const uint8_t* children, uint8_t hash[MT_HASH_SIZE]) {
    EVP_MD_CTX* sha = EVP_MD_CTX_new();
    int ok = sha && tree_branch_digest(sha, level, children, hash);
    EVP_MD_CTX_free(sha);
    return ok;
}

int merkle_tree_update(sqlite3* db, const char* ns, uint64_t key, merkle_leaf_hash_fn leaf_hash, void* ctx) {
    EVP_MD_CTX* sha = NULL;
    uint8_t hash[MT_HASH_SIZE], children[MT_BUCKETS][MT_HASH_SIZE];
    int changed = 0, error = SQLITE_MISUSE; const char* stage = "arguments";
    if (!db || !ns || !*ns || !leaf_hash) goto fail;
    stage = "digest allocation"; error = SQLITE_NOMEM; sha = EVP_MD_CTX_new(); if (!sha) goto fail;
    for (int level = MT_MAX_LEVEL; level >= 0; level--) {
        uint64_t prefix = merkle_sync_level_prefix(key, (uint8_t)level);
        error = SQLITE_ERROR;
        if (level == MT_MAX_LEVEL) {
            stage = "digest init";
            if (!merkle_tree_digest_begin(sha, MT_MAX_LEVEL)) goto fail;
            int count = leaf_hash(ctx, ns, MT_MAX_LEVEL, prefix, sha);
            if (count < 0) { stage = "leaf"; error = -count; goto fail; }
            stage = "digest final";
            if (EVP_DigestFinal_ex(sha, hash, NULL) != 1) goto fail;
            if (!count) memset(hash, 0, sizeof(hash));
        } else {
            int rc = merkle_tree_children(db, ns, (uint8_t)level, prefix, children);
            if (rc < 0) { stage = "children"; error = -rc; goto fail; }
            stage = "children digest";
            if (!tree_branch_digest(sha, (uint8_t)level, &children[0][0], hash)) goto fail;
        }
        int rc = tree_store(db, ns, (uint8_t)level, prefix, hash);
        if (rc < 0) { stage = "store"; error = -rc; goto fail; }
        if (rc == 0) break;
        changed = 1;
    }
    EVP_MD_CTX_free(sha);
    return changed;
fail:
    EVP_MD_CTX_free(sha);
    DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_tree recompute failed ns=%s key=%016llx stage=%s rc=%d: %s",
        ns ? ns : "-", (unsigned long long)key, stage, error,
        db && (sqlite3_errcode(db) & 255) == (error & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(error));
    return -error;
}
