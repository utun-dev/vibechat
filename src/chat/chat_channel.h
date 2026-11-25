/* Асинхронное создание/подготовка общей группы: SQL WRITE и подписи выполняет worker,
 * topology/протокол содержимого публикуются в uasync только после commit. Одна группа сохраняет MEMBERS/роли.
 * Входы копируются; inst/arg живут до done/DB drain. Stop/restart отменяет очередь и позднюю публикацию. */
#ifndef UTUN_CHAT_CHANNEL_H
#define UTUN_CHAT_CHANNEL_H
#include "chat_core.h"
#include "../group/group_protocol.h"
#ifdef __cplusplus
extern "C" {
#endif
struct chat_channel_result {
    int status, sqlite_code; /* 0=сохранено и runtime подготовлен,-1=ошибка/stop; SQLite уточняет причину. */
    int committed; /* WRITE committed, в том числе при ошибке поздней runtime активации. */
    char channel[65]; /* Публичный Ed25519 группы в HEX, если запись сохранена. */
};
typedef void (*chat_channel_done_fn)(void* arg, const struct chat_channel_result* result);
/* Создать ключи CHAT-группы в четырёх раздельных буферах. ed — открытый Ed25519 со знаком 0, seed — его raw seed.
 * x_private — скаляр Ed25519 для X25519, x — соответствующий публичный ключ. Оба секрета доступны только владельцам.
 * 0 при успехе, -1 при ошибке. При отказе генерации все четыре буфера обнуляются. */
int chat_channel_generate_keys(uint8_t x[32], uint8_t x_private[32], uint8_t ed[32], uint8_t seed[32]);
/* До принятия описания проверить публичные ключи группы. channel нужен для журнала.
 * ed кодирует допустимую точку основной подгруппы со знаком 0 вне диапазона UTUN; x получается преобразованием ed.
 * SQLITE_OK при совпадении, SQLITE_AUTH при неверной паре, SQLITE_MISUSE/SQLITE_ERROR при локальной ошибке. */
int chat_channel_check_public_keys(const char* channel, const uint8_t x[32], const uint8_t ed[32]);
/* SQL-free runtime activation для bootstrap после общей WRITE; group topology создаётся при отсутствии.
 * prepared заимствован до возврата;0/-1, повтор допустим. */
int chat_channel_activate(struct UTUN_INSTANCE* inst, const char* channel, const struct group_protocol_prepared* prepared);
/* Uasync: ненулевой done вызывается ровно один раз (также синхронный отказ/OOM/FULL); результат заимствован.
 * done=NULL допустим для fire-and-forget trampoline: ошибки остаются в журнале. */
void chat_channel_create_submit(struct UTUN_INSTANCE* inst, const struct chat_channel_create* request,
    chat_channel_done_fn done, void* arg);
void chat_channel_create_auto_submit(struct UTUN_INSTANCE* inst, const char* name, const char* type,
    chat_channel_done_fn done, void* arg);
void chat_channel_prepare_submit(struct UTUN_INSTANCE* inst, const char* channel, chat_channel_done_fn done, void* arg);
#ifdef __cplusplus
}
#endif
#endif
