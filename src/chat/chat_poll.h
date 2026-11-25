/* Общий формат опросов, ответов и реакций. Подписанное poll-сообщение неизменяемо.
 * single допускает один пункт ИЛИ свой ответ; multiple — пункты и необязательный свой ответ.
 * Один пользователь выбирает одну реакцию. Отмена/пустой ответ кодируются нулевой длиной с новым ver.
 * Сетевые API вызываются в uasync; snapshot принимает SQL владельца и допускает DB worker.
 * Кодеки не зависят от экземпляра.
 * JSON-снимки и результаты команд выделяются через u_malloc, освобождаются u_free. */
#ifndef CHAT_POLL_H
#define CHAT_POLL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct UTUN_INSTANCE;
struct sqlite3;

#define CHAT_POLL_SINGLE 1
#define CHAT_POLL_MULTIPLE 2
#define CHAT_POLL_OPTIONS_MAX 32
#define CHAT_POLL_QUESTION_MAX 512
#define CHAT_POLL_OPTION_MAX 256
#define CHAT_POLL_CUSTOM_MAX 1024
#define CHAT_POLL_WIRE_MAX 2048
#define CHAT_REACTION_COUNT 16
#define CHAT_REACTION_SELECTED_MAX 1

struct chat_poll {
    uint8_t mode, allow_custom_answer, option_count; /* SINGLE/MULTIPLE,0/1,число пунктов2..32. */
    char question[CHAT_POLL_QUESTION_MAX + 1]; /* Непустой UTF-8 с NUL,лимиты в байтах. */
    char options[CHAT_POLL_OPTIONS_MAX][CHAT_POLL_OPTION_MAX + 1]; /* Непустые различные строки UTF-8 с NUL. */
};

struct chat_poll_answer {
    uint8_t count, choices[CHAT_POLL_OPTIONS_MAX]; /* Число/номера пунктов0..31;encode сортирует выход,decode требует порядок. */
    char custom[CHAT_POLL_CUSTOM_MAX + 1]; /* UTF-8 с NUL,пустая строка допустима. */
};

/* Encode:длина>=0/-1;decode:0/-1. Память заимствована,выход при ошибке может быть частичным.
 * Decode требует точное каноническое представление,UTF-8 без NUL и лишних байтов.
 * Poll wire<=2048,не каждая комбинация пределов полей помещается;переполнение отклоняется.
 * Answer/reaction с нулевой длиной — отмена;для encode out обязателен даже при отмене. */
int chat_poll_encode(const struct chat_poll* poll, uint8_t* out, size_t capacity);
int chat_poll_decode(const uint8_t* data, size_t len, struct chat_poll* poll);
int chat_answer_encode(const struct chat_poll_answer* answer, uint8_t* out, size_t capacity);
int chat_answer_decode(const uint8_t* data, size_t len, struct chat_poll_answer* answer);
int chat_reactions_encode(const uint8_t* ids, size_t count, uint8_t* out, size_t capacity);
int chat_reactions_decode(const uint8_t* data, size_t len, uint8_t ids[CHAT_REACTION_SELECTED_MAX], size_t* count);
/* Постоянный общий каталог: существующие номера не менять. NULL при неверном номере. */
const char* chat_reaction_emoji(unsigned id);
/* Структурная проверка data, независимая от наличия исходного сообщения. 0/-1. */
int chat_feedback_validate_data(uint8_t kind, const uint8_t* data, size_t len);

/* Публикация подписанного poll: 0/-1, out_id — общий ID сохранённого сообщения. */
int chat_poll_create(struct UTUN_INSTANCE* inst, const char* channel, const struct chat_poll* poll, uint8_t out_id[32]);
/* Снимок сообщения и результатов:pending(нет исходного сообщения),poll,reactions,mine и ответы.
 * SQL/worker: db — соединение владельца, self — ID локального узла для mine (0 отключает выделение своих ответов).
 * 0 либо отрицательный SQLite-код; json обязателен,при успехе получает u_malloc-строку,при отказе NULL.
 * Не меняет данные/версии; runtime/регистрация истории не нужны, отсутствие истории даёт pending. */
/* previous_admission=1 только внутри транзакции MEMBERS до записи нового admitted: зафиксировать прежний локальный результат. */
int chat_feedback_snapshot(struct sqlite3* db, const uint8_t self[32], const char* channel, const uint8_t message_id[32],
    int previous_admission, char** json);
/* JSON-команды poll_create, vote, react, feedback, reaction_catalog. 0/-1.
 * Command — NUL JSON<=16384байт,channel — публичный Ed25519 группы в строчном HEX.
 * Ошибка возвращает JSON с error,при OOM/неверных аргументах возможен NULL;result обязателен.
 * Успех — снимок/каталог либо подтверждение {ch,message_id,committed:true,snapshot_pending:true},
 * если изменение сохранено,но его чтение отказало. В этом случае повторяют только feedback,
 * не poll_create/vote/react. Память подтверждения резервируется до изменения.
 * vote: choices:[0..31], custom:string; пустые значения отменяют голос.
 * react:emoji:0..15 либо полная строка из каталога;новая заменяет прежнюю.
 * Без on — переключить выбранную,on:false отменяет только выбранную,clear:true — любую. */
int chat_feedback_command(struct UTUN_INSTANCE* inst, const char* command, char** result);
struct chat_feedback_command_result {
    int status, sqlite_code, committed; /* 0=успех,-1=отказ; exact SQL-причина; изменение уже committed. */
    const char* json; /* Заимствован до возврата done; JSON снимка/каталога/ошибки, при OOM возможен NULL. */
};
typedef void (*chat_feedback_command_done_fn)(void* arg, const struct chat_feedback_command_result* result);
/* Uasync: скопировать JSON, выполнить SQL/подписи через worker, затем уведомить интерфейсы после commit.
 * done обязателен/ровно один раз, включая отказ/OOM/FULL/stop; inst/arg живут до done/drain.
 * Повторять изменяющую команду после committed=1 нельзя; недоступный снимок перечитать через feedback.
 * RAM может завершить callback синхронно; прежний sync command используется только RAM/READONLY. */
void chat_feedback_command_submit(struct UTUN_INSTANCE* inst, const char* command, chat_feedback_command_done_fn done, void* arg);
/* Проверка отложенных ответов и диагностика при получении сообщения/нового состояния. */
void chat_feedback_check_message(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t message_id[32]);
/* Команда из UI: req и следующая за ней NUL-строка выделены одним блоком через u_malloc.
 * Результат возвращается событием CHAT_EVT_FEEDBACK_RESULT и включает request_id. */
struct chat_feedback_request { struct UTUN_INSTANCE* inst; uint64_t request_id; }; /* Заимствованный instance,ID результата UI. */
void chat_feedback_command_trampoline(void* arg);

#ifdef __cplusplus
}
#endif
#endif
