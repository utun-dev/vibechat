/** test_member_sync.c — standalone hash tree protocol test with 1000 members */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sqlite3.h>
#include <openssl/evp.h>

#define HASH_SZ 32
#define BUCKETS 32
#define MAX_LVL 5

static void _sanitize(const char* ch_id, char* out, size_t sz) {
    size_t i = 0;
    while (*ch_id && i < sz - 1) {
        char c = *ch_id++;
        if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_') out[i++]=c; else out[i++]='_';
    }
    out[i]=0;
}

static void _init_db(sqlite3* db) {
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS nodes(node_id INTEGER PRIMARY KEY,"
        " x25519_pubkey BLOB NOT NULL, ed25519_pubkey BLOB, online INTEGER DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS member_tree_hash(channel_id TEXT NOT NULL,"
        " level INTEGER NOT NULL, prefix64 INTEGER NOT NULL, hash BLOB NOT NULL,"
        " member_count INTEGER NOT NULL, PRIMARY KEY(channel_id, level, prefix64));",
        NULL, NULL, NULL);
}

static void _create_peers(sqlite3* db, const char* ch_id) {
    char san[64]; _sanitize(ch_id, san, sizeof(san));
    char sql[256]; snprintf(sql, sizeof(sql),
        "CREATE TABLE IF NOT EXISTS peers_%s(node_id INTEGER NOT NULL,"
        " join_sig BLOB NOT NULL, PRIMARY KEY(node_id))", san);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
}

static void _member_hash(uint64_t nid, const uint8_t* x25, const uint8_t* ed,
                         const uint8_t* sig, uint8_t out[HASH_SZ]) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, &nid, 8);
    EVP_DigestUpdate(ctx, x25, 32);
    EVP_DigestUpdate(ctx, ed, 32);
    EVP_DigestUpdate(ctx, sig, 64);
    EVP_DigestFinal_ex(ctx, out, NULL);
    EVP_MD_CTX_free(ctx);
}

static uint64_t _mask(int lvl) { int s=63-lvl*5; return s>=0 ? ~0ULL<<s : UINT64_MAX; }

static void _tree_hash(sqlite3* db, const char* ch_id, int lvl, uint64_t pref, uint8_t out[HASH_SZ]) {
    char san[64]; _sanitize(ch_id, san, sizeof(san));
    char sql[512]; snprintf(sql, sizeof(sql),
        "SELECT p.node_id, n.x25519_pubkey, n.ed25519_pubkey, p.join_sig"
        " FROM peers_%s p JOIN nodes n ON p.node_id=n.node_id"
        " WHERE (p.node_id & %lld)==%lld ORDER BY p.node_id ASC",
        san, (long long)_mask(lvl), (long long)pref);
    sqlite3_stmt* st=NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL)!=SQLITE_OK) { memset(out,0,HASH_SZ); return; }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    int cnt=0;
    while (sqlite3_step(st)==SQLITE_ROW) {
        uint64_t nid=(uint64_t)sqlite3_column_int64(st,0);
        const uint8_t* x=sqlite3_column_blob(st,1),*e=sqlite3_column_blob(st,2),*s=sqlite3_column_blob(st,3);
        if(!x||!e||!s)continue;
        uint8_t mh[HASH_SZ]; _member_hash(nid,x,e,s,mh); EVP_DigestUpdate(ctx,mh,HASH_SZ); cnt++;
    }
    sqlite3_finalize(st);
    if(cnt==0){EVP_MD_CTX_free(ctx);memset(out,0,HASH_SZ);return;}
    EVP_DigestFinal_ex(ctx,out,NULL);
    EVP_MD_CTX_free(ctx);
}

static void _insert(sqlite3* db, const char* ch_id, uint64_t nid,
                    const uint8_t* x, const uint8_t* e, const uint8_t* s) {
    char san[64]; _sanitize(ch_id, san, sizeof(san));
    sqlite3_stmt* st=NULL;
    sqlite3_prepare_v2(db,"INSERT OR REPLACE INTO nodes(node_id,x25519_pubkey,ed25519_pubkey) VALUES(?,?,?)",-1,&st,NULL);
    if(st){sqlite3_bind_int64(st,1,(sqlite3_int64)nid);sqlite3_bind_blob(st,2,x,32,SQLITE_STATIC);sqlite3_bind_blob(st,3,e,32,SQLITE_STATIC);sqlite3_step(st);sqlite3_finalize(st);}
    char sql[256]; snprintf(sql,sizeof(sql),"INSERT OR REPLACE INTO peers_%s(node_id,join_sig) VALUES(?,?)",san);
    st=NULL; sqlite3_prepare_v2(db,sql,-1,&st,NULL);
    if(st){sqlite3_bind_int64(st,1,(sqlite3_int64)nid);sqlite3_bind_blob(st,2,s,64,SQLITE_STATIC);sqlite3_step(st);sqlite3_finalize(st);}
}

