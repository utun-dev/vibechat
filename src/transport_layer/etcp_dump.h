/* etcp_dump — снимки состояния ETCP-соединений/линков, очередей и сокетов экземпляра.
 * Вызывается в uasync-потоке, пишет DEBUG категории etcp_dump; не меняет состояние и не владеет объектами.
 * conn_state выводит один conn, all_conns/sockets — соответствующие списки, all — оба списка. */
#ifndef ETCP_DUMP_H
#define ETCP_DUMP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "etcp.h"
#include "utun_instance.h"

void etcp_dump_conn_state(struct ETCP_CONN* conn);
void etcp_dump_all_conns(struct UTUN_INSTANCE* instance);
void etcp_dump_sockets(struct UTUN_INSTANCE* instance);
void etcp_dump_all(struct UTUN_INSTANCE* instance);

#ifdef __cplusplus
}
#endif
#endif
