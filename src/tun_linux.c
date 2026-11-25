// tun_linux.c - Linux TUN/TAP implementation
// Uses /dev/net/tun device with ioctl

#if defined(__linux__)

#include "tun_if.h"
#include "../lib/debug_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <net/if.h>
#include <netinet/in.h>
#include "../lib/platform_compat.h"
#include <linux/if.h>
#include <linux/if_tun.h>
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

// Create TUN device
static int create_tun_device(char *ifname, size_t ifname_len) {
    struct ifreq ifr;
    int fd;

    fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to open /dev/net/tun: %s", strerror(errno));
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;

    if (ifname && ifname_len > 0 && ifname[0] != '\0') {
        strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    }

    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to configure TUN device: %s", strerror(errno));
        close(fd);
        return -1;
    }

    if (ifname && ifname_len > 0) {
        strncpy(ifname, ifr.ifr_name, ifname_len - 1);
        ifname[ifname_len - 1] = '\0';
    }

    return fd;
}

// Set IP address on TUN interface
static int tun_set_ip(const char *ifname, const char *ip_addr) {
    if (!ifname || !ip_addr) {
        errno = EINVAL;
        return -1;
    }

    struct in_addr ip, netmask;
    if (parse_ip_mask(ip_addr, &ip, &netmask) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Invalid IP address format: %s", ip_addr);
        errno = EINVAL;
        return -1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));

    // Set IP address
    struct sockaddr_in *addr = (struct sockaddr_in *)&ifr.ifr_addr;
    addr->sin_family = AF_INET;
    addr->sin_addr = ip;
    if (if_ioctl(ifname, SIOCSIFADDR, &ifr) < 0) return -1;

    // Set netmask
    addr = (struct sockaddr_in *)&ifr.ifr_netmask;
    addr->sin_family = AF_INET;
    addr->sin_addr = netmask;
    if (if_ioctl(ifname, SIOCSIFNETMASK, &ifr) < 0) return -1;

    return 0;
}

// Bring TUN interface up
static int tun_set_up(const char *ifname) {
    if (!ifname) {
        errno = EINVAL;
        return -1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));

    // Get current flags
    if (if_ioctl(ifname, SIOCGIFFLAGS, &ifr) < 0) return -1;

    // Set UP and RUNNING
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    if (if_ioctl(ifname, SIOCSIFFLAGS, &ifr) < 0) return -1;

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

    if (if_ioctl(ifname, SIOCSIFMTU, &ifr) < 0) return -1;

    return 0;
}

// Platform-specific initialization for Linux
int tun_platform_init(struct tun_if* tun, const char* ifname, const char* ip_str, int mtu) {
    (void)ifname;  // ifname is stored in tun->ifname by caller
    
    // Create TUN device
    tun->fd = create_tun_device(tun->ifname, sizeof(tun->ifname));
    if (tun->fd < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to create TUN device");
        return -1;
    }

    // Configure IP address
    if (tun_set_ip(tun->ifname, ip_str) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to set TUN IP: %s", ip_str);
        close(tun->fd);
        tun->fd = -1;
        return -1;
    }

    // Set MTU
    if (tun_set_mtu(tun->ifname, mtu) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to set MTU %d on %s: %s", mtu, tun->ifname, strerror(errno));
        goto fail;
    }

    // Bring interface up
    if (tun_set_up(tun->ifname) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to bring up TUN interface %s", tun->ifname);
        close(tun->fd);
        tun->fd = -1;
        return -1;
    }
    
    tun->ifindex = if_nametoindex(tun->ifname);
    if (tun->ifindex == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to get ifindex for %s", tun->ifname);
        goto fail;
    }

    // Set non-blocking mode
    int flags = fcntl(tun->fd, F_GETFL, 0);
    if (flags < 0 || fcntl(tun->fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to set non-blocking mode on %s: %s", tun->ifname, strerror(errno));
        goto fail;
    }

    return 0;
fail:
    tun_platform_cleanup(tun); return -1;
}

// Platform-specific cleanup for Linux
void tun_platform_cleanup(struct tun_if* tun) {
    if (tun->fd >= 0) {
        close(tun->fd);
        tun->fd = -1;
    }
}

// Platform-specific read for Linux
ssize_t tun_platform_read(struct tun_if* tun, uint8_t* buf, size_t len) {
    ssize_t nread = read(tun->fd, buf, len);
    if (nread < 0 && errno == EINTR) {
        return 0;  // Interrupted, not an error
    }
    return nread;
}

// Platform-specific write for Linux
ssize_t tun_platform_write(struct tun_if* tun, const uint8_t* buf, size_t len) {
    return write(tun->fd, buf, len);
}

// Get poll fd for uasync
int tun_platform_get_poll_fd(struct tun_if* tun) {
    return tun->fd;
}

#endif // __linux__
