/**
 * getmyip.h - Определение локального IP-адреса для исходящих соединений
 *
 * Определяет локальный IP, который ОС выберет для отправки
 * на заданный удаленный адрес через заданный локальный адрес.
 * Передать local/remote одного семейства AF_INET или AF_INET6 и буфер result.
 * Создаёт и закрывает временный UDP-сокет; предназначен для выбора маршрута, не для обработки каждого пакета.
 */

#ifndef GETMYIP_H
#define GETMYIP_H

#include <stdint.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Определяет локальный IP-адрес, который ОС выберет для отправки
 * на заданный удаленный адрес через заданный локальный адрес.
 *
 * Создает временный UDP-сокет, выполняет bind на копию local_addr с портом 0,
 * connect к remote_addr и получает выбранный локальный IP через getsockname.
 *
 * @param local_addr  Локальный адрес (0.0.0.0 / :: для автоопределения); входной порт игнорируется
 * @param remote_addr Удаленный адрес того же семейства (для определения маршрута)
 * @param result      Заполняемый локальный IP (порт = 0)
 * @return 0 при успехе, -1 при NULL, неподдерживаемом/разном семействе или ошибке socket/bind/connect/getsockname
 */
int get_outgoing_local_ip(const struct sockaddr_storage *local_addr,
                          const struct sockaddr_storage *remote_addr,
                          struct sockaddr_storage *result);

#ifdef __cplusplus
}
#endif

#endif
