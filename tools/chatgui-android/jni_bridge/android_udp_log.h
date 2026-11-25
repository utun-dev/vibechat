/*
 * Android UDP-лог: set_target → attach к uasync → write → detach → stop.
 * Общий буфер защищён mutex; send неблокирующий, доставка без повторов.
 * Flush при 16KiB, переполнении либо каждые 200мс при привязанном uasync.
 * Один IPv4 datagram содержит до 65507 байтов, строки длиннее отбрасываются целиком.
 * Ошибки идут в utun-udplog/stderr без debug hook, чтобы не создавать рекурсию.
 */
#ifndef ANDROID_UDP_LOG_H
#define ANDROID_UDP_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

struct UASYNC;

/* Только поток ядра: attach после create, detach перед каждым destroy/restart. */
void udp_log_attach(struct UASYNC* ua);
void udp_log_detach(struct UASYNC* ua);

/* Любой поток: ip — числовой IPv4, port=1..65535; неверный адрес/порт выключает отправку.
 * Старый буфер отправляется прежнему адресату; таймер обновляется posted-командой в uasync. */
void udp_log_set_target(const char* ip, int port);

/* Любой поток: копирует len байтов (включая нужные caller разделители); без target ничего не сохраняет. */
void udp_log_write(const char* data, int len);

/* Любой поток: отправить накопленное немедленно; ошибка отправки также очищает буфер. */
void udp_log_flush(void);

/* После shutdown/detach: flush, закрыть сокет и выключить target; stop сам не снимает uasync-таймер. */
void udp_log_stop(void);

#ifdef __cplusplus
}
#endif
#endif /* ANDROID_UDP_LOG_H */
