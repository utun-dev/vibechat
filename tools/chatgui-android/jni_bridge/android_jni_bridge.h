/*
 * C API Android bridge; JNI-точки входа Kotlin находятся в .c под __ANDROID__ и не все входят в этот заголовок.
 * NDK собирает bridge с общим ядром в libutun.so. Команды обычно ставятся в uasync, готовность/результат приходят событиями.
 * C getters читают SQLite/живые структуры в потоке владельца; JNI SQL-команды ждут deferred completion во frontend.
 * Runtime getters используют instance_lite_call, включая inline вызов из native event callback.
 * Команды публикуются через instance_lite_post с защитой времени жизни worker и освобождением данных при отмене.
 * JSON/getters могут ждать worker: frontend выполняет тяжёлые чтения на IO, не удерживая нужные worker блокировки.
 * Все char* JSON-результаты принадлежат caller (u_free); при ошибке возможны []/{}, при OOM также NULL.
 * channel_id — десятичная NUL-строка; conv_id — ID DM без dm:. Префикс dm: нужен только voice/video target.
 * reply_to в send/voice/photo/video для канала — обязательный указатель на 32 байта (нули = без ответа).
 * request_id коррелирует локальный commit канала/PM с CHAT_EVT_MSG_SUBMITTED; постановка в очередь не означает доставку.
 * Аргументы строк/PCM заимствованы на вызов, асинхронные команды копируют их; сроки жизни файлов описаны ниже.
 * JNI-логи передают обычный UTF-8 как byte[] в LogManager.onNativeLog(int, byte[], byte[]), а не через NewStringUTF.
 * Каждый вызов удерживает свой local ref callback; замена/очистка global ref защищена mutex, пересылка C callback — atomic.
 * Ошибки доставки/рекурсия идут прямо в logcat без debug hook. Свои Java-исключения снимаются, чужое pending exception сохраняется.
 */
#ifndef ANDROID_JNI_BRIDGE_H
#define ANDROID_JNI_BRIDGE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../../lib/db_async.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Callback types (C → Kotlin) ── */

/* Вызывающий native-поток; category/message — NUL-строки UTF-8, заимствованные на вызов.
 * level: TRACE=0, DEBUG=1, INFO=2, WARN=3, ERROR=4. Пересылка в JNI синхронна; отключение не ждёт уже начатый callback. */
typedef void (*utun_log_fn)(int level, const char* category, const char* message);

/* Отдельный C JSON-callback, вызываемый utun_bridge_event; это не бинарный CHAT_EVT_* callback JNI.
 * Рабочие события instance_lite передаются instance_lite_event_fn / Kotlin onEvent(type, ByteArray?). */
typedef void (*utun_event_fn)(int event_type, const char* json_data);

struct UTUN_INSTANCE;
enum utun_bridge_member_query_kind { UTUN_BRIDGE_OWNER, UTUN_BRIDGE_MEMBERSHIP, UTUN_BRIDGE_LOCAL_NICK };
/* Успех: value=0/1 для owner/membership, 1 для сохранённого nickname; ошибка: value=-1.
 * result заимствован до возврата. Callback в uasync ровно один раз, включая OOM/FULL/close. */
typedef void (*utun_bridge_member_done_fn)(void* arg, const struct db_async_result* result, int value);
/* Uasync: SQL-чтение прав/членства либо WRITE локального nickname через общий worker.
 * Для LOCAL_NICK обязателен ожидаемый Ed25519 из карточки. channel/node/ed/nickname копируются до возврата; nickname=NULL/"" удаляет локальное имя.
 * node обязателен для LOCAL_NICK. MEMBERSHIP ищет локальный публичный ключ в таблице канала. done обязателен, arg/inst живут до done/DB drain.
 * Stop/restart отменяет queued работу и поздний результат; начатая WRITE ещё может завершить commit.
 * READONLY backend временно сохраняет прежнего писателя; failed RW никогда не возвращается к нему. */
void utun_bridge_member_query(struct UTUN_INSTANCE* inst, enum utun_bridge_member_query_kind kind,
    const char* channel, const uint8_t* ed, const char* nickname, utun_bridge_member_done_fn done, void* arg);

