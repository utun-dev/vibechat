#include "kanban_attachment.h"
#include "kanban_priv.h"
#include "../attachments/group_file.h"
#include "../attachments/service.h"
#include "../attachments/manifest.h"
#include "../media_delivery/media_delivery.h"
#include "../routing_layer/topo_group.h"
#include "../../lib/db_async.h"

struct card_file {
    struct kanban_write write; /* Frozen SQL-вход с собственными строками/ключами. */
    char group[65], card[65], path[1024], type[32], description[384];
    struct attachment_info info;
    char* json;
    struct media_destination* destination;
    struct media_index_result indexed;
    struct UTUN_INSTANCE* inst; /* Только guard/done. */
    uint64_t generation, core_generation, registration, publisher_epoch, request;
    void (*done)(void*, int); void* arg;
};
static int current(void* data) {
    struct card_file* file = data; struct UTUN_INSTANCE* inst = file->inst;
    struct TOPO_GROUP* group = inst->topo_groups ? topo_groups_find_channel(inst->topo_groups, file->group) : NULL;
    return !inst->destroying && inst->kanban_started && inst->kanban_generation == file->generation &&
        chat_core_generation(inst) == file->core_generation && group && !group->stopping && group->generation == file->registration &&
        attachment_service_owner_epoch(inst, ATTACHMENT_OWNER_KANBAN) == file->publisher_epoch && file->publisher_epoch;
}
static void finish(struct card_file* file, int success) {
    if (current(file)) chat_event_submitted(file->inst, file->request, success);
    media_delivery_release(file->inst, file->destination); media_index_result_free(&file->indexed);
    void (*done)(void*, int) = file->done; void* arg = file->arg;
    OPENSSL_cleanse(&file->write.writer, sizeof(file->write.writer)); u_free(file->write.result); u_free(file->json); u_free(file);
    if (done) done(arg, success);
}
static void committed(void* data, const struct db_async_result* result) {
    struct card_file* file = data; int success = !result->status && current(file);
    DEBUG_INFO(DEBUG_CATEGORY_KANBAN, "card file publication ch=%s card=%s committed=%d current=%d status=%d rc=%d",
        file->group, file->card, !result->status, current(file), result->status, result->sqlite_code);
    if (success) {
        media_delivery_announce_media(file->inst, topo_groups_channel_slot(file->inst->topo_groups, file->group), file->indexed.media_id, file->indexed.block_ids, file->indexed.num_blocks);
        if (current(file)) kb_publish(file->inst, file->group);
    }
    finish(file, success);
}
static int save(sqlite3* db, void* data) { return kanban_command_write(db, &((struct card_file*)data)->write); }
static void publish_body(struct card_file* file, const char* body) {
    struct strbuf json = {0};
    int failed = strbuf_addf(&json, "{\"action\":\"message_create\",\"entity\":\"%s\",\"text\":\"\",\"ct\":\"%s\",\"d\":", file->card, file->type) < 0 ||
        strbuf_add_json_string(&json, body) || strbuf_addf(&json, "}") < 0 || attachment_manifest_parse(body, &file->indexed);
    file->json = failed ? NULL : strbuf_detach(&json); strbuf_free(&json);
    if (!file->json) { finish(file, 0); return; }
    file->write.group = file->group; file->write.json = file->json;
    file->write.writer.now = ntp_time_get_us(file->inst);
    memcpy(file->write.writer.public_key, file->inst->my_ed25519_pubkey, 32);
    memcpy(file->write.writer.private_key, file->inst->my_ed25519_privkey, 32); memcpy(file->write.attachment_object, file->indexed.media_id, 16);
    if (db_async_can_write(file->inst->db_worker)) {
        db_async_run(file->inst->db_worker, &(struct db_async_task){.name="kanban-attachment", .mode=DB_ASYNC_WRITE,
            .guard=current, .work=save, .data=file, .done=committed}); return;
    }
    sqlite3* db = file->inst->topo_sqlite_db;
    int rc = !db || !sqlite3_get_autocommit(db) ? SQLITE_MISUSE : !current(file) ? SQLITE_ABORT :
        sqlite3_exec(db, "BEGIN IMMEDIATE /* kanban-attachment */", NULL, NULL, NULL);
    int owns = !rc; if (!rc) rc = save(db, file);
    if (!rc) rc = sqlite3_exec(db, "COMMIT /* kanban-attachment */", NULL, NULL, NULL);
    if (rc && owns && !sqlite3_get_autocommit(db)) {
        int rollback = sqlite3_exec(db, "ROLLBACK /* kanban-attachment */", NULL, NULL, NULL);
        if (rollback) DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "card attachment rollback rc=%d: %s", rollback, sqlite3_errmsg(db));
        if (!sqlite3_get_autocommit(db)) utun_instance_stop(file->inst);
    }
    committed(file, &(struct db_async_result){.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK, .sqlite_code=rc});
}
static void registered(void* data, int error, const struct media_index_result* result) {
    struct card_file* file = data;
    if (error || !result || !current(file)) { finish(file, 0); return; }
    int visual = !strncmp(file->type, "video/", 6) || !strncmp(file->type, "image/", 6);
    char* body = attachment_manifest_format(file->description, strlen(file->description), result,
        !strncmp(file->type, "video/", 6) ? file->info.duration_ms : 0, visual ? file->info.width : 0, visual ? file->info.height : 0);
    if (!body) { finish(file, 0); return; }
    publish_body(file, body); u_free(body);
}

