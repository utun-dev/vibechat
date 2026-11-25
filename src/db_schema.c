#include "db_schema.h"
#include "db_changes.h"
#include "../lib/debug_config.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

struct schema_table { const char* name; const char* body; };
/* Единственный источник описаний таблиц, включая локальные и служебные поля. */
static const struct schema_table tables[] = {
    {"node_snapshots", "(node_id INTEGER NOT NULL,x25519 BLOB NOT NULL CHECK(length(x25519)=32),timestamp INTEGER NOT NULL,"
        "record BLOB NOT NULL,format INTEGER NOT NULL DEFAULT 1,PRIMARY KEY(x25519)) WITHOUT ROWID"},
    {"nodes", "(node_id INTEGER NOT NULL,name TEXT,x25519_pubkey BLOB NOT NULL PRIMARY KEY CHECK(typeof(x25519_pubkey)='blob' AND length(x25519_pubkey)=32),ed25519_pubkey BLOB,last_seen_at INTEGER,"
        "online INTEGER DEFAULT 0,rtt INTEGER,client_type INTEGER NOT NULL DEFAULT 0,update_ts INTEGER DEFAULT 0,"
        "created_at INTEGER DEFAULT (unixepoch()),devgroup_ed BLOB,devgroup_x BLOB,devgroup_member_sig BLOB,"
        "devgroup_json TEXT,devgroup_profile_sig BLOB)"},
    {"node_addresses", "(id INTEGER PRIMARY KEY AUTOINCREMENT,node_x BLOB NOT NULL REFERENCES nodes(x25519_pubkey) ON DELETE CASCADE,"
        "family INTEGER NOT NULL CHECK(family IN (4,6)),protocol INTEGER NOT NULL DEFAULT 1,address BLOB NOT NULL,"
        "port INTEGER NOT NULL CHECK(port >= 0 AND port <= 65535),rtt INTEGER,addr_type INTEGER DEFAULT 0,config_type INTEGER DEFAULT 0,"
        "nat_mapping INTEGER NOT NULL DEFAULT 0,nat_inbound INTEGER NOT NULL DEFAULT 0,nat_source INTEGER NOT NULL DEFAULT 0,"
        "address_kind INTEGER NOT NULL DEFAULT 0,socket_id INTEGER NOT NULL DEFAULT 0,options BLOB,created_at INTEGER DEFAULT (unixepoch()))"},
    {"channels", "(channel_id TEXT NOT NULL UNIQUE,description_json TEXT NOT NULL,"
        "name TEXT GENERATED ALWAYS AS (json_extract(description_json,'$.name')) VIRTUAL NOT NULL,"
        "owner_node_id BLOB CHECK(owner_node_id IS NULL OR (typeof(owner_node_id)='blob' AND length(owner_node_id)=32 AND owner_node_id<>zeroblob(32))),x25519_pubkey BLOB NOT NULL UNIQUE CHECK(typeof(x25519_pubkey)='blob' AND length(x25519_pubkey)=32),"
        "x25519_privkey BLOB,ed25519_pubkey BLOB NOT NULL PRIMARY KEY CHECK(typeof(ed25519_pubkey)='blob' AND length(ed25519_pubkey)=32),"
        "ed25519_privkey BLOB,signature BLOB NOT NULL,"
        "avatar_version INTEGER NOT NULL DEFAULT 0,avatar_data BLOB,avatar_hash BLOB,avatar_signature BLOB,"
        "last_msg_at INTEGER DEFAULT 0,created_at INTEGER DEFAULT (unixepoch()),"
        "group_type TEXT GENERATED ALWAYS AS (json_extract(description_json,'$.group_type')) VIRTUAL,"
        "CHECK(channel_id=lower(hex(ed25519_pubkey))))"},
    {"group_policy", "(channel_id BLOB PRIMARY KEY NOT NULL CHECK(typeof(channel_id)='blob' AND length(channel_id)=32)"
        " REFERENCES channels(ed25519_pubkey) ON DELETE CASCADE,version INTEGER NOT NULL,all_members INTEGER NOT NULL,signature BLOB NOT NULL)"},
    {"member_invitations", "(channel TEXT NOT NULL REFERENCES channels(channel_id) ON DELETE CASCADE,"
        "subject BLOB NOT NULL CHECK(length(subject)=32),signer BLOB NOT NULL CHECK(length(signer)=32),"
        "stamp INTEGER NOT NULL CHECK(stamp>0),signature BLOB NOT NULL CHECK(length(signature)=64),"
        "PRIMARY KEY(channel,subject,signer)) WITHOUT ROWID"},
    {"member_sync_items", "(channel TEXT NOT NULL REFERENCES channels(channel_id) ON DELETE CASCADE,"
        "key BLOB NOT NULL CHECK(length(key)=32),wire BLOB NOT NULL,PRIMARY KEY(channel,key)) WITHOUT ROWID"},
    {"chat_join_recovery", "(channel TEXT NOT NULL PRIMARY KEY REFERENCES channels(channel_id) ON DELETE CASCADE,"
        "peer BLOB NOT NULL CHECK(length(peer)=32),inviter BLOB NOT NULL CHECK(length(inviter)=32),hints BLOB NOT NULL) WITHOUT ROWID"},
    {"member_sync_receives", "(id INTEGER PRIMARY KEY AUTOINCREMENT)"},
    {"member_sync_records", "(receive_id INTEGER NOT NULL REFERENCES member_sync_receives(id) ON DELETE CASCADE,"
        "key BLOB NOT NULL CHECK(length(key)=32),wire BLOB NOT NULL,PRIMARY KEY(receive_id,key)) WITHOUT ROWID"},
    {"devgroup_local", "(device_ed BLOB PRIMARY KEY NOT NULL CHECK(typeof(device_ed)='blob' AND length(device_ed)=32),ed_priv BLOB NOT NULL,x_priv BLOB NOT NULL,record BLOB NOT NULL)"},
    {"devgroup_receipts", "(device_ed BLOB NOT NULL CHECK(typeof(device_ed)='blob' AND length(device_ed)=32),"
        "id BLOB NOT NULL,peer BLOB NOT NULL CHECK(typeof(peer)='blob' AND length(peer)=32),PRIMARY KEY(device_ed,id))"},
    {"local_identity", "(id INTEGER PRIMARY KEY CHECK(id=1),name TEXT NOT NULL,"
        "x25519_pubkey BLOB NOT NULL,ed25519_pubkey BLOB NOT NULL CHECK(length(ed25519_pubkey)=32),"
        "created_at INTEGER DEFAULT (unixepoch()),updated_at INTEGER DEFAULT (unixepoch()))"},
    {"accounts", "(node_x BLOB NOT NULL PRIMARY KEY REFERENCES nodes(x25519_pubkey),display_name TEXT NOT NULL,"
        "avatar_color TEXT DEFAULT '#4A90E2',avatar_letter TEXT NOT NULL,is_contact INTEGER DEFAULT 1,created_at INTEGER DEFAULT (unixepoch()))"},
    {"ui_state", "(key TEXT PRIMARY KEY,value TEXT)"},
    {"clock_offsets", "(source INTEGER PRIMARY KEY CHECK(source BETWEEN 1 AND 3),offset_us INTEGER NOT NULL,received_us INTEGER NOT NULL)"},
    {"auto_socket_ports", "(if_name TEXT NOT NULL,family INTEGER NOT NULL,protocol INTEGER NOT NULL,port INTEGER NOT NULL,"
        "updated_at INTEGER DEFAULT (unixepoch()),UNIQUE(if_name,family,protocol))"},
    {"nat_contacts", "(local BLOB NOT NULL,remote BLOB NOT NULL,PRIMARY KEY(local,remote)) WITHOUT ROWID"},
    {"merkle_tree_hash", "(namespace TEXT NOT NULL,level INTEGER NOT NULL CHECK(level BETWEEN 0 AND 5),"
        "prefix64 INTEGER NOT NULL,hash BLOB NOT NULL CHECK(length(hash)=32),PRIMARY KEY(namespace,level,prefix64))"},
    {"media_files", "(id INTEGER PRIMARY KEY AUTOINCREMENT,media_id BLOB NOT NULL,block_id BLOB NOT NULL,content_hash BLOB NOT NULL,"
        "chat_id TEXT NOT NULL,location TEXT NOT NULL,holder BLOB NOT NULL CHECK(typeof(holder)='blob' AND length(holder)=32 AND holder<>zeroblob(32)),"
        "node_sign BLOB NOT NULL,timestamp INTEGER NOT NULL,"
        "file_size INTEGER NOT NULL,chunk_size INTEGER NOT NULL,chunk INTEGER NOT NULL,offset INTEGER NOT NULL)"},
    {"media_preparations", "(chat_id TEXT NOT NULL,location TEXT NOT NULL,PRIMARY KEY(chat_id,location))"},
    {"media_replication_meta", "(singleton INTEGER PRIMARY KEY CHECK(singleton=1),version INTEGER NOT NULL,source_epoch BLOB NOT NULL)"},
    {"block_availability", "(id INTEGER PRIMARY KEY AUTOINCREMENT,block_uuid BLOB NOT NULL CHECK(length(block_uuid)=16),"
        "group_id TEXT NOT NULL CHECK(length(group_id)=64),holder BLOB NOT NULL CHECK(typeof(holder)='blob' AND length(holder)=32 AND holder<>zeroblob(32)),"
        "chunk INTEGER NOT NULL CHECK(chunk BETWEEN 0 AND 4294967295),"
        "timestamp INTEGER NOT NULL CHECK(timestamp>=0),status INTEGER NOT NULL CHECK(status IN (0,1)),"
        "version INTEGER NOT NULL CHECK(version>0),signature BLOB NOT NULL CHECK(length(signature)=64),UNIQUE(group_id,block_uuid,holder))"},
    {"super_sync", "(group_id TEXT NOT NULL CHECK(length(group_id)=64),peer BLOB NOT NULL CHECK(typeof(peer)='blob' AND length(peer)=32 AND peer<>zeroblob(32)),"
        "source_epoch BLOB NOT NULL CHECK(length(source_epoch)=16),last_recv_id INTEGER NOT NULL CHECK(last_recv_id>=0),PRIMARY KEY(group_id,peer))"},
    {"attachment_objects", "(scope INTEGER NOT NULL CHECK(scope IN(1,2)),namespace TEXT NOT NULL CHECK(length(namespace) BETWEEN 1 AND 64),"
        "id BLOB NOT NULL CHECK(length(id)=16),author BLOB NOT NULL CHECK(typeof(author)='blob' AND length(author)=32),size INTEGER NOT NULL CHECK(size>=0),"
        "hash BLOB NOT NULL CHECK(length(hash)=32),crypto INTEGER NOT NULL,manifest BLOB NOT NULL CHECK(length(manifest) BETWEEN 1 AND 16384),"
        "PRIMARY KEY(scope,namespace,id),CHECK((scope=1 AND crypto IN(0,1)) OR (scope=2 AND crypto=2)))"},
    {"attachment_refs", "(owner INTEGER NOT NULL CHECK(owner IN(1,2,3)),scope INTEGER NOT NULL,namespace TEXT NOT NULL,"
        "message BLOB NOT NULL CHECK(length(message)=32),object BLOB NOT NULL CHECK(length(object)=16),"
        "released INTEGER NOT NULL DEFAULT 0 CHECK(released IN(0,1)),PRIMARY KEY(owner,namespace,message,object),"
        "FOREIGN KEY(scope,namespace,object) REFERENCES attachment_objects(scope,namespace,id),"
        "CHECK((owner IN(1,2) AND scope=1) OR (owner=3 AND scope=2)))"},
    {"attachment_jobs", "(id BLOB PRIMARY KEY CHECK(length(id)=16),owner INTEGER NOT NULL CHECK(owner IN(1,2,3)),"
        "namespace TEXT NOT NULL CHECK(length(namespace) BETWEEN 1 AND 64),namespace_token BLOB NOT NULL CHECK(length(namespace_token)=32),"
        "author BLOB NOT NULL CHECK(typeof(author)='blob' AND length(author)=32),source TEXT NOT NULL CHECK(length(source) BETWEEN 1 AND 1023),"
        "request BLOB NOT NULL CHECK(length(request) BETWEEN 1 AND 16384),state INTEGER NOT NULL CHECK(state BETWEEN 1 AND 5),"
        "object BLOB CHECK(object IS NULL OR length(object)=16),local_path TEXT,message BLOB,"
        "CHECK(state NOT IN(2,3) OR (object IS NOT NULL AND local_path IS NOT NULL AND length(local_path) BETWEEN 1 AND 1023)),"
        "CHECK((state=3 AND message IS NOT NULL AND length(message)=32) OR (state!=3 AND message IS NULL)))"},
    {"attachment_keys", "(channel TEXT NOT NULL,crypto_id BLOB NOT NULL CHECK(length(crypto_id)=16),message_id BLOB,author BLOB NOT NULL CHECK(typeof(author)='blob' AND length(author)=32),"
        "key BLOB CHECK(key IS NULL OR length(key)=32),cipher_hash BLOB,cipher_path TEXT NOT NULL,plain_path TEXT NOT NULL,"
        "info BLOB,content_type TEXT,ready INTEGER NOT NULL DEFAULT 0,downloaded INTEGER NOT NULL DEFAULT 0,"
        "wanted INTEGER NOT NULL DEFAULT 0,decrypting INTEGER NOT NULL DEFAULT 0,error INTEGER NOT NULL DEFAULT 0,"
        "PRIMARY KEY(channel,crypto_id),UNIQUE(channel,message_id))"},
    {"attachment_key_requests", "(channel TEXT NOT NULL,message_id BLOB NOT NULL,peer BLOB NOT NULL CHECK(typeof(peer)='blob' AND length(peer)=32),incoming INTEGER NOT NULL,"
        "state INTEGER NOT NULL,created_at INTEGER NOT NULL,PRIMARY KEY(channel,message_id,peer))"},
    {"attachment_key_outbox", "(channel TEXT NOT NULL,message_id BLOB NOT NULL,peer BLOB NOT NULL CHECK(typeof(peer)='blob' AND length(peer)=32),command INTEGER NOT NULL,"
        "retry_at INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(channel,message_id,peer))"},
    {"dm_conversations", "(conv_id TEXT PRIMARY KEY,peer_x25519 BLOB NOT NULL,"
        "peer_ed25519 BLOB NOT NULL,peer_name TEXT DEFAULT '',group_id TEXT NOT NULL DEFAULT '' CHECK(length(group_id) IN(0,64)),last_out_seq INTEGER DEFAULT 0,"
        "last_in_seq INTEGER DEFAULT 0,created_at INTEGER DEFAULT 0,last_ts INTEGER DEFAULT 0,mode INTEGER NOT NULL DEFAULT 0,"
        "local_party BLOB,peer_party BLOB,local_key BLOB,closed INTEGER NOT NULL DEFAULT 0)"},
    {"dm_messages", "(conv_id TEXT NOT NULL,dir INTEGER NOT NULL,seq INTEGER NOT NULL,ts INTEGER NOT NULL,author BLOB NOT NULL CHECK(typeof(author)='blob' AND length(author)=32),"
        "ct TEXT DEFAULT '',data BLOB,sig BLOB,body BLOB,PRIMARY KEY(conv_id,author,seq))"},
    {"dm_deleted_messages", "(conv_id TEXT NOT NULL,dir INTEGER NOT NULL,seq INTEGER NOT NULL,author BLOB NOT NULL CHECK(typeof(author)='blob' AND length(author)=32),"
        "body_hash BLOB NOT NULL,media_id BLOB,media_receipt BLOB,PRIMARY KEY(conv_id,author,seq))"},
    {"dm_outbox", "(conv_id TEXT NOT NULL,seq INTEGER NOT NULL,body BLOB NOT NULL,retry_at INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(conv_id,seq))"},
    {"dm_delivery", "(conv_id TEXT NOT NULL,author BLOB NOT NULL CHECK(typeof(author)='blob' AND length(author)=32),seq INTEGER NOT NULL,"
        "recipient BLOB NOT NULL CHECK(typeof(recipient)='blob' AND length(recipient)=32),receipt BLOB,"
        "first_attempt INTEGER NOT NULL DEFAULT 0,direct_until INTEGER NOT NULL DEFAULT 0,retry_at INTEGER NOT NULL DEFAULT 0,"
        "supernode BLOB CHECK(length(supernode)=32),submitted_to BLOB CHECK(length(submitted_to)=32),submitted_hash BLOB,"
        "PRIMARY KEY(conv_id,author,seq,recipient))"},
    {"dm_sequence", "(ed25519 BLOB PRIMARY KEY NOT NULL CHECK(typeof(ed25519)='blob' AND length(ed25519)=32),value INTEGER NOT NULL)"},
    {"dm_files", "(id BLOB PRIMARY KEY,conv_id TEXT NOT NULL,dir INTEGER NOT NULL,seq INTEGER NOT NULL,author BLOB NOT NULL CHECK(typeof(author)='blob' AND length(author)=32),"
        "recipient BLOB NOT NULL CHECK(typeof(recipient)='blob' AND length(recipient)=32),plain_size INTEGER NOT NULL,cipher_hash BLOB NOT NULL,body BLOB NOT NULL,local_path TEXT NOT NULL,"
        "ready INTEGER NOT NULL DEFAULT 0,holder BLOB NOT NULL CHECK(typeof(holder)='blob' AND length(holder)=32),expires INTEGER NOT NULL DEFAULT 0,receipt BLOB,"
        "downloading INTEGER NOT NULL DEFAULT 0,retry_at INTEGER NOT NULL DEFAULT 0,manual INTEGER NOT NULL DEFAULT 0,"
        "first_attempt INTEGER NOT NULL DEFAULT 0,UNIQUE(conv_id,author,seq))"},
    {"dm_file_targets", "(id BLOB NOT NULL,node BLOB NOT NULL CHECK(typeof(node)='blob' AND length(node)=32),"
        "side INTEGER NOT NULL,received BLOB,taken BLOB,PRIMARY KEY(id,node))"},
    {"dm_file_sources", "(id BLOB NOT NULL,node BLOB NOT NULL CHECK(typeof(node)='blob' AND length(node)=32),"
        "issuer BLOB NOT NULL CHECK(typeof(issuer)='blob' AND length(issuer)=32),expires INTEGER NOT NULL DEFAULT 0,"
        "PRIMARY KEY(id,node,issuer))"},
    {"dm_custody", "(id BLOB PRIMARY KEY,author BLOB NOT NULL CHECK(typeof(author)='blob' AND length(author)=32),recipient BLOB NOT NULL CHECK(typeof(recipient)='blob' AND length(recipient)=32),"
        "plain_size INTEGER NOT NULL,"
        "cipher_size INTEGER NOT NULL,body BLOB NOT NULL,body_hash BLOB NOT NULL,expires INTEGER NOT NULL,ready INTEGER NOT NULL DEFAULT 0,"
        "receipt BLOB,retry_at INTEGER NOT NULL DEFAULT 0,delete_on_open INTEGER NOT NULL DEFAULT 1)"},
    {"dm_custody_done", "(id BLOB PRIMARY KEY,body_hash BLOB NOT NULL,receipt BLOB,delete_on_open INTEGER NOT NULL DEFAULT 1)"},
    {"dm_custody_requests", "(supernode BLOB NOT NULL CHECK(typeof(supernode)='blob' AND length(supernode)=32),"
        "assignment_id BLOB NOT NULL,id BLOB NOT NULL,body_hash BLOB NOT NULL,"
        "rejected INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(supernode,assignment_id))"},
    {"dm_media_jobs", "(id BLOB PRIMARY KEY,author BLOB NOT NULL CHECK(typeof(author)='blob' AND length(author)=32),"
        "recipient_ed BLOB NOT NULL CHECK(typeof(recipient_ed)='blob' AND length(recipient_ed)=32),seq INTEGER NOT NULL,body BLOB,"
        "body_hash BLOB NOT NULL,holder BLOB CHECK(holder IS NULL OR (typeof(holder)='blob' AND length(holder)=32)),"
        "stored INTEGER NOT NULL DEFAULT 0,expires INTEGER NOT NULL DEFAULT 0,"
        "expired INTEGER NOT NULL DEFAULT 0,receipt BLOB,retry_at INTEGER NOT NULL DEFAULT 0,delete_on_open INTEGER NOT NULL DEFAULT -1)"},
    {"dm_media_holders", "(id BLOB NOT NULL,node BLOB NOT NULL CHECK(typeof(node)='blob' AND length(node)=32),"
        "assignment_id BLOB NOT NULL,released INTEGER NOT NULL DEFAULT 0,"
        "PRIMARY KEY(id,node))"},
    {"dm_pending_messages", "(recipient BLOB NOT NULL CHECK(length(recipient)=32),sender BLOB NOT NULL CHECK(length(sender)=32),seq INTEGER NOT NULL,msg BLOB NOT NULL,"
        "retry_at INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(recipient,sender,seq))"},
    {"dm_mail_receipts", "(recipient BLOB NOT NULL CHECK(length(recipient)=32),sender BLOB NOT NULL CHECK(length(sender)=32),seq INTEGER NOT NULL,receipt BLOB NOT NULL,"
        "PRIMARY KEY(recipient,sender,seq))"},
    {"dm_history", "(conv_id TEXT PRIMARY KEY,generation INTEGER NOT NULL,hidden INTEGER NOT NULL)"},
    {"dm_local_files", "(path TEXT PRIMARY KEY,conv_id TEXT NOT NULL,generation INTEGER NOT NULL,busy INTEGER NOT NULL DEFAULT 0,"
        "retry_at INTEGER NOT NULL DEFAULT 0)"},
    {"kanban_ops", "(group_id BLOB NOT NULL CHECK(typeof(group_id)='blob' AND length(group_id)=32) REFERENCES channels(ed25519_pubkey) ON DELETE CASCADE,"
        "id BLOB NOT NULL CHECK(length(id)=32),wire BLOB NOT NULL,body TEXT NOT NULL,author BLOB NOT NULL CHECK(typeof(author)='blob' AND length(author)=32),"
        "time INTEGER NOT NULL,generation BLOB NOT NULL CHECK(length(generation)=32),action INTEGER NOT NULL,"
        "entity TEXT NOT NULL,bucket INTEGER NOT NULL,active INTEGER NOT NULL DEFAULT 1,PRIMARY KEY(group_id,id),"
        "UNIQUE(group_id,author,time))"},
    {"kanban_state", "(group_id BLOB PRIMARY KEY NOT NULL CHECK(typeof(group_id)='blob' AND length(group_id)=32) REFERENCES channels(ed25519_pubkey) ON DELETE CASCADE,"
        "snapshot TEXT NOT NULL DEFAULT '{}',pending INTEGER NOT NULL DEFAULT 0)"},
    {"kanban_clock", "(group_id BLOB NOT NULL CHECK(typeof(group_id)='blob' AND length(group_id)=32) REFERENCES channels(ed25519_pubkey) ON DELETE CASCADE,"
        "author BLOB NOT NULL CHECK(typeof(author)='blob' AND length(author)=32),last_time INTEGER NOT NULL,PRIMARY KEY(group_id,author))"},
    {"kanban_items", "(group_id BLOB NOT NULL CHECK(typeof(group_id)='blob' AND length(group_id)=32) REFERENCES channels(ed25519_pubkey) ON DELETE CASCADE,"
        "id TEXT NOT NULL,kind TEXT NOT NULL CHECK(kind IN('card','column')),ordinal INTEGER NOT NULL DEFAULT 0,"
        "data_json TEXT NOT NULL CHECK(json_valid(data_json)),"
        "PRIMARY KEY(group_id,id))"},
    {"kanban_card_properties", "(group_id BLOB NOT NULL CHECK(typeof(group_id)='blob' AND length(group_id)=32),card_id TEXT NOT NULL,kind TEXT NOT NULL,"
        "property_id TEXT NOT NULL,data_json TEXT NOT NULL CHECK(json_valid(data_json)),"
        "created_by BLOB NOT NULL CHECK(length(created_by)=32),created_at INTEGER NOT NULL,"
        "updated_by BLOB NOT NULL CHECK(length(updated_by)=32),updated_at INTEGER NOT NULL,"
        "revision BLOB NOT NULL CHECK(length(revision)=32),deleted INTEGER NOT NULL DEFAULT 0,"
        "PRIMARY KEY(group_id,card_id,kind,property_id),"
        "FOREIGN KEY(group_id,card_id) REFERENCES kanban_items(group_id,id) ON DELETE CASCADE)"},
    {"member_chains", "(group_id BLOB NOT NULL CHECK(typeof(group_id)='blob' AND length(group_id)=32) REFERENCES channels(ed25519_pubkey) ON DELETE CASCADE,"
        "member_id BLOB NOT NULL CHECK(length(member_id)=32),head_seq INTEGER NOT NULL CHECK(head_seq>0),"
        "head_hash BLOB NOT NULL CHECK(length(head_hash)=32),"
        "PRIMARY KEY(group_id,member_id)) WITHOUT ROWID"},
    {"member_faults", "(group_id BLOB NOT NULL CHECK(typeof(group_id)='blob' AND length(group_id)=32) REFERENCES channels(ed25519_pubkey) ON DELETE CASCADE,"
        "member_id BLOB NOT NULL CHECK(length(member_id)=32),seq INTEGER NOT NULL CHECK(seq>0),"
        "first_wire BLOB NOT NULL,second_wire BLOB NOT NULL,PRIMARY KEY(group_id,member_id)) WITHOUT ROWID"},
    {"member_log", "(group_id BLOB NOT NULL CHECK(typeof(group_id)='blob' AND length(group_id)=32),member_id BLOB NOT NULL CHECK(length(member_id)=32),"
        "seq INTEGER NOT NULL CHECK(seq>0),prev_hash BLOB NOT NULL CHECK(length(prev_hash)=32),"
        "block_hash BLOB NOT NULL CHECK(length(block_hash)=32),time_us INTEGER NOT NULL CHECK(time_us>0),"
        "scope_id BLOB NOT NULL CHECK(length(scope_id)=32),grant_id BLOB NOT NULL CHECK(length(grant_id)=32),"
        "body BLOB NOT NULL CHECK(length(body) BETWEEN 1 AND 7860),signature BLOB NOT NULL CHECK(length(signature)=64),"
        "applied INTEGER NOT NULL CHECK(applied IN(0,1)),PRIMARY KEY(group_id,member_id,seq),"
        "FOREIGN KEY(group_id,member_id) REFERENCES member_chains(group_id,member_id) ON DELETE CASCADE"
        " DEFERRABLE INITIALLY DEFERRED) WITHOUT ROWID"},
    {"chat_seals", "(group_id BLOB NOT NULL CHECK(typeof(group_id)='blob' AND length(group_id)=32) REFERENCES channels(ed25519_pubkey) ON DELETE CASCADE,"
        "id BLOB NOT NULL CHECK(length(id)=32),wire BLOB NOT NULL CHECK(length(wire)=249),PRIMARY KEY(group_id,id)) WITHOUT ROWID"},
    {"chat_seal_evidence", "(group_id BLOB NOT NULL CHECK(typeof(group_id)='blob' AND length(group_id)=32) REFERENCES channels(ed25519_pubkey) ON DELETE CASCADE,"
        "seal_id BLOB NOT NULL CHECK(length(seal_id)=32),seq INTEGER NOT NULL CHECK(seq>0),wire BLOB NOT NULL,"
        "PRIMARY KEY(group_id,seal_id,seq)) WITHOUT ROWID"},
    {"chat_seal_anchors", "(group_id BLOB NOT NULL CHECK(typeof(group_id)='blob' AND length(group_id)=32) REFERENCES channels(ed25519_pubkey) ON DELETE CASCADE,"
        "seal_id BLOB NOT NULL CHECK(length(seal_id)=32),seq INTEGER NOT NULL,hash BLOB NOT NULL CHECK(length(hash)=32),"
        "PRIMARY KEY(group_id,seal_id)) WITHOUT ROWID"},
    {"chat_heads", "(group_id BLOB NOT NULL CHECK(typeof(group_id)='blob' AND length(group_id)=32) REFERENCES channels(ed25519_pubkey) ON DELETE CASCADE,"
        "id BLOB NOT NULL CHECK(length(id)=32),bucket INTEGER NOT NULL,wire BLOB NOT NULL,PRIMARY KEY(group_id,id)) WITHOUT ROWID"},
    {"chat_effects", "(group_id BLOB NOT NULL CHECK(typeof(group_id)='blob' AND length(group_id)=32) REFERENCES channels(ed25519_pubkey) ON DELETE CASCADE,"
        "message_id BLOB NOT NULL CHECK(length(message_id)=32),version INTEGER NOT NULL,"
        "initial INTEGER NOT NULL CHECK(initial IN(0,1)),updated INTEGER NOT NULL CHECK(updated IN(0,1)),"
        "notified INTEGER NOT NULL DEFAULT 0 CHECK(notified IN(0,1)),PRIMARY KEY(group_id,message_id)) WITHOUT ROWID"},
    /* Последние три записи — шаблоны таблиц групп, не общие таблицы БД. */
    {"peers", "(id INTEGER PRIMARY KEY AUTOINCREMENT,node_id INTEGER NOT NULL,"
        "record_state INTEGER NOT NULL DEFAULT 0 CHECK(record_state IN (0,2)),"
        "x25519_pubkey BLOB NOT NULL,ed25519_pubkey BLOB NOT NULL,"
        "join_sig BLOB NOT NULL,join_ts INTEGER NOT NULL,update_sig BLOB,update_ts INTEGER DEFAULT 0,userinfo TEXT NOT NULL DEFAULT '',"
        "signed_by BLOB CHECK(signed_by IS NULL OR (typeof(signed_by)='blob' AND length(signed_by)=32 AND signed_by<>zeroblob(32))),signature BLOB,adm_tags TEXT,adm_tags_sig BLOB,node_type INTEGER NOT NULL DEFAULT 0,"
        "connected INTEGER NOT NULL DEFAULT 0,storage INTEGER NOT NULL DEFAULT 0,deleted INTEGER NOT NULL DEFAULT 0,"
        "local_nick TEXT,radio_muted INTEGER NOT NULL DEFAULT 0,admitted INTEGER NOT NULL DEFAULT 0,identity_verified INTEGER NOT NULL DEFAULT 0,"
        "source INTEGER NOT NULL DEFAULT 0)"},
    {"messages", "(timestamp INTEGER NOT NULL,id INTEGER NOT NULL,flags INTEGER NOT NULL DEFAULT 0,data BLOB,"
        "author_signature BLOB NOT NULL,message_id BLOB NOT NULL UNIQUE CHECK(length(message_id)=32),bucket INTEGER NOT NULL,"
        "local_attrs TEXT DEFAULT '',delivered_peers INTEGER NOT NULL DEFAULT 0,delivery_chain TEXT NOT NULL DEFAULT '',"
        "nonce BLOB NOT NULL CHECK(length(nonce)=16),ver INTEGER NOT NULL CHECK(ver>0),edited_at INTEGER NOT NULL CHECK(edited_at>=0),"
        "generation BLOB NOT NULL CHECK(length(generation)=32),accepted_key BLOB NOT NULL CHECK(length(accepted_key)=32),feedback_snapshot TEXT)"},
    {"feedback", "(record_key BLOB PRIMARY KEY CHECK(length(record_key)=32),bucket INTEGER NOT NULL,"
        "message_id BLOB NOT NULL CHECK(length(message_id)=32),author_ed BLOB NOT NULL CHECK(length(author_ed)=32),kind INTEGER NOT NULL,"
        "ver INTEGER NOT NULL CHECK(ver>0),data BLOB NOT NULL,signature BLOB NOT NULL CHECK(length(signature)=64),UNIQUE(message_id,author_ed,kind))"}
};

