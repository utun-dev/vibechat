/* Общий владелец групповой доставки и файловых задач. Публикации принадлежат моделям сообщений. */
#include "service_priv.h"
#include "download.h"
#include "recovery.h"
#include "../chat/chat_attachment.h"
#include "../chat/chat_core.h"
#include "../chat/chat_core_priv.h"
#include "../media_async/attachment_send.h"
#include "../media_async/media_async.h"
#include "../media_delivery/media_delivery.h"
#include "../../lib/db_async.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"

int attachment_service_prepare_write(sqlite3* db, uint8_t epoch[16]) {
    int rc = attachment_store_prepare_write(db);
    if (!rc) rc = media_delivery_prepare_write(db, epoch);
    if (!rc) rc = chat_attachment_prepare_write(db);
    if (!rc) {
        rc = sqlite3_exec(db, "UPDATE attachment_jobs SET state=4 WHERE state=1", NULL, NULL, NULL);
        if (!rc && sqlite3_changes(db)) DEBUG_WARN(DEBUG_CATEGORY_MEDIA,
            "attachment startup: %d interrupted PREPARING jobs marked failed; source files retained", sqlite3_changes(db));
    }
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment service preparation rc=%d: %s", rc, sqlite3_errstr(rc));
    return rc;
}

int attachment_service_start(struct UTUN_INSTANCE* inst, const uint8_t epoch[16]) {
    if (!inst || inst->destroying || !chat_core_is_initialized(inst)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment service start requires active group core"); return -1;
    }
    if (inst->attachments) return inst->attachments->closing ? -1 : 0;
    if (db_async_can_write(inst->db_worker) && !epoch) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment service start requires committed transport epoch"); return -1;
    }
    struct attachment_service* service = u_calloc(1, sizeof(*service));
    if (!service) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment service allocation failed"); return -1; }
    uint8_t prepared[16];
    if (!epoch) {
        sqlite3* db = CC(inst)->db;
        int rc = !db || !sqlite3_get_autocommit(db) ? SQLITE_MISUSE : sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
        int owns = !rc;
        if (!rc) rc = attachment_service_prepare_write(db, prepared);
        if (!rc) rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
        if (rc) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment legacy startup preparation rc=%d: %s", rc, sqlite3_errstr(rc));
            if (owns && !sqlite3_get_autocommit(db)) {
                int rollback = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
                if (rollback) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment startup rollback rc=%d", rollback);
                if (!sqlite3_get_autocommit(db)) utun_instance_stop(inst);
            }
            u_free(service); return -1;
        }
        epoch = prepared; chat_attachment_recover_legacy(inst);
    }
    inst->attachments = service; service->workers = media_async_create();
    if (!service->workers || media_delivery_activate(inst, epoch) || chat_attachment_activate(inst)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment service activation failed; draining partial runtime");
        attachment_service_stop(inst); return -1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "attachment service started: independent workers, group delivery and key requests"); return 0;
}

void attachment_service_stop(struct UTUN_INSTANCE* inst) {
    struct attachment_service* service = inst ? inst->attachments : NULL;
    if (!service || service->closing) return;
    service->closing = 1; service->owners = 0;
    attachment_group_download_detach(inst);
    attachment_recovery_stop(inst);
    attachment_send_quiesce(inst); media_delivery_quiesce(inst); chat_attachment_quiesce(inst);
    media_async_destroy(service->workers); service->workers = NULL;
    attachment_send_destroy(inst); chat_attachment_destroy(inst); media_delivery_destroy(inst);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "attachment service stopped after worker drain active_files=%u", service->attachment_file_count);
    inst->attachments = NULL; u_free(service);
}

struct media_async* attachment_service_workers(struct UTUN_INSTANCE* inst) {
    return inst && inst->attachments && !inst->attachments->closing ? inst->attachments->workers : NULL;
}

void attachment_service_set_owner(struct UTUN_INSTANCE* inst, enum attachment_owner owner, int enabled) {
    struct attachment_service* service = inst ? inst->attachments : NULL;
    if (!service || service->closing) return;
    if (owner < ATTACHMENT_OWNER_CHAT || owner > ATTACHMENT_OWNER_DM) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment publisher invalid owner=%d", owner); return;
    }
    unsigned bit = 1u << owner;
    if (!!(service->owners & bit) == !!enabled) return;
    if (service->owner_epoch[owner] == UINT64_MAX) {
        service->owners &= ~bit; DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment publisher epoch exhausted owner=%d", owner); return;
    }
    service->owner_epoch[owner]++;
    if (enabled) service->owners |= bit; else service->owners &= ~bit;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "attachment publisher owner=%d enabled=%d epoch=%llu", owner, !!enabled,
        (unsigned long long)service->owner_epoch[owner]);
    attachment_recovery_schedule(inst);
}

uint64_t attachment_service_owner_epoch(struct UTUN_INSTANCE* inst, enum attachment_owner owner) {
    struct attachment_service* service = inst ? inst->attachments : NULL;
    return service && !service->closing && owner >= ATTACHMENT_OWNER_CHAT && owner <= ATTACHMENT_OWNER_DM &&
        (service->owners & (1u << owner)) ? service->owner_epoch[owner] : 0;
}
