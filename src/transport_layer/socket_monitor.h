/* socket_monitor — наблюдение за адресами интерфейсов и маршрутами ОС для ETCP-сокетов.
 * init подключает уведомления ОС к uasync экземпляра; изменения обновляют interface_addr и сетевое состояние.
 * auto_socket отдельно создаёт/удаляет сокеты. destroy отключает наблюдение до освобождения экземпляра.
 * Публичные операции и изменения ETCP_SOCKET — в потоке uasync.
 * События используют снимок ID/поколений: callback может удалить/заменить endpoint.
 * Сам monitor/instance нельзя уничтожать из его callback. Диагностика — категория socket. */
#ifndef SOCKET_MONITOR_H
#define SOCKET_MONITOR_H

#ifdef __cplusplus
extern "C" {
#endif

struct UTUN_INSTANCE;
struct ETCP_SOCKET;

/* instance и его ua обязательны; повторный init — ошибка. 0 при успехе, -1 при отказе с откатом ресурсов.
 * В режиме auto_sockets=android адресами управляет frontend; нативный монитор не создаётся.
 * Linux: NETLINK_ROUTE; FreeBSD: PF_ROUTE; Windows: IP Helper. На прочих ОС — 0 без монитора. */
int  socket_monitor_init(struct UTUN_INSTANCE* instance);

/* Снимает регистрацию/закрывает native socket или ждёт завершения IP Helper callbacks
 * и отменяет ожидающую задачу uasync. NULL/отсутствующий монитор допустимы.
 * Только вне callbacks монитора; ua остаётся жив. Если Windows не смогла отменить native подписку,
 * отключённый контекст сохраняется с ERROR-логом, чтобы поздний callback не использовал освобождённую память. */
void socket_monitor_destroy(struct UTUN_INSTANCE* instance);

/* Обновляет interface_addr без событий: адрес привязанного интерфейса либо default source
 * для wildcard bind; явный bind без netif_index сохраняет свой IP. Порт берёт из local_addr.
 * При потере адреса очищает старый IP; IPv6 link-local получает scope интерфейса.
 * es заимствован; NULL логируется. Вызывать из uasync, в том числе из auto_socket. */
void socket_monitor_update_if_addr(struct ETCP_SOCKET* es);

#ifdef __cplusplus
}
#endif
#endif
