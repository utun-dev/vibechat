/* Отправка подготовленного MP4 в канал или dm:conv_id через общий attachment_send.
 * Android передаёт duration_ms/width/height; повторного транскодирования здесь нет. db_path не используется.
 * 0 = запрос поставлен в uasync, -1 = отказ. Передаётся владение временным cache-файлом:
 * При принятом запросе задача удаляет src_file_path также при ошибке/отмене; при -1 файл остаётся у caller.
 * Вызовы сериализовать со stop/restart ядра; строки копируются. Результат приходит событиями ядра. */
#ifndef VIDEO_SENDER_H
#define VIDEO_SENDER_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* target — десятичный ID канала либо dm:conv_id; source — NUL-путь короче 1024 байтов.
 * duration_ms=1..86400000, width/height=1..16384. Для канала reply_to — обязательные 32 байта
 * (нули = без ответа), request_id → CHAT_EVT_MSG_SUBMITTED для канала и DM. В DM reply_to игнорируется;
 * reply_quote — UTF-8 <=4000 байт, NULL/пусто — обычный ответ; копируется до возврата.
 * generation фиксирует поколение беседы, 0 захватывает его в trampoline. */
int video_send(const char* channel_id, const char* src_file_path, const char* db_path,
               int duration_ms, int width, int height, const uint8_t reply_to[32], uint64_t request_id, uint64_t generation,
               const char* reply_quote);

#ifdef __cplusplus
}
#endif

#endif
