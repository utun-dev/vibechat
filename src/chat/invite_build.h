/*
 * invite_build.h — сборка invite-ссылок utun:// (выбор узла + адреса)
 *
 * Общий модуль для всех GUI (desktop chatgui, Android, headless CLI).
 * Работает в uasync-потоке, использует chat_core_ctx конкретного UTUN_INSTANCE.
 * Кандидаты — полные неудалённые участники активной CHAT-группы: self либо узлы
 * с живым BGP-путём. Приоритет auto: суперузел (включая self), другой участник, self;
 * при равенстве — лексикографически меньший Ed25519. Ошибка SQLite не подменяется self.
 * Адреса берутся из общей проекции NODEINFO; нулевые IP/порты в ссылку не входят.
 */
#ifndef INVITE_BUILD_H
#define INVITE_BUILD_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct UTUN_INSTANCE;
struct chat_invite_build;
typedef void (*chat_invite_ready_fn)(void* arg, int result, const char* link);
/* Uasync, после возврата start: result=chat_join_result, ids заимствованы только на время callback.
 * При ошибке best=NULL/ids=NULL/count=0. Stop/cancel также вызывают callback ровно один раз. */
typedef void (*chat_invite_candidates_fn)(void* arg, int result, const uint8_t best[32], const uint8_t (*ids)[32], size_t count);

/* uasync: серверная ссылка с постоянным add_key и текущими опубликованными endpoints self.
 * Требует работающий чат и ключ из конфигурации. 0/-1; link получает NUL-строку при успехе.
 * Ссылка секретна: передавать только явному получателю, не писать в обычный лог. */
int chat_server_link_build(struct UTUN_INSTANCE* inst, char* link, size_t capacity);

/* Собрать invite-ссылку для канала.
 * target = NULL либо нулевой ключ → auto (достижимый суперузел → обычный узел → self).
 * target — полный Ed25519 узла (должен быть достижимым мембером канала).
 * NULL=ошибка запуска, callback не вызывается. Иначе callback после возврата,
 * ровно один раз, result из enum chat_join_result. link доступен только внутри callback,
 * только при успехе, после сохранения ключа у пригласившего.
 * Handle действителен до callback; cancel/destroy завершают запрос синхронно с CANCELLED. */
struct chat_invite_build* chat_invite_build_link(struct UTUN_INSTANCE* inst, uint64_t channel_id, const uint8_t target[32],
                                                chat_invite_ready_fn callback, void* arg);
void chat_invite_build_cancel(struct chat_invite_build* request);
/* Uasync: отменить все виды pending запросов instance до освобождения chat_core_ctx.
 * Синхронно завершает callbacks с CANCELLED; SQL-копии живут до DB drain без обращения к старому cc. Идемпотентен. */
void chat_invite_build_stop(struct UTUN_INSTANCE* inst);
/* Асинхронные запросы через общий READ worker: возвращают handle либо NULL без callback.
 * Callback после возврата, ровно один раз; handle/cancel/lifetime как у build_link.
 * max_ids=1..65535, копии аргументов принадлежат запросу. Сеть/проверка BGP только в uasync.
 * server_link_start копирует add_key/endpoints и не создаёт JOIN-ключ; ссылка секретна, не логировать. */
struct chat_invite_build* chat_invite_candidates_start(struct UTUN_INSTANCE* inst, uint64_t channel_id, unsigned max_ids,
    chat_invite_candidates_fn callback, void* arg);
struct chat_invite_build* chat_server_link_start(struct UTUN_INSTANCE* inst, chat_invite_ready_fn callback, void* arg);

/* Список достижимых узлов-кандидатов канала (включая self) — тот же фильтр, что в auto.
 * Возвращает количество (0..max_ids), либо -1; max_ids=1..65535.
 * Порядок — лексикографический порядок Ed25519, при переполнении берётся начало списка.
 * При ошибке out_ids не изменяется; присутствие self также требует полной записи. */
int chat_invite_candidate_nodes(struct UTUN_INSTANCE* inst, uint64_t channel_id, uint8_t (*out_ids)[32], int max_ids);

/* Возвращает 0 и выбранный узел; -1 при отсутствии кандидатов/ошибке, out неизменен. */
int chat_invite_best_node(struct UTUN_INSTANCE* inst, uint64_t channel_id, uint8_t out[32]);

/* Трамплин для gui_bridge (arg = u_malloc(struct chat_invite_build_req)).
 * Принимает владение arg. Канал — публичный Ed25519 группы в формате 64 строчных HEX-символов.
 * Новый корректный запрос отменяет прежнее построение и отзывает предыдущую ссылку окна.
 * Результат: CHAT_EVT_INVITE_LINK_READY; некорректный arg только логируется. */
struct chat_invite_build_req {
    struct UTUN_INSTANCE* inst;
    char     ch_id[65];
    uint64_t request_id;      /* идентификатор запроса UI, возвращается в событии */
    uint8_t target[32];        /* Ed25519 узла; нули — автоматический выбор. */
    struct chat_invite_build* pending; /* внутреннее поле trampoline */
};
void chat_invite_build_link_trampoline(void* arg);
/* Uasync: отменить построение и отозвать ссылку окна указанного запроса; 0 — для любого окна. */
void chat_invite_cancel_window(struct UTUN_INSTANCE* inst, uint64_t request);
/* Трамплин принимает владение arg; request_id принадлежит закрываемому окну. */
struct chat_invite_cancel_req { struct UTUN_INSTANCE* inst; uint64_t request_id; };
void chat_invite_cancel_window_trampoline(void* arg);

/* Трамплин (arg = u_malloc(struct chat_invite_cand_req)). Результат: CHAT_EVT_INVITE_CANDIDATES
 * (формат: [ch_id_len:1][ch_id][auto_ed25519:32][count:2LE][ed25519:32]*). */
struct chat_invite_cand_req { struct UTUN_INSTANCE* inst; char ch_id[65]; };
void chat_invite_candidates_trampoline(void* arg);

#ifdef __cplusplus
}
#endif
#endif /* INVITE_BUILD_H */