static void _gen(uint64_t id, uint8_t x[32], uint8_t e[32], uint8_t s[64]) {
    memset(x,0,32); *(uint64_t*)x=id; memset(e,0,32); *(uint64_t*)e=id+1; memset(s,0,64); *(uint64_t*)s=id;
}

static int _count(sqlite3* db, const char* ch) {
    char san[64]; _sanitize(ch,san,sizeof(san)); char sql[256]; snprintf(sql,sizeof(sql),"SELECT COUNT(*) FROM peers_%s",san);
    sqlite3_stmt* st=NULL; sqlite3_prepare_v2(db,sql,-1,&st,NULL); int c=0;
    if(st&&sqlite3_step(st)==SQLITE_ROW)c=sqlite3_column_int(st,0); if(st)sqlite3_finalize(st); return c;
}

/* protocol resolve — recursively compare trees, exchange DATA for differences */
static void _resolve(sqlite3* a, sqlite3* b, const char* ch, int lvl, uint64_t pref) {
    uint8_t ha[BUCKETS][HASH_SZ], hb[BUCKETS][HASH_SZ];
    uint32_t bm_a=0,bm_b=0;
    int nl=lvl+1; if(nl>MAX_LVL)nl=MAX_LVL;
    for(int i=0;i<BUCKETS;i++) {
        uint64_t cp=pref|((uint64_t)i<<(63-nl*5>0?63-nl*5:0));
        _tree_hash(a,ch,nl,cp,ha[i]);_tree_hash(b,ch,nl,cp,hb[i]);
        int ae=1,be=1; for(int j=0;j<HASH_SZ;j++){if(ha[i][j])ae=0;if(hb[i][j])be=0;}
        if(!ae)bm_a|=(1u<<i); if(!be)bm_b|=(1u<<i);
    }
    for(int i=0;i<BUCKETS;i++) {
        int ah=(bm_a>>i)&1,bh=(bm_b>>i)&1; if(!ah&&!bh)continue;
        if(ah&&bh&&memcmp(ha[i],hb[i],HASH_SZ)==0)continue;
        uint64_t cp=pref|((uint64_t)i<<(63-nl*5>0?63-nl*5:0));
        char san[64];_sanitize(ch,san,sizeof(san)); int ca=0,cb=0;
        sqlite3_stmt* st=NULL; char sql[512];
        snprintf(sql,sizeof(sql),"SELECT COUNT(*) FROM peers_%s WHERE (node_id & %lld)==%lld",
                 san,(long long)_mask(nl),(long long)cp);
        sqlite3_prepare_v2(a,sql,-1,&st,NULL);if(st&&sqlite3_step(st)==SQLITE_ROW)ca=sqlite3_column_int(st,0);if(st)sqlite3_finalize(st);
        st=NULL;sqlite3_prepare_v2(b,sql,-1,&st,NULL);if(st&&sqlite3_step(st)==SQLITE_ROW)cb=sqlite3_column_int(st,0);if(st)sqlite3_finalize(st);
        if(nl>=MAX_LVL||ca<8||cb<8) {
            for(int side=0;side<2;side++) { sqlite3*src=side?a:b,*dst=side?b:a; sqlite3_stmt*r=NULL;
                sqlite3_prepare_v2(src,sql,-1,&r,NULL); /* count query already has our WHERE — rebuild for SELECT */
                char sq2[512]; snprintf(sq2,sizeof(sq2),
                    "SELECT p.node_id,n.x25519_pubkey,n.ed25519_pubkey,p.join_sig"
                    " FROM peers_%s p JOIN nodes n ON p.node_id=n.node_id"
                    " WHERE (p.node_id & %lld)==%lld ORDER BY p.node_id",
                    san,(long long)_mask(nl),(long long)cp);
                r=NULL; sqlite3_prepare_v2(src,sq2,-1,&r,NULL);
                if(r){while(sqlite3_step(r)==SQLITE_ROW){uint64_t nid=(uint64_t)sqlite3_column_int64(r,0);
                    const uint8_t* x=sqlite3_column_blob(r,1),*e=sqlite3_column_blob(r,2),*s=sqlite3_column_blob(r,3);
                    if(x&&e&&s)_insert(dst,ch,nid,x,e,s);} sqlite3_finalize(r);}
            }
        } else _resolve(a,b,ch,nl,cp);
    }
}

