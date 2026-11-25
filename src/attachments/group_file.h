/* Общая регистрация подготовленного группового файла: durable intent → index worker → READY.
 * Модель сохраняет сообщение и вызывает store_publish_write в одной WRITE. Файл/блоки анонсировать после commit.
 * Данные request копируются до возврата. inst/arg живут до callback/SQL drain; done вызывается ровно один раз.
 * guard вызывается только в uasync; отмена оставляет durable intent для восстановления. */
#ifndef UTUN_ATTACHMENT_GROUP_FILE_H
#define UTUN_ATTACHMENT_GROUP_FILE_H
#include "store.h"
#include "../media_delivery/media_index.h"
struct UTUN_INSTANCE;
#ifdef __cplusplus
extern "C" {
#endif
struct attachment_group_file {
    enum attachment_owner owner; /* CHAT/KANBAN; транспорт у них общий. */
    const char *group, *card, *source, *destination, *type; /* card только KANBAN, остальные обязательны. */
    const char* description; /* Имя/voice metadata, NUL, 1..4095 UTF-8 байт. */
    const uint8_t* reply; /* 32 байта либо NULL; часть неизменяемого intent CHAT. */
    const char* reply_quote; /* Заимствованный UTF-8 снимок; NULL/пусто — обычный ответ, копируется до возврата. */
    int copy; /* Копирование source в destination выполняет файловый worker. */
    uint32_t duration_ms; uint16_t width, height; /* Публичные подготовленные размеры, без пробинга в SQL/uasync. */
};
/* job[16] заполняется синхронно до submit; result заимствован до возврата done. 0=READY committed, иначе ошибка. */
void attachment_group_register(struct UTUN_INSTANCE* inst, const struct attachment_group_file* request,
    uint8_t job[16], int (*guard)(void*), void (*done)(void*, int, const struct media_index_result*), void* arg);
#ifdef __cplusplus
}
#endif
#endif