enum utun_bridge_json_kind { UTUN_BRIDGE_JSON_CHANNELS, UTUN_BRIDGE_JSON_MESSAGES, UTUN_BRIDGE_JSON_MEMBERS, UTUN_BRIDGE_JSON_DETAIL };
struct utun_bridge_json_query {
    enum utun_bridge_json_kind kind; /* Формат JSON совпадает с соответствующим native getter. */
    const char* channel; /* Обязателен кроме CHANNELS; канонический ID группы. Копируется. */
    const uint8_t* peer; /* Ed25519 участника для DETAIL, 32 байта; копируется до возврата. */
    int limit, exact; /* MESSAGES: limit<=0 даёт50; exact+anchor выбирает только указанное сообщение. */
    const uint8_t* anchor; /* NULL либо32 байта ID сообщения; копируется до возврата. */
};
/* Uasync, ровно один callback, включая синхронный отказ FULL/OOM/close. result заимствован до возврата.
 * Успех: json — собственность callback/caller (u_free). Ошибка/stop/restart: json=NULL, без частичного снимка. */
typedef void (*utun_bridge_json_done_fn)(void* arg, const struct db_async_result* result, char* json);
/* Uasync: READ SQL+JSON через общий worker с копиями self/base/online IDs, без runtime в worker.
 * query и строки не нужны после возврата; done обязателен, arg/inst живут до done/DB drain.
 * RAM fixtures без worker разрешены; файлового SQL fallback в uasync нет. */
void utun_bridge_json_read(struct UTUN_INSTANCE* inst, const struct utun_bridge_json_query* query,
    utun_bridge_json_done_fn done, void* arg);

/* Дополнительный callback-конфиг обёртки; основной конфиг ядра передаётся INI в start. */
typedef const char* (*utun_config_string_fn)(const char* key);
typedef int64_t      (*utun_config_int64_fn)(const char* key);
typedef int          (*utun_config_int_fn)(const char* key);

struct utun_bridge_config {
    utun_config_string_fn get_string; /* NUL-строка, NULL → default; результат не копируется wrapper. */
    utun_config_int64_fn  get_int64;  /* NULL → default, установленный callback сам определяет отсутствие ключа. */
    utun_config_int_fn    get_int;    /* То же для int. Функции и их окружение живут до destroy. */
};

/* Сохранить callbacks, установить debug hook, инициализировать voice_recorder(db_path).
 * Таблицы callbacks копируются, config=NULL отключает прежний config-provider; функции/окружение остаются у caller.
 * control_port только логируется: listener и ядро здесь не запускаются. true — recorder инициализирован,
 * false — его ошибка с логом; start нужен отдельно. Регистрация callbacks сохраняется для повторного init. */

bool utun_bridge_init(const char* db_path, int control_port,
                       utun_log_fn on_log,
                       utun_event_fn on_event,
                       const struct utun_bridge_config* config);

/* INI копируется в instance_lite: 0 — поток создан/уже существует, -1 — отказ.
 * Готовность сервисов — CHAT_EVT_SERVICE_STARTED, а не возврат start. */
int utun_bridge_start(const char* config_text);

/* После полного stop, без параллельного start: удалить chats.db и все его журналы из db_path.
 * db_path — непустой каталог. 0 — очищено; отрицательный errno — ошибка. Живой worker даёт -1.
 * При отказе запуск запрещён до успешного повтора; настройки/ключи не удаляются. */
int utun_bridge_clear_database(const char* db_path);

/* Полностью остановить/join worker, освободить recorder и снять C log/config callbacks.
 * Config-provider сбрасывается к defaults; повторный init начинает новую регистрацию.
 * Перед вызовом остановить внешние media workers/DSP, ждать вне UI; контракт ожидания — instance_lite.h.
 * 0 — очистка завершена; -1 — stop/join отклонён, recorder и callbacks сохранены, новый init запрещён.
 * JNI nativeStop дополнительно останавливает UDP-лог, nativeDestroy освобождает JNI global refs. */
int utun_bridge_destroy(void);

/* ── Actions (Kotlin → C) ── */

/* text — NUL-строка UTF-8; 0 — запрос поставлен в uasync, -1 — отказ подготовки. */
/* Канальные send/start ниже копируют reply_quote до возврата: NULL/пусто — обычный ответ, иначе UTF-8 <=4000 байт. */
int utun_bridge_send_message(const char* channel_id, const char* text, const uint8_t reply_to[32], uint64_t request_id,
                             const char* reply_quote);
