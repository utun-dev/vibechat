/* member_files — файловые каталоги участников на storage-узлах CHAT-группы и клиент доступа к ним.
 * Для списка/операций: request с командой из protocol.h → callback с ответом. Для файла: transfer_start → progress/finished.
 * Клиент указывает группу, storage-сервер, владельца каталога и путь; права проверяются по членству и роли узла.
 * Все API — uasync. Запрос копируется; callback получает заимствованный ответ, действительный только до возврата.
 * Чат-сервис вызывает init; quiesce запрещает новые команды и отменяет producers; destroy вне callbacks ждёт
 * собственный дисковый поток до удаления CHAT-групп. Cancel не удаляет опубликованные файлы.
 * Отзыв отменяет ожидающее I/O; начатая команда завершается, её данные отозванному peer не выдаются.
 * GUI-команды используют trampolines; результат — CHAT_EVT_MEMBER_FILES_RESPONSE/TRANSFER. */
#ifndef MEMBER_FILES_H
#define MEMBER_FILES_H
#include "protocol.h"
#ifdef __cplusplus
extern "C" {
#endif
struct UTUN_INSTANCE;
struct mf_pending;
typedef void (*mf_response_fn)(void* arg, const struct mf_message* response);

// ── Жизненный цикл модуля ──

// Создать состояние сервера/клиента и worker; 0/-1. Требует живые ua/config/router/chat/SQLite, повторный init — ошибка.
// Стартовый scan асинхронный: init=0 ещё не означает готовность локального storage; remote client остаётся доступен.
int member_files_init(struct UTUN_INSTANCE* inst);

// Запретить новые команды и начать отмену запросов/передач; запущенный дисковый I/O завершается отдельно.
void member_files_quiesce(struct UTUN_INSTANCE* inst);

// Дождаться дискового worker и освободить контекст; вне callbacks, до удаления CHAT-групп.
void member_files_destroy(struct UTUN_INSTANCE* inst);

// Отменить свои запросы/передачи одной группы перед её удалением; дерево storage этим не удаляется.
void member_files_cancel_group(struct UTUN_INSTANCE* inst, uint64_t group);
// После commit состава/ролей: отменить утратившие права запросы, задания и передачи только указанной группы.
void member_files_revalidate_members(struct UTUN_INSTANCE* inst, uint64_t group);

// ── Команды файлового сервера ──

/* Отправить одну команду; request_id=0 выбирает случайный ID. NULL — отказ запуска, callback не вызывается.
 * При успехе callback ровно один, включая отмену; handle освобождён до callback, arg принадлежит вызывающему.
 * Общий timeout120с. BUSY для COMMIT/MKDIR/RENAME/REMOVE сохраняет ID и ждёт подсказанный сервером срок
 * освобождения квитанции (иначе5с), не продлевая общий timeout. Остальные BUSY передаются callback. */
struct mf_pending* member_files_request(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t server[MF_ADDRESS_SIZE],
                                       const struct mf_message* request, mf_response_fn callback, void* arg);

// Синхронно завершить свой запрос ответом MF_CANCELLED; handle после вызова недействителен, NULL допустим.
// Это отмена локального ожидания; уже запущенная серверная мутация не откатывается.
void member_files_cancel(struct mf_pending* pending);

// Отменить pending-запрос с этим request_id; не отменяет потоковую передачу файла.
void member_files_cancel_request_id(struct UTUN_INSTANCE* inst, uint64_t id);

// ── Проверка доступа ──

// 1 — полная source=0/deleted=0 запись мембера в локальной CHAT-таблице, иначе0.
// storage!=0 требует adm_tags.storage=yes и сохранённую64-байтную подпись; peers.storage не даёт полномочий.
int member_files_member(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t node[MF_ADDRESS_SIZE], int storage);

// ── Команда интерфейса ──

struct mf_gui_request {
    struct UTUN_INSTANCE* inst; // Заимствованное ядро до ответа.
    uint8_t group[MF_ADDRESS_SIZE]; // Ed25519 CHAT-группы.
    uint8_t server[MF_ADDRESS_SIZE]; // Ed25519 выбранного storage, включая собственный ключ.
    struct mf_message message; // Команда; для корреляции GUI выбирает ненулевой request_id.
};

// arg — u_malloc/u_calloc mf_gui_request; владение переходит при вызове trampoline, освобождается после ответа.
// CHAT_EVT_MEMBER_FILES_RESPONSE: group:32/server:32, затем mf_encode(response). Отказ запуска даёт ACCESS_DENIED.
void member_files_gui_request(void* arg);

// ── Потоковая передача файлов ──

struct mf_transfer_request {
    struct UTUN_INSTANCE* inst; // ядро с работающим чат-сервисом
    uint64_t id, group; // ID операции (0=авто), внутренний индекс маршрута.
    uint8_t group_key[MF_ADDRESS_SIZE]; // Ed25519 группы для команды и событий интерфейса.
    uint8_t server[MF_ADDRESS_SIZE], owner[MF_ADDRESS_SIZE]; // Ed25519 storage-узла и владельца.
    // Дополнительный лимит размера; 0 — без него. Upload: после открытия источника, до хеширования;
    // download: по ответу STAT, до открытия локального temp и передачи данных.
    uint64_t max_bytes;
    int upload; // ненулевой — загрузить local на сервер, 0 — скачать remote
    uint8_t version[16]; // Upload:ожидаемая версия/нули для создания; download:ожидаемая версия/нули для текущей.
    char remote[MF_PATH_MAX + 1], local[MF_PATH_MAX + 1]; // путь в каталоге owner и локальный путь приложения
};

struct mf_transfer;
struct mf_transfer_progress {
    uint64_t id; // Фактический ID handle, включая автоматически выбранный.
    uint64_t done, total; // Upload:принятые router байты; download:записанные на диск, до финального sync/publish.
    uint16_t status; // mf_status; результат окончательный только при finished.
    uint8_t finished; // 0 progress (может отсутствовать),1 ровно одно завершение.
};
typedef void (*mf_transfer_fn)(void* arg, const struct mf_transfer_progress* progress);

// Один handle на файл; до 16 клиентских передач одновременно, включая upload/download к одному пиру.
// Download удаляет temp только после своего успешного O_EXCL-create; отказ/отмена до create сохраняют чужой temp и назначение.
// Все вызовы/callbacks — uasync. id=0 выбирает случайный ID. NULL: отказ запуска, callback не вызывается.
// finished приходит ровно один раз после закрытия/публикации; handle уже освобождён до callback.
// Прогресс заимствован на время callback. Cancel асинхронно ждёт свой I/O и не затрагивает другие handles.
// Request копируется; исходную структуру можно освободить сразу, arg должен жить до finished.
// Download заменяет local только после проверки/закрытия своего temp; отказ/отмена до создания не удаляет чужой temp.
// Совпадение строк local двух download запрещено; aliases/подмену локальных файлов исключает caller.
// Upload требует стабильный regular source; изменение снимка даёт STALE. Пустой файл допустим.
struct mf_transfer* member_files_transfer_start(const struct mf_transfer_request* request, mf_transfer_fn callback, void* arg);

// Запросить отмену своей передачи; завершение приходит через finished после закрытия её файлового I/O.
// Допустимо сразу после start: поздний CM UP не запускает подготовку/STAT заново. Финализируемый результат уже не отменяется.
void member_files_transfer_cancel(struct mf_transfer* transfer);

// ── Команды интерфейса для передачи и отмены ──

/* id выбирает вызывающий. События: id:8LE, group/server/owner:32, done/total:8LE, status:2LE, state:1;
 * state=1 progress, 2 finished. arg — u_malloc/u_calloc mf_transfer_request, освобождается после finished. */
void member_files_gui_transfer(void* arg);

struct mf_cancel_request { struct UTUN_INSTANCE* inst; uint64_t id; }; // Заимствованное ядро и фактический ID команды/передачи.

// arg — u_malloc/u_calloc mf_cancel_request, освобождается здесь; отменяет передачу либо одиночный запрос по id.
void member_files_gui_cancel(void* arg);

#ifdef __cplusplus
}
#endif
#endif
