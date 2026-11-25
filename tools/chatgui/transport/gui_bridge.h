/* Мост одного desktop GUI ↔ ядра: post копирует бинарное событие в Qt-очередь, callbacks работают в GUI.
 * Инициализация/регистрация callbacks принадлежат GUI; NULL снимает callback. Данные callbacks
 * заимствованы на время вызова, строки не обязательно NUL-terminated, если передана длина.
 * post_uasync_fn передаёт заимствованный arg без копирования; owned_fn принимает отдельный u_malloc-блок.
 * Перед destroy ядра его поток вызывает set_inst(NULL) вне poll/callbacks: закрывает приём команд,
 * отменяет ожидающие post и освобождает переданные мосту аргументы. Затем можно снять и уничтожить uasync.
 * Полученный inst заимствован: само чтение указателя не гарантирует время жизни при stop/finalize.
 * Сырые callbacks (data,len) не заменяют проверку формата в получателе соответствующего codec. */
#ifndef GUI_BRIDGE_H
#define GUI_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include "../../../src/chat/chat_member.h"
#include "../../../src/chat/chat_event.h"

struct UASYNC;
struct TOPO_GROUP;
struct TOPO_GROUP_NODE;
struct UTUN_INSTANCE;
struct sqlite3;

/* ── Типы уведомлений uasync→GUI (fire-and-forget) ── */

/* Коды/payload определяет chat_event.h; GUI-алиасы не дублируют числовую таблицу ядра. */
#define GUI_EVT_MSG_RECEIVED CHAT_EVT_MSG_RECEIVED
#define GUI_EVT_CONNECT_RESULT CHAT_EVT_CONNECT_RESULT
#define GUI_EVT_CHANNEL_UPDATED CHAT_EVT_CHANNEL_UPDATED
#define GUI_EVT_MEMBERS_CHANGED CHAT_EVT_MEMBERS_CHANGED
#define GUI_EVT_AUTO_CONNECT_STATUS CHAT_EVT_AUTO_CONNECT_STATUS
#define GUI_EVT_CHANNEL_PEERS_ONLINE CHAT_EVT_CHANNEL_PEERS_ONLINE
#define GUI_EVT_DB_READY CHAT_EVT_DB_READY
#define GUI_EVT_STATUS_REFRESH CHAT_EVT_STATUS_REFRESH
#define GUI_EVT_KEYS_GENERATED CHAT_EVT_KEYS_GENERATED
#define GUI_EVT_ATTACHMENT_DOWNLOADED CHAT_EVT_ATTACHMENT_DOWNLOADED
#define GUI_EVT_DOWNLOAD_PROGRESS CHAT_EVT_DOWNLOAD_PROGRESS
#define GUI_EVT_NODEINFO_UPDATE CHAT_EVT_NODEINFO_UPDATED
#define GUI_EVT_CONN_LIST CHAT_EVT_CONN_LIST
#define GUI_EVT_CONN_METRICS CHAT_EVT_CONN_METRICS
#define GUI_EVT_MEMBER_DETAIL CHAT_EVT_MEMBER_DETAIL
#define GUI_EVT_MEMBER_LIST CHAT_EVT_MEMBER_LIST
#define GUI_EVT_MEMBER_UPDATED CHAT_EVT_MEMBER_UPDATED
#define GUI_EVT_MEMBER_REMOVED CHAT_EVT_MEMBER_REMOVED
#define GUI_EVT_ADMIN_KEY_RECEIVED CHAT_EVT_ADMIN_KEY_RECEIVED
#define GUI_EVT_INVITE_LINK_READY CHAT_EVT_INVITE_LINK_READY
#define GUI_EVT_INVITE_CANDIDATES CHAT_EVT_INVITE_CANDIDATES
#define GUI_EVT_DM_MSG_RECEIVED CHAT_EVT_DM_MSG_RECEIVED
#define GUI_EVT_DM_CONV_UPDATED CHAT_EVT_DM_CONV_UPDATED
#define GUI_EVT_CALL_INCOMING CHAT_EVT_CALL_INCOMING
#define GUI_EVT_CALL_RINGING CHAT_EVT_CALL_RINGING
#define GUI_EVT_CALL_ACCEPTED CHAT_EVT_CALL_ACCEPTED
#define GUI_EVT_CALL_ENDED CHAT_EVT_CALL_ENDED
#define GUI_EVT_CALL_DECLINED CHAT_EVT_CALL_DECLINED
#define GUI_EVT_CALL_STATS CHAT_EVT_CALL_STATS
#define GUI_EVT_CALL_ERROR CHAT_EVT_CALL_ERROR
#define GUI_EVT_DM_MESSAGES CHAT_EVT_DM_MESSAGES
#define GUI_EVT_CALL_PATH CHAT_EVT_CALL_PATH
#define GUI_EVT_RADIO_TALK CHAT_EVT_RADIO_TALK
#define GUI_EVT_CALL_CONNECTION CHAT_EVT_CALL_CONNECTION

