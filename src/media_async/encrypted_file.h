/* Контейнер группового вложения: публичный заголовок, зашифрованные метаданные и
 * порции AES-256-CCM. Полный SHA256 контейнера берётся из подписанного сообщения.
 * Write/read вызывать только в media_async worker: они готовят отдельный durable temp,
 * конечное имя публикует владелец через file_io_temp_publish. Source не изменяется. */
#ifndef ENCRYPTED_FILE_H
#define ENCRYPTED_FILE_H
#include "attachment.h"
#include "../../lib/file_io.h"
#ifdef __cplusplus
extern "C" {
#endif
#define ENCRYPTED_ATTACHMENT_TYPE "application/x-utun-encrypted"
/* Ограничение MIME существующего контейнера: NUL-строка длиной 1..31 байт,
 * ASCII 33..126 без двойной кавычки и обратной косой черты. NULL/нет NUL в первых 32 байтах → -1.
 * 0 — подходит, -1 — ошибка с диагностикой без вывода сырой строки. Без I/O, допустим вызов в uasync.
 * Это проверка представления поля контейнера, а не полный разбор MIME-грамматики. */
int encrypted_file_validate_type(const char* type);
/* source/target — UTF-8 пути разных файлов; target отсутствует либо regular без конечной ссылки.
 * output заранее обнулён и имеет стабильный адрес до publish/discard; активный output отклоняется без очистки.
 * 0 — output содержит закрытый проверенный temp рядом с target, target ещё не изменён.
 * -1 — ошибка; source/target сохранены, собственный temp передан discard (ошибки удаления логируются).
 * Ключ — 32 байта, UUID — 16, author — полный Ed25519. type должен пройти encrypted_file_validate_type;
 * неверный type отвергается до файловых операций и не меняет output/source/target.
 * Пути и файлы не подменяются во время вызова; публикация повторно проверяет их идентичность. */
int encrypted_file_write(const char* source, const char* target, struct file_io_temp* output, const uint8_t key[32],
                         const uint8_t author[32], const uint8_t id[16], const struct attachment_info* info, const char* type);
/* Проверить подписанный hash всего контейнера, UUID, метаданные и все tags. Контракт output — как у write.
 * info/type[32] присваиваются только при успехе; при ошибке прежние значения сохраняются. */
int encrypted_file_read(const char* source, const char* target, struct file_io_temp* output, const uint8_t key[32], const uint8_t author[32],
                        const uint8_t id[16], const uint8_t hash[32], struct attachment_info* info, char type[32]);
#ifdef __cplusplus
}
#endif
#endif
