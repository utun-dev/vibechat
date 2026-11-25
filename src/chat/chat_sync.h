/*
 * chat_sync — связь чат-сервиса с транспортом и группами.
 * Отслеживает пиров, запускает member_sync, поддерживает кеш каналов и статус в GUI.
 * UP/REINIT/пробуждение восстанавливают только общие каналы соответствующего пира.
 * Отложенные запросы объединяются по (канал, пир); активными обменами и обновлениями владеет Merkle.
 * Очередь удерживает NCD до запуска раунда; раунд — до публикации результата. Закрытие BGP не прерывает начальный MEMBERS.
 * При старте NTP обмен ждёт до 4 с: задания сохраняются, входящие кадры — в ограниченной очереди.
 * После успеха NTP/таймаута продолжаем JOIN и синхронизацию по уже готовым соединениям.
 * Вход по ссылке держит отдельный NCD handle до результата join, а не только до UP.
 * JOIN_INFO_REQ → JOIN_INFO_RESP → JOIN_REQUEST → JOIN_READY; правила — в chat_join.h.
 * Жизненным циклом управляет chat_service_start/stop. Вызовы — в потоке uasync.
 * Вход из GUI/JNI через отдельный chat_join_port, который копирует ссылку и удерживает очередь до close.
 * Кеш строится полным снимком БД (до65536 каналов/пиров на канал);ошибка сохраняет прежний снимок.
 * Callback UI может удалить канал: обходы уведомлений используют собственные копии ID.
 */

#ifndef CHAT_SYNC_H
#define CHAT_SYNC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

struct UTUN_INSTANCE;
struct sqlite3;
struct chat_sync_catalog;
struct chat_join_path;
struct InviteAddr;
/* SQL-owner READ: собственный полный каталог каналов/пиров; SQLITE_OK либо positive SQLite code, out=NULL при отказе.
 * Без runtime/callbacks/BEGIN/COMMIT; caller освобождает catalog_free после activation либо ошибки startup. */
int chat_sync_catalog_read(struct sqlite3* db, struct chat_sync_catalog** out);
void chat_sync_catalog_free(struct chat_sync_catalog* catalog);
/* Uasync: runtime без SQL после готовности chat_core, MEMBERS/Merkle и avatar.
 * catalog заимствован до возврата: каналы перемещаются в runtime и обнуляются внутри catalog, сам catalog остаётся caller.
 * 0/-1, частичный runtime освобождается; повтор при готовом chat_sync даёт0. */
int chat_sync_activate(struct UTUN_INSTANCE* inst, struct chat_sync_catalog* catalog);
/* Uasync/noSQL:1 — runtime chat_sync активирован,0 — отсутствует/quiesce. */
int chat_sync_is_initialized(struct UTUN_INSTANCE* inst);
/* После снятия стартового ожидания NTP запустить накопленные обмены по уже живым соединениям. */
void chat_sync_time_ready(struct UTUN_INSTANCE* inst);

/* CHAT_SYNC использует Ed25519 группы, кроме сквозного кадра JOIN_RELAY. */
#define CHAT_SYNC_TYPE_OFF 1
#define CHAT_SYNC_SCOPE_OFF 2
#define CHAT_SYNC_BOOTSTRAP_SIZE (CHAT_SYNC_SCOPE_OFF + 8u)
#define CHAT_SYNC_GROUP_SIZE (CHAT_SYNC_SCOPE_OFF + 32u)
#define CHAT_SYNC_ATTEMPT_SIZE 8u

#define CHAT_JOIN_WIRE_MAX (16u * 1024u) /* JOIN с полным JSON группы и профилем участника, включая envelope. */
/* Подтверждённая личность и путь конкретной попытки; handle=0 обозначает обычный ETCP. */
struct chat_join_route {
    uint64_t peer, handle;
    uint8_t public_key[32]; /* X25519 прямого транспорта. */
    uint8_t peer_ed[32]; /* Ed25519 подтверждённого ETCP, нужен для push-приглашения. */
};
/* Заимствованный транспорт только JOIN, все вызовы в uasync, без синхронных событий.
 * open закрепляет X25519 из ссылки: 0=принято и handle!=0, 1=пир недоступен, -1=ошибка.
 * handle идентифицирует сессию и не переиспользуется до замены provider; после closed-события send отклоняет его.
 * send копирует весь CHAT_SYNC frame: 0=принят, -1=отказ без передачи владения.
 * close освобождает удержание JOIN после отправки принятых сообщений; физический GATT не закрывает.
 * success разрешает повышение до ETCP только при независимой проверке known-key. */