static struct card_file* new_file(struct UTUN_INSTANCE* inst, const char* group, const char* card, const char* path,
    const char* type, uint64_t request, void (*done)(void*, int), void* arg) {
    uint8_t id[32];
    if (!inst || !inst->kanban_started || !chat_channel_valid(group) || kb_unhex(card, id) || !path || !*path || strlen(path) >= 1024 ||
        !type || strlen(type) >= 32) {
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "card file submission invalid/stopped");
        if (inst && inst->kanban_started) chat_event_submitted(inst, request, 0);
        if (done) done(arg, 0); return NULL;
    }
    struct card_file* file = u_calloc(1, sizeof(*file));
    if (!file) {
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "card file allocation failed"); chat_event_submitted(inst, request, 0);
        if (done) done(arg, 0); return NULL;
    }
    file->inst = inst; file->request = request; file->done = done; file->arg = arg;
    strcpy(file->group, group); strcpy(file->card, card); strcpy(file->path, path); strcpy(file->type, type);
    file->generation = inst->kanban_generation; file->core_generation = chat_core_generation(inst);
    struct TOPO_GROUP* topology = topo_groups_find_channel(inst->topo_groups, group);
    file->registration = topology ? topology->generation : 0; file->publisher_epoch = attachment_service_owner_epoch(inst, ATTACHMENT_OWNER_KANBAN);
    file->destination = media_delivery_reserve(inst, path);
    if (!file->destination || !current(file)) { finish(file, 0); return NULL; }
    return file;
}

void kanban_attachment_send(struct UTUN_INSTANCE* inst, const char* group, const char* card, const char* path,
    const struct attachment_info* info, const char* type, uint64_t request, void (*done)(void*, int), void* arg) {
    if (!info || attachment_validate(info)) {
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "card attachment metadata invalid");
        if (inst && inst->kanban_started) chat_event_submitted(inst, request, 0); if (done) done(arg, 0); return;
    }
    struct card_file* file = new_file(inst, group, card, path, type, request, done, arg); if (!file) return;
    file->info = *info;
    if (info->kind == ATTACHMENT_VOICE) snprintf(file->description, sizeof(file->description), "wf=%s;dur=%u;", info->waveform, info->duration_ms);
    else snprintf(file->description, sizeof(file->description), "%s", info->name);
    int visual = !strncmp(type, "video/", 6) || !strncmp(type, "image/", 6);
    struct attachment_group_file intent = {.owner=ATTACHMENT_OWNER_KANBAN, .group=file->group, .card=file->card, .source=file->path,
        .destination=file->path, .type=file->type, .description=file->description,
        .duration_ms=!strncmp(type, "video/", 6) ? info->duration_ms : 0, .width=visual ? info->width : 0, .height=visual ? info->height : 0};
    DEBUG_INFO(DEBUG_CATEGORY_KANBAN, "card file registration ch=%s card=%s type=%s duration_ms=%u dimensions=%ux%u",
        group, card, type, intent.duration_ms, intent.width, intent.height);
    attachment_group_register(inst, &intent, file->write.attachment_job, current, registered, file);
}

void kanban_attachment_publish_ready(struct UTUN_INSTANCE* inst, const char* group, const char* card, const char* path,
    const char* type, const char* body, const uint8_t job[16], void (*done)(void*, int), void* arg) {
    if (!body || !job) { DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "card READY attachment missing intent/manifest"); if (done) done(arg, 0); return; }
    struct card_file* file = new_file(inst, group, card, path, type, 0, done, arg); if (!file) return;
    memcpy(file->write.attachment_job, job, 16); publish_body(file, body);
}
