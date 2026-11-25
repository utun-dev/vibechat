/* Постоянные ссылки на сообщение группы, карточку и сообщение карточки.
 * parse проверяет полный UTF-8 URL без сети/SQL; подписи игнорируются при адресации.
 * build сокращает подписи до48 Unicode-символов, заменяет пробелы дефисами и экранирует разделители.
 * Буферы принадлежат caller; parse не меняет target при ошибке и не логирует пользовательский текст. */
#ifndef UTUN_OBJECT_LINK_H
#define UTUN_OBJECT_LINK_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define OBJECT_LINK_MAX 4096 /* Максимальная длина URL в UTF-8 байтах, без NUL. */
enum object_link_kind { OBJECT_LINK_CHAT_MESSAGE = 1, OBJECT_LINK_CARD, OBJECT_LINK_CARD_MESSAGE };
struct object_link_target {
    enum object_link_kind kind; /* chat/group/message либо kanban/group/card[/message]. */
    char group[65];             /* Полный публичный Ed25519 группы, lowercase hex. */
    char card[65];              /* Постоянный ID карточки, lowercase hex; пуст для chat. */
    char message[65];           /* Постоянный ID сообщения, lowercase hex; пуст для карточки. */
};
/* 0/-1; необязательный error получает краткую причину. UTF-8 и %XX допустимы только в подписях. */
int object_link_parse(const char* url, size_t size, struct object_link_target* target, char* error, size_t error_size);
/* Длина URL/-1; out_size включает NUL. Нужные ID берутся из target; NULL-подпись равнозначна пустой. */
int object_link_build(const struct object_link_target* target, const char* group_label, const char* card_label,
    const char* message_label, char* out, size_t out_size);
#ifdef __cplusplus
}
#endif
#endif
