/* Серверное хранилище. После create все initialize/execute/sweep/destroy выполняются
 * последовательно одним disk worker; блокировки внутри storage нет. create не обращается к диску;
 * ещё не инициализированный пустой owner допустимо destroy в потоке создателя.
 * initialize выполняет стартовый обход до запросов. ACL проверяет member_files.c перед каждой работой.
 * Storage сам проверяет group/peer/owner и принадлежность мутации ключу владельца, но не членство/подписанные роли.
 * Файлы временных загрузок
 * закрываются после 10 секунд простоя, удаляются после upload_ttl; restart отменяет их.
 * Открытые regular-файлы кешируются, максимум64 (включая temp); активных upload до16.
 * Квота/числа объектов общие для владельца во всех группах и восстанавливаются только при initialize.
 * Успешные COMMIT/MKDIR/RENAME/REMOVE занимают одну из64 квитанций на MF_REPLAY_TB от начала обработки.
 * Родительские пути не следуют ссылкам. Файлы и счётчики не образуют откатываемую транзакцию. */
#ifndef MEMBER_FILES_STORAGE_H
#define MEMBER_FILES_STORAGE_H
#include "protocol.h"
struct mf_storage;
/* root — NUL-путь1..2047 байт, quota/TTL>0 (байты и timebase0.1мс); копирует root, возвращает owner/NULL. */
struct mf_storage* mf_storage_create(const char* root, uint64_t quota, uint64_t upload_ttl_tb);
/* MF_OK/положительный mf_status: создать/открыть root, удалить старые .uploads, восстановить квоты.
 * Повтор после успеха — no-op; после ошибки возвращает тот же код, нужен новый owner/restart.
 * До завершения execute возвращает BUSY, после ошибки — её код, частичные счётчики не выдаются. */
int mf_storage_initialize(struct mf_storage* storage);
/* Закрыть fd/корень, удалить активные temp и освободить owner; опубликованные файлы сохраняются, NULL допустим. */
void mf_storage_destroy(struct mf_storage* storage);
/* Закрыть idle fd и удалить просроченные uploads; now — монотонный timebase0.1мс, NULL допустим. */
void mf_storage_sweep(struct mf_storage* storage, uint64_t now);
/* Выполнить LIST..REMOVE либо QUOTA; STREAM преобразует transfer.c. Аргументы обязательны и не перекрываются.
 * request не меняется; response получает копию запроса и результат, status/data_len определяют пригодность данных.
 * Одинаковый ID/содержимое мутации возвращает квитанцию; конфликт — BAD_PACKET. После окна гарантий повтора нет.
 * Полный кеш: BUSY.cursor — остаток до ближайшего освобождения в timebase; квитанции не вытесняются. */
void mf_storage_execute(struct mf_storage* storage, const char* group, const uint8_t peer[MF_ADDRESS_SIZE],
                        const struct mf_message* request, struct mf_message* response);
/* Диагностика/тест: число файловых дескрипторов, исключая каталог корня. */
unsigned mf_storage_open_count(const struct mf_storage* storage);
#endif
