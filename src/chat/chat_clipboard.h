/* Буфер обмена своих устройств и участников разрешённой CHAT-группы через штатный E2E router.
 * Только uasync: одна исходящая рассылка, до четырёх входящих, без БД/outbox и повторов после reset.
 * Frontend применяет свой снимок или сохраняет предложение участника из RECEIVED, затем вызывает applied.
 * Чанки до 8 КиБ, текст до 1 МиБ, PNG до 32 МиБ; общий входящий бюджет 64 МиБ, простой 30 с.
 * Все wire/event числа LE; содержимое никогда не выводится в диагностику. */
#ifndef CHAT_CLIPBOARD_H
#define CHAT_CLIPBOARD_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct UTUN_INSTANCE;
#define CHAT_CLIPBOARD_TEXT 1
#define CHAT_CLIPBOARD_PNG 2
#define CHAT_CLIPBOARD_FILES 3 /* UTF-8 manifest; содержимое запрашивает файловый provider ОС. */
#define CHAT_CLIPBOARD_SOURCES_MAX 8192u
#define CHAT_CLIPBOARD_TEXT_MAX (1024u * 1024u)
#define CHAT_CLIPBOARD_IMAGE_MAX (32u * 1024u * 1024u)
#define CHAT_CLIPBOARD_OWN 0
#define CHAT_CLIPBOARD_MEMBER 1
#define CHAT_CLIPBOARD_EVENT_GROUP_OFF 16
#define CHAT_CLIPBOARD_EVENT_PEER_ED_OFF 48
#define CHAT_CLIPBOARD_EVENT_TYPE_OFF 80
#define CHAT_CLIPBOARD_EVENT_SCOPE_OFF 81
#define CHAT_CLIPBOARD_EVENT_HEADER 82 /* id16 + group Ed25519:32 + peer Ed25519:32 + type1 + scope1. */
enum chat_clipboard_result { CHAT_CLIPBOARD_STARTED, CHAT_CLIPBOARD_FINISHED, CHAT_CLIPBOARD_REJECTED };
/* init0/-1 после chat_core, destroy до уничтожения router/групп; повтор destroy допустим. */
int chat_clipboard_init(struct UTUN_INSTANCE* inst);
void chat_clipboard_destroy(struct UTUN_INSTANCE* inst);
/* 0=рассылка принята,-1=отказ; в обоих случаях RESULT с request. data копируется до возврата.
 * request ненулевой, type TEXT/PNG/FILES, size1..лимит; frontend проверяет содержимое до отправки/применения. */
int chat_clipboard_send(struct UTUN_INSTANCE* inst, uint64_t request, uint64_t group, const uint8_t peer[32],
    int type, const uint8_t* data, size_t size); /* group=0 и peer=NULL — своим устройствам; иначе адресно участнику разрешённой группы. */
void chat_clipboard_revalidate(struct UTUN_INSTANCE* inst, uint64_t group); /* После commit: отозвать файловые offers с утраченными правами;0 все. */
/* Подтверждение применения только текущего полного снимка; id16/group/peer из RECEIVED.
 * success0/1; повтор/устаревший результат игнорируется с WARN. После возврата аргументы не нужны. */
void chat_clipboard_applied(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], const uint8_t id[16], int success);
/* Единый owned-блок команды GUI/JNI; SEND содержит size байт сразу после структуры.
 * Трамплин забирает блок и очищает payload перед освобождением; остальные поля SEND не нужны. */
struct chat_clipboard_request {
    struct UTUN_INSTANCE* inst; /* Заимствован до выполнения/отмены команды. */
    uint64_t request;
    uint8_t group[32]; /* Нулевой ключ только для отправки своим устройствам. */
    uint8_t id[16], peer_ed[32]; /* Идентичность принимаемой передачи для APPLIED. */
    uint32_t size; /* Число байтов после структуры для SEND. */
    uint32_t source_count; /* PUBLISH_FILES: сначала массив source_count источников, затем size байт manifest. */
    int type, action, success; /* type TEXT/PNG; action0 SEND,1 APPLIED,2 PUBLISH_FILES; success только APPLIED. */
};
void chat_clipboard_trampoline(void* arg);
struct chat_clipboard_source {
    uint8_t object[16]; /* Случайный ID неизменяемого файла. */
    uint64_t size; /* Точный размер; пустые файлы создаются frontend без сети. */
    char path[1024]; /* Собственный локальный snapshot UTF-8 с NUL; живёт до stop либо смены devgroup. */
};
/* Worker/frontend передаёт запрос в uasync; done вызывается в uasync ровно один раз после передачи/отказа.
 * destination — собственный temp, caller проверяет SHA256 и публикует. Трамплин владеет u_malloc-запросом.
 * done/arg должны пережить stop; при отказе постановки команды done вызывает frontend самостоятельно. */
struct chat_clipboard_pull_request {
    struct UTUN_INSTANCE* inst;
    uint64_t route_index, size;
    uint8_t group[32]; /* Полный адрес группы из RECEIVED. */
    uint8_t peer_ed[32], object[16];
    char destination[1024];
    void (*done)(void* arg, int error);
    void* arg;
};
void chat_clipboard_pull_trampoline(void* arg);
#ifdef __cplusplus
}
#endif
#endif
