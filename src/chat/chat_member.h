/*
 * chat_member.h — единая структура мембера для отображения в GUI
 *
 * Используется chatgui (desktop), headless CLI, Android.
 * GUI/JNI-трамплины читают файловую БД общим worker, затем добавляют live-поля в uasync.
 * Расширенная JSON-интроспекция с ключами/адресами описана отдельно в chat_core.h.
 * Снимок содержит свойства участника и состояние маршрута именно его CHAT-группы.
 * Все вызовы — в uasync-потоке. ch_id — публичный Ed25519 группы в строчном HEX.
 * В список входят self,placeholders и deleted;deleted показывается offline,self обычно online.
 *
 * Формат на проводе (ручная сериализация, фиксированный размер 313 байт):
 *   [0]     online    uint8_t
 *   [1]     is_self   uint8_t
 *   [2]    flags     uint8_t     0x01=supernode 0x02=admin 0x04=moder 0x08=deleted 0x10=radio 0x20=local radio mute
 *                               0x40=phone status known, 0x80=busy on a call (live BGP hint)
 *   [3-4] rtt       uint16_t LE (средний за 300 мс RTT выбранного пути в 0,1 мс; 0xFFFF=нет данных)
 *   [5]    nat_type  uint8_t     ADDR_TYPE_*
 *   [6]    storage   uint8_t
 *   [7-70] name      char[64]   local_nick если задан, иначе node name. \0-padded
 *   [71-102] avatar_server Ed25519, 32 байта; нули — нет аватарки
 *   [103-172] avatar_file char[70], sha256.json с NUL
 *   [173] storage_role uint8_t, подписанная роль, независимо от сетевого приоритета storage
 *   [174] verified uint8_t, административный тег verified=yes
 *   [175] node_type uint8_t, вычисленный тип узла из peers
 *   [176] conn_presence uint8_t, известные виды подключения NCONN_*
 *   [177] conn_up uint8_t, работающие виды подключения NCONN_*
 *   [180-211] devgroup Ed25519 public key, нули — без группы
 *   [212-275] devgroup_name UTF-8 char[64], NUL-padded
 *   [276] protocol_state: 0 неизвестно, 1 совместимо, 2 несовместимо
 *   [277-278] protocol_local uint16_t LE
 *   [279-280] protocol_remote uint16_t LE
 *   [281-312] ed25519 полный адрес участника, 32 байта
 *   [178-179] intermediaries uint16_t LE, число посредников выбранного пути
 */

#ifndef CHAT_MEMBER_H
#define CHAT_MEMBER_H

#include <stdint.h>
#include <stddef.h>

struct UTUN_INSTANCE;
struct TOPO_GROUP;