static __thread char last_error[2048];
static __thread int last_incompatible;
const char* db_schema_last_error(void) { return last_error; }
int db_schema_is_incompatible(void) { return last_incompatible; }
void db_schema_clear_error(void) { last_error[0] = '\0'; last_incompatible = 0; }

static int schema_error(sqlite3* db, const char* object, const char* reason, int incompatible) {
    last_incompatible = incompatible;
    const char* path = db ? sqlite3_db_filename(db, "main") : NULL;
    if (!path || !*path) path = ":memory:";
    snprintf(last_error, sizeof(last_error), "%s\n"
        "Файл: %s\nОбъект: %s\nПричина: %s\n"
        "Запуск остановлен, база не изменена.",
        incompatible ? "База данных несовместима с этой версией приложения." : "Не удалось проверить структуру базы данных.",
        path, object ? object : "(database)", reason);
    DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Database schema rejected: file=%s object=%s reason=%s. Startup stopped; database unchanged.",
        path, object ? object : "(database)", reason);
    return -1;
}

static int schema_failure(sqlite3* db, const char* object, const char* reason, int incompatible, int code) {
    char detail[1536]; snprintf(detail, sizeof(detail), "%s (SQLite %d: %s)", reason, code, sqlite3_errstr(code));
    schema_error(db, object, detail, incompatible);
    return -(code != SQLITE_OK && code != SQLITE_ROW && code != SQLITE_DONE ? code : SQLITE_ERROR);
}