/* ── API ── */

/* Идемпотентная инициализация из GUI после создания QCoreApplication, до запуска worker. Она владеет приёмником;
 * все producers должны завершиться до её уничтожения. Повторный init сохраняет очередь и callbacks. */
void gui_bridge_init(void);

/* Установить uasync перед публикацией instance; снять после set_inst(NULL) и завершения worker.
 * Замена uasync при опубликованном instance отклоняется с ERROR. uasync мосту не принадлежит. */
void gui_bridge_set_uasync(struct UASYNC* ua);

/* Опубликовать готовый instance из uasync после set_uasync. NULL снимается в uasync до destroy,
 * вне callbacks: синхронно отменяет ожидающие команды и освобождает аргументы owned-запросов.
 * Начатый callback завершается до остановки poll; отмена post_uasync_* не вызывает fn.
 * Специализированные API (member_edit.h, member_files_*) доставляют результат отмены в GUI. */
void gui_bridge_set_inst(struct UTUN_INSTANCE* inst);
/* GUI: зарегистрировать собственный READONLY handle только как идентификатор текущего профиля.
 * Worker его не использует для SQL; NULL снимает reader перед его закрытием в GUI. */
void gui_bridge_set_reader(struct sqlite3* reader);

/* Текущий заимствованный UTUN_INSTANCE (NULL пока не создан/после сброса моста). */
struct UTUN_INSTANCE* gui_bridge_get_inst(void);

/* uasync → GUI: синхронно скопировать data_len байт и поставить событие в Qt::QueuedConnection.
 * len>=0, при len>0 data обязателен; (NULL,0) — пустое событие. Ошибки аргументов/доставки логируются. */
void gui_bridge_post(int event_type, const uint8_t* data, int data_len);

/* GUI → uasync: передать fn(arg) без владения arg. Нужны опубликованные ua/instance.
 * При отказе/остановке fn не вызывается, arg остаётся владельцу; ошибка постановки логируется.
 * Владелец обеспечивает жизнь arg до callback/отмены. Для malloc-команд использовать owned_fn. */
void gui_bridge_post_uasync_fn(void (*fn)(void*), void* arg);
/* Передать отдельный u_malloc/u_calloc-блок arg: 0 — мост принял владение, callback обязан освободить arg;
 * отмена до callback освобождает его мостом. -1 — arg остаётся вызывающему. Вложенные ресурсы не освобождаются. */
int gui_bridge_post_uasync_owned_fn(void (*fn)(void*), void* arg);
/* Один u_malloc-блок команды ядру; binding — адрес её поля inst внутри arg.
 * Проверка running, запись inst и enqueue выполняются под mutex. GUI не читает instance.
 * 0 передаёт владение: fn освобождает arg после исполнения, мост — при отмене.
 * -1 оставляет блок вызывающему для u_free; вложенные ресурсы этим API не поддерживаются. */
