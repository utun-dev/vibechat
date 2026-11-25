/* Android-обёртка отправки обычного cache-файла в канал через общий attachment_send worker.
 * Назначение выбирает ядро, копирование/индексация выполняются вне UI.
 * 0 = запрос поставлен в uasync, -1 = отказ подготовки, результат публикации — CHAT_EVT_MSG_SUBMITTED.
 * При 0 worker забирает собственный временный файл caller и удаляет также при ошибке/отмене.
 * При -1 файл остаётся caller. Вызовы сериализовать со stop/restart ядра; строки копируются. */
#ifndef ATTACHMENT_SENDER_H
#define ATTACHMENT_SENDER_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* channel_id — десятичный ID канала, db_path не используется; пути/строки NUL-терминированы.
 * reply_to обязательно указывает на 32 байта (нули = без ответа), request_id коррелирует результат с UI.
 * reply_quote — UTF-8 <=4000 байт, NULL/пусто — обычный ответ; копируется до возврата.
 * MIME application/octet-stream, имя — UTF-8 basename источника. Пользовательский исходник сначала копировать в cache. */
int attachment_send(const char* channel_id, const char* src_file_path, const char* db_path,
                    const uint8_t reply_to[32], uint64_t request_id, const char* reply_quote);

#ifdef __cplusplus
}
#endif

#endif