/* Создать канал по name; переданный channel_id игнорируется, ID генерирует ядро. Результат — CHAT_EVT_CHANNEL_UPDATED. */
void utun_bridge_create_channel(const char* name, const char* channel_id);
/* Заглушка: только пишет address/port/pubkey_hex в лог, соединение не создаёт. */
void utun_bridge_connect_node(const char* address, int port, const char* pubkey_hex);

/* Поставить запрос автоподключения канала; выбор пиров выполняет общий topo_group_connect. */
void utun_bridge_connect_channel(const char* channel_id);

/* Однократная попытка подключения к конкретному узлу канала (ручной "Connect"). */
void utun_bridge_connect_member_node(const char* channel_id, const uint8_t ed[32]);

/* Синхронные JSON-снимки в потоке ядра; память освобождать u_free.
 * [] — подтверждённый пустой снимок, NULL — ошибка SQL/памяти/неинициализированное ядро; частичного результата нет. */

char* utun_bridge_get_channels_json(void);
/* limit<=0 → 50; anchor=NULL — последние сообщения, иначе окно вокруг 32-байтового message_id.
 * exact!=0 с anchor выбирает только этот ID; порядок результата timestamp/message_id по возрастанию.
 * Зашифрованный файл доступен UI только после decrypt с валидными метаданными. */
char* utun_bridge_get_messages_json(const char* channel_id, int limit, const uint8_t* anchor, int exact);
/* Текущий node_id либо 0, если чат не инициализирован. */

/* Передать INI в instance_lite_restart: 0 — запрос принят, -1 — отказ с логом. Ожидания готовности здесь нет.
 * Событие CHAT_EVT_SERVICE_STARTED подтверждает запуск нового ядра; перед вызовом остановить media workers/DSP. */

int utun_bridge_restart(const char* config_text);

/* Копирует INI, ставит reload [server:*] в uasync (auto_sockets=android); 0 — принято, -1 — отказ с логом. */

int utun_bridge_update_sockets(const char* config_text);

/* Локальный ping uasync; is_responsive=1 после обработки последнего ping, 0 до первого/после stop.
 * Ожидания и таймаута здесь нет, доступность пиров не проверяется. */

void utun_bridge_ping(void);
int utun_bridge_is_responsive(void);

/* Voice recorder: start лениво делает init, фиксирует target канала/dm:conv_id, reply/request/generation.
 * Формат int16 mono 48kHz; feed count — число отсчётов. 0 — успех, -1 — отказ.
 * stop(out_duration_ms nullable) передаёт PCM/compressor фоновой подготовке, запись <500мс отбрасывает с 0.
 * is_active=0/1; preset=0..2; peak=(2*RMS+peak)/2 последнего raw feed. Подробности — voice_recorder.h. */

int utun_bridge_voice_start(const char* channel_id, const uint8_t reply_to[32], uint64_t request_id, uint64_t generation,
                            const char* reply_quote);
int  utun_bridge_voice_feed(const int16_t* samples, int count);
int  utun_bridge_voice_stop(int* out_duration_ms);
void utun_bridge_voice_cancel(void);
int  utun_bridge_voice_is_active(void);
void utun_bridge_voice_set_preset(int preset);
int  utun_bridge_voice_get_preset(void);
void utun_bridge_voice_set_compressor(int enabled);
int  utun_bridge_voice_get_compressor(void);
void utun_bridge_voice_set_compressor_config(int max_gain_db, int lookback_ms, int lookahead_ms,
                                              float rise_rate_per_sec, float target_level_db);
float utun_bridge_voice_get_peak_level(void);
/* Применить подтверждённый снимок NS: -40..-1 dB либо 0 для обхода без задержки.
 * Звонок/рация — между кадрами, запись — со следующего start; 0/-1 с логом. */
int utun_bridge_set_noise_suppression(int suppress_db);

/* Обычный cache-файл в канал: 0 — поставлено и передано владение временным source, -1 — файл остаётся caller.
 * Принятый source удаляется также при ошибке/отмене worker. db_path не используется; результат — MSG_SUBMITTED. */

int  utun_bridge_attachment_send(const char* channel_id, const char* file_path, const char* db_path,
                                 const uint8_t reply_to[32], uint64_t request_id, const char* reply_quote);