int gui_bridge_post_bound_fn(void (*fn)(void*), void* arg, struct UTUN_INSTANCE** binding);
/* Скачать вложение канала: полный Ed25519 группы в channel, msg_id>0.
 * Копия/привязка/владение принадлежат мосту. 0 — запрос принят, -1 — отказ с логом без heap-обязательств GUI. */
int gui_bridge_download_attachment(const char* channel, int64_t msg_id);

/* Команды обычного звонка: GUI не передаёт instance или heap-аргументы.
 * Под mutex моста копируют данные, привязываются к живому running-ядру и передают владение очереди.
 * 0 — принято, результат приходит штатными CHAT_EVT_CALL_*; -1 — отказ с логом, память не остаётся за вызывающим.
 * Detach до исполнения отменяет команду и освобождает запрос, не вызывая native API. */
enum gui_call_action { GUI_CALL_ACCEPT, GUI_CALL_DECLINE, GUI_CALL_HANGUP, GUI_CALL_CONTINUE };
int gui_bridge_call_start_devices(const char* channel, const uint8_t peer[32], uint64_t call_id);
int gui_bridge_call_start(const char* channel, const uint8_t peer[32], uint64_t call_id); // channel — Ed25519 группы или dm:<conv>. peer — Ed25519 участника, id ненулевой.
int gui_bridge_call_action(enum gui_call_action action, uint64_t call_id); // Только перечисленные действия, call_id ненулевой.

/* Приглашение в рацию: GUI передаёт значения без instance/heap-аргументов.
 * START требует ненулевые id/group/peer; остальные действия — ненулевой id и group=peer=0.
 * Под mutex привязывает копию к текущему running-ядру и передаёт владение очереди.
 * 0 — принято, результат приходит через CHAT_EVT_RADIO_CALL; -1 — отказ с логом без оставшейся памяти.
 * Detach до исполнения отменяет команду и освобождает её, не вызывая native API. */
enum gui_radio_call_action {
    GUI_RADIO_CALL_START, GUI_RADIO_CALL_READY, GUI_RADIO_CALL_ACCEPT, GUI_RADIO_CALL_DECLINE,
    GUI_RADIO_CALL_CANCEL, GUI_RADIO_CALL_FAIL, GUI_RADIO_CALL_REJECT_BUSY, GUI_RADIO_CALL_CONTINUE, GUI_RADIO_CALL_ANSWER, GUI_RADIO_CALL_START_DEVICES
};
int gui_bridge_radio_call(enum gui_radio_call_action action, uint64_t id, const uint8_t group[32], const uint8_t peer[32]);

/* Назначение пары key/value: локальный ui_state SQLite либо live chat_setting ядра. */
enum gui_setting_target { GUI_SETTING_UI_STATE, GUI_SETTING_CHAT };
/* Скопировать NUL-строки key/value в собственный запрос и поставить его текущему ядру под mutex моста.
 * key непустой; key+NUL+value+NUL должны помещаться в data соответствующего запроса ядра.
 * 0 — принято (не подтверждение записи/валидации ядром), -1 — отказ с логом, память за вызывающим не остаётся.
 * При отмене запрос освобождает мост, после исполнения — trampoline. INI-файл эта функция не меняет. */
int gui_bridge_post_setting(enum gui_setting_target target, const char* key, const char* value);
/* Скопировать name (включая пустое, в пределах update_my_name_arg::name) и поставить обновление текущему ядру.
 * Владение и результат — как у post_setting; строка/форма вызывающего после возврата больше не нужны. */
int gui_bridge_update_name(const char* name);

/* Composer-запрос: 0 — владение req передано, callback/отмена освободят блок; -1 — req остаётся вызывающему.
 * inst привязывается к опубликованному ядру под mutex; data должен жить вместе с req (обычно в том же u_malloc-блоке).
 * Ненулевой request_id получает MSG_SUBMITTED после сохранения либо отказ при отмене до исполнения.
 * При -1 результата нет: вызывающий освобождает req и сам сообщает об отказе своему composer. */
