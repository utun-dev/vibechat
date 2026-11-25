/* Публичное описание группового файла, независимо от истории chat/Kanban.
 * Формат существующей доставки: base64url-описание, геометрия, UUID/хеш, подписанные блоки и размеры медиа.
 * parse проверяет формат, но не подпись сообщения/блоков; это обязанность владельца/загрузчика.
 * Результат parse владеет массивами media_index_result_free; format возвращает строку caller/u_free. */
#ifndef UTUN_ATTACHMENT_MANIFEST_H
#define UTUN_ATTACHMENT_MANIFEST_H
#include <stddef.h>
#include <stdint.h>
#include "../media_delivery/media_index.h"
#ifdef __cplusplus
extern "C" {
#endif
#define ATTACHMENT_MANIFEST_BODY_MAX 16384
/* NUL-строка до BODY_MAX; 0=разобрано,-1=неверный формат,-2=OOM. output обнулён при отказе. */
int attachment_manifest_parse(const char* body, struct media_index_result* output);
/* description — непустые исходные байты имени/голосовых метаданных; result заимствован.
 * duration добавляется только при размерах w/h; 0 означает отсутствие длительности. NULL=ошибка с логом. */
char* attachment_manifest_format(const void* description, size_t length, const struct media_index_result* result,
    uint32_t duration_ms, uint16_t width, uint16_t height);
#ifdef __cplusplus
}
#endif
#endif