/* ── Tests ── */
static void t_empty(void) {
    printf("t_empty... "); sqlite3 *a,*b;
    sqlite3_open(":memory:",&a);sqlite3_open(":memory:",&b); _init_db(a);_init_db(b); _create_peers(a,"e");_create_peers(b,"e");
    uint8_t x[32],e[32],s[64]; _gen(1,x,e,s); _insert(a,"e",1,x,e,s); _insert(b,"e",1,x,e,s);
    assert(_count(a,"e")==1&&_count(b,"e")==1);
    uint8_t ha[HASH_SZ],hb[HASH_SZ]; _tree_hash(a,"e",1,_mask(1)&1,ha);_tree_hash(b,"e",1,_mask(1)&1,hb);
    assert(memcmp(ha,hb,HASH_SZ)==0); sqlite3_close(a);sqlite3_close(b); printf("OK\n");
}
static void t_1000(void) {
    printf("t_1000 (500+500)... "); sqlite3 *a,*b;
    sqlite3_open(":memory:",&a);sqlite3_open(":memory:",&b); _init_db(a);_init_db(b); _create_peers(a,"B");_create_peers(b,"B");
    for(int i=0;i<1000;i++){uint64_t id=((uint64_t)(i+1))&0x7FFFFFFFFFFFFFFFULL; uint8_t x[32],e[32],s[64]; _gen(id,x,e,s);
        if(i<500)_insert(a,"B",id,x,e,s); else _insert(b,"B",id,x,e,s);}
    assert(_count(a,"B")==500&&_count(b,"B")==500); _resolve(a,b,"B",1,0);
    printf("A=%d B=%d ",_count(a,"B"),_count(b,"B")); assert(_count(a,"B")==1000&&_count(b,"B")==1000);
    for(int i=0;i<BUCKETS;i++){uint64_t cp=(uint64_t)i<<58; uint8_t ha[HASH_SZ],hb[HASH_SZ];
        _tree_hash(a,"B",1,cp,ha);_tree_hash(b,"B",1,cp,hb); assert(memcmp(ha,hb,HASH_SZ)==0);}
    sqlite3_close(a);sqlite3_close(b); printf("OK\n");
}
static void t_overlap(void) {
    printf("t_overlap (300 shared, 200 unique each)... "); sqlite3 *a,*b;
    sqlite3_open(":memory:",&a);sqlite3_open(":memory:",&b); _init_db(a);_init_db(b); _create_peers(a,"o");_create_peers(b,"o");
    for(int i=0;i<700;i++){uint64_t id=((uint64_t)(i+1))&0x7FFFFFFFFFFFFFFFULL; uint8_t x[32],e[32],s[64]; _gen(id,x,e,s);
        if(i<500)_insert(a,"o",id,x,e,s); if(i>=200)_insert(b,"o",id,x,e,s);}
    assert(_count(a,"o")==500&&_count(b,"o")==500); _resolve(a,b,"o",1,0);
    assert(_count(a,"o")==700&&_count(b,"o")==700);
    int all_ok=1; for(int i=0;i<BUCKETS;i++){uint64_t cp=(uint64_t)i<<58; uint8_t ha[HASH_SZ],hb[HASH_SZ];
        _tree_hash(a,"o",1,cp,ha);_tree_hash(b,"o",1,cp,hb); if(memcmp(ha,hb,HASH_SZ)){all_ok=0;break;}} assert(all_ok);
    sqlite3_close(a);sqlite3_close(b); printf("OK\n");
}

int main(void) {
    printf("=== test_member_sync ===\n");
    t_empty(); t_overlap(); t_1000();
    printf("=== ALL TESTS PASSED ===\n");
    return 0;
}