struct chat_msg_submit;
int gui_bridge_submit_message(struct chat_msg_submit* req);
/* PM-текст с request_id: привязка/владение/отмена и MSG_SUBMITTED как у канального composer. */
struct dm_send_req;
int gui_bridge_submit_dm_message(struct dm_send_req* req);
/* Подготовка вложения: тот же контракт результата, но отмена также освобождает PCM/compressor/временный source
 * и сообщает MSG_SUBMITTED(false). При -1 caller вызывает attachment_send_discard(req). */
struct attachment_send_req;
int gui_bridge_submit_attachment(struct attachment_send_req* req);

/* Проверить наличие опубликованных uasync и instance. Только подсказка готовности, не резервация их lifetime. */
int gui_bridge_is_uasync_ready(void);

/* Callback для результатов подключения (вызывается из GUI-потока) */
typedef void (*gui_connect_result_fn)(const uint8_t peer[32], const uint8_t group[32], int result, uint64_t request);
/* Скопировать invite-ссылку в JOIN port. request обязателен: при 0 записан ID будущего CONNECT_RESULT;
 * <0 — CHAT_CONNECT_* из chat_sync.h, команда не принята и *request не меняется. channel — собственный канал offer. */
int gui_bridge_offer_link(const char* channel, const char* link, size_t len, uint64_t* request);
int gui_bridge_join_link(const char* link, size_t len, uint64_t* request);
void gui_bridge_set_connect_result_cb(gui_connect_result_fn cb);

/* Callback для входящих сообщений (вызывается из GUI-потока) */
typedef void (*gui_msg_received_fn)(const char* ch_id, int ch_id_len, const uint8_t author[32], int initial_sync);
void gui_bridge_set_msg_received_cb(gui_msg_received_fn cb);

/* Обновление списка после изменения или завершённого локального удаления канала; GUI-поток. */
typedef void (*gui_channel_updated_fn)(const char* ch_id, int ch_id_len);
void gui_bridge_set_channel_updated_cb(gui_channel_updated_fn cb);

/* Callback для получения своего node_id после инициализации uTun */

/* Callback для статуса авто-подключения (вызывается из GUI-потока)
 * status: 0=in_progress, 1=done, 2=stopped */
typedef void (*gui_auto_connect_status_fn)(uint8_t status, uint16_t total_tried, uint16_t node_count, uint16_t connected);
void gui_bridge_set_auto_connect_status_cb(gui_auto_connect_status_fn cb);

/* Callback для обновления числа онлайн-пиров канала (вызывается из GUI-потока) */
typedef void (*gui_channel_peers_online_fn)(const char* ch_id, int ch_id_len, uint16_t online_count);
void gui_bridge_set_channel_peers_online_cb(gui_channel_peers_online_fn cb);

/* Callback: DB открыта, таблицы созданы, можно загружать данные (вызывается из GUI-потока) */
typedef void (*gui_db_ready_fn)(void);
void gui_bridge_set_db_ready_cb(gui_db_ready_fn cb);

/* Callback для обновления статуса (NTP + connections) — text без \0, вызывается из GUI-потока */
typedef void (*gui_status_refresh_fn)(const char* text, int len);
void gui_bridge_set_status_refresh_cb(gui_status_refresh_fn cb);

/* Секундное среднее CPU uasync, 0..100; callback выполняется в GUI-потоке. */
typedef void (*gui_cpu_load_fn)(int percent);
void gui_bridge_set_cpu_load_cb(gui_cpu_load_fn cb);
int gui_bridge_request_cpu_load(void); // 1 = запрос поставлен в очередь; 0 = ядро недоступно/ошибка

/* Callback для завершения загрузки медиа-аттача */
typedef void (*gui_attachment_downloaded_fn)(const char* ch_id, int ch_id_len, int64_t msg_id);
void gui_bridge_set_attachment_downloaded_cb(gui_attachment_downloaded_fn cb);

