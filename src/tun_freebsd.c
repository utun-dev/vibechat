// tun_freebsd.c - FreeBSD TUN/TAP implementation
// Uses /dev/tun device with ioctl
#if defined(__FreeBSD__)
#include <sys/sockio.h>        // SIOCIFDESTROY
#include "tun_if.h"
#include "../lib/debug_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if_tun.h> // Для TUNSIFINFO, TUNSIFMODE, TUNSIFHEAD
#include <libutil.h> // Для devname()
#include "../lib/platform_compat.h"
#include <errno.h>
#include "../lib/mem.h"
// Helper for interface ioctl operations
static int if_ioctl(const char *ifname, unsigned long request, struct ifreq *ifr) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to create ioctl socket: %s", strerror(errno));
        return -1;
    }
    strncpy(ifr->ifr_name, ifname, IFNAMSIZ - 1);
    ifr->ifr_name[IFNAMSIZ - 1] = '\0';
    int ret = ioctl(sock, request, ifr);
    if (ret < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "ioctl failed for %s (request=0x%lx): %s", ifname, request, strerror(errno));
    }
    close(sock);
    return ret;
}
// Parse IP address and CIDR mask
static int parse_ip_mask(const char *ip_addr, struct in_addr *ip, struct in_addr *netmask) {
    if (!ip_addr || !ip || !netmask) return -1;
    char ip_str[INET_ADDRSTRLEN];
    char *slash = strchr(ip_addr, '/');
    if (!slash) return -1;
    size_t ip_len = slash - ip_addr;
    if (ip_len >= sizeof(ip_str)) return -1;
    strncpy(ip_str, ip_addr, ip_len);
    ip_str[ip_len] = '\0';
    if (inet_pton(AF_INET, ip_str, ip) <= 0) return -1;
    char *endptr;
    long cidr = strtol(slash + 1, &endptr, 10);
    if (*endptr != '\0' || cidr < 0 || cidr > 32) return -1;
    uint32_t mask = (cidr == 0) ? 0 : ~((1U << (32 - cidr)) - 1);
    netmask->s_addr = htonl(mask);
    return 0;
}
// Create TUN device.
// Если имя оканчивается на цифру (tun4, tun0) — создаём интерфейс с этим именем
// через SIOCIFCREATE. Иначе (tun или пусто) — клонирующее устройство /dev/tun,
// ядро само выдаёт следующий свободный номер.
static int create_tun_device(char *ifname, size_t ifname_len) {
    int fd;
    size_t name_len = ifname ? strlen(ifname) : 0;
    int manual = name_len > 0 && isdigit((unsigned char)ifname[name_len - 1]);

    if (manual) {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to create ioctl socket: %s", strerror(errno));
            return -1;
        }
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
        ifr.ifr_name[IFNAMSIZ - 1] = '\0';
        int created = 1;
        if (ioctl(sock, SIOCIFCREATE, &ifr) < 0) {
            if (errno == EEXIST) {
                created = 0;
                DEBUG_INFO(DEBUG_CATEGORY_TUN, "TUN interface %s already exists, reusing it", ifname);
            } else {
                DEBUG_ERROR(DEBUG_CATEGORY_TUN, "SIOCIFCREATE failed for %s: %s", ifname, strerror(errno));
                close(sock);
                return -1;
            }
        }
        char dev_path[IFNAMSIZ + 6];
        snprintf(dev_path, sizeof(dev_path), "/dev/%s", ifname);
        fd = open(dev_path, O_RDWR);
        if (fd < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to open %s: %s", dev_path, strerror(errno));
            if (created && ioctl(sock, SIOCIFDESTROY, &ifr) < 0)
                DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to roll back interface %s: %s", ifname, strerror(errno));
            close(sock);
            return -1;
        }
        close(sock);
        DEBUG_INFO(DEBUG_CATEGORY_TUN, "Opened %s (fd=%d)", dev_path, fd);
        return fd;
    }

    // Открываем cloning device /dev/tun
    fd = open("/dev/tun", O_RDWR);
    if (fd < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to open /dev/tun: %s", strerror(errno));
        return -1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_TUN, "Opened cloning device /dev/tun (fd=%d)", fd);
    // Получаем имя созданного интерфейса через devname()
    // devname() возвращает имя устройства по major/minor номерам
    struct stat st;
    if (fstat(fd, &st) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "fstat failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    const char *dev_name = devname(st.st_rdev, S_IFCHR);
    if (!dev_name) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "devname failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    // dev_name будет в формате "tunN" или "/dev/tunN"
    const char *base_name = dev_name;
    if (strncmp(dev_name, "/dev/", 5) == 0) {
        base_name = dev_name + 5;
    }
    strncpy(ifname, base_name, ifname_len - 1);
    ifname[ifname_len - 1] = '\0';
    DEBUG_INFO(DEBUG_CATEGORY_TUN, "Created TUN device: %s", ifname);
    return fd;
}
// Configure TUN device mode (POINTOPOINT + MULTICAST)
static int tun_set_mode(int fd, const char *ifname) {
    int mode = IFF_POINTOPOINT | IFF_MULTICAST;
    if (ioctl(fd, TUNSIFMODE, &mode) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUNSIFMODE failed for %s: %s", ifname, strerror(errno));
        return -1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_TUN, "Set TUNSIFMODE (POINTOPOINT | MULTICAST) on %s", ifname);
    return 0;
}
// Enable TUNSIFHEAD mode (multi-af, 4-byte header with address family)
static int tun_set_head(int fd, const char *ifname) {
    int head = 1;
    if (ioctl(fd, TUNSIFHEAD, &head) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUNSIFHEAD failed for %s: %s", ifname, strerror(errno));
        return -1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_TUN, "Set TUNSIFHEAD on %s", ifname);
    return 0;
}
// Сформировать dst-адрес point-to-point по номеру интерфейса: tunN -> 192.0.2.(N+1)
static void tun_dst_addr(const char *ifname, char *dst, size_t dst_len) {
    const char *p = ifname + strlen(ifname);
    while (p > ifname && isdigit((unsigned char)p[-1])) p--;
    unsigned long n = strtoul(p, NULL, 10);
    if (p == ifname + strlen(ifname) || n > 253) {
        n = 253; // нет цифр в имени (не должно быть на FreeBSD) или переполнение диапазона
    }
    snprintf(dst, dst_len, "192.0.2.%lu", n + 1);
}
// Set IP address on TUN interface using ifconfig
// FreeBSD требует отдельно IP и netmask для TUN
// For point-to-point, require destination address, set to same as local for /32
static int tun_set_ip(const char *ifname, const char *ip_addr) {
    if (!ifname || !ip_addr) {
        errno = EINVAL;
        return -1;
    }
    // Уникальный dst-адрес на каждый интерфейс: tunN -> 192.0.2.(N+1)
    char dst_str[INET_ADDRSTRLEN];
    tun_dst_addr(ifname, dst_str, sizeof(dst_str));
    // Парсим IP и CIDR
    char ip_str[INET_ADDRSTRLEN] = {0};
    int cidr = -1;
    char *slash = strchr(ip_addr, '/');
    if (slash) {
        size_t ip_len = slash - ip_addr;
        if (ip_len >= sizeof(ip_str)) {
            DEBUG_ERROR(DEBUG_CATEGORY_TUN, "IP address too long");
            return -1;
        }
        strncpy(ip_str, ip_addr, ip_len);
        ip_str[ip_len] = '\0';
        char *endptr;
        cidr = (int)strtol(slash + 1, &endptr, 10);
        if (*endptr != '\0' || cidr < 0 || cidr > 32) {
            DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Invalid CIDR: %s", slash + 1);
            return -1;
        }
    } else {
        strncpy(ip_str, ip_addr, sizeof(ip_str) - 1);
        cidr = 32; // По умолчанию /32 для POINTOPOINT
    }
    // Вычисляем netmask из CIDR
    struct in_addr netmask;
    if (cidr == 0) {
        netmask.s_addr = 0;
    } else {
        netmask.s_addr = htonl(~((1U << (32 - cidr)) - 1));
    }
    char mask_str[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &netmask, mask_str, sizeof(mask_str)) == NULL) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "inet_ntop failed for netmask");
        return -1;
    }
    // Формируем команду ifconfig с destination address (same as local for /32)
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ifconfig %s inet %s %s netmask %s up",
             ifname, ip_str, dst_str, mask_str);
    DEBUG_INFO(DEBUG_CATEGORY_TUN, "Executing: %s", cmd);
    int ret = system(cmd);
    if (ret != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "ifconfig failed for %s with IP %s dst %s netmask %s",
                    ifname, ip_str, dst_str, mask_str);
        // Fallback: пробуем через ioctl напрямую
        DEBUG_INFO(DEBUG_CATEGORY_TUN, "Trying ioctl fallback...");
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_TUN, "socket failed: %s", strerror(errno));
            return -1;
        }
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
        // Устанавливаем адрес
        struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
        sin->sin_family = AF_INET;
        if (inet_pton(AF_INET, ip_str, &sin->sin_addr) <= 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_TUN, "inet_pton failed for %s", ip_str);
            close(sock);
            return -1;
        }
        if (ioctl(sock, SIOCSIFADDR, &ifr) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_TUN, "SIOCSIFADDR failed: %s", strerror(errno));
            close(sock);
            return -1;
        }
        // Устанавливаем destination address (уникальный на каждый интерфейс)
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
        struct sockaddr_in *sin_dst = (struct sockaddr_in *)&ifr.ifr_dstaddr;
        sin_dst->sin_family = AF_INET;
        inet_pton(AF_INET, dst_str, &sin_dst->sin_addr);
        if (ioctl(sock, SIOCSIFDSTADDR, &ifr) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_TUN, "SIOCSIFDSTADDR failed: %s", strerror(errno));
            close(sock);
            return -1;
        }
        // Устанавливаем netmask через ifr_addr (union в ifreq)
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
        sin = (struct sockaddr_in *)&ifr.ifr_addr;
        sin->sin_family = AF_INET;
        sin->sin_addr = netmask;
        if (ioctl(sock, SIOCSIFNETMASK, &ifr) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_TUN, "SIOCSIFNETMASK failed: %s", strerror(errno));
            close(sock);
            return -1;
        }
        // Поднимаем интерфейс
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
        if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_TUN, "SIOCGIFFLAGS failed: %s", strerror(errno));
            close(sock);
            return -1;
        }
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        if (ioctl(sock, SIOCSIFFLAGS, &ifr) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_TUN, "SIOCSIFFLAGS failed: %s", strerror(errno));
            close(sock);
            return -1;
        }
        close(sock);
        DEBUG_INFO(DEBUG_CATEGORY_TUN, "Successfully configured %s (ip=%s dst=%s) via ioctl", ifname, ip_str, dst_str);
        return 0;
    }
    DEBUG_INFO(DEBUG_CATEGORY_TUN, "Set IP %s/%d dst %s on %s", ip_str, cidr, dst_str, ifname);
    return 0;
}
// Set MTU on TUN interface
static int tun_set_mtu(const char *ifname, int mtu) {
    if (!ifname || mtu <= 0) {
        errno = EINVAL;
        return -1;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_mtu = mtu;
    if (if_ioctl(ifname, SIOCSIFMTU, &ifr) < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_TUN, "Failed to set MTU %d on %s: %s", mtu, ifname, strerror(errno));
        return -1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_TUN, "Set MTU %d on %s", mtu, ifname);
    return 0;
}
// Platform-specific initialization for FreeBSD
int tun_platform_init(struct tun_if* tun, const char* ifname, const char* ip_str, int mtu) {
    (void)ifname; // имя уже в tun->ifname, передаётся в create_tun_device
    // Create TUN device
    tun->fd = create_tun_device(tun->ifname, sizeof(tun->ifname));
    if (tun->fd < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to create TUN device");
        return -1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_TUN, "TUN interface created: %s (fd=%d)", tun->ifname, tun->fd);
    // Устанавливаем режим POINTOPOINT (должно быть ДО поднятия интерфейса)
    if (tun_set_mode(tun->fd, tun->ifname) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to set TUN mode on %s", tun->ifname);
        goto fail;
    }
    // Включаем TUNSIFHEAD для multi-af режима
    if (tun_set_head(tun->fd, tun->ifname) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to set TUN head mode on %s", tun->ifname);
        goto fail;
    }
    // Configure IP address (это также поднимает интерфейс)
    if (tun_set_ip(tun->ifname, ip_str) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to set TUN IP: %s", ip_str);
        goto fail;
    }
    // Set MTU
    if (tun_set_mtu(tun->ifname, mtu) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to set MTU %d on %s", mtu, tun->ifname);
        goto fail;
    }
    // Получаем ifindex
    tun->ifindex = if_nametoindex(tun->ifname);
    if (tun->ifindex == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to get ifindex for %s", tun->ifname);
        goto fail;
    } else {
        DEBUG_INFO(DEBUG_CATEGORY_TUN, "Interface %s has index %d", tun->ifname, tun->ifindex);
    }
    // Set non-blocking mode
    if (fcntl(tun->fd, F_SETFL, O_NONBLOCK) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to set non-blocking mode: %s", strerror(errno));
        goto fail;
    }
    return 0;
fail:
    tun_platform_cleanup(tun); return -1;
}
// Platform-specific cleanup for FreeBSD
void tun_platform_cleanup(struct tun_if* tun) {
    if (!tun) return;

    if (tun->fd >= 0) {
        close(tun->fd);
        tun->fd = -1;
        DEBUG_INFO(DEBUG_CATEGORY_TUN, "Closed TUN fd for %s", tun->ifname);
    }

    if (tun->ifname[0] != '\0') {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock >= 0) {
            struct ifreq ifr = {0};
            strncpy(ifr.ifr_name, tun->ifname, IFNAMSIZ - 1);
            ifr.ifr_name[IFNAMSIZ - 1] = '\0';

            if (ioctl(sock, SIOCIFDESTROY, &ifr) < 0) {
                // ENXIO / ENOENT = interface already gone (normal after close)
                if (errno != ENXIO && errno != ENOENT) {
                    DEBUG_WARN(DEBUG_CATEGORY_TUN,
                               "SIOCIFDESTROY failed for %s: %s (errno=%d)",
                               tun->ifname, strerror(errno), errno);
                } else {
                    DEBUG_DEBUG(DEBUG_CATEGORY_TUN, "TUN interface %s already destroyed", tun->ifname);
                }
            } else {
                DEBUG_INFO(DEBUG_CATEGORY_TUN, "Successfully destroyed TUN interface %s", tun->ifname);
            }
            close(sock);
        } else DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUN %s destroy socket failed: %s", tun->ifname, strerror(errno));
    }
}
// Platform-specific read for FreeBSD
// Возвращает данные без 4-байтового заголовка (TUNSIFHEAD обрабатывается ядром)
ssize_t tun_platform_read(struct tun_if* tun, uint8_t* buf, size_t len) {
    uint32_t family;
    struct iovec iv[2];
    // Читаем family + данные
    iv[0].iov_base = &family;
    iv[0].iov_len = sizeof(family);
    iv[1].iov_base = buf;
    iv[1].iov_len = len;
    ssize_t nread = readv(tun->fd, iv, 2);
    if (nread < 0) {
        if (errno == EINTR) {
            return 0;
        }
        return -1;
    }
    // Возвращаем только размер данных (без family)
    if (nread <= (ssize_t)sizeof(family)) {
        return 0;
    }
    return nread - sizeof(family);
}
// Platform-specific write for FreeBSD
// Добавляет 4-байтовый заголовок с family перед записью
ssize_t tun_platform_write(struct tun_if* tun, const uint8_t* buf, size_t len) {
    uint32_t family = htonl(AF_INET); // Предполагаем IPv4, можно определять по заголовку пакета
    struct iovec iv[2];
    iv[0].iov_base = &family;
    iv[0].iov_len = sizeof(family);
    iv[1].iov_base = (void*)buf;
    iv[1].iov_len = len;
    ssize_t wr = writev(tun->fd, iv, 2);
    return wr > 0 ? wr - 4 : wr;   // вычесть 4-байтовый AF_INET заголовок
}
// Get poll fd for uasync
int tun_platform_get_poll_fd(struct tun_if* tun) {
    return tun->fd;
}
#endif // __FreeBSD__