#ifdef __cplusplus
extern "C" {
#endif

#define CHAT_MEMBER_FLAG_SUPERNODE 0x01
#define CHAT_MEMBER_FLAG_ADMIN     0x02
#define CHAT_MEMBER_FLAG_MODER     0x04
#define CHAT_MEMBER_FLAG_DELETED   0x08
#define CHAT_MEMBER_FLAG_RADIO     0x10
#define CHAT_MEMBER_FLAG_RADIO_MUTED 0x20 /* локальное отключение голоса участника */
#define CHAT_MEMBER_FLAG_PHONE_KNOWN 0x40 /* опубликовано состояние телефонной занятости */
#define CHAT_MEMBER_FLAG_PHONE_BUSY  0x80 /* занят звонком; цвет трубки в интерфейсе */

#define CHAT_MEMBER_DISPLAY_SIZE 313
#define CHAT_MEMBER_ONLINE_OFF 0
#define CHAT_MEMBER_SELF_OFF 1
#define CHAT_MEMBER_FLAGS_OFF 2
#define CHAT_MEMBER_DEVGROUP_OFF 180
#define CHAT_MEMBER_DEVGROUP_NAME_OFF 212
#define CHAT_MEMBER_PROTOCOL_STATE_OFF 276
#define CHAT_MEMBER_ED_OFF 281

/* Legacy синхронный доступ для ещё не перенесённых SQL/CLI-путей, не frontend trampoline.
 * Снять полный список:0 — успех,-1 — ошибка/более65535 записей. Обязательные out/count сбрасываются в NULL/0.
 * out — u_malloc-массив count*CHAT_MEMBER_DISPLAY_SIZE байт,освобождает caller через u_free;пустой список — NULL/0.
 * Порядок:online первыми,затем Ed25519 в лексикографическом порядке. SQL/память не дают частичный успешный результат. */
int chat_core_get_member_list(struct UTUN_INSTANCE* inst, const char* ch_id, uint8_t** out, int* count);

/* Снять одного участника:0/-1 (ошибка/нет строки). out — обязательный буфер CHAT_MEMBER_DISPLAY_SIZE байт;
 * при отказе не меняется. Формирование полей совпадает с полным списком. */
int chat_core_get_single_member(struct UTUN_INSTANCE* inst, const char* ch_id, const uint8_t ed[32], uint8_t* out);

/* Live-онлайн: маршрутизатор располагает живым путём в группе канала (self всегда онлайн).
   Не читает nodes.online и не проверяет членство/deleted;это отдельная проверка транспорта,self всегда1. NULL/неверный ID дают0. */
int chat_core_member_online(struct UTUN_INSTANCE* inst, const char* ch_id, const uint8_t ed[32]);

/* Внутренний снимок:поля далее кодируются вручную,struct-layout в wire не передаётся. */
struct chat_member_display {
    uint8_t devgroup[32];    /* Проверенный Ed25519 ID группы устройств; нули — нет группы. */
    char devgroup_name[64];  /* Имя наиболее свежего профиля из nodes. */
    uint8_t ed25519[32];     /* Полный адрес из списка участников. */
    uint8_t  online;         /* Живой путь/self,сброшен для deleted. */
    uint8_t  is_self;        /* Совпадение Ed25519 в списке с ключом instance. */
    uint8_t  flags;          /* CHAT_MEMBER_FLAG_*;offline не несёт phone known/busy. */
    uint16_t rtt;            /* Средний за 300мс RTT выбранного пути,0.1мс;UINT16_MAX — неизвестен/насыщен. */
    uint8_t  nat_type;       /* Агрегированный ADDR_TYPE_* адресов узла;255 — адресов нет. */
    uint8_t  storage;        /* Локальный приоритет хранения,отдельно от подписанной роли. */
    char     name[64];       /* Local nick,имя nodes или начало публичного ключа;до63 байт+NUL. */
    uint8_t avatar_server[32]; /* Ed25519 сервера; нули — ссылки нет. */
    char     avatar_file[70];/* Проверенный sha256.json+NUL. */
    uint8_t  storage_role;   /* source=0,не deleted,adm_tags.storage=yes и64-байтовая подпись owner-блока;доверие записи — у member_sync. */
    uint8_t  verified;        /* административное подтверждение участника */
    uint8_t  node_type;       /* классификация узла, не состояние маршрута */
    uint8_t  conn_presence;   /* известные виды подключения NCONN_* */
    uint8_t  conn_up;         /* работающие виды подключения NCONN_* */
    uint16_t protocol_local, protocol_remote; /* версии протокола группы */
    uint8_t protocol_state; /* 0 — неизвестно, 1 — совместимо, 2 — несовместимо */
    uint16_t intermediaries; /* число посредников выбранного живого пути */
};

/* Uasync: добавить/заменить live-поля собственного SQL-снимка, без обращения к БД.
 * inst/ch/member обязательны, полный Ed25519 находится в member->ed25519; 0/-1 при неверных аргументах.
 * Имя/роли/аватар/тип/NAT/devgroup не меняются; повторный вызов обновляет маршрут/RTT/рацию/телефон/протокол. */
int chat_member_apply_live(struct UTUN_INSTANCE* inst, const char* ch, struct chat_member_display* member);

/* Копирование снимка: src/dst обязательны,caller обеспечивает размеры.
 * Serialize завершает/дополняет NUL поля name/avatar_file;NULL только логируется. */
void chat_core_serialize_member(const struct chat_member_display* src, uint8_t* dst);
/* Deserialize читает CHAT_MEMBER_DISPLAY_SIZE доступных байт;0/-1 при NULL,длину/семантику полей проверяет caller. */
int  chat_core_deserialize_member(const uint8_t* src, struct chat_member_display* dst);

/* Трамплин для gui_bridge, только uasync: принимает собственный u_malloc-блок arg и освобождает его.
 * ch_id — канонический ID+NUL. Успех: CHAT_EVT_MEMBER_LIST с uint16 native count (0..65535), записи CHAT_MEMBER_DISPLAY_SIZE.
 * Файловый READ асинхронный, повторные запросы объединяются; inst живёт до закрытия DB worker.
 * Непредставимый count/ошибка чата/памяти логируются, события нет; ложный/усечённый снимок не отправляется. */
struct chat_member_list_req { struct UTUN_INSTANCE* inst; char ch_id[65]; };
void chat_core_request_member_list_trampoline(void* arg);

/* Владение/поток/аргумент как у list trampoline. Для каждого участника — один полный
 * CHAT_EVT_MEMBER_UPDATED, включая остальные live-поля. Пустой список не даёт событий.
 * SQL/владение как у list; live-поля добавляются непосредственно перед публикацией, новых сетевых измерений нет. */
void chat_core_request_member_rtt_trampoline(void* arg);

/* Локальный переключатель рации другого участника:0 — committed,-1 — отказ,чужая транзакция запрещена.
 * Снимок готовится до изменения;после radio commit обновлённый флаг приходит MEMBER_UPDATED без нового SQL-чтения.
 * Ошибка сохраняет настройку/кэш/звук и не выдаёт успешное событие. */
int chat_core_toggle_member_radio_mute(struct UTUN_INSTANCE* inst, const char* ch_id, const uint8_t ed[32]);
struct chat_member_radio_mute_req { struct UTUN_INSTANCE* inst; char ch_id[65]; uint8_t ed[32]; };
#define CHAT_RADIO_MUTE_RESULT_SIZE 65
/* Забирает u_malloc-запрос; inst заимствован до done/закрытия worker. RW toggle выполняется одной WRITE,
 * после commit обновляются mute/audio, затем заказывается свежий member READ для UI.
 * Ответ — RADIO_MUTE_RESULT(group,Ed25519,success), включая отказ; ошибка последующего READ не отменяет commit. */
void chat_core_toggle_member_radio_mute_trampoline(void* arg);

/* Подписки свойств/маршрутов → MEMBER_UPDATED; вызываются в uasync при init/destroy чата.
 * Файловые свойства читаются worker. Маршруты объединяются по (канал,узел) до следующего posted callback:
 * UI получает свежий снимок вне topo dispatch и может удалить канал, не повредив его group/node.
 * Ожидающие route-события отменяются при destroy;ошибка памяти логируется без частичного события. */
/* Установить обе подписки0/-1;при отказе частичная регистрация снята. Вызывается один раз из chat_core_init. */
/* Только uasync: отложить MEMBER_UPDATED после изменения локального состояния участника.
 * group заимствована на время вызова; повторные события объединяются по группе и Ed25519. */
void chat_member_state_changed(struct TOPO_GROUP* group, const uint8_t ed[32]);
/* Uasync: после успешного удаления канала отсоединить принятые SQL/route-ответы; channel=NULL — все.
 * Не ждёт worker и не освобождает его данные; done позже освобождает снимок без доступа к прежнему runtime. */
void chat_member_forget_channel(struct UTUN_INSTANCE* inst, const char* channel);
int chat_member_init(struct UTUN_INSTANCE* inst);
void chat_member_destroy(struct UTUN_INSTANCE* inst);

/* Дополнительная диагностика карточки; online/RTT берутся только из chat_member_display.
 * Запрос выделяет GUI через u_calloc и передаёт в uasync; trampoline освобождает его.
 * Ответ CHAT_EVT_MEMBER_DETAIL всегда повторяет request_id/group_id/ed, включая ошибки. */
struct chat_member_detail_req {
    struct UTUN_INSTANCE* inst; /* ядро, в uasync которого выполняется сбор */
    uint8_t group[32];         /* Публичный Ed25519 CHAT-группы. */
    uint8_t ed[32];            /* Ed25519 выбранного участника */
    uint64_t request_id;       /* ненулевой ID запроса, не повторяется при смене выбора */
};

/* Wire LE: request:8, group Ed25519:32, member Ed25519:32, result:1, transport_flags:1, own_tcp:2,
 * hop_count:1, link_count:1, sock_count:1; далее hops:32*, links:94*, socks:48*.
 * transport_flags: bit0=conn_present, bit1=links_up, bit2=initialized; не доступность в группе.
 * hops — Ed25519 на выбранном пути от назначения к первому посреднику; у offline/self пустой.
 * Ошибка содержит только заголовок;INVALID допускает нулевые поля неверного исходного запроса.
 * Счётчики own_tcp и ссылок сокета насыщаются на65535;списки ограничены16 элементами каждый.
 * link: direction:1 (0=unknown,1=out,2=in),state:1,status:1,has_ip:1,local_family:1,local_addr:16,local_port:2,
 *       remote_family:1,remote_addr:16,remote_port:2,socket_name:20,protocol:32. Не-IP не имеет адресов/портов.
 * sock: id:4,port:2,links_up:2,links_down:2,ifname:38. Строки завершаются NUL. */
#define CHAT_MEMBER_DETAIL_HEADER_SIZE 79
#define CHAT_MEMBER_DETAIL_RESULT_OFF 72
#define CHAT_MEMBER_DETAIL_HOP_COUNT_OFF 76
#define CHAT_MEMBER_DETAIL_LINK_COUNT_OFF 77
#define CHAT_MEMBER_DETAIL_SOCK_COUNT_OFF 78
#define CHAT_MEMBER_DETAIL_MAX_HOPS 16
#define CHAT_MEMBER_DETAIL_MAX_LINKS 16
#define CHAT_MEMBER_DETAIL_MAX_SOCKS 16
#define CHAT_MEMBER_DETAIL_LINK_SIZE 94
#define CHAT_MEMBER_DETAIL_SOCK_SIZE 48
#define CHAT_MEMBER_DETAIL_MAX_SIZE (CHAT_MEMBER_DETAIL_HEADER_SIZE + 32 * CHAT_MEMBER_DETAIL_MAX_HOPS + \
    CHAT_MEMBER_DETAIL_LINK_SIZE * CHAT_MEMBER_DETAIL_MAX_LINKS + CHAT_MEMBER_DETAIL_SOCK_SIZE * CHAT_MEMBER_DETAIL_MAX_SOCKS)