/* Скачать вложение: msg_id>0 — локальный SQLite row ID; 0 — запрос поставлен, -1 — отказ. */
int  utun_bridge_attachment_download(const char* channel_id, int64_t msg_id);

/* Готовый MP4 в канал/dm:conv_id, без транскода; db_path игнорируется.
 * 0 передаёт владение временным файлом задаче (удаление также при ошибке/отмене), -1 оставляет файл caller.
 * duration_ms=1..86400000, размеры=1..16384; generation применяется к DM. См. video_sender.h. */
int  utun_bridge_video_send(const char* channel_id, const char* file_path, const char* db_path,
                           int duration_ms, int width, int height, const uint8_t reply_to[32], uint64_t request_id, uint64_t generation,
                           const char* reply_quote);

/* Готовое изображение: MIME/размеры проверяет Android, JPEG/PNG в канал или dm:<conv> с generation.
 * 0 передаёт JPEG/PNG cache-файл задаче, -1 оставляет caller; дополнительные форматы — см. photo_sender.h. */
int  utun_bridge_photo_send(const char* channel_id, const char* file_path, const char* db_path,
                           int width, int height, const uint8_t reply_to[32], uint64_t request_id,
                           const char* content_type, uint64_t generation, const char* reply_quote);

/* Декодирование собственного OPUS-файла (не Ogg): open возвращает owned handle либо NULL.
 * Все три out обязательны: частота, число каналов, общее число int16 (включая interleaved каналы).
 * Один последовательный reader на handle; close освобождает decoder/FILE, NULL допустим. */

void* utun_bridge_voice_decode_open(const char* file_path,
                                     int* out_sample_rate, int* out_channels,
                                     int* out_total_samples);
/* buf вмещает max_samples int16, выдаются только целые 20мс кадры (rate/50*channels).
 * Возврат: число int16, 0 — EOF либо ёмкость меньше кадра, -1 — аргументы/ошибка чтения или decode.
 * Последний батч ограничивается оставшимися отсчётами: EOF возвращает 0 при следующих чтениях.
 * После ошибки чтения/decode закрыть handle; продолжение этого потока не поддерживается. */
int   utun_bridge_voice_decode_read(void* handle, int16_t* buf, int max_samples);
void  utun_bridge_voice_decode_close(void* handle);

/* Сигналинг ставится в uasync: ch_id — десятичный ID группы либо dm:conv_id, peer_node_id и call_id ненулевые.
 * Возврат 0 — копия запроса и очередь выделены и переданы живому worker; -1 — отказ без события.
 * Результат сигналинга — CHAT_EVT_CALL_*; accept/decline/hangup применяются к указанному call_id. */

int utun_bridge_call_start(const char* ch_id, const uint8_t peer[32], uint64_t call_id);
int utun_bridge_call_accept(uint64_t call_id);
int utun_bridge_call_decline(uint64_t call_id);
int utun_bridge_call_hangup(uint64_t call_id);

/* Аудио-движок звонка (call_audio.c, единый голосовой стек): PCM из AudioRecord →
 * Opus → peer (feed); принятый Opus → джиттер+декод+stretch → PCM (pull). */
/* Начать DSP после готовности ядра; call_audio уже привязан его worker. 0 = успех, -1 = ошибка. */
int  utun_bridge_call_audio_start(uint64_t call_id);
/* Capture worker: настоящий raw PCM, ровно 960 mono-сэмплов @48кГц; muted!=0 применяется после AEC/NS/AGC.
 * capture_pts_us — монотонная метка первого сэмпла либо INT64_MIN. 0 = поставлено в uasync, -1 = отказ/ошибка. */
int  utun_bridge_call_audio_feed(uint64_t call_id, const int16_t* pcm, int count, int muted, int64_t capture_pts_us);
/* Playback worker: int16 mono 48kHz, активный DSP выдаёт max_samples; 0 — нет движка/неверные аргументы. */
int  utun_bridge_call_audio_pull(uint64_t call_id, int16_t* out, int max_samples);
/* После остановки внешних feed/pull workers освободить DSP; привязку к instance снимает worker ядра. */
void utun_bridge_call_audio_stop(void);

/* Подписка — posted-команда; subscribers читает текущий счётчик синхронно (0 без instance). */