static const char* table_body(const char* name, const char* type) {
    if (!type) {
        const char* prefixes[] = {"peers_", "archive_msg_", "msg_", "feedback_"};
        const char* types[] = {"peers", "messages", "messages", "feedback"};
        for (unsigned i = 0; i < 4; i++) {
            size_t n = strlen(prefixes[i]);
            if (!strncmp(name, prefixes[i], n) && name[n] && strlen(name + n) == 64 && strspn(name + n, "0123456789abcdef") == 64) type = types[i];
        }
    }
    unsigned limit = sizeof(tables) / sizeof(tables[0]) - (type ? 0 : 3);
    for (unsigned i = 0; i < limit; i++)
        if (!strcmp(type ? type : name, tables[i].name)) return tables[i].body;
    return NULL;
}

/* Сравниваем SQL-токены: пробелы не склеивают имя поля и тип. Литералы сохраняют регистр. */
static size_t sql_token(const char** sql, const char** text, int* literal) {
    const char* p = *sql;
    while (isspace((unsigned char)*p)) p++;
    const char* start = p; *literal = 0;
    if (*p == '"' || *p == '`' || *p == '[') {
        char end = *p == '[' ? ']' : *p;
        start = ++p;
        while (*p) {
            if (*p != end) { p++; continue; }
            if (end != ']' && p[1] == end) { p += 2; continue; }
            break;
        }
        *text = start; size_t n = (size_t)(p - start); *sql = *p ? p + 1 : p; return n;
    }
    if (*p == '\'') {
        *literal = 1; p++;
        while (*p) {
            if (*p++ != '\'') continue;
            if (*p != '\'') break;
            p++;
        }
    } else if (isalnum((unsigned char)*p) || *p == '_' || (unsigned char)*p >= 128) {
        do { p++; } while (isalnum((unsigned char)*p) || *p == '_' || *p == '$' || (unsigned char)*p >= 128);
    } else if (*p) {
        char first = *p++;
        if ((strchr("<>=!", first) && *p == '=') || (first == '<' && *p == '>')) p++;
    }
    *text = start; *sql = p; return (size_t)(p - start);
}