struct chat_join_carrier {
    void* ctx;
    int (*open)(void* ctx, const uint8_t public_key[32], uint64_t* handle);
    int (*send)(void* ctx, uint64_t handle, const uint8_t* data, size_t len);
    void (*close)(void* ctx, uint64_t handle, int success);
};
/* Установить/снять provider; замена освобождает его пути и отменяет ожидания ответа. 0/-1. */
int chat_sync_set_join_carrier(struct UTUN_INSTANCE* inst, const struct chat_join_carrier* carrier);
/* События provider после проверки личности и свежести сессии. Получение разрешает только JOIN-типы;
 * data/public_key заимствованы до возврата. closed освобождает указанный путь; после JOIN_READY продолжается сохранение. */
void chat_sync_join_carrier_ready(struct UTUN_INSTANCE* inst, uint64_t handle, const uint8_t public_key[32]);
void chat_sync_join_carrier_receive(struct UTUN_INSTANCE* inst, uint64_t handle, const uint8_t public_key[32],
                                  const uint8_t* data, size_t len);
void chat_sync_join_carrier_closed(struct UTUN_INSTANCE* inst, uint64_t handle);
/* Допуск двух конечных участников к первому MEMBERS по подтверждённому маршруту JOIN. */
int chat_sync_join_members_allowed(struct UTUN_INSTANCE* inst, const struct chat_join_path* path, const char* channel);
/* После публикации допуска MEMBERS завершить ожидающий JOIN, если локальный участник допущен. */
void chat_sync_members_published(struct UTUN_INSTANCE* inst, const char* channel);
/* Запустить административный обмен после COMMIT до закрытия групповых сессий.
 * admin_changed=1 помечает изменение корня, 0 добавляет сверку с новыми проверенными пирами. */
void chat_sync_admission_changed(struct UTUN_INSTANCE* inst, const char* channel, int admin_changed);

/* etcp service IDs (direct P2P): ETCP_ID_CHAT_SYNC / ETCP_ID_MEMBER_SYNC
 * (etcp_bind, см. etcp_api.h) */

/* Управление JOIN, приглашениями и настройкой звука; история сообщений — в db_sync/Merkle. */
#define CS_MSG_ERROR            0x07  /* result:4 (CHAT_CONNECT_AUTH или CHAT_CONNECT_ERROR), в фазах ожидания INFO_RESP/READY */
#define CS_MSG_CHANNEL_INVITE   0x0F  /* push-invite: inviter → joiner */
#define CS_MSG_JOIN_INFO_REQ    0x10  /* J → A: ключ приглашения (u64BE) */
#define CS_MSG_JOIN_INFO_RESP   0x11  /* A → J: ключи группы, подписанное описание и запись A */
#define CS_MSG_JOIN_REQUEST     0x12  /* J → A: Ed25519 группы, ключ приглашения и запись J */
#define CS_MSG_JOIN_READY       0x13  /* A → J: Ed25519 группы после COMMIT и публикации допуска */
#define CS_MSG_SOUND_SETTING    0x14  /* peer → server: {sound_on:1} — per-chat play sound on message */
#define CS_MSG_SERVER_JOIN_REQ  0x15  /* client → server: operation:8BE, add_key:32, link_len:2BE, CHANNEL-link */
#define CS_MSG_SERVER_JOIN_RESULT 0x16 /* server → client: operation:8BE, result:4BE (CHAT_CONNECT_*) */
#define CS_MSG_JOIN_RELAY        0x17  /* Непрозрачный сквозной кадр JOIN; разрешён на JOIN carrier. */
#define CS_MSG_JOIN_MEMBERS      0x18  /* Только внутри защищённого кадра JOIN. */

/* Доставить расшифрованный кадр JOIN конечному автомату в uasync. Все аргументы заимствованы. */
void chat_sync_join_receive(struct UTUN_INSTANCE* inst, const struct chat_join_path* path,
    uint8_t type, const uint8_t* data, size_t size);

/* SERVER_JOIN_REQ: add_key проверяется до обычного chat_sync_join_link. CHANNEL-link обязан
 * относиться к Ed25519 группы из заголовка и вести на отправителя. JOIN использует уже открытый транспорт.
 * operation_id не является attempt_id серверного JOIN. RESULT возвращается после его завершения;
 * повторы активного operation_id не создают второй JOIN. Клиент держит NCD до RESULT/общего deadline.
 * Обычный CHANNEL_INVITE сервер не принимает: этот путь не проверяет серверный add_key. */

/* ── Public API ── */

/* Создать контекст,обработчики/подписки и первый кеш поверх готового chat_core. 0/-1;повтор допустим.
 * Ошибка обязательной подписки/таймера/первого кеша откатывает runtime;данные БД сохраняются. */