void utun_bridge_radio_set_active(uint64_t group_id, int on);
int  utun_bridge_radio_subscribers(uint64_t group_id);
/* В потоке ядра создать/привязать DSP и начать рацию: capture_channels=0 только RX, 1/2 RX+TX. 0/-1.
 * Один TX writer для feed/talk_begin/end и отдельный RX writer для pull; lifecycle сериализует frontend. */
int  utun_bridge_radio_audio_start(uint64_t group_id, int capture_channels);
/* Запустить TX/VAD и применить текущий NS-снимок без гонки с обновлением настроек; 0/-1. */
int utun_bridge_radio_capture_start(uint64_t group_id, int channels, int mode);
/* Raw PCM 48kHz int16 interleaved, count=960*channels (1/2); NS выполняется перед level/VAD/TX. См. radio_audio.h. */
int  utun_bridge_radio_audio_feed(uint64_t group_id, const int16_t* pcm, int count);
/* PCM 48kHz stereo, max_samples/возврат — число int16, а не кадров; см. radio_audio.h. */
int  utun_bridge_radio_audio_pull(uint64_t group_id, int16_t* out, int max_samples);
/* Остановить после завершения внешних feed/pull callers. */
void utun_bridge_radio_audio_stop(void);
/* RADIO_AUTO_OFF/VAD/LEVEL; ручные talk_begin/end управляют PTT текущей группы. */
int  utun_bridge_radio_auto_mode(void);
void utun_bridge_radio_talk_begin(uint64_t group_id);
void utun_bridge_radio_talk_end(uint64_t group_id);

/* ── Invite link (for sharing) — асинхронно через uasync-поток.
       Результат приходит событиями CHAT_EVT_INVITE_CANDIDATES / CHAT_EVT_INVITE_LINK_READY. ── */
/* 0 — принято worker, -1 — ошибка аргументов/памяти/готовности, события не будет. */
int utun_bridge_request_invite_candidates(const char* channel_id);
int utun_bridge_request_invite_link(const char* channel_id, const uint8_t target[32], uint64_t request_id);

/* Применить уровень синхронно: пустая/NULL category → общий уровень, иначе имя из debug_config. */

void utun_bridge_set_debug_level(const char* category, const char* level);

/* C API: set синхронно вызывает chat_core_set_setting, только из uasync; get читает с fallback def.
 * JNI nativeSetChatSetting отдельно копирует запрос и ставит trampoline в uasync. */

void utun_bridge_chat_setting_set(const char* name, const char* value);
int  utun_bridge_chat_setting_get_int(const char* name, int def);

/* ── Channel members ── */

/* Снимок БД, поле on здесь всегда 0; актуальное состояние — отдельное событие CHAT_EVT_MEMBER_LIST. */
char* utun_bridge_get_channel_members_json(const char* channel_id);
/* Данные БД (адреса/инвайтер); live-достижимость получать отдельным CHAT_EVT_MEMBER_LIST. */
char* utun_bridge_get_member_detail_json(const char* channel_id, const uint8_t ed[32]);
/* Синхронная локальная правка SQLite: пустой/NULL nick сбрасывает псевдоним. В сеть не распространяется. */
void  utun_bridge_set_member_nick(const char* channel_id, const uint8_t ed[32], const char* nick);
/* Асинхронный toggle локального mute участника рации: 0 — поставлено, -1 — отказ. */
int   utun_bridge_toggle_member_radio_mute(const char* channel_id, const uint8_t ed[32]);
/* 1 — в локальной БД есть приватный ключ канала, 0 — нет/ошибка; не общий тест всех административных прав. */
int   utun_bridge_is_channel_owner(const char* channel_id);
/* Поставить подписанную правку всех шести тегов; проверку полномочий выполняет chat_member_tags_commit. */
void  utun_bridge_set_member_flags(const char* channel_id, const uint8_t ed[32],
                                    int admin, int supernode, int moder, int storage,
                                    int verified, int deleted);

/* Синхронный обход живых ETCP links, {} без соединения. Не вызывать параллельно изменениям списка. */
char* utun_bridge_get_member_links_json(const uint8_t ed[32]);

/* Асинхронно передать канальный приватный ключ (права админа) другому узлу. */
void utun_bridge_transfer_admin(const char* channel_id, const char* ed_hex);

/* Поставить локальное удаление канала (таблицы, соединения, медиа, db_sync); удаление не распространяется по сети. */
void utun_bridge_delete_channel(const char* channel_id);