/* Callback для прогресса загрузки медиа */
typedef void (*gui_download_progress_fn)(const char* ch_id, int ch_id_len, int64_t msg_id, int blocks_done, int num_blocks);
void gui_bridge_set_download_progress_cb(gui_download_progress_fn cb);

/* Callback для входящих приглашений (вызывается из GUI-потока)
   data: [ch_id_len:1][ch_id][name_len:1][name][inviter X25519:32][inviter_name_len:1][inviter_name] */
typedef void (*gui_invite_received_fn)(const uint8_t* data, int len);
void gui_bridge_set_invite_received_cb(gui_invite_received_fn cb);

/* Callback для списка ETCP-соединений (вызывается из GUI-потока) */
typedef void (*gui_conn_list_fn)(const uint8_t* data, int len);
void gui_bridge_set_conn_list_cb(gui_conn_list_fn cb);

/* Callback для метрик выбранного соединения (вызывается из GUI-потока) */
typedef void (*gui_conn_metrics_fn)(const uint8_t* data, int len);
void gui_bridge_set_conn_metrics_cb(gui_conn_metrics_fn cb);

/* Независимый канал графиков, callback в GUI-потоке; формат — chat_network_stats.h. */
void gui_bridge_set_network_stats_cb(gui_conn_metrics_fn cb);
/* Под mutex привязать запрос к running-ядру. 1 — принят (*request_id заполнен), 2 — предыдущий ещё выполняется,
 * 0 — ядро недоступно, -1 — ошибка с логом. Не более одного запроса до ответа/отключения ядра. */
int gui_bridge_request_network_stats(uint64_t* request_id);

/* Callback для live-снапшота панели деталей участника (вызывается из GUI-потока)
   data: см. CHAT_MEMBER_DETAIL_* в chat_member.h; проверка целого пакета до применения. */
typedef void (*gui_member_detail_fn)(const uint8_t* data, int len);
void gui_bridge_set_member_detail_cb(gui_member_detail_fn cb);

/* Callback для получения полного списка мемберов канала (вызывается из GUI-потока)
   data: [ch_id_len:1][ch_id:var][count:2][member:CHAT_MEMBER_DISPLAY_SIZE]* */
typedef void (*gui_member_list_fn)(const uint8_t* data, int len);
void gui_bridge_set_member_list_cb(gui_member_list_fn cb);

/* Callback для обновления одного мембера (вызывается из GUI-потока)
   data: [ch_id_len:1][ch_id:var][count:1][member:CHAT_MEMBER_DISPLAY_SIZE] */
typedef void (*gui_member_updated_fn)(const uint8_t* data, int len);
void gui_bridge_set_member_updated_cb(gui_member_updated_fn cb);

/* Callback для удаления мембера из канала (вызывается из GUI-потока)
   data: [ch_id_len:1][ch_id:var][node_id:8] */
typedef void (*gui_member_removed_fn)(const uint8_t* data, int len);
void gui_bridge_set_member_removed_cb(gui_member_removed_fn cb);

/* Callback: получен канальный приватный ключ (стал совладельцем) — data: [ch_id_len:1][ch_id][from_ed:32] */
typedef void (*gui_admin_key_received_fn)(const uint8_t* data, int len);
void gui_bridge_set_admin_key_received_cb(gui_admin_key_received_fn cb);

/* Готовая invite-ссылка: link — заимствованный NUL-terminated UTF-8; пустая строка при ошибке, детали в логе. */
typedef void (*gui_invite_link_ready_fn)(const char* ch_id, int ch_id_len, uint64_t request_id, const char* link);
void gui_bridge_set_invite_link_ready_cb(gui_invite_link_ready_fn cb);

/* Callback: список достижимых узлов-кандидатов для invite — data: [auto_node_id:8][count:2][node_id:8]* */
typedef void (*gui_invite_candidates_fn)(const char* ch_id, int ch_id_len, const uint8_t* data, int len);
void gui_bridge_set_invite_candidates_cb(gui_invite_candidates_fn cb);

