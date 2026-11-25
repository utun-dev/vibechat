/*
 * Android-владелец одного ядра: INI из Kotlin → pthread с uasync → core_start + chat_service_start.
 * UTUN выключен по умолчанию; [gui] utun_enabled=yes включает его явно. Поток владеет instance/ua; UI передаёт работу через uasync.
 * С UTUN_WITH_VOICE worker регистрирует call_audio до SERVICE_STARTED и снимает привязку перед уничтожением ядра.
 * Полученные handles заимствованы и могут смениться при restart; сохранять их между рестартами нельзя.
 * Start/stop/restart сериализованы внутри владельца pthread. Вызовы по сырым handles сериализует frontend:
 * атомарный getter не удерживает ресурс от уничтожения. JOIN использует отдельный защищённый port.
 */
#ifndef INSTANCE_LITE_H
#define INSTANCE_LITE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct UASYNC;
struct posted_task;
#include "debug_config.h"
/* Frontend → worker; принимает владение ненулевым task при любом результате. Все данные находятся
 * в одном выделении (task первым полем) либо arg — скаляр. Callback не освобождает task.
 * До teardown принятая задача может исполниться; при удалении очереди uasync освобождает её без callback.
 * 0 = поставлена; -1 = отказ с логом. Для отдельного arg/discard использовать instance_lite_post. */
int instance_lite_post_frontend(struct posted_task* task, debug_category_t category, const char* name);
struct UTUN_INSTANCE;
struct utun_config;

/* Копирует INI и запускает поток. 0 = поток создан либо уже запущен, -1 = ошибка запуска.
 * Готовность самого ядра появляется позже: проверять is_running и события инициализации.
 * Startup/restart читает идентичность из keys.conf, если он есть (она старше снимка INI в очереди).
 * Формат: public_key=<64 HEX> LF private_key=<64 HEX> LF; ошибка чтения/пары запрещает запуск.
 * Без файла используются ключи INI; при их отсутствии сначала сохраняется новая пара. */
int instance_lite_start(const char* config_text);

/* Остановить и собрать joinable worker, включая инициализацию до появления instance и уже завершившийся startup.
 * Блокирует до полного cleanup/pthread_join; каждые 2с ожидания пишет предупреждение, поток не отсоединяет.
 * Worker снимает media-привязки, уничтожает instance, затем uasync.
 * Управляющий frontend предварительно останавливает аудио I/O и освобождает DSP звонка.
 * Вызывать вне UI и callbacks ядра. Без worker повторный вызов безопасен и сразу возвращается.
 * 0 — worker полностью собран/отсутствует; -1 — ошибка join/недопустимый поток, callbacks освобождать нельзя. */
int instance_lite_stop(void);

/* 1 — instance опубликован и завершил запуск сервисов; 0 также с момента принятия restart до нового ready.
 * Отказ подготовки restart возвращает ready прежнему ядру и SERVICE_STARTED для восстановления frontend.
 * SERVICE_STARTED сам по себе не подтверждает пересоздание instance или смену ключей. Это не проверка сети. */
int instance_lite_is_running(void);

/* Текущий заимствованный uasync, NULL до create/после destroy; ненулевой handle ещё не означает готовность ядра. */
struct UASYNC* instance_lite_get_uasync(void);

/* Текущий заимствованный instance, NULL до create/после снятия; может быть ненулевым во время инициализации. */
struct UTUN_INSTANCE* instance_lite_get_instance(void);
/* Передать single-allocation arg callback в готовый worker, не заимствуя inst/ua.
 * instance_field (если задан) указывает на поле arg: оно заполняется текущим instance под защитой владельца.
 * 0 — callback владеет arg; при отмене до исполнения discard(arg) либо u_free(arg) освобождает его.
 * -1 — отказ с логом, arg остаётся caller. Callback/discard вызываются в worker и не могут stop/join его. */
