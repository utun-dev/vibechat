/* Отправка подготовленного изображения: JPEG/PNG в канал/PM через attachment_send, другие форматы — только в канал.
 * MIME и размеры с учётом EXIF определяет Android по содержимому. JPEG/PNG забирает собственный cache-файл caller.
 * 0 = запрос поставлен в uasync, -1 = отказ (cache остаётся caller); результат — CHAT_EVT_MSG_SUBMITTED.
 * Вызовы сериализовать со stop/restart; для дополнительных форматов источник живёт до результата и удаляется caller. */
#ifndef PHOTO_SENDER_H
#define PHOTO_SENDER_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* channel_id — ID канала либо dm:<conv>, generation — зафиксированное поколение PM (для канала 0).
 * db_path нужен только дополнительным форматам канала; content_type — проверенный MIME.
 * reply_to — обязательные 32 байта (нули = без ответа), request_id — корреляция результата локального commit с UI.
 * reply_quote — UTF-8 <=4000 байт, NULL/пусто — обычный ответ; копируется до возврата. */
int photo_send(const char* channel_id, const char* src_file_path, const char* db_path,
               int width, int height, const uint8_t reply_to[32], uint64_t request_id,
               const char* content_type, uint64_t generation, const char* reply_quote);

#ifdef __cplusplus
}
#endif

#endif