static int sql_equal(const char* a, const char* b) {
    for (;;) {
        const char *x, *y; int literal_x, literal_y;
        size_t nx = sql_token(&a, &x, &literal_x), ny = sql_token(&b, &y, &literal_y);
        if (nx != ny || literal_x != literal_y) return 0;
        if (!nx) return 1;
        for (size_t i = 0; i < nx; i++)
            if ((literal_x ? (unsigned char)x[i] : tolower((unsigned char)x[i])) !=
                (literal_y ? (unsigned char)y[i] : tolower((unsigned char)y[i]))) return 0;
    }
}

struct schema_index { const char* owner; const char* name; const char* columns; int unique; };
static const struct schema_index indexes[] = {
    {"attachment_refs", "attachment_refs_object", "(scope,namespace,object)", 0},
    {"member_log", "member_log_scope", "(group_id,scope_id,member_id,seq)", 0},
    {"chat_heads", "chat_heads_bucket", "(group_id,bucket,id)", 0},
    {"chat_seals", "chat_seals_checkpoint", "(group_id,substr(wire,50,32),substr(wire,114,40))", 0},
    {"kanban_ops", "kanban_ops_bucket", "(group_id,bucket,id) WHERE active=1", 0},
    {"kanban_ops", "kanban_ops_entity", "(group_id,entity,time)", 0},
    {"nodes", "idx_nodes_route", "(node_id)", 0},
    {"nodes", "idx_nodes_ed", "(ed25519_pubkey)", 0},
    {"node_snapshots", "idx_node_snapshots_route", "(node_id)", 0},
    {"node_addresses", "idx_na_node", "(node_x)", 0},
    {"node_addresses", "idx_na_unique", "(node_x,family,socket_id,protocol,address,port,address_kind)", 1},
    {"node_addresses", "idx_na_addr_lookup", "(family,address,port,socket_id)", 0},
    {"nodes", "nodes_devgroup", "(devgroup_ed)", 0},
    {"media_files", "idx_media_files_block_id", "(block_id)", 1},
    {"media_files", "idx_media_files_media_id", "(media_id)", 0},
    {"media_files", "idx_media_files_chat_id", "(chat_id)", 0},
    {"media_files", "idx_media_files_holder", "(holder)", 0},
    {"media_files", "idx_media_files_content_hash", "(content_hash)", 0},
    {"block_availability", "idx_ba_group_id", "(group_id,id)", 0},
    {"dm_messages", "dm_messages_time", "(conv_id,ts,author,seq)", 0},
    {"dm_media_holders", "dm_media_assignment_id", "(assignment_id)", 1},
    {"peers", "idx_%s_storage", "(storage)", 0},
    {"peers", "idx_%s_member_version", "(ed25519_pubkey,record_state)", 1},
    {"peers", "idx_%s_member_node", "(node_id) WHERE record_state=0", 0},
    {"peers", "idx_%s_transport_key", "(x25519_pubkey) WHERE record_state=0", 0},
    {"peers", "idx_%s_member_join", "(ed25519_pubkey) WHERE record_state=0", 0},
    {"messages", "idx_%s_ttl", "(accepted_key,timestamp)", 0},
    {"messages", "idx_%s_bucket", "(bucket,message_id)", 0},
    {"feedback", "idx_%s_bucket", "(bucket,record_key)", 0},
    {"feedback", "idx_%s_author", "(author_ed,bucket)", 0}
};