enum chat_member_detail_result {
    CHAT_MEMBER_DETAIL_OK = 0,
    CHAT_MEMBER_DETAIL_UNAVAILABLE = 1, /* чат/группа остановлены */
    CHAT_MEMBER_DETAIL_NOT_MEMBER = 2, /* запись участника отсутствует */
    CHAT_MEMBER_DETAIL_INVALID = 3,   /* неверные аргументы запроса */
    CHAT_MEMBER_DETAIL_ERROR = 4      /* ошибка чтения данных участника */
};

/* Проверенный view заимствует массивы из входного пакета; живёт не дольше data. */
struct chat_member_detail_view {
    uint64_t request_id;                   /* Корреляция запроса. */
    uint8_t group[32];                     /* Публичный Ed25519 группы. */
    uint8_t ed[32];                        /* Ed25519 адресата ответа */
    uint8_t result, transport_flags;       /* результат и состояние физического транспорта */
    uint16_t own_tcp_active;               /* TCP-соединения self */
    uint8_t hop_count, link_count, sock_count; /* длины массивов */
    const uint8_t *hops, *links, *socks;    /* неизменяемые wire-массивы */
};

/* Проверить весь пакет до применения: 0=успех, -1=ошибка с логом; out при ошибке не меняется. */
int chat_core_parse_member_detail(const uint8_t* data, size_t len, struct chat_member_detail_view* out);
/* Uasync: принять владение req, прочитать наличие записи через общий DB worker, затем собрать live-диагностику.
 * Ответ сохраняет request/group/node при любой ошибке. После stop/start прежний ответ — UNAVAILABLE.
 * inst должен жить до drain worker; только изолированные :memory: fixtures допускают синхронный fallback. */
void chat_core_collect_member_detail_trampoline(void* arg);

#ifdef __cplusplus
}
#endif
#endif /* CHAT_MEMBER_H */