/* LAN-события в GUI-потоке; wire-форматы определены CHAT_EVT_LOCAL_* в chat_event.h. */
typedef void (*gui_local_event_fn)(const uint8_t* data, int len);
void gui_bridge_set_local_devices_cb(gui_local_event_fn cb);
void gui_bridge_set_local_invite_cb(gui_local_event_fn cb);
void gui_bridge_set_local_delivery_cb(gui_local_event_fn cb);

/* Callback для входящего звонка (вызывается из GUI-потока) */
typedef void (*gui_call_incoming_fn)(const char* ch_id, int ch_id_len, const uint8_t caller[32], uint64_t call_id);
void gui_bridge_set_call_incoming_cb(gui_call_incoming_fn cb);

/* Callback для звонкового события с call_id (ringing/accepted) */
typedef void (*gui_call_id_fn)(uint64_t call_id);
void gui_bridge_set_call_ringing_cb(gui_call_id_fn cb);
void gui_bridge_set_call_phone_busy_cb(gui_call_id_fn cb);
typedef void (*gui_call_peer_fn)(uint64_t call_id, const uint8_t peer[32]);
void gui_bridge_set_call_accepted_cb(gui_call_peer_fn cb);

/* Callback для завершения/отклонения звонка (вызывается из GUI-потока) */
typedef void (*gui_call_ended_fn)(uint64_t call_id, uint8_t reason);
void gui_bridge_set_call_ended_cb(gui_call_ended_fn cb);
void gui_bridge_set_call_declined_cb(gui_call_ended_fn cb);

/* Callback для ошибки звонка (пир офлайн/нет маршрута). text может быть пустым. */
typedef void (*gui_call_error_fn)(uint64_t call_id, uint8_t err, const char* text, int len);
void gui_bridge_set_call_error_cb(gui_call_error_fn cb);

/* CALL_STATS: rtt/buffer/minimum/maximum/reserve — мс; tempo — скорость ×100, dropped/underruns — счётчики.
 * Все восемь значений передаются uint16 BE после call_id в native endian; callback получает int.
 * Extrema относятся к последним десяти секундным интервалам. */
typedef void (*gui_call_stats_fn)(uint64_t call_id, int rtt, int buffer, int tempo_x100,
                                 int dropped, int underruns, int minimum, int maximum, int reserve);
void gui_bridge_set_call_stats_cb(gui_call_stats_fn cb);

/* Callback для BGP-маршрута звонка (вызывается из GUI-потока) */
typedef void (*gui_call_path_fn)(uint64_t call_id, const char* text, int len);
void gui_bridge_set_call_path_cb(gui_call_path_fn cb);

typedef void (*gui_call_connection_fn)(uint64_t call_id, uint8_t type, const uint8_t next_hop_ed[32]);
void gui_bridge_set_call_connection_cb(gui_call_connection_fn cb);

/* Callback индикации говорящего в рации (вызывается из GUI-потока)
 * group_id — канал, source — Ed25519 говорящего, on — 1=начал / 0=закончил. */
typedef void (*gui_radio_talk_fn)(const uint8_t group[32], const uint8_t source[32], uint8_t on);
void gui_bridge_set_radio_talk_cb(gui_radio_talk_fn cb);
/* На старте локального аудио разрешает Ed25519 в индекс маршрута. 0 при отказе/остановке. */
uint64_t gui_bridge_radio_route(const char* channel);

/* Callback для входящего DM-сообщения (вызывается из GUI-потока)
 * conv_id_len/conv_id — идентификатор беседы (десятичная строка). */
typedef void (*gui_dm_msg_received_fn)(const char* conv_id, int conv_id_len, const uint8_t author[32]);
void gui_bridge_set_dm_msg_received_cb(gui_dm_msg_received_fn cb);