static const char attachment_trigger[] = "CREATE TRIGGER attachment_channel_delete AFTER DELETE ON channels BEGIN"
    " DELETE FROM attachment_refs WHERE scope=1 AND namespace=OLD.channel_id;"
    " UPDATE attachment_jobs SET state=5 WHERE owner IN(1,2) AND namespace=OLD.channel_id AND state IN(1,2);"
    " DELETE FROM attachment_key_outbox WHERE channel=OLD.channel_id;"
    " DELETE FROM attachment_key_requests WHERE channel=OLD.channel_id;"
    " DELETE FROM attachment_keys WHERE channel=OLD.channel_id;"
    " SELECT CASE WHEN EXISTS(SELECT 1 FROM attachment_key_outbox WHERE channel=OLD.channel_id)"
    " OR EXISTS(SELECT 1 FROM attachment_key_requests WHERE channel=OLD.channel_id)"
    " OR EXISTS(SELECT 1 FROM attachment_keys WHERE channel=OLD.channel_id)"
    " THEN RAISE(ABORT,'attachment channel cleanup incomplete') END; END";

static const char public_objects_view[] = "CREATE VIEW attachment_public_objects AS SELECT o.* FROM attachment_objects o"
    " WHERE o.scope=1 AND EXISTS(SELECT 1 FROM attachment_refs r WHERE r.scope=o.scope AND r.namespace=o.namespace AND r.object=o.id)";