/* Политика звука канала: get синхронный (fallback 1), set ставится в uasync и нормализует on к 0/1. */

int  utun_bridge_get_sound_on_message(const char* channel_id);
void utun_bridge_set_sound_on_message(const char* channel_id, int on);

/* Синхронный обход живых сокетов/links для UI auto-mode; probe_only скрыты, [] без готового чата. */

char* utun_bridge_get_local_sockets_json(void);

/* Делегировать auto_socket_on_network_change: режим android игнорирует вызов (нужен update_sockets),
 * в других режимах auto_socket ставит scan в uasync. */

void utun_bridge_on_network_change(void);

/* Поставить запрос client_activity в uasync: active!=0 — ACTIVE, 0 — STANDBY.
 * 0 — команда поставлена живому worker под защитой его владельца; -1 — отказ с диагностикой.
 * При stop ожидающая команда может отмениться. Возврат не подтверждает применение; ядро учитывает удержание трансляций,
 * ошибки перехода логирует trampoline в потоке ядра. */

int utun_bridge_set_active(int active);

/* Состояние SIM: state=0 UNKNOWN/1 IDLE/2 BUSY, policy — маска call_policy (0..31).
 * Frontend: 0 — копия данных поставлена живому worker под защитой владельца, -1 — отказ с диагностикой.
 * Применение асинхронно; stop/restart может отменить команду без утечки. После SERVICE_STARTED отправить текущее состояние снова. */
int utun_bridge_set_phone_state(int state, unsigned policy);

/* Поставить уведомление standby в uasync; досрочный wake только в SLEEP после min_sleep. */

void utun_bridge_notify_network_active(void);

/* Асинхронные снимки через CHAT_EVT_CONN_LIST/CHAT_EVT_CONN_METRICS. */

void utun_bridge_collect_conn_list(void);
void utun_bridge_collect_conn_metrics(uint64_t peer_node_id);

/* ── DM (личные сообщения) ──
 * dm_start/dm_send выполняются асинхронно через uasync (трамплины dm_core).
 * Результат dm_start — CHAT_EVT_DM_CONV_UPDATED; source_ch_id/peer_name могут быть NULL.
 * generation в send/history/file относится к поколению беседы, не к instance. */
int utun_bridge_dm_start(const uint8_t peer_ed[32], const char* source_ch_id, const char* peer_name); /* 0 — принято, -1 — отказ без события. */
/* Frontend/JNI: дождаться dm_start_submit и вернуть фактический ID;0=ошибка/отмена.
 * Ожидание происходит в вызывающем потоке; на RW backend SQL выполняется DB worker, uasync не ждёт.
 * groups=0 — DEVICE,1 — GROUPS; строки заимствованы до возврата. Из uasync вызывать запрещено.
 * Kotlin-контракт синхронного nativeDmStart сохраняется; CONV_UPDATED выдаёт ядро после commit. */
uint64_t utun_bridge_dm_start_wait(const uint8_t peer_ed[32], const char* source_ch_id, const char* peer_name, int groups);
/* Копирует 1..DM_DATA_MAX UTF-8 байт без NUL в очередь; generation/request_id ненулевые.
 * 0 — запрос принят, результат локальной записи придёт CHAT_EVT_MSG_SUBMITTED; -1 — отказ без события. */
int utun_bridge_dm_send(const char* conv_id, const uint8_t* text, size_t length, uint64_t generation, uint64_t request_id);
/* Запросить CHAT_EVT_DM_CONVERSATIONS. */
void utun_bridge_dm_request_conversations(void);
/* Снимок видимой истории указанного поколения; запоздалый запрос отбрасывается в uasync. */
void utun_bridge_dm_request_messages(const char* conv_id, uint64_t generation);
/* 0 — принято и передано владение временным path (удаляется задачей), -1 — отказ, файл остаётся caller.
 * name — отображаемое имя, строки копируются. Поколение беседы проверяется в uasync.
 * Ненулевой request_id получает CHAT_EVT_MSG_SUBMITTED после commit либо ошибки; generation также ненулевой. */
int utun_bridge_dm_send_file(const char* conv_id, const char* path, const char* name, uint64_t generation, uint64_t request_id);

#ifdef __cplusplus
}
#endif
#endif /* ANDROID_JNI_BRIDGE_H */
