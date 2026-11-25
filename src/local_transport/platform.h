/* Внутренний API ОС (Linux/FreeBSD/Windows): снимок LAN-адресов и метаданные входящего UDP.
 * local_interfaces даёт привязки для сокетов отправки; local_receive_options включается до local_receive.
 * Полученный ifindex используется для выбора интерфейса; local_on_interface проверяет только IP/порт,
 * поэтому сопоставление ifindex и выставление IPv6 scope выполняет вызывающий код.
 * Указатели обязательны, массивы/буферы принадлежат вызывающему коду; диагностика — discovery. */
#ifndef LOCAL_TRANSPORT_PLATFORM_H
#define LOCAL_TRANSPORT_PLATFORM_H

#include "../../lib/platform_compat.h"
#include "../../lib/socket_compat.h"

#define LOCAL_INTERFACE_MAX 64          // вместимость снимка, используемая транспортом
#define LOCAL_MULTICAST6 "ff12::7574:756e"

struct local_interface {
    uint32_t index;                     // индекс интерфейса ОС; задаёт IPv6 scope
    struct sockaddr_storage address;   // локальный unicast IP, порт 0 для отдельного сокета отправки
    struct sockaddr_storage broadcast; // broadcast IPv4 или multicast IPv6 с фиксированным discovery-портом
    uint32_t mask4;                     // IPv4 netmask в network byte order; 0 для IPv6
};

/* Заполнить out[capacity] снимком UP LAN-адресов: IPv4 с маской != /0,/32 и IPv6 link-local.
 * POSIX: без loopback/P2P, IPv4 требует IFF_BROADCAST, IPv6 — IFF_MULTICAST.
 * Windows: без software-loopback/PPP/tunnel. IPv4-адреса одного ifindex различаются по IP;
 * IPv6 — не более одного link-local на ifindex. Порядок определяется ОС.
 * Возврат: число записанных (<= capacity), -1 при ошибке. Переполнение capacity логируется, лишнее пропускается.
 * На ошибке out не является новым снимком; успешный результат 0 допустим. */
int local_interfaces(struct local_interface* out, size_t capacity);
/* Включить ancillary ifindex для UDP fd семейства AF_INET/AF_INET6; иные family не поддерживаются.
 * Возврат 0/-1 с диагностикой ошибки setsockopt; fd остаётся у вызывающего кода. */
int local_receive_options(socket_t fd, int family);
/* Прочитать одну датаграмму в data[capacity]; >=0 — размер, включая 0 для пустого UDP.
 * -2 = усечены данные или ancillary metadata (пакет потреблён), -1 = ошибка ОС, включая would-block/EINTR.
 * source/index обнуляются перед чтением; при успехе source содержит IP/порт, index — ingress (0 без metadata).
 * При отрицательном результате выходы нельзя считать пакетом; ошибка ОС доступна через socket_get_error().
 * Сам не логирует ошибки чтения; caller различает ожидаемый would-block и реальные ошибки.
 * IPv6 scope не нормализуется; проверку допустимости источника выполняет caller. */
ssize_t local_receive(socket_t fd, void* data, size_t capacity, struct sockaddr_storage* source, uint32_t* index);
/* Проверить IP/порт источника для iface из local_interfaces: одинаковое семейство, ненулевой UDP-порт.
 * IPv4: та же подсеть, без нулевого IP, limited/subnet broadcast и адреса сети; IPv6: только link-local.
 * 1 = подходит, иначе 0. ifindex/scope и равенство порта discovery-порту здесь не проверяются. */
int local_on_interface(const struct local_interface* iface, const struct sockaddr_storage* source);

#endif