static const char public_files_view[] = "CREATE VIEW media_public_files AS SELECT f.* FROM media_files f JOIN attachment_public_objects o"
    " ON o.namespace=f.chat_id AND o.id=f.media_id AND o.hash=f.content_hash AND o.size=f.file_size";

/* create разрешён только для нового объекта. Проверка никогда не восстанавливает отсутствующие объекты. */
static int schema_object(sqlite3* db, const char* name, const char* type, const char* ddl, int create) {
    if (!name || !type || !ddl) return schema_failure(db, name, "schema definition unavailable", 0, SQLITE_MISUSE);
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT type,sql FROM sqlite_master WHERE name=?", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int present = rc == SQLITE_ROW;
    const char* actual_type = present ? (const char*)sqlite3_column_text(st, 0) : NULL;
    const char* actual_sql = present ? (const char*)sqlite3_column_text(st, 1) : NULL;
    int equal = actual_type && actual_sql && !strcmp(actual_type, type) && sql_equal(actual_sql, ddl);
    int final = sqlite3_finalize(st);
    if ((present && (!actual_type || !actual_sql)) || (rc != SQLITE_ROW && rc != SQLITE_DONE) || final != SQLITE_OK) {
        int code = rc != SQLITE_ROW && rc != SQLITE_DONE ? rc : present && (!actual_type || !actual_sql)
            ? (sqlite3_errcode(db) == SQLITE_NOMEM ? SQLITE_NOMEM : SQLITE_CORRUPT) : final;
        return schema_failure(db, name, sqlite3_errmsg(db), 0, code);
    }
    if (equal) return 0;
    if (present) return schema_failure(db, name, "определение индекса, представления или триггера отличается", 1, SQLITE_SCHEMA);
    if (!create) return schema_failure(db, name, "отсутствует обязательный объект схемы", 1, SQLITE_SCHEMA);
    rc = sqlite3_exec(db, ddl, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return schema_failure(db, name, sqlite3_errmsg(db), 0, rc);
    return 0;
}

static int schema_indexes(sqlite3* db, const char* table, const char* type, int create) {
    const char* body = table_body(table, type);
    for (unsigned i = 0; i < sizeof(indexes) / sizeof(indexes[0]); i++) {
        const struct schema_index* index = &indexes[i];
        if (table_body(index->owner, index->owner) != body) continue;
        if (!strncmp(table, "archive_", 8)) continue;
        char* name = sqlite3_mprintf(index->name, table);
        char* ddl = name ? sqlite3_mprintf("CREATE %sINDEX \"%w\" ON \"%w\"%s",
            index->unique ? "UNIQUE " : "", name, table, index->columns) : NULL;
        int failed = !ddl ? schema_failure(db, table, "index allocation failed", 0, SQLITE_NOMEM) : schema_object(db, name, "index", ddl, create);
        sqlite3_free(ddl); sqlite3_free(name);
        if (failed) return failed;
    }
    return 0;
}

int db_schema_exists(sqlite3* db, const char* name) {
    if (!db || !name || !*name) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "schema lookup: missing database/name"); return -SQLITE_MISUSE;
    }
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE name=? AND type IN ('table','view')", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int found = rc == SQLITE_ROW, final = sqlite3_finalize(st);
    if ((rc != SQLITE_ROW && rc != SQLITE_DONE) || final != SQLITE_OK) {
        int code = rc != SQLITE_ROW && rc != SQLITE_DONE ? rc : final;
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "schema lookup failed name=%s step=%d finalize=%d: %s", name, rc, final,
            (sqlite3_errcode(db) & 255) == (code & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(code)); return -code;
    }
    return found;
}

