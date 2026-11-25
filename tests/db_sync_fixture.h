/* Подключение настоящей MESSAGES-модели к общему Merkle для тестов с заранее заданными участниками.
 * При отсутствии inst->msync создаёт его с пустой MEMBERS-моделью: она не выдаёт/не принимает удостоверения.
 * Существующий Merkle и его MEMBERS-модель не заменяются. READY, допуск пира и подписи истории остаются настоящими.
 * Вызовы в uasync-потоке; фикстура не запускает core/chat и не владеет instance.
 * Если движок создан вне chat_sync, caller вызывает merkle_sync_destroy до destroy ядра, в том числе после ошибки setup. */
#ifndef TEST_DB_SYNC_FIXTURE_H
#define TEST_DB_SYNC_FIXTURE_H
#include "../src/utun_instance.h"
#include "../src/transport_layer/etcp_api.h"
#include "../src/chat/db_sync.h"
#include "../src/chat/merkle_sync.h"
#include <string.h>

// Пустая MEMBERS-модель не дописывает данные в digest бакета.
static int fixture_member_hash(void* ctx, const char* ns, uint8_t level, uint64_t prefix, EVP_MD_CTX* hash) {
    (void)ctx; (void)ns; (void)level; (void)prefix; (void)hash; return 0;
}
// Вернуть пустую страницу: len=0, more=0, next обнулён; output-параметры обязательны.
static int fixture_member_page(void* ctx, const char* ns, uint64_t prefix, int av, const merkle_key_t* after,
        uint8_t* data, size_t* len, merkle_key_t* next, int* more) {
    (void)ctx; (void)ns; (void)prefix; (void)av; (void)after; (void)data;
    *len = 0; *more = 0; memset(next, 0, sizeof(*next)); return 0;
}
// Принять только пустой MEMBERS payload (0), непустой отвергнуть (-1), состояние не меняется.
static int fixture_member_apply(void* ctx, const char* ns, const uint8_t peer[32], const uint8_t* data, size_t len) {
    (void)ctx; (void)ns; (void)peer; (void)data; return len ? -1 : 0;
}
/* После init ядра/db_sync и регистрации таблиц; inst обязателен, db_sync должен быть enabled.
 * 0 — MESSAGES зарегистрирована/уже была, -1 — отказ. Созданный Merkle при отказе MESSAGES не откатывается. */
static int setup_history_model(struct UTUN_INSTANCE* inst) {
    static const struct merkle_sync_data_ops ops = {
        .update_bucket_hash = fixture_member_hash, .get_page = fixture_member_page, .apply_items = fixture_member_apply
    };
    if (!inst->msync && merkle_sync_init(inst, ETCP_ID_MEMBER_SYNC, &ops, inst) < 0) return -1;
    return db_sync_register_model(inst);
}
#endif