int  chat_sync_init(struct UTUN_INSTANCE* inst);
/* uasync: закрыть приём команд и отменить локальный JOIN до остановки workers. */
void chat_sync_quiesce(struct UTUN_INSTANCE* inst);
/* Uasync: отменить текущий request или его фоновое восстановление после результата. */
void chat_sync_cancel_join(struct UTUN_INSTANCE* inst, uint64_t request);
/* Отменить ожидания, синхронизацию и таймеры, освободить контекст; данные каналов сохраняются. */
void chat_sync_destroy(struct UTUN_INSTANCE* inst);

/* Локальная попытка JOIN: uasync. Отрицательный возврат — отказ запуска без события;
 * 0 — запущено, CHAT_EVT_CONNECT_RESULT ровно один раз, включая stop/delete.
 * request=0 — без адресата UI; ненулевые ID вызывающий не переиспользует до завершения.
 * link заимствован до возврата; request — корреляция интерфейса, не сетевой attempt_id. */
#define CHAT_CONNECT_ERROR -1
#define CHAT_CONNECT_TIMEOUT 2
#define CHAT_CONNECT_INVALID -7
#define CHAT_CONNECT_BUSY -8
#define CHAT_CONNECT_CANCELLED -9
#define CHAT_CONNECT_AUTH -10          /* Отказ авторизации JOIN/приглашения без более точной причины. */
#define CHAT_CONNECT_INVITE_DENIED -11 /* Локальный участник не имеет права приглашать в группу. */
#define CHAT_CONNECT_SERVER_KEY -12    /* Сервер отклонил постоянный ключ добавления из ссылки. */
int chat_sync_join_link(struct UTUN_INSTANCE* inst, uint64_t request, const char* link, size_t len);
/* Единый формат результата: peer:8,result:4,channel:8,request:8, native; только uasync. */
void chat_sync_join_result(struct UTUN_INSTANCE* inst, uint64_t request, uint64_t channel, const uint8_t peer[32], int result);
/* uasync: пригласить адресата в свой channel. SERVER-ссылка запускает запрос с add_key и обычным
 * приглашением через self; успех возвращает сервер после своего JOIN. CHANNEL — обычный push-invite.
 * Те же правила запуска/результата; серверная ссылка не является приглашением в канал. */
int chat_sync_offer_link(struct UTUN_INSTANCE* inst, uint64_t request, uint64_t channel, const char* link, size_t len);

/* Uasync: отправить CHANNEL_INVITE в свой channel устройству с каноническим peer_ed.
 * address — необязательная подсказка IPv4/IPv6 UDP/TCP. Аргументы заимствованы до возврата.
 * Правила запуска и завершения как у chat_sync_join_link с request=0. */
int chat_sync_invite_to_channel(struct UTUN_INSTANCE* inst, uint64_t channel, const uint8_t peer_ed[32], const struct InviteAddr* address);

/* Принять сохранённое приглашение (до4 пар канал/инвайтер). Успешный запуск убирает его из очереди;
 * BUSY/отказ запуска сохраняет предложение. Описание/участник проверяются и сохраняются только в JOIN_INFO_RESP. */
void chat_sync_accept_invite(struct UTUN_INSTANCE* inst, const uint8_t group_ed[32], const uint8_t inviter[32]);

/* Убрать пару из очереди предложений без сетевого ответа;освободившийся слот можно использовать повторно. */
void chat_sync_deny_invite(struct UTUN_INSTANCE* inst, const uint8_t group_ed[32], const uint8_t inviter[32]);

/* Снять паузу и сбросить задержки повторных попыток во всех кешированных каналах с автоматическим планировщиком.
   Планировщик пересматривает подключения; работающие соединения сохраняются.
   Вызывается из auto_socket при появлении/изменении сетевой связности. */
void chat_sync_retry_channels_on_socket_change(struct UTUN_INSTANCE* inst);

/* Отменить JOIN,waiters,предложения и отложенные запросы канала;немедленно убрать его из кеша (локальное удаление).
 * Активные Merkle-сессии отменяет вызывающий код через member_sync_cancel_channel(). */
void chat_sync_on_channel_deleted(struct UTUN_INSTANCE* inst, const char* ch_id, const uint8_t group_ed[32]);

/* Разослать всем подключённым пирам канала смену per-chat настройки «play sound on message». */
void chat_sync_broadcast_sound_setting(struct UTUN_INSTANCE* inst, const char* ch_id, int on);

#ifdef __cplusplus
}
#endif
#endif /* CHAT_SYNC_H */