static int schema_table_result(sqlite3* db, const char* name, const char* type) {
    const char* body = name ? table_body(name, type) : NULL;
    if (!db || !body) return schema_failure(db, name, "unknown table", db && name && !type, SQLITE_MISUSE);
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT type,sql FROM sqlite_master WHERE name=?", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    char* ddl = sqlite3_mprintf("CREATE TABLE \"%w\"%s", name, body);
    int present = rc == SQLITE_ROW, matches = 0;
    const char* actual_type = present ? (const char*)sqlite3_column_text(st, 0) : NULL;
    const char* actual_sql = present ? (const char*)sqlite3_column_text(st, 1) : NULL;
    if (actual_type && actual_sql && ddl && !strcmp(actual_type, "table")) matches = sql_equal(actual_sql, ddl);
    int final = sqlite3_finalize(st);
    if (!ddl || (present && (!actual_type || !actual_sql)) || (rc != SQLITE_ROW && rc != SQLITE_DONE) || final != SQLITE_OK) {
        const char* reason = !ddl ? "table definition allocation failed" : sqlite3_errmsg(db);
        int code = rc != SQLITE_ROW && rc != SQLITE_DONE ? rc : !ddl ? SQLITE_NOMEM :
            present && (!actual_type || !actual_sql) ? (sqlite3_errcode(db) == SQLITE_NOMEM ? SQLITE_NOMEM : SQLITE_CORRUPT) : final;
        sqlite3_free(ddl); return schema_failure(db, name, reason, 0, code);
    }
    if (present && !matches) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Database expected table=%s definition=%s", name, body);
        sqlite3_free(ddl);
        return schema_failure(db, name, "отличаются столбцы, значения по умолчанию или ограничения таблицы", 1, SQLITE_SCHEMA);
    }
    if (present) { sqlite3_free(ddl); return schema_indexes(db, name, type, 0); }
    rc = sqlite3_exec(db, "SAVEPOINT create_schema_table", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { sqlite3_free(ddl); return schema_failure(db, name, sqlite3_errmsg(db), 0, rc); }
    rc = sqlite3_exec(db, ddl, NULL, NULL, NULL);
    sqlite3_free(ddl);
    if (rc != SQLITE_OK) { schema_failure(db, name, sqlite3_errmsg(db), 0, rc); goto fail; }
    { int indexed = schema_indexes(db, name, type, 1); if (indexed) { rc = -indexed; goto fail; } }
    db_changes_watch(db, name);
    rc = sqlite3_exec(db, "RELEASE create_schema_table", NULL, NULL, NULL); if (rc == SQLITE_OK) return 0;
    schema_failure(db, name, sqlite3_errmsg(db), 0, rc);
fail:
    { int rollback = sqlite3_exec(db, "ROLLBACK TO create_schema_table", NULL, NULL, NULL);
      if (!rollback) rollback = sqlite3_exec(db, "RELEASE create_schema_table", NULL, NULL, NULL);
      if (rollback) DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Table schema rollback failed table=%s rc=%d primary=%d: %s",
          name, rollback, rc, sqlite3_errmsg(db)); }
    return -rc;
}

int db_schema_table_ex(sqlite3* db, const char* name, const char* type) {
    int rc = schema_table_result(db, name, type); return rc < 0 ? -rc : SQLITE_OK;
}

int db_schema_table(sqlite3* db, const char* name, const char* type) {
    return db_schema_table_ex(db, name, type) == SQLITE_OK ? 0 : -1;
}

