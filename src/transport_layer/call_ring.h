// call_ring.h — статический кольцевой трейс вызовов (диагностика UAF в teardown)
//
// Каждая контрольная точка пишет в кольцевой буфер текущего потока номер (code) и два
// указателя-объекта (a, b). По последовательности записей + адресам из core-дампа
// можно размотать, кто кого вызвал и кто освободил линк без обнуления обратных ссылок.
#ifndef CALL_RING_H
#define CALL_RING_H

#include <stdint.h>
#include <stddef.h>

#define CALLRING_SIZE 1024

typedef struct {
    uint32_t  code;   // код контрольной точки (число)
    uintptr_t a;      // объект 1 (link/sl/conn/etcp)
    uintptr_t b;      // объект 2 (close_arg/tcp_link/etcp_link)
} callring_entry_t;

/* TLS отделяет одновременно работающие uasync-потоки; без аллокаций и блокировок.
 * Индекс циклический uint32_t, count насыщается на CALLRING_SIZE; переменные доступны для core dump. */
extern __thread callring_entry_t g_callring[CALLRING_SIZE];
extern __thread uint32_t g_callring_idx;
extern __thread uint32_t g_callring_count;

/* Добавляет code/a/b без обращения по указателям a/b. Перезаписывает старейшую запись.
 * Асинхронный сигнал может прервать запись: аварийный dump остаётся best effort. */
static inline void callring_put(uint32_t code, uintptr_t a, uintptr_t b) {
    callring_entry_t *e = &g_callring[g_callring_idx % CALLRING_SIZE];
    e->code = code; e->a = a; e->b = b;
    g_callring_idx++;
    if (g_callring_count < CALLRING_SIZE) g_callring_count++;
}

/* Последние count записей текущего потока в хронологическом порядке.
 * POSIX: /tmp/utun_callring.<pid> с перезаписью, при отказе — stderr; Windows: stderr.
 * Ошибки I/O отражаются напрямую в stderr (обычный logger в crash-handler не используется).
 * Собственных аллокаций нет; snprintf не даёт общей гарантии async-signal-safety libc. */
void callring_dump(void);

/* коды контрольных точек */
enum {
    CR_LINK_NEW        =  1,  /* etcp_link_new → a=link b=etcp */
    CR_LINK_CLOSE_IN   =  2,  /* etcp_link_close вход → a=link b=etcp */
    CR_LINK_CLOSE_TCP  =  3,  /* etcp_link_close is_tcp-ветка → a=tcp_link b=link */
    CR_LINK_CLOSE_FREE =  4,  /* etcp_link_close перед u_free → a=link */
    CR_TCPCB_IN        =  5,  /* tcp_link_close_cb вход → a=sl b=arg(link) */
    CR_TCPCB_FREE      =  6,  /* tcp_link_close_cb → etcp_link_close → a=link */
    CR_SRV_ONLINK      =  7,  /* tcp_server_on_link → a=tlink b=sl */
    CR_SLINK_CLOSE     = 10,  /* stcp_link_close → a=sl b=etcp_link */
    CR_SLINK_IMPL      = 11,  /* stcp_link_close_impl → a=sl b=conn */
    CR_SLINK_ONSTCP    = 12,  /* stcp_link_on_stcp_close → a=conn b=arg(sl) */
    CR_CONN_SETONCLOSE = 20,  /* stcp_conn_set_on_close → a=c b=arg */
    CR_CONN_DOCLOSE_IN = 21,  /* stcp_conn_do_close вход → a=c */
    CR_CONN_DOCLOSE_CB = 22,  /* stcp_conn_do_close перед cb → a=c b=close_arg */
    CR_CONN_FREE       = 23,  /* stcp_conn_free → a=c */
    CR_CCLOSE_IN       = 30,  /* etcp_connection_close вход → a=etcp */
    CR_CFREE_RES       = 31,  /* etcp_connection_free_resources → a=etcp */
    CR_NCD_DEFER       = 40,  /* зарезервирован, текущий NCD не пишет эту точку */
};

#endif /* CALL_RING_H */
