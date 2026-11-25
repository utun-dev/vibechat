/*
 * chat_setting.h — chat-related settings (key-value API)
 *
 * Один API для всех потребителей: парсер конфига (headless) и GUI (через bridge).
 * Нулевое состояние инициализируется значениями реестра при первом успешном set.
 * Ошибка валидации не меняет состояние. Чтение const, не выделяет память и не пишет в хранилище.
 *
 * Состояние — per-instance. Конфиг-парсер заполняет struct chat_setting_state
 * из [chatserver] ещё до создания instance (state-уровень), затем runtime-модули
 * читают через instance-уровень (chat_setting_get_int(inst, ...)). State не синхронизирован:
 * до старта им владеет парсер, runtime-вызовы выполняются в uasync-потоке instance.
 */

#ifndef CHAT_SETTING_H
#define CHAT_SETTING_H

#include <stdint.h>

struct UTUN_INSTANCE;

#ifdef __cplusplus
extern "C" {
#endif

/* ─── per-instance хранилище ─── */

#define CHAT_SETTING_MAX_SETTINGS 64
#define CHAT_SETTING_STRING_MAX   16

struct chat_setting_state {
    int  values[CHAT_SETTING_MAX_SETTINGS];       /* Числовые значения по индексу реестра. */
    int  string_idx[CHAT_SETTING_STRING_MAX];     /* Индексы занятых строковых настроек. */
    char string_val[CHAT_SETTING_STRING_MAX][256]; /* NUL-строки, не более255 байт. */
    int  string_count;                           /* Число занятых string_val. */
    int  initialized;                            /* 0 до первого успешного set. */
    char int_text[CHAT_SETTING_MAX_SETTINGS][16]; /* Текст числа, обновляемый только setter. */
};

/* ─── state-уровень (конфиг-парсер, до создания instance) ─── */

/* s изначально обнулён. name/value заимствованы до возврата; значение копируется.
 * 0 — сохранено, -1 — неизвестное имя, -2 — неверные аргументы/значение/нет места.
 * Числа — decimal в диапазоне реестра; bool — 0/1,no/yes,false/true; пути допускают пустую строку. */
int  chat_setting_set_state(struct chat_setting_state* s, const char* name, const char* value);
/* Заимствованная NUL-строка до изменения этой настройки/уничтожения s; другие getters её не меняют.
 * NULL — неизвестное имя/NULL s/неинициализированное состояние. Для bitrate range даже до set — "300:4000".
 * Незаполненный строковый путь после инициализации — "". */
const char* chat_setting_get_state(const struct chat_setting_state* s, const char* name);
/* Число/bool либо def для неизвестного/строкового имени, NULL s и неинициализированного состояния. */
int  chat_setting_get_int_state(const struct chat_setting_state* s, const char* name, int def);

/* ─── instance-уровень (runtime) ─── */

/* Те же коды; NULL inst — -2. Большинство потребителей читают значение при следующем обращении/старте.
 * incoming_call_policy сразу обновляет телефонную политику, local_* — LAN permissions, opus_codec_preset — битрейт звонка.
 * -3 — значение сохранено, применение не удалось; повтор set того же значения повторяет apply. */
int  chat_setting_set(struct UTUN_INSTANCE* inst, const char* name, const char* value);
/* Instance-варианты чтения с теми же lifetime/default; NULL inst даёт NULL/def. */
const char* chat_setting_get(struct UTUN_INSTANCE* inst, const char* name);
int  chat_setting_get_int(struct UTUN_INSTANCE* inst, const char* name, int def);

#ifdef __cplusplus
}
#endif
#endif /* CHAT_SETTING_H */
