/*
 * chat_event — доставка событий ядра чата интерфейсу или headless-клиенту.
 *
 * Производители (chat_core, chat_sync, ...) вызывают chat_event_post().
 * На экземпляр регистрируется один обработчик через chat_event_set_handler().
 * post вызывает его сразу, в том же uasync-потоке; очереди событий здесь нет.
 * data заимствован только до возврата обработчика. Для передачи в другой поток нужна копия.
 *
 * Без обработчика — DEBUG_DEBUG + log_dump; LOCAL_INVITE/INVITE_LINK_READY с секретами не дампятся.
 * Обработчик может удалить канал; stop/destroy сервиса выполняются вне callbacks.
 * Все вызовы — из uasync-потока.
 */

#ifndef CHAT_EVENT_H
#define CHAT_EVENT_H

#include <stdint.h>
#include <stddef.h>

struct UTUN_INSTANCE;

#ifdef __cplusplus
extern "C" {
#endif

// Типы событий (совпадают с GUI_EVT_* в gui_bridge.h для прямой совместимости)
#define CHAT_EVT_MSG_RECEIVED        1   /* [ch_id_len:1][ch_id:var][author_ed25519:32][initial_sync:1] */
#define CHAT_MSG_EVENT_SUFFIX_SIZE   33  /* Полный ключ и признак начальной синхронизации. */
#define CHAT_EVT_CONNECT_RESULT      2   /* [peer X25519:32][result:4][group Ed25519:32][request_id:8], native */
#define CHAT_CONNECT_RESULT_SIZE 76
#define CHAT_CONNECT_RESULT_CODE_OFF 32
#define CHAT_CONNECT_RESULT_GROUP_OFF 36
#define CHAT_CONNECT_RESULT_REQUEST_OFF 68
#define CHAT_EVT_CHANNEL_UPDATED     4   /* [ch_id_len:1][ch_id:var] */
#define CHAT_EVT_MEMBERS_CHANGED     5   /* [ch_id_len:1][ch_id:var] */
#define CHAT_EVT_AUTO_CONNECT_STATUS 7   /* [status:1][total_tried:2][node_count:2][success:2] */
#define CHAT_EVT_CHANNEL_PEERS_ONLINE 8  /* [ch_id_len:1][ch_id:var][online_count:2] */
#define CHAT_EVT_DB_READY            9   /* нет data;готов chat_core,последующие части chat_service_start ещё могут отказать */
#define CHAT_EVT_STATUS_REFRESH     10   /* data: status text */
#define CHAT_EVT_KEYS_GENERATED     11   /* [pub_hex:64] */
#define CHAT_EVT_SERVICE_STARTED    12   /* data: none */
#define CHAT_EVT_SERVICE_STOPPED    13   /* data: none */
#define CHAT_EVT_ATTACHMENT_DOWNLOADED 14  /* [ch_id_len:1][ch_id:var][msg_id:8] */
#define CHAT_EVT_DOWNLOAD_PROGRESS    15  /* [ch_id_len:1][ch_id:var][msg_id:8][blocks_done:4][num_blocks:4] */
#define CHAT_EVT_NODEINFO_UPDATED     16  /* без payload; состояние участников приходит в снимках группы */
#define CHAT_EVT_LINK_STATUS_CHANGED  17  /* [peer Ed25519:32][link_id:1][state:1][status:1][links_up:1][initialized:1] */
#define CHAT_EVT_INVITE_RECEIVED      18  /* [key_hex_len:1][Ed25519 группы в HEX][name_len:1][name][inviter X25519:32][inviter_name_len:1][inviter_name] */
#define CHAT_EVT_LOCAL_SOCKETS       19  /* JSON: [{"si":id,"nm":"...","ad":"...","po":port,"pt":"TCP|UDP","st":0|1|2,"lk":n},...] */
#define CHAT_EVT_CONN_LIST          20  /* [count:2][entry:46B]* — список ETCP-соединений */
#define CHAT_EVT_CONN_METRICS       21  /* текст — полный дамп метрик выбранного соединения */
#define CHAT_EVT_MEMBER_DETAIL      22  /* Адресованный ответ диагностики; wire описан в chat_member.h. */
#define CHAT_EVT_MEMBER_LIST        23  /* [ch_id_len:1][ch_id:var][count:2][member:CHAT_MEMBER_DISPLAY_SIZE]* */
#define CHAT_EVT_MEMBER_UPDATED     24  /* [ch_id_len:1][ch_id:var][count:1][member:CHAT_MEMBER_DISPLAY_SIZE] */
#define CHAT_EVT_MEMBER_REMOVED     25  /* [ch_id_len:1][ch_id:var][Ed25519:32] — мембер удалён из канала */
#define CHAT_EVT_CONNECTING_NODES   27  /* [ch_id_len:1][ch_id:var][count:2][Ed25519:32]* */
#define CHAT_EVT_ADMIN_KEY_RECEIVED 28  /* [ch_id_len:1][ch_id:var][from_ed:32] — получен канальный ключ (стал совладельцем) */
#define CHAT_EVT_INVITE_LINK_READY  29  /* [ch_id_len:1][ch_id:var][request_id:8][link_len:2][link:var] — пустой link = ошибка */
#define CHAT_EVT_INVITE_CANDIDATES  30  /* [ch_id_len:1][ch_id:var][auto_ed25519:32][count:2LE][ed25519:32]* — достижимые узлы для invite */
#define CHAT_EVT_CHANNEL_DELETED    31  /* [ch_id_len:1][ch_id:var] — канал удалён локально */
#define CHAT_EVT_DM_MSG_RECEIVED    32  /* [conv_id_len:1][conv_id:var][author Ed25519:32] */
#define CHAT_EVT_DM_CONV_UPDATED    33  /* [conv_id_len:1][conv_id:var][generation:8 native] — беседа создана/изменена */
#define CHAT_EVT_CALL_INCOMING      34  /* [ch_id_len:1][ch_id:var][caller Ed25519:32][call_id:8] */
#define CHAT_EVT_CALL_RINGING       35  /* [call_id:8][reason:1=0][peer Ed25519:32], окончательно выбранный собеседник */
#define CHAT_EVT_CALL_ACCEPTED      36  /* [call_id:8][reason:1=0][peer Ed25519:32], окончательно выбранный собеседник */
#define CHAT_EVT_CALL_ENDED         37  /* [call_id:8][reason:1] */
#define CHAT_EVT_CALL_DECLINED      38  /* [call_id:8][reason:1] */
#define CHAT_EVT_CALL_STATS         39  /* [call_id:8][rtt:2][buffer:2][tempo:2][dropped:2][underruns:2][min:2][max:2][reserve:2] */
#define CHAT_EVT_CALL_ERROR         40  /* [call_id:8][err:1] */
#define CHAT_EVT_DM_MESSAGES        41  /* [conv_id_len:1][conv_id:var][generation:8][JSON:utf8] — история указанного поколения */
#define CHAT_EVT_CALL_PATH          42  /* [call_id:8][text:var] — BGP-маршрут до пира звонка */
#define CHAT_EVT_RADIO_TALK         43  /* [group Ed25519:32][source Ed25519:32][stream_id:2][on:1] — кто-то начал/закончил говорить */
#define CHAT_EVT_CALL_CONNECTION    44  /* [call_id:8][CONN_TYPE:1][next_hop Ed25519:32] — текущий путь отправки звонка */
#define CHAT_CALL_CONNECTION_SIZE 41
#define CHAT_EVT_DM_CONVERSATIONS   45  /* [JSON:utf8] — последние 200 личных бесед */
#define CHAT_EVT_ATTACHMENT_PREPARATION 46 /* [target_len:1][target:var][job_uuid:16][state:1], target=channel либо "dm:"+conv;0/1/2=start/ready/failed */

#define CHAT_EVT_DM_CALL_ROUTE 47 /* [conv_id_len:1][conv_id:var][peer Ed25519:32][group Ed25519:32], нули = нет BGP-пути */
#define CHAT_EVT_DM_INVITE_RESULT 48 /* [request_id:8][result:4][peer Ed25519:32][conv_id:8][generation:8], dm_invite_result */
#define CHAT_EVT_MEMBER_FILES_RESPONSE 49 /* [group Ed25519:32][server Ed25519:32][mf wire response] */
#define CHAT_EVT_MEMBER_FILES_TRANSFER 50 /* id:8LE,group/server/owner:32,done/total:8LE,status:2LE,state:1 */
#define CHAT_AVATAR_RESULT_STATUS 40
#define CHAT_AVATAR_RESULT_SIZE (CHAT_AVATAR_RESULT_STATUS + 1)
#define CHAT_EVT_AVATAR_RESULT 51 /* request_id:8LE, group Ed25519:32, failed:1; результат публикации ссылки профиля */
#define CHAT_EVT_GROUP_AVATAR_RESULT 52 /* request_id:8LE, group Ed25519:32, failed:1; результат публикации аватарки группы */
#define CHAT_EVT_FEEDBACK_CHANGED 53 /* [channel_len:1][channel][message_id:32][kind:1] — состояние после commit */
#define CHAT_EVT_FEEDBACK_RESULT 54 /* [request_id:8LE][success:1][JSON] — результат команды интерфейса */
#define CHAT_EVT_RADIO_MUTE_RESULT 55 /* [group Ed25519:32][member Ed25519:32][success:1] — локальный переключатель рации */
#define CHAT_EVT_MSG_SUBMITTED 56 /* [request_id:8LE][result:1]: 0=отказ,1=commit,2=превышен лимит JSON. */
#define CHAT_EVT_CPU_LOAD 57 /* [valid:1][average_percent:1] — CPU потока uasync, среднее за секунду, 0..100 */
#define CHAT_EVT_STREAM_SOURCES 58 /* [group Ed25519:32][valid:1][JSON:utf8] — доступные публикации выбранного канала */
#define CHAT_EVT_DM_DELETE_RESULT 59 /* [request:8LE][conv:8LE][generation:8LE][success:1] — после транзакции */
#define CHAT_EVT_ATTACHMENT_KEYS_CHANGED 60 /* [channel_len:1][channel][success?:1], без секретов; success — результат команды GUI */

#define CHAT_EVT_LOCAL_DEVICES 61 /* [count:2BE][address:33,name_len:1,name:UTF8]*; адрес в chat_local.h */
#define CHAT_EVT_LOCAL_INVITE 62 /* [address:33][name_len:1][name:UTF8][invite_link:UTF8], без NUL */
#define CHAT_EVT_LOCAL_DELIVERY 63 /* [request:8BE][address:33][status:1], local_delivery_status */
#define CHAT_EVT_LOCAL_DISCOVERY_ACTIVE 64 /* [active:1], Android MulticastLock */
#define CHAT_RADIO_CALL_PEER 40
#define CHAT_RADIO_CALL_STATE 72
#define CHAT_RADIO_CALL_EVENT_SIZE 75
#define CHAT_EVT_RADIO_CALL 65 /* [id:8LE][group Ed25519:32][peer Ed25519:32][state:1][reason:1][incoming:1] */
#define CHAT_EVT_CALL_PHONE_BUSY 66 /* [id:8LE][reserved:1]; вызывающий выбирает продолжить либо завершить */
#define CHAT_EVT_MSG_UPDATED 67 /* [channel_len:1][channel][message_id:32] — правка после commit, без unread/звука */
#define CHAT_EVT_GROUP_AVATAR_SNAPSHOT 68 /* Только Android JNI: id:8LE, group Ed25519:32, version:8LE, hash:32, can_edit/failed:1, size:2LE, image:size. */
#define CHAT_EVT_DEVGROUP_RESULT 71 /* request_id LE8 + success1 + JSON; может содержать PIN, не логировать */
#define CHAT_EVT_DEVGROUP_CHANGED 70 /* Обновление nodes/devgroup: перезагрузить снимки участников. */
#define CHAT_EVT_NETWORK_STATS 69 /* Native POD снимок ETCP-подключений и линков, chat_network_stats.h. */
#define CHAT_EVT_DM_START_RESULT 72 /* request/conv/generation:8 native + success:1; только результат конкретного запроса UI. */
#define CHAT_EVT_DM_MEMBERS 73 /* conv_len:1,conv,generation:LE8,count:LE2,(side:1,member:wire260)*count. */
#define CHAT_EVT_GROUP_POLICY_RESULT 74 /* request:8LE + group Ed25519:32 + JSON: preview или результат смены прав приглашения. */
#define CHAT_EVT_STARTUP_FAILED 75 /* kind:1, directory_len:2LE, directory:UTF8, reason:UTF8. */
enum chat_startup_failure_kind { CHAT_STARTUP_ERROR, CHAT_STARTUP_DATABASE, CHAT_STARTUP_INVALID_KEY, CHAT_STARTUP_KEY_ACCESS };
#define CHAT_EVT_GROUP_INFO_RESULT 76 /* request:8LE + group Ed25519:32 + JSON; снимок либо результат переименования группы. */
#define CHAT_EVT_CLIPBOARD_RECEIVED 77 /* id16,group_ed32,peer_ed32,type1,scope1,data; frontend подтверждает применение. */
#define CHAT_EVT_KANBAN_CHANGED 79 /* group Ed25519:32, reload board after commit. */
#define CHAT_EVT_KANBAN_RESULT 80 /* request:8LE + group Ed25519:32 + JSON: command result or snapshot. */
#define CHAT_EVT_CLIPBOARD_RESULT 78 /* request8LE,state1,total/delivered/unavailable/failed:4LE; chat_clipboard_result. */

// ── Подключение получателя событий ──

typedef void (*chat_event_handler_fn)(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len);

// Заменить обработчик; NULL отключает доставку. Предыдущий обработчик не вызывается.
void chat_event_set_handler(struct UTUN_INSTANCE* inst, chat_event_handler_fn handler);

// ── Отправка событий из ядра ──

// Синхронно передать type>0 и len>=0 байт data; inst обязателен,data может быть NULL только при len=0.
// Неверные аргументы логируются без доставки;формат CHAT_EVT_* проверяет получатель,память остаётся отправителю.
void chat_event_post(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len);
/* Уведомить о новом сообщении. author=NULL означает обновление истории без одного автора. */
void chat_event_message(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t author[32], int initial);

// Сообщить этап подготовки: state=0 начало,1 готово,2 ошибка; target — ID без префикса, is_dm добавляет "dm:".
// target/id обязательны, копируются в синхронный payload; неверный state/переполнение строки только логируется.
void chat_event_attachment(struct UTUN_INSTANCE* inst, int is_dm, const char* target, const uint8_t id[16], int state);

// request_id=0 отключает подтверждение; вызывается один раз после сохранения либо ошибки подготовки.
void chat_event_submitted(struct UTUN_INSTANCE* inst, uint64_t request_id, int result); /* result: 0/1/2, как в MSG_SUBMITTED. */

#ifdef __cplusplus
}
#endif
#endif /* CHAT_EVENT_H */