int instance_lite_post(void (*callback)(void*), void* arg, struct UTUN_INSTANCE** instance_field, void (*discard)(void*));
/* Выполнить короткое чтение в потоке ядра и дождаться результата; callback получает заимствованный arg.
 * Worker вызывает callback сразу. Frontend не должен держать блокировки, необходимые worker; тяжёлые чтения выполнять с IO.
 * 0 — callback завершён; -1 — неготовое ядро/ошибка памяти/отмена при stop/restart, callback не исполнялся.
 * Ожидание заканчивается после callback либо отмены; стек caller остаётся жив до этого подтверждения. */
int instance_lite_call(void (*callback)(void*), void* arg);
/* Завершить начатую deferred-операцию ровно один раз, также при ошибке/отмене.
 * После complete нельзя обращаться к arg/completion_arg: ожидающий frontend уже может освободить их. */
typedef void (*instance_lite_complete_fn)(void* completion_arg);
typedef void (*instance_lite_deferred_fn)(void* arg, instance_lite_complete_fn complete, void* completion_arg);
/* Frontend: выполнить begin в uasync, дождаться его отложенного complete в вызывающем потоке.
 * Uasync не ждёт: begin ставит SQL/другую async-работу и возвращается. Вызов из самого worker отклоняется.
 * arg заимствован до возврата; begin/его callback обязаны вызвать complete на любом исходе.
 * 0=операция завершилась через complete, бизнес-результат находится в arg;-1=отказ/отмена до begin.
 * Stop/restart отменяет ещё не начатый begin; после начала ждём completion, включая DB drain при shutdown.
 * Не удерживать нужные worker блокировки. API не меняет поток UI и не выполняет SQL в uasync. */
int instance_lite_call_deferred(instance_lite_deferred_fn begin, void* arg);
/* Потокобезопасный JOIN через удержанный port: копирует len байтов ссылки, request ненулевой.
 * 0 — принят, результат CHAT_EVT_CONNECT_RESULT с тем же request; <0 — отказ без события.
 * Не требует от caller удерживать inst/ua; при stop/restart port закрывается. */
int instance_lite_join_link(uint64_t request, const char* link, size_t len);
/* Любой поток: добавить выбранный локальный channel на сервер по SERVER-ссылке.
 * 0 — команда принята, результат CONNECT_RESULT с request; <0 — отказ без события. */
int instance_lite_add_server(uint64_t request, const char* channel, const char* link, size_t len);

/* События CHAT_EVT_* из native-потока; data заимствован только на вызов, формат зависит от type (не всегда JSON).
 * Установить до start; callback и его окружение должны жить до завершения worker. NULL отключает доставку. */
typedef void (*instance_lite_event_fn)(int type, const uint8_t* data, int len);
void instance_lite_set_event_handler(instance_lite_event_fn handler);

/* Создать seed устройства Ed25519 и производный публичный X25519, атомарно сохранить в config->global.db_path/keys.conf с правами 0600.
 * config — разобранный конфиг с непустым db_path; caller исключает параллельную смену идентичности.
 * 0 — файл и каталог синхронизированы, ключи/node_id config обновлены; -1 — config не изменён, ошибка залогирована.
 * До rename прежний файл сохраняется; при ошибке sync каталога после rename файл уже заменён (published=1 в логе).
 * Не меняет работающее ядро; для новой идентичности его нужно пересоздать. */
int instance_lite_generate_keys(struct utun_config* config);

/* Явная замена ключа в directory после stop/join. 0 — сохранён, 1 — заменён, sync каталога не подтверждён, -1 — отказ до замены. */
int instance_lite_replace_stopped_keys(const char* directory);