/* Уведомление об изменениях сети, GUI-поток: CHANNEL_UPDATED/MEMBERS_CHANGED, SERVICE_STARTED/STOPPED,
 * NODEINFO_UPDATED/LINK_STATUS_CHANGED, MEMBER_UPDATED/REMOVED, CHANNEL_DELETED и DM_CONV_UPDATED.
 * nodeinfo callback может отправить только сигнал без payload; живые поля карточки получает MEMBER_DETAIL. */
typedef void (*gui_network_changed_fn)(int event_type);
void gui_bridge_set_network_changed_cb(gui_network_changed_fn cb);
void gui_bridge_set_dm_call_route_cb(void (*cb)(const uint8_t* data, int len));

/* DM-беседа создана/изменена: conv_id — заимствованная строка длины conv_id_len, generation — версия удаления. */
typedef void (*gui_dm_conv_updated_fn)(const char* conv_id, int conv_id_len, uint64_t generation);
void gui_bridge_set_dm_conv_updated_cb(gui_dm_conv_updated_fn cb);

/* Callback: расшифрованные сообщения DM-беседы (вызывается из GUI-потока)
 * data: [conv_id_len:1][conv_id:var][generation:8][JSON:utf8]. */
typedef void (*gui_dm_messages_fn)(const uint8_t* data, int len);
void gui_bridge_set_dm_messages_cb(gui_dm_messages_fn cb);
void gui_bridge_set_dm_members_cb(gui_dm_messages_fn cb); // CHAT_EVT_DM_MEMBERS, полный снимок текущего поколения.
/* CHAT_EVT_DM_INVITE_RESULT: коррелированный результат отправки приглашения. */
void gui_bridge_set_dm_invite_result_cb(gui_dm_messages_fn cb);
/* Запрос локального удаления из GUI: 0=поставлен в uasync, -1=отказ до постановки.
 * conv_id — десятичный ID, generation/request_id ненулевые; callback получает CHAT_EVT_DM_DELETE_RESULT в GUI. */
int gui_bridge_delete_dm(const char* conv_id, uint64_t generation, uint64_t request_id);
void gui_bridge_set_dm_delete_result_cb(gui_dm_messages_fn cb);
/* CHAT_EVT_DM_START_RESULT: request/conv/generation:8 native + success:1. */
void gui_bridge_set_dm_start_result_cb(gui_dm_messages_fn cb);
/* Файловый браузер: wire-ответы и прогресс общих передач, в GUI-потоке. */
void gui_bridge_set_member_files_response_cb(gui_dm_messages_fn cb);
void gui_bridge_set_member_files_transfer_cb(gui_dm_messages_fn cb);
struct mf_message;
struct mf_transfer_request;
/* Скопировать команду/передачу и привязать instance под mutex моста. Исходные данные после возврата не нужны.
 * Ненулевые group/server и request_id/id обязательны; transfer требует owner, поле inst входа игнорируется.
 * 0 — принято: обычный RESPONSE/TRANSFER, в том числе MF_CANCELLED при отмене до trampoline.
 * -1 — не принято: результата не будет, память не остаётся, вызывающий немедленно завершает операцию с ошибкой.
 * Нужны gui_bridge_init и опубликованное работающее ядро; UI callbacks всегда через очередь Qt. */
int gui_bridge_member_files_request(const uint8_t group[32], const uint8_t server[32], const struct mf_message* message);
int gui_bridge_self_ed(uint8_t ed[32]);
int gui_bridge_member_files_transfer(const struct mf_transfer_request* request);
/* Запросить отмену ID у текущего ядра; id=0 ничего не делает. 0 — принято/no-op, -1 — отказ с логом.
 * Память команды принадлежит мосту. Итог операции приходит по её RESPONSE/TRANSFER, отдельного ответа cancel нет. */
