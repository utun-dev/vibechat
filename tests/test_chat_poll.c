#include "test_utils.h"
/* Кодеки и пользовательский API поверх настоящего ядра, подписей и SQLite. */
#include "../src/chat/chat_feedback.c"
#include "../src/chat/chat_poll.h"
#include "../src/chat/chat_core.h"
#include "../src/config_parser.h"
#include <assert.h>

enum poll_fault { P_NONE, P_PREPARE, P_BIND, P_STEP, P_FINALIZE };
enum poll_query { P_VALID, P_FIELD, P_TARGET, P_SNAPSHOT, P_CHOICES, P_OPTIONS };
static enum poll_fault fault;
static enum poll_query query;
static unsigned query_number, matching_queries, fault_bind, changed_events;
static sqlite3_stmt* fault_statement;
static int fail_ack_allocation;

extern void* __real_u_malloc_impl(uint32_t, const char*);
void* __wrap_u_malloc_impl(uint32_t size, const char* location) {
    if (fail_ack_allocation && strstr(location, "chat_poll.c")) {
        fail_ack_allocation = 0; DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "poll test: committed ack allocation failed bytes=%u", size); return NULL;
    }
    return __real_u_malloc_impl(size, location);
}

static int query_matches(const char* sql) {
    const char* prefix[] = {"SELECT json_valid(?1)", "SELECT json_type(?1,?2),json_extract(?1,?2)",
        "SELECT CASE WHEN json_valid", "SELECT author_ed,kind,ver,data FROM feedback_",
        "SELECT type,value FROM json_each(?1,?2)", "SELECT type,value FROM json_each(?1,'$.options')"};
    return !strncmp(sql, prefix[query], strlen(prefix[query]));
}

static int take_fault(enum poll_fault point, sqlite3_stmt* st, unsigned index) {
    if (fault != point || st != fault_statement || (point == P_BIND && index != fault_bind)) return 0;
    fault = P_NONE;
    DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "poll test: injected query=%d occurrence=%u stage=%d bind=%u", query, query_number, point, index);
    return 1;
}

extern int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int length, sqlite3_stmt** st, const char** tail) {
    int target = fault != P_NONE && query_matches(sql) && ++matching_queries == query_number;
    if (target && fault == P_PREPARE) {
        fault = P_NONE; *st = NULL; DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "poll test: prepare query=%d occurrence=%u", query, query_number);
        return SQLITE_NOMEM;
    }
    int rc = __real_sqlite3_prepare_v2(db, sql, length, st, tail);
    if (target && rc == SQLITE_OK) fault_statement = *st;
    return rc;
}
extern int __real_sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, void (*)(void*));
int __wrap_sqlite3_bind_blob(sqlite3_stmt* st, int index, const void* bytes, int length, void (*destroy)(void*)) {
    if (take_fault(P_BIND, st, index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_blob(st, index, bytes, length, destroy);
}
extern int __real_sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void (*)(void*));
int __wrap_sqlite3_bind_text(sqlite3_stmt* st, int index, const char* bytes, int length, void (*destroy)(void*)) {
    if (take_fault(P_BIND, st, index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_text(st, index, bytes, length, destroy);
}
extern int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) {
    if (take_fault(P_STEP, st, 0)) return SQLITE_IOERR;
    return __real_sqlite3_step(st);
}
extern int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int fail = st && take_fault(P_FINALIZE, st, 0);
    int rc = __real_sqlite3_finalize(st); if (st == fault_statement) fault_statement = NULL;
    return fail ? SQLITE_IOERR : rc;
}

static void poll_event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    (void)data; (void)len;
    assert(sqlite3_get_autocommit(inst->topo_sqlite_db));
    if (type == CHAT_EVT_FEEDBACK_CHANGED || type == CHAT_EVT_MSG_RECEIVED) changed_events++;
}

static sqlite3_int64 json_number(sqlite3* db, const char* json, const char* path) {
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT json_extract(?1,?2)", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC); sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC);
    assert(sqlite3_step(st) == SQLITE_ROW); sqlite3_int64 value = sqlite3_column_int64(st, 0); sqlite3_finalize(st); return value;
}

static void json_string(sqlite3* db, const char* json, const char* path, char* out, size_t size) {
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT json_extract(?1,?2)", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC); sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_TEXT);
    const char* text = (const char*)sqlite3_column_text(st, 0); assert(strlen(text) < size);
    memcpy(out, text, strlen(text) + 1); sqlite3_finalize(st);
}

