/*
 * chat_admin.h — передача канального приватного ключа (прав админа) другому узлу
 *
 * Владелец канала (имеющий ed25519_privkey + x25519_privkey) может передать оба ключа
 * выбранному узлу. Передача идёт через сервисный пакет etcp_router с
 * ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT: X25519 ECDH + AES-128-CCM и Ed25519-подпись.
 * Получатель сверяет выведенные публичные ключи с уже известными ключами канала,
 * сохраняет оба privkey и собственную подписанную admin-роль одной worker WRITE.
 * После durable commit uasync публикует права/MEMBERS и CHAT_EVT_ADMIN_KEY_RECEIVED.
 * Ошибка SQL/crypto сохраняет прежнюю пару без события успеха; корректный повтор допустим.
 * Исходящая передача читает ключи/допуск одной worker READ, затем повторно проверяет runtime-допуск.
 * Запросы копируют пакет и секреты; stop/rejoin отменяет queued работу и позднюю публикацию.
 * Модуль включается/отключается chat_sync; публичные вызовы/callbacks — в uasync, SQL только в worker.
 */

#ifndef CHAT_ADMIN_H
#define CHAT_ADMIN_H

#define CHAT_ADMIN_KEY_PAYLOAD_SIZE 64 /* Ed25519 seed и X25519 private key по 32 байта. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct UTUN_INSTANCE;

/* Зарегистрировать ETCP_RT_ID_ADMIN_KEY; 0/-1. inst обязателен, повтор заменяет callback. */
int  chat_admin_init(struct UTUN_INSTANCE* inst);
/* Снять обработчик; NULL допустим, отсутствие регистрации логирует router. */
void chat_admin_destroy(struct UTUN_INSTANCE* inst);

#ifdef __cplusplus
}
#endif
#endif /* CHAT_ADMIN_H */
