/* conn_mgr — оптимальная P2P-доставка больших объёмов данных: медиа, файлы, звонки.
 * Один менеджер принадлежит одной группе; DIRECT/REVERSE/INDIRECT подбирают путь к получателю.
 *
 * Архитектурная роль CONN_MGR_HANDLE:
 *   - принадлежит сервису и выражает его потребность в доставке данных конкретному узлу;
 *   - существует, пока идёт звонок или передача файла; владелец закрывает его по окончании/отмене;
 *   - потеря пути и TIMEOUT попытки не освобождают handle и не отменяют потребность владельца;
 *   - не является NCD handle и не привязан временем жизни к одному ETCP_CONN.
 *
 * NCD обеспечивает общий прямой мультиадресный транспорт для BGP/topo_group.
 * CM удерживает свой NCD handle для позднего UP и мониторинга прямого пути;
 * группа приобретает независимое NCD-владение при согласовании своей сессии.
 * CM handle остаётся у медиа-сервиса. Закрытие CM handle отменяет его запрос, а не владение группы.
 * Групповые сессии открывают через topo_group_peer_open(). Все операции — в потоке uasync. */
#ifndef CONN_MGR_H
#define CONN_MGR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "../lib/ll_queue.h"
#include "topo_node.h"

struct CONN_MGR;           /* opaque */
struct CONN_MGR_HANDLE;    /* opaque */
struct UTUN_INSTANCE;
struct ETCP_CONN;
struct TOPO_GROUP;

enum conn_mgr_event {
    CONN_EVENT_UP      = 0,  /* связь есть. handle жив, можно отправлять данные               */
    CONN_EVENT_DOWN    = 1,  /* все пути потеряны; восстановление продолжается, пока жив handle */
    CONN_EVENT_TIMEOUT = 2,  /* подключение не удалось или допуск отозван; handle закрывает владелец */
};

/**
 * Единый коллбэк для всех операций.
 * @param h        handle (NULL при ошибках до создания handle)
 * @param ed       Ed25519 целевого узла, заимствован до возврата callback
 * @param group_id группа
 * @param event    UP / DOWN / TIMEOUT
 */
typedef void (*conn_mgr_cb_t)(struct CONN_MGR_HANDLE* h,
                               const uint8_t ed[32], uint64_t group_id,
                               enum conn_mgr_event event, void* arg);

/* ═══════ lifecycle ═══════ */

/* Создать менеджер группы с обоими фоновыми таймерами; NULL с очисткой ресурсов при ошибке. */
struct CONN_MGR* conn_mgr_init(struct TOPO_GROUP* group);
/* Остановить проверки и освободить менеджер вне его callbacks.
 * Живые клиентские handles отсоединяются, callbacks отменяются; владельцы закрывают их сами. */
void conn_mgr_destroy(struct CONN_MGR* mgr);
/* После commit членства CHAT: TIMEOUT запрещённым владельцам, отмена попыток и отсоединение handles.
 * Владельцы закрывают handles сами; транспорт других групп сохраняется. UTUN не затрагивается. */
void conn_mgr_revalidate_members(struct CONN_MGR* mgr);
/* Приём управляющего пакета от etcp_router; забирает entry. */
void conn_mgr_router_recv_handler(struct ETCP_CONN* conn, struct ll_entry* entry);

/* ═══════ единая точка входа ═══════ */

/**
 * Параллельные пробы: DIRECT сразу, REVERSE через 0/500мс, INDIRECT через 0/1000мс от общего старта.
 * Посредник используется сразу после проверки; поздний прямой путь заменяет его без повторного UP.
 * Возврат 0 — запрос принят, -1 — ошибка запуска. cb обязателен, cb_arg заимствован.
 * Группа должна существовать; для CHAT требуется актуальное членство self/peer до запуска попыток.
 * При отсутствии узла загружает запись из общей SQLite во временный групповой узел;
 * для файловой БД чтение асинхронное: handle уже принят, пробы начинаются после done и повторной проверки допуска.
 * Закрытие последнего handle/менеджера отменяет ожидание; ошибка чтения завершает его TIMEOUT.
 * отказ запуска очищает приобретённые ссылки. Последний CM-владелец убирает такой
 * узел, если у него не появились BGP-пути.
 * Ошибка до запуска может возвращаться без callback. В callback h может быть NULL.
 * out_handle необязателен: иначе вызывающий получает handle из callback.
 * При ошибке *out_handle=NULL. Успешный handle закрывает вызывающий, в том числе после TIMEOUT.
 */
int conn_mgr_open(struct UTUN_INSTANCE* inst,
                  uint64_t group_id,
                  const uint8_t ed[32],
                  conn_mgr_cb_t cb, void* cb_arg,
                  struct CONN_MGR_HANDLE** out_handle);

/** Закрыть свой handle и отменить его callbacks, в том числе отложенный UP.
 * Допустимо из callback (также закрытие другого handle). После close указатель больше не использовать.
 * Очистка внутренней записи CM отсоединяет handles, но владельцы всё равно обязаны вызвать close.
 * Последний handle снимает запись CM; чужие NCD-владельцы сохраняют транспорт.
 * Освобождение памяти закрытых объектов выполняют фоновые callbacks/destroy менеджера,
 * без новых выделений памяти в close. После destroy отсоединённый handle допускает только close. */
void conn_mgr_close(struct CONN_MGR_HANDLE* h);

/* ═══════ доступ к соединению ═══════ */

/** Заимствованный READY прямой ETCP_CONN; NULL для INDIRECT, ожидания или отсоединённого handle. */
struct ETCP_CONN* conn_mgr_get_conn(struct CONN_MGR_HANDLE* h);

/* Данные сервиса отправляются через etcp_route_send/etcp_router_conn_send с его svc_id.
 * CM handle всё это время сохраняет потребность в доставке; политика сервиса задаёт шифрование. */

/* ═══════ мониторинг (вызывается извне) ═══════ */

/** Обновить кандидата (≤3, сортировка по RTT, затем Ed25519); собственный ключ и RTT=0/FFFF игнорируются.
 * При заполнении списка READY-кандидаты вытесняют недоступных независимо от прежнего RTT. */
void conn_mgr_update_best_candidates(struct CONN_MGR* mgr, const uint8_t ed[32], uint16_t rtt);

/** Запланировать перепроверку путей после изменения маршрутов или READY групповых сессий. */
void conn_mgr_routes_changed(struct CONN_MGR* mgr);

/** Изменились локальные сокеты: начать восстановление без ожидания backoff, сохраняя работающие пути. */
void conn_mgr_network_changed(struct CONN_MGR* mgr);

/** Заимствованный next hop: READY прямой путь/подтверждённый посредник; allow_probe — только для управляющего обмена CM. */
struct ETCP_CONN* conn_mgr_route_conn(struct CONN_MGR* mgr, const uint8_t ed[32], int allow_probe);

/** Тип живого транспорта к Ed25519: исходящий DIRECT, только входящие REVERSE, другой next hop INDIRECT. */
uint8_t conn_mgr_connection_type(const struct ETCP_CONN* conn, const uint8_t ed[32]);

#ifdef __cplusplus
}
#endif
#endif