static char* command(struct UTUN_INSTANCE* inst, const char* text, int expected) {
    char* result = NULL; assert(chat_feedback_command(inst, text, &result) == expected && result);
    assert(json_number(inst->topo_sqlite_db, "{\"valid\":1}", "$.valid") == 1); return result;
}

static int message_count(struct UTUN_INSTANCE* inst, const char* channel) {
    char sql[128]; snprintf(sql, sizeof(sql), "SELECT count(*) FROM msg_%s", channel); sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(inst->topo_sqlite_db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    int count = sqlite3_column_int(st, 0); assert(sqlite3_finalize(st) == SQLITE_OK); return count;
}

/* Ошибка SQL не становится отсутствующим полем/пустой сводкой и не публикует команду. */
static void sql_faults(struct UTUN_INSTANCE* inst, const char* channel, const char* hex, const uint8_t message[32]) {
    char react[512], vote[512], create[512]; uint8_t before[32], after[32]; unsigned cases = 0;
    snprintf(react, sizeof(react), "{\"cmd\":\"react\",\"ch\":\"%s\",\"message_id\":\"%s\",\"emoji\":0}", channel, hex);
    snprintf(vote, sizeof(vote), "{\"cmd\":\"vote\",\"ch\":\"%s\",\"message_id\":\"%s\",\"choices\":[0]}", channel, hex);
    snprintf(create, sizeof(create), "{\"cmd\":\"poll_create\",\"ch\":\"%s\",\"question\":\"SQL retry\","
        "\"mode\":\"single\",\"options\":[\"one\",\"two\"]}", channel);
    chat_event_set_handler(inst, poll_event);
    for (enum poll_query operation = P_VALID; operation <= P_OPTIONS; operation++) {
        unsigned occurrences = operation == P_FIELD ? 6 : 1;
        for (unsigned occurrence = 1; occurrence <= occurrences; occurrence++) {
            for (enum poll_fault point = P_PREPARE; point <= P_FINALIZE; point++) {
                unsigned bindings = point == P_BIND && (operation == P_FIELD || operation == P_CHOICES) ? 2 : 1;
                for (unsigned bind = 1; bind <= bindings; bind++) {
                    struct chat_feedback_record saved, actual;
                    assert(chat_feedback_get(inst->topo_sqlite_db, channel, message, inst->my_ed25519_pubkey, CHAT_FEEDBACK_REACTION, &saved) == 1);
                    assert(!merkle_sync_read_hash(inst, MT_DATA_CHAT_LOG, channel, 0, 0, before));
                    unsigned events = changed_events; int messages = message_count(inst, channel);
                    query = operation; query_number = occurrence; matching_queries = 0; fault_statement = NULL;
                    fault_bind = bind; fault = point; char* result = NULL;
                    const char* request = operation == P_CHOICES ? vote : operation == P_OPTIONS ? create : react;
                    if (operation == P_SNAPSHOT) {
                        assert(chat_feedback_snapshot(inst->topo_sqlite_db, inst->my_ed25519_pubkey, channel, message, 0, &result) < 0 && !result);
                    } else { result = command(inst, request, -1); u_free(result); }
                    assert(fault == P_NONE && !fault_statement && changed_events == events && message_count(inst, channel) == messages);
                    assert(!merkle_sync_read_hash(inst, MT_DATA_CHAT_LOG, channel, 0, 0, after) && !memcmp(before, after, 32));
                    assert(chat_feedback_get(inst->topo_sqlite_db, channel, message, inst->my_ed25519_pubkey, CHAT_FEEDBACK_REACTION, &actual) == 1);
                    assert(actual.ver == saved.ver && actual.data_len == saved.data_len && !memcmp(actual.data, saved.data, saved.data_len));
                    assert(!memcmp(actual.signature, saved.signature, 64));
                    if (operation == P_SNAPSHOT) assert(!chat_feedback_snapshot(inst->topo_sqlite_db, inst->my_ed25519_pubkey, channel, message, 0, &result) && result);
                    else result = command(inst, request, 0);
                    u_free(result); cases++;
                }
            }
        }
    }
    struct chat_feedback_record saved, actual;
    assert(chat_feedback_get(inst->topo_sqlite_db, channel, message, inst->my_ed25519_pubkey, CHAT_FEEDBACK_REACTION, &saved) == 1);
    snprintf(react, sizeof(react), "{\"cmd\":\"react\",\"ch\":\"%s\",\"message_id\":\"%s\",\"emoji\":\"👍\\u0000tail\"}", channel, hex);
    char* result = command(inst, react, -1); u_free(result);
    assert(chat_feedback_get(inst->topo_sqlite_db, channel, message, inst->my_ed25519_pubkey, CHAT_FEEDBACK_REACTION, &actual) == 1 && actual.ver == saved.ver);
    assert(actual.data_len == saved.data_len && !memcmp(actual.data, saved.data, saved.data_len));
    /* Snapshot после commit может отказать: ответ остаётся успехом, повторяем только чтение. */
    snprintf(react, sizeof(react), "{\"cmd\":\"react\",\"ch\":\"%s\",\"message_id\":\"%s\",\"emoji\":0}", channel, hex);
    for (unsigned publish = 0; publish < 2; publish++) {
        const char* request = publish ? create : react;
        for (enum poll_fault point = P_PREPARE; point <= P_FINALIZE; point++) {
            int messages = message_count(inst, channel);
            query = P_SNAPSHOT; query_number = 1; matching_queries = 0; fault_statement = NULL; fault_bind = 1; fault = point;
            result = command(inst, request, 0); assert(fault == P_NONE && !fault_statement);
            assert(json_number(inst->topo_sqlite_db, result, "$.committed") == 1 &&
                json_number(inst->topo_sqlite_db, result, "$.snapshot_pending") == 1);
            char acknowledged[65], read[256]; json_string(inst->topo_sqlite_db, result, "$.message_id", acknowledged, sizeof(acknowledged));
            u_free(result);
            assert(message_count(inst, channel) == messages + (int)publish);
            snprintf(read, sizeof(read), "{\"cmd\":\"feedback\",\"ch\":\"%s\",\"message_id\":\"%s\"}", channel, acknowledged);
            assert(!merkle_sync_read_hash(inst, MT_DATA_CHAT_LOG, channel, 0, 0, before)); unsigned events = changed_events;
            result = command(inst, read, 0); u_free(result);
            assert(message_count(inst, channel) == messages + (int)publish && changed_events == events);
            assert(!merkle_sync_read_hash(inst, MT_DATA_CHAT_LOG, channel, 0, 0, after) && !memcmp(before, after, 32));
        }
        int messages = message_count(inst, channel); unsigned events = changed_events;
        assert(!merkle_sync_read_hash(inst, MT_DATA_CHAT_LOG, channel, 0, 0, before));
        fail_ack_allocation = 1; result = command(inst, request, -1); u_free(result);
        assert(!fail_ack_allocation && changed_events == events && message_count(inst, channel) == messages);
        assert(!merkle_sync_read_hash(inst, MT_DATA_CHAT_LOG, channel, 0, 0, after) && !memcmp(before, after, 32));
    }
    chat_event_set_handler(inst, NULL);
    printf("PASS: %u poll SQL faults,8 post-commit read failures,2 pre-commit allocation failures; state/events preserved, read-only retry, complete emoji text\n", cases);
}

/* Проверяем границы, каноничность и невозможность интерпретации битого UTF-8. */
static void codecs(void) {
    struct chat_poll poll = { .mode = CHAT_POLL_SINGLE, .allow_custom_answer = 1, .option_count = 2 }, decoded;
    strcpy(poll.question, "Вопрос \"с кавычками\"\nи переносом"); strcpy(poll.options[0], "Да"); strcpy(poll.options[1], "Нет");
    uint8_t wire[CHAT_POLL_WIRE_MAX]; int n = chat_poll_encode(&poll, wire, sizeof(wire)); assert(n > 0);
    assert(chat_poll_decode(wire, n, &decoded) == 0 && !memcmp(&poll, &decoded, sizeof(poll)));
    for (int i = 0; i < n; i++) assert(chat_poll_decode(wire, i, &decoded) < 0);
    uint8_t saved = wire[2]; wire[2] = 2; assert(chat_poll_decode(wire, n, &decoded) < 0); wire[2] = saved;
    wire[n] = 0; assert(chat_poll_decode(wire, n + 1, &decoded) < 0);
    strcpy(poll.options[1], poll.options[0]); assert(chat_poll_encode(&poll, wire, sizeof(wire)) < 0);
    poll.options[1][0] = (char)0xc0; poll.options[1][1] = (char)0x80; poll.options[1][2] = 0;
    assert(chat_poll_encode(&poll, wire, sizeof(wire)) < 0);
    struct chat_poll_answer answer = { .count = 2, .choices = {3, 1} }, parsed;
    strcpy(answer.custom, "Свой ответ");
    n = chat_answer_encode(&answer, wire, sizeof(wire)); assert(n > 0);
    assert(chat_answer_decode(wire, n, &parsed) == 0 && parsed.choices[0] == 1 && parsed.choices[1] == 3);
    wire[1] = 3; wire[2] = 1; assert(chat_answer_decode(wire, n, &parsed) < 0);
    answer.choices[1] = 3; assert(chat_answer_encode(&answer, wire, sizeof(wire)) < 0);
    memset(&answer, 0, sizeof(answer)); assert(chat_answer_encode(&answer, wire, sizeof(wire)) == 0);
    assert(chat_answer_decode(NULL, 0, &parsed) == 0 && !parsed.count);
    uint8_t empty[] = {0, 0, 0}; assert(chat_answer_decode(empty, sizeof(empty), &parsed) < 0);
    uint8_t ids[] = {5, 0}, read[CHAT_REACTION_SELECTED_MAX]; size_t count;
    assert(chat_reactions_encode(ids, 2, wire, sizeof(wire)) < 0);
    uint8_t multi[] = {2, 0, 5}; assert(chat_reactions_decode(multi, sizeof(multi), read, &count) < 0);
    n = chat_reactions_encode(ids, 1, wire, sizeof(wire)); assert(n == 2);
    assert(chat_reactions_decode(wire, n, read, &count) == 0 && count == 1 && read[0] == 5);
    wire[1] = CHAT_REACTION_COUNT; assert(chat_reactions_decode(wire, n, read, &count) < 0);
    assert(chat_reactions_encode(NULL, 0, wire, sizeof(wire)) == 0);
    puts("PASS: canonical codecs, truncation, duplicate choices and UTF-8 rejection");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); codecs();
    struct UASYNC* ua = uasync_create(); assert(ua);
    char directory[512]="/tmp/utun-poll-XXXXXX",config[2048]; assert(!test_mkdtemp(directory));
    snprintf(config,sizeof(config),"[global]\ndb_path=%s\n%s",directory,
        "[global]\nmy_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
        "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
        "[routing]\nmy_subnet=192.168.42.0/24\n[server: udp]\naddr=127.0.0.1:0\ntype=public\n"
        "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n");
    struct UTUN_INSTANCE* inst=utun_instance_create_from_str(ua,config);
    assert(inst && utun_core_start(inst) == 0 && chat_service_start(inst) == 0);
    chat_core_create_channel_auto(inst, "Poll tests");
    sqlite3* db = inst->topo_sqlite_db; sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT channel_id FROM channels", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW); char channel[65]; snprintf(channel, sizeof(channel), "%s", sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    char request[4096], id[65]; uint8_t message_id[32];
    snprintf(request, sizeof(request),
        "{\"cmd\":\"poll_create\",\"ch\":\"%s\",\"question\":\"Куда? \\\"Да\\\"\\nНовая строка\","
        "\"mode\":\"single\",\"options\":[\"Домой\",\"В парк\"],\"allow_custom_answer\":false}", channel);
    char* result = command(inst, request, 0); json_string(db, result, "$.message_id", id, sizeof(id));
    assert(sc_hex_to_binary(id, message_id, 32) == 0);
    assert(json_number(db, result, "$.poll.voters") == 0);
    char question[1024]; json_string(db, result, "$.poll.question", question, sizeof(question));
    assert(!strcmp(question, "Куда? \"Да\"\nНовая строка")); u_free(result);
    snprintf(request, sizeof(request), "{\"cmd\":\"vote\",\"ch\":\"%s\",\"message_id\":\"%s\",\"choices\":[0]}", channel, id);
    result = command(inst, request, 0); assert(json_number(db, result, "$.poll.voters") == 1);
    sqlite3_int64 first_vote = json_number(db, result, "$.poll.ver"); assert(first_vote > 0);
    assert(json_number(db, result, "$.poll.options[0].count") == 1); u_free(result);
    snprintf(request, sizeof(request),
        "{\"cmd\":\"vote\",\"ch\":\"%s\",\"message_id\":\"%s\",\"choices\":[0,1]}", channel, id);
    result = command(inst, request, -1); u_free(result);
    snprintf(request, sizeof(request),
        "{\"cmd\":\"vote\",\"ch\":\"%s\",\"message_id\":\"%s\",\"custom\":\"Свой вариант\"}", channel, id);
    result = command(inst, request, -1); u_free(result);
    snprintf(request, sizeof(request), "{\"cmd\":\"vote\",\"ch\":\"%s\",\"message_id\":\"%s\",\"choices\":[1]}", channel, id);
    result = command(inst, request, 0); assert(json_number(db, result, "$.poll.options[0].count") == 0);
    assert(json_number(db, result, "$.poll.options[1].count") == 1 && json_number(db, result, "$.poll.ver") == first_vote + 1); u_free(result);
    snprintf(request, sizeof(request), "{\"cmd\":\"vote\",\"ch\":\"%s\",\"message_id\":\"%s\"}", channel, id);
    result = command(inst, request, 0); assert(json_number(db, result, "$.poll.voters") == 0);
    assert(json_number(db, result, "$.poll.ver") == first_vote + 2); u_free(result);
    puts("PASS: single-choice rules, disabled custom answer, replacement and withdrawal");
    snprintf(request, sizeof(request), "{\"cmd\":\"react\",\"ch\":\"%s\",\"message_id\":\"%s\",\"emoji\":0}", channel, id);
    result = command(inst, request, 0); assert(json_number(db, result, "$.reactions[0].count") == 1); u_free(result);
    result = command(inst, request, 0); assert(json_number(db, result, "$.reaction_ver") == first_vote + 4); u_free(result);
    snprintf(request, sizeof(request), "{\"cmd\":\"react\",\"ch\":\"%s\",\"message_id\":\"%s\",\"emoji\":0}", channel, id);
    result = command(inst, request, 0); assert(json_number(db, result, "$.reactions[0].id") == 0); u_free(result);
    snprintf(request, sizeof(request), "{\"cmd\":\"react\",\"ch\":\"%s\",\"message_id\":\"%s\",\"emoji\":4}", channel, id);
    result = command(inst, request, 0);
    assert(json_number(db, result, "$.reactions[0].id") == 4 && json_number(db, result, "$.reaction_ver") == first_vote + 6);
    assert(json_number(db, result, "$.reactions[1].count") == 0); u_free(result);
    snprintf(request, sizeof(request), "{\"cmd\":\"react\",\"ch\":\"%s\",\"message_id\":\"%s\",\"emoji\":0,\"on\":false}", channel, id);
    result = command(inst, request, 0);
    assert(json_number(db, result, "$.reactions[0].id") == 4 && json_number(db, result, "$.reaction_ver") == first_vote + 6); u_free(result);
    snprintf(request, sizeof(request), "{\"cmd\":\"react\",\"ch\":\"%s\",\"message_id\":\"%s\",\"emoji\":4}", channel, id);
    result = command(inst, request, 0); assert(json_number(db, result, "$.reactions[0].count") == 0);
    assert(json_number(db, result, "$.reaction_ver") == first_vote + 7); u_free(result);
    puts("PASS: selecting another emoji replaces the previous one; unselected withdrawal is inert; repeat click cancels");
    sql_faults(inst, channel, id, message_id);
    snprintf(request, sizeof(request),
        "{\"cmd\":\"poll_create\",\"ch\":\"%s\",\"question\":\"Выбор\",\"mode\":\"multiple\","
        "\"options\":[\"Первый\",\"Второй\"],\"allow_custom_answer\":true}", channel);
    result = command(inst, request, 0); json_string(db, result, "$.message_id", id, sizeof(id)); u_free(result);
    assert(sc_hex_to_binary(id, message_id, 32) == 0);
    snprintf(request, sizeof(request),
        "{\"cmd\":\"vote\",\"ch\":\"%s\",\"message_id\":\"%s\",\"choices\":[1,0],\"custom\":\"Мой \\\"ответ\\\"\"}", channel, id);
    result = command(inst, request, 0);
    assert(json_number(db, result, "$.poll.voters") == 1 && json_number(db, result, "$.poll.custom_count") == 1);
    assert(json_number(db, result, "$.poll.options[0].percent") == 100 && json_number(db, result, "$.poll.options[1].percent") == 100);
    u_free(result);
    /* Исходное сообщение временно отсутствует: ответ остаётся в Merkle, но не считается. */
    char sql[512]; snprintf(sql, sizeof(sql), "CREATE TEMP TABLE poll_backup AS SELECT * FROM msg_%s", channel);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    snprintf(sql, sizeof(sql), "DELETE FROM msg_%s WHERE message_id=?", channel);
    assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_blob(st, 1, message_id, 32, SQLITE_STATIC); assert(sqlite3_step(st) == SQLITE_DONE); sqlite3_finalize(st);
    struct chat_feedback_record answer;
    assert(chat_feedback_get(inst->topo_sqlite_db, channel, message_id, inst->my_ed25519_pubkey, CHAT_FEEDBACK_ANSWER, &answer) == 1);
    struct member_sync_write_context witness={.self=inst->node_id,.now=1000000};
    memcpy(witness.public_key,inst->my_ed25519_pubkey,32); memcpy(witness.private_key,inst->my_ed25519_privkey,32);
    assert(!sqlite3_exec(db,"BEGIN IMMEDIATE /* poll journal fixture */",NULL,NULL,NULL));
    assert(!chat_log_feedback(db,&witness,channel,&answer,NULL));
    assert(!sqlite3_exec(db,"COMMIT /* poll journal fixture */",NULL,NULL,NULL));
    assert(chat_log_feedback_check(db,channel,&answer)==1);
    /* Replay знает исходный блок; удаляем только cache для проверки UI pending, затем восстанавливаем его. */
    snprintf(sql,sizeof(sql),"DELETE FROM msg_%s WHERE message_id=?",channel);
    assert(!sqlite3_prepare_v2(db,sql,-1,&st,NULL)); sqlite3_bind_blob(st,1,message_id,32,SQLITE_STATIC);
    assert(sqlite3_step(st)==SQLITE_DONE && !sqlite3_finalize(st));
    assert(chat_feedback_snapshot(inst->topo_sqlite_db, inst->my_ed25519_pubkey, channel, message_id, 0, &result) == 0 && json_number(db, result, "$.pending") == 1);
    u_free(result);
    snprintf(sql, sizeof(sql), "INSERT OR IGNORE INTO msg_%s SELECT * FROM poll_backup", channel);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK); chat_feedback_check_message(inst, channel, message_id);
    assert(chat_feedback_snapshot(inst->topo_sqlite_db, inst->my_ed25519_pubkey, channel, message_id, 0, &result) == 0 && json_number(db, result, "$.pending") == 0);
    assert(json_number(db, result, "$.poll.voters") == 1 && json_number(db, result, "$.poll.ver") == (sqlite3_int64)answer.ver); u_free(result);
    /* Неверный подписанный ответ синхронизируется, но исключается из результатов. */
    struct chat_poll_answer wrong = { .count = 1, .choices = {7} };
    answer.data_len = chat_answer_encode(&wrong, answer.data, sizeof(answer.data)); answer.ver++;
    assert(!sqlite3_exec(db,"BEGIN IMMEDIATE /* poll journal fixture */",NULL,NULL,NULL));
    assert(!chat_log_feedback(db,&witness,channel,&answer,NULL));
    assert(!sqlite3_exec(db,"COMMIT /* poll journal fixture */",NULL,NULL,NULL));
    assert(chat_log_feedback_check(db,channel,&answer)==1);
    assert(chat_feedback_snapshot(inst->topo_sqlite_db, inst->my_ed25519_pubkey, channel, message_id, 0, &result) == 0 && json_number(db, result, "$.poll.voters") == 0);
    u_free(result);
    char listing[16384]; size_t len;
    assert(chat_core_get_messages_json(inst, channel, 20, 0, listing, sizeof(listing), &len) == 0 && strstr(listing, id));
    char tiny[8]; assert(chat_core_get_messages_json(inst, channel, 20, 0, tiny, sizeof(tiny), &len) < 0 && !len);
    result = command(inst, "{\"cmd\":\"reaction_catalog\"}", 0);
    assert(json_number(db, result, "$.catalog[0].id") == 0); u_free(result);
    puts("PASS: multi-choice custom answers, delayed source, invalid signed answer and complete message listing");
    utun_instance_destroy(inst); uasync_poll(ua, 0);
    assert(ua->timer_alloc_count == ua->timer_free_count && ua->socket_alloc_count == ua->socket_free_count);
    uasync_destroy(ua, 0);
    puts("ALL PASS: poll API and codecs");
    return 0;
}