int gui_bridge_member_files_cancel(uint64_t id);
void gui_bridge_set_avatar_result_cb(gui_dm_messages_fn cb);
void gui_bridge_set_group_avatar_result_cb(gui_dm_messages_fn cb);
void gui_bridge_set_group_policy_result_cb(gui_dm_messages_fn cb);
/* Снимок/результат rename: request:8LE, group Ed25519:32 + JSON, callback выполняется в GUI-потоке. */
void gui_bridge_set_group_info_result_cb(gui_dm_messages_fn cb);
/* Kanban changed: group Ed25519:32; result: request:8LE, group Ed25519:32 + JSON, GUI thread. */
void gui_bridge_set_kanban_cb(gui_dm_messages_fn cb);
/* Опросы/реакции: адресное изменение и коррелированный ответ команды, в GUI-потоке. */
void gui_bridge_set_feedback_changed_cb(gui_dm_messages_fn cb);
/* Изменение локальных запросов/ключей вложения, без секретных данных. */
void gui_bridge_set_attachment_keys_cb(gui_dm_messages_fn cb);
void gui_bridge_set_devgroup_result_cb(gui_dm_messages_fn cb);
/* События clipboard в GUI-потоке; payload описан в chat_clipboard.h/chat_event.h. */
void gui_bridge_set_clipboard_received_cb(gui_dm_messages_fn cb);
void gui_bridge_set_clipboard_result_cb(gui_dm_messages_fn cb);
/* Копирует снимок;0=queued,-1=отказ. group=0 и peer=NULL — свои устройства, иначе участник. applied принимает CHAT_CLIPBOARD_EVENT_HEADER. */
int gui_bridge_send_clipboard(uint64_t request, int type, const uint8_t* data, size_t size, const uint8_t group[32], const uint8_t peer[32]);
int gui_bridge_clipboard_applied(const uint8_t* header, int success);
struct chat_clipboard_source;
/* Одной командой регистрирует локальные immutable snapshots и рассылает только manifest. */
int gui_bridge_offer_clipboard_files(uint64_t request, const struct chat_clipboard_source* sources, size_t count,
    const uint8_t* manifest, size_t size, const uint8_t group[32], const uint8_t peer[32]);
/* Thread-safe из provider ОС;0=queued,done придёт ровно один раз в uasync/потоке отмены моста.
 * При -1 callback не вызывается. Аргументы копируются, arg живёт до callback. */
int gui_bridge_fetch_clipboard_file(const uint8_t group[32], const uint8_t peer[32], const uint8_t object[16], uint64_t size,
    const char* destination, void (*done)(void*, int), void* arg);
void gui_bridge_set_feedback_result_cb(gui_dm_messages_fn cb);
void gui_bridge_set_msg_submitted_cb(gui_dm_messages_fn cb);
void gui_bridge_set_msg_updated_cb(gui_dm_messages_fn cb);
void gui_bridge_set_radio_mute_result_cb(gui_dm_messages_fn cb);
/* CHAT_EVT_RADIO_CALL: id:8LE, group/member Ed25519:32, state/reason/incoming:1, GUI-поток. */
void gui_bridge_set_radio_call_cb(gui_dm_messages_fn cb);
void gui_bridge_set_stream_sources_cb(gui_dm_messages_fn cb);
/* Переключить локальное приглушение участника. 0 — поставлено в uasync; -1 — ядро/аргументы/память недоступны.
 * channel_id и ed — полные ключи группы и участника; результат приходит через RADIO_MUTE_RESULT. */
int gui_bridge_toggle_member_radio_mute(const char* channel_id, const uint8_t ed[32]);
/* CHAT_EVT_ATTACHMENT_PREPARATION: target + job UUID + state. */
void gui_bridge_set_attachment_preparation_cb(gui_dm_messages_fn cb);

/* uTun nodeinfo callback — registered via utun_add_nodeinfo_cbk, called from uasync thread */
void gui_nodeinfo_cb_impl(struct TOPO_GROUP* group, struct TOPO_GROUP_NODE* nq, void* arg);

#ifdef __cplusplus
}
#endif
#endif /* GUI_BRIDGE_H */
