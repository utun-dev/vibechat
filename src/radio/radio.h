// radio.h — рация (PTT walkie-talkie): мультикаст голоса по CHAT-группе.
//
// Флудит Opus-кадры всем подписчикам канала (узлы с TOPO_FLAG_RADIO в NODEINFO).
// Автор подписывает каждую пачку и FIN; слушатель проверяет подпись и допуск по MEMBERS своей группы.
// Для каждого автора хранится последний timestamp: принимается только больший, без сравнения с местными часами.
// Повторы/опоздавшие пачки отбрасываются; timestamps сохраняются до закрытия группы, включая паузы прослушивания.
// Чистый ретранслятор сохраняет подпись без проверки прав автора; дописываемый путь в неё не входит.
// Передача полудуплексная: радио включается на прослушивание, PTT — на передачу.
// Все функции — в uasync-потоке, кроме регистрации frame/mute callbacks
// аудио-потребителем при init/destroy.
#ifndef RADIO_H
#define RADIO_H

#include <stdint.h>

#include "radio_proto.h"

struct TOPO_GROUP;
struct UTUN_INSTANCE;

#ifdef __cplusplus
extern "C" {
#endif

/* ── жизненный цикл ── */

int  radio_init(struct TOPO_GROUP* group);     /* из topo_group_create */
void radio_destroy(struct TOPO_GROUP* group);  /* из topo_group_destroy */
/* После commit членства CHAT: отменить недопущенную локальную передачу и снять индикацию удалённых участников. */
void radio_revalidate_members(struct TOPO_GROUP* group);

int  radio_init_instance(struct UTUN_INSTANCE* inst);     /* etcp_bind(ETCP_ID_RADIO) */
void radio_destroy_instance(struct UTUN_INSTANCE* inst);  /* etcp_unbind */

/* ── приём Opus-кадра пира (для аудио-движка radio_audio) ── */

/* После проверки всей пачки вызывается для каждого кадра; Opus заимствован до возврата.
 * fin=1 приходит отдельно с opus=NULL, len=0 и seq после последнего кадра. */
typedef void (*radio_frame_fn)(struct UTUN_INSTANCE* inst, uint64_t group_id,
                               const uint8_t src_ed[32], uint16_t stream_id, uint16_t seq,
                               uint8_t fin, const uint8_t* opus, int len, void* arg);
void radio_set_frame_cb(struct UTUN_INSTANCE* inst, radio_frame_fn fn, void* arg);

/* Локальные настройки по Ed25519 участника. Только uasync. */
int radio_member_muted(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t ed[32]);
/* Итог для воспроизведения: ручное отключение или просмотр трансляции источника со звуком. */
int radio_member_silenced(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t ed[32]);
/* Изменилось временное подавление; настройки в БД и подписка остаются прежними. */
void radio_refresh_member_mute(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t ed[32]);
/* Синхронный путь RAM/переходного legacy writer; RW worker отклоняется. 0 — committed,-1 — отказ.
 * Сохранить muted=0/1 для другого участника CHAT. Собственная BEGIN IMMEDIATE,чужая транзакция запрещена;
 * cache и mute callback меняются после commit. При отказе настройка/звук сохраняются;
 * неудачный rollback с открытой транзакцией запрашивает stop/закрытие БД владельцем. */
int radio_set_member_muted(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t ed[32], int muted);
/* Uasync: сохранить 0/1 либо переключить (2) одной WRITE-транзакцией; done обязателен, ровно один раз.
 * RW worker не использует legacy handle; кэш и аудио меняются после commit. 0=применено,-1=ошибка/устаревший контекст,
 * muted в done — новое значение только при успехе. До принятия/на legacy backend done может быть синхронным.
 * arg/inst живут до done/закрытия worker. Удаление группы отменяет ожидающую запись и отсоединяет начатую. */
typedef void (*radio_mute_done_fn)(void* arg, int result, int muted);
void radio_member_mute_save(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t ed[32], int muted,
    radio_mute_done_fn done, void* arg);
/* После удаления записи участника из БД удалить её из локального кэша. */
void radio_forget_member_mute(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t ed[32]);
/* Аудио-потребитель очищает накопленный звук источника при изменении настройки. */
typedef void (*radio_mute_fn)(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t ed[32], int muted, void* arg);
void radio_set_mute_cb(struct UTUN_INSTANCE* inst, radio_mute_fn fn, void* arg);

/* ── передача (PTT), uasync-поток ── */

int radio_talk_begin(struct UTUN_INSTANCE* inst, uint64_t group_id);   /* новая передача, seq=0 */
/* Копирует один Opus-кадр (1..512 байт); собирает radio_burst_frames (1..10, default4).
 * Настройка фиксируется при BEGIN. Неполная пачка уходит при END или через target*20мс от первого кадра.
 * 0 — принят/устаревший хвост после END проигнорирован, -1 — неверные аргументы/нет группы/отозван CHAT-допуск. */
int radio_talk_send(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t* opus, int len);
int radio_talk_end(struct UTUN_INSTANCE* inst, uint64_t group_id);     /* Сначала хвост DATA, затем отдельный FIN. */

/* Включить/выключить прослушивание (флаг + BGP). Первый CHAT start ждёт READ локальных mutes;
 * повтор объединяется, off немедленный и отменяет ожидающее включение. Ошибка READ оставляет подписку выключенной. */
void radio_set_active(struct UTUN_INSTANCE* inst, uint64_t group_id, int on);
int  radio_is_active(struct UTUN_INSTANCE* inst, uint64_t group_id);
/* 1 — start ожидает READ локальных настроек; 0 — включён/выключен/READ завершился ошибкой. Только RAM/uasync. */
int radio_start_pending(struct UTUN_INSTANCE* inst, uint64_t group_id);

/* Число подписчиков рации канала (без себя) — для индикатора в GUI. */
int  radio_subscribers(struct UTUN_INSTANCE* inst, uint64_t group_id);

/* Число повторов/опоздавших пачек и дублей транзита — для диагностики/тестов. */
uint32_t radio_dup_dropped(struct UTUN_INSTANCE* inst, uint64_t group_id);

/* ── trampoline-структуры (GUI/аудио-поток → uasync) ── */

struct radio_talk_arg {
    struct UTUN_INSTANCE* inst;
    uint64_t group_id;
};

struct radio_media_arg {
    struct UTUN_INSTANCE* inst;
    uint64_t group_id;
    uint16_t len;
    uint8_t  opus[RADIO_MAX_OPUS];
};

struct radio_set_active_arg {
    struct UTUN_INSTANCE* inst;
    uint64_t group_id;
    int      on;
};

void radio_talk_begin_trampoline(void* arg);
void radio_talk_end_trampoline(void* arg);
void radio_talk_send_trampoline(void* arg);
void radio_set_active_trampoline(void* arg);

#ifdef __cplusplus
}
#endif
#endif /* RADIO_H */