int db_schema_history(sqlite3* db, const char* table) {
    if (!db || !table || !*table) return schema_error(db, table, "invalid history table", 0);
    if (sqlite3_exec(db, "SAVEPOINT history_schema", NULL, NULL, NULL) != SQLITE_OK)
        return schema_error(db, table, sqlite3_errmsg(db), 0);
    char* archive = sqlite3_mprintf("archive_%s", table);
    char* view = sqlite3_mprintf("history_%s", table);
    char* ddl = archive && view ? sqlite3_mprintf("CREATE VIEW \"%w\" AS SELECT *,0 AS archived FROM \"%w\""
        " UNION ALL SELECT *,1 AS archived FROM \"%w\"", view, table, archive) : NULL;
    int failed = !ddl ? schema_error(db, table, "history allocation failed", 0) :
        db_schema_table(db, table, "messages") || db_schema_table(db, archive, "messages") || schema_object(db, view, "view", ddl, 1);
    sqlite3_free(ddl); sqlite3_free(view); sqlite3_free(archive);
    if (!failed && sqlite3_exec(db, "RELEASE history_schema", NULL, NULL, NULL) == SQLITE_OK) return 0;
    if (!failed) schema_error(db, table, sqlite3_errmsg(db), 0);
    if (sqlite3_exec(db, "ROLLBACK TO history_schema; RELEASE history_schema", NULL, NULL, NULL) != SQLITE_OK)
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "History schema rollback failed: %s", sqlite3_errmsg(db));
    return -1;
}

int db_schema_attachment_views(sqlite3* db) {
    if (db_schema_table(db, "attachment_objects", NULL) || db_schema_table(db, "attachment_refs", NULL)) return -1;
    return schema_object(db, "attachment_public_objects", "view", public_objects_view, 1) ||
        schema_object(db, "media_public_files", "view", public_files_view, 1) ? -1 : 0;
}

static int schema_create(sqlite3* db) {
    for (unsigned i = 0; i < sizeof(tables) / sizeof(tables[0]) - 3; i++)
        if (db_schema_table(db, tables[i].name, NULL)) return -1;
    return db_schema_attachment_views(db) || schema_object(db, "attachment_channel_delete", "trigger", attachment_trigger, 1) ? -1 : 0;
}

/* Эталон уже содержит общие таблицы; добавляем только обнаруженные шаблоны групп. */
static int schema_check_reference(sqlite3* db, sqlite3* expected) {
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'", -1, &st, NULL);
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char* name = (const char*)sqlite3_column_text(st, 0);
        if (!name) {
            sqlite3_finalize(st); return schema_error(db, NULL, "table name unavailable", 0);
        }
        int dynamic = !strncmp(name, "peers_", 6) || !strncmp(name, "msg_", 4) ||
            !strncmp(name, "archive_msg_", 12) || !strncmp(name, "feedback_", 9);
        if (!table_body(name, NULL)) {
            schema_error(db, name, "unknown table", 1); sqlite3_finalize(st); return -1;
        }
        if (!dynamic) continue;
        int failed = !strncmp(name, "msg_", 4) ? db_schema_history(expected, name) : db_schema_table(expected, name, NULL);
        if (failed) { sqlite3_finalize(st); return -1; }
    }
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK) {
        return schema_error(db, NULL, sqlite3_errmsg(db), 0);
    }
    int failed = 0;
    /* Сверка в обе стороны обнаруживает пропущенные и лишние объекты, включая триггеры и views. */
    for (unsigned direction = 0; direction < 2 && !failed; direction++) {
        sqlite3* source = direction ? db : expected;
        sqlite3* target = direction ? expected : db;
        rc = sqlite3_prepare_v2(source, "SELECT name,type,sql FROM sqlite_master WHERE name NOT LIKE 'sqlite_%' ORDER BY name", -1, &st, NULL);
        if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            if (schema_object(target, (const char*)sqlite3_column_text(st, 0), (const char*)sqlite3_column_text(st, 1),
                (const char*)sqlite3_column_text(st, 2), 0)) {
                if (direction && last_incompatible) schema_error(db, (const char*)sqlite3_column_text(st, 0), "лишний или изменённый объект схемы", 1);
                failed = 1; break;
            }
        }
        final = sqlite3_finalize(st);
        if (!failed && (rc != SQLITE_DONE || final != SQLITE_OK)) failed = schema_error(db, NULL, sqlite3_errmsg(source), 0);
    }
    if (failed) return -1;
    return 0;
}

int db_schema_check(sqlite3* db) {
    db_schema_clear_error();
    if (!db) return schema_error(db, NULL, "database unavailable", 0);
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name", -1, &st, NULL);
    unsigned count = 0;
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char* name = (const char*)sqlite3_column_text(st, 0);
        if (!name) { sqlite3_finalize(st); return schema_error(db, NULL, "table name unavailable", 0); }
        if (db_schema_table(db, name, NULL)) { sqlite3_finalize(st); return -1; }
        count++;
    }
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK) return schema_error(db, NULL, sqlite3_errmsg(db), 0);
    sqlite3* expected = NULL;
    if (sqlite3_open(":memory:", &expected) != SQLITE_OK || schema_create(expected)) {
        schema_error(db, NULL, expected ? sqlite3_errmsg(expected) : "reference schema allocation failed", 0);
        sqlite3_close(expected); return -1;
    }
    int result = schema_check_reference(db, expected);
    sqlite3_close(expected);
    if (!result) DEBUG_INFO(DEBUG_CATEGORY_SYS, "Database schema verified: file=%s tables=%u",
        sqlite3_db_filename(db, "main"), count);
    return result;
}

int db_schema_init(sqlite3* db) {
    db_schema_clear_error();
    sqlite3_stmt* st = NULL;
    int rc = db ? sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE name NOT LIKE 'sqlite_%' LIMIT 1", -1, &st, NULL) : SQLITE_MISUSE;
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int final = sqlite3_finalize(st);
    if (rc == SQLITE_ROW && final == SQLITE_OK) return db_schema_check(db);
    if (rc != SQLITE_DONE || final != SQLITE_OK) return schema_error(db, NULL, db ? sqlite3_errmsg(db) : "database unavailable", 0);
    if (sqlite3_exec(db, "SAVEPOINT create_database", NULL, NULL, NULL) != SQLITE_OK)
        return schema_error(db, NULL, sqlite3_errmsg(db), 0);
    if (schema_create(db)) goto fail;
    if (sqlite3_exec(db, "RELEASE create_database", NULL, NULL, NULL) != SQLITE_OK) {
        schema_error(db, NULL, sqlite3_errmsg(db), 0); goto fail;
    }
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Created current database schema");
    return 0;
fail:
    if (sqlite3_exec(db, "ROLLBACK TO create_database; RELEASE create_database", NULL, NULL, NULL) != SQLITE_OK)
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "New database schema rollback failed: %s", sqlite3_errmsg(db));
    return -1;
}