/* Принять INI для полной смены идентичности в worker: сохранить новую пару и пересоздать ядро/сервисы.
 * Нужен готовый core; frontend предварительно останавливает media workers/DSP. 0 — копия INI принята, -1 — отказ с логом.
 * Ошибка разбора/сохранения оставляет прежнее ядро и возвращает его ready с SERVICE_STARTED;
 * только успех даёт KEYS_GENERATED (64 HEX), затем SERVICE_STARTED нового ядра.
 * Последний ещё не взятый worker запрос restart/regenerate заменяет предыдущий. */
int instance_lite_regenerate_keys(const char* config_text);

/* Frontend: поставить ACTIVE (active!=0) / STANDBY (0) без заимствования сырого uasync.
 * 0 — задача зарезервирована и поставлена живому worker; -1 — ядро не готово/останавливается, ошибка памяти или неверный поток.
 * При stop ожидающая команда может отмениться. Результат применения/stream hold логируется в потоке ядра. */
int instance_lite_set_active(int active);

/* Frontend: поставить состояние SIM (0 UNKNOWN/1 IDLE/2 BUSY) и маску call_policy (0..31) живому worker.
 * 0 — данные скопированы в задачу под защитой времени жизни uasync; -1 — неверные аргументы/поток, неготовое ядро или OOM.
 * Применение асинхронно в текущем ядре; stop/restart может отменить ожидающую команду, освободив её данные.
 * После SERVICE_STARTED frontend повторно отправляет актуальные state/policy. */
int instance_lite_set_phone_state(int state, unsigned policy);

#define INSTANCE_LITE_RADIO_SETTINGS_COUNT 7
/* Результат атомарной проверки/применения в worker; request_id возвращается без изменения.
 * Callback живёт до join; из него нельзя синхронно останавливать worker. */
typedef void (*instance_lite_radio_configured_fn)(int64_t request_id, int success);
/* Frontend: копирует 7 читаемых int: enabled, mode, levelOn, levelOff, hangoverMs, vadThreshold, vadGain.
 * 0 — копия поставлена живому worker под защитой владельца; -1 — неготовое ядро, OOM, NULL values/callback или неверный поток.
 * В worker все поля проверяются и применяются целиком; configured(request_id, 1/0) означает успех/отказ без частичного изменения.
 * Stop/restart может отменить ожидающий снимок без callback, освободив всё выделение. Новый запуск аудио присылает актуальный снимок. */
int instance_lite_configure_radio(int64_t request_id, const int* values, instance_lite_radio_configured_fn configured);

/* Копирует INI, будит poll-loop и пересоздаёт instance и uasync внутри worker.
 * До остановки проверяет конфиг и сохраняет отсутствующие ключи; отказ оставляет прежнее ядро.
 * При инициализации ставит запрос в очередь; при завершившемся worker делает join/start.
 * Обновление асинхронное; старые handles утрачивают силу.
 * Stop имеет приоритет перед ожидающим restart. Последний ещё не взятый worker запрос заменяет предыдущий. */
int instance_lite_restart(const char* new_config_text); /* 0 — запрос принят, -1 — отказ с логом. */

/* Лёгкое обновление сокетов (auto_sockets=android): перечитывает [server] секции
 * из нового конфига и сверяет сокеты без полного рестарта. Posts to uasync thread.
 * 0 — копия принята worker, -1 — ошибка памяти/неготовое ядро с логом. */
int instance_lite_update_sockets(const char* config_text);

/* Поставить локальный ping в uasync (не сетевой). is_responsive=1 после исполнения последнего ping;
 * до первого ping/при остановке возвращает 0. Встроенного ожидания и проверки таймаута нет. */
void instance_lite_ping(void);
int  instance_lite_is_responsive(void);

/* Асинхронные снимки: CHAT_EVT_CONN_LIST и CHAT_EVT_CONN_METRICS; peer_node_id — ID интересующего пира. */
void instance_lite_collect_conn_list(void);
void instance_lite_collect_conn_metrics(uint64_t peer_node_id);

#ifdef __cplusplus
}
#endif
#endif /* INSTANCE_LITE_H */
