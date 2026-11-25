#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

#define MAX_DATA_SIZE 65500
#define MAX_PINGS_PER_BURST 10000
#define RECV_TIMEOUT_SEC 5

static char *host = NULL;
static int data_size_min = 56;
static int data_size_max = 56;
static int pings_per_burst = 10;
static int bursts = 0;          // 0 = бесконечно
static double interval_sec = 1.0;

unsigned short in_cksum(unsigned short *addr, int len) {
    int nleft = len;
    unsigned short *w = addr;
    unsigned int sum = 0;
    unsigned short answer = 0;

    while (nleft > 1) {
        sum += *w++;
        nleft -= 2;
    }
    if (nleft == 1) {
        *(unsigned char *)(&answer) = *(unsigned char *)w;
        sum += answer;
    }
    sum = (sum >> 16) + (sum & 0xffff);
    sum += (sum >> 16);
    answer = ~sum;
    return answer;
}

void send_one_ping(int sock, const struct sockaddr_in *to, int datalen, uint16_t id, uint16_t seq) {
    char packet[sizeof(struct icmphdr) + MAX_DATA_SIZE];
    struct icmphdr *icmp = (struct icmphdr *)packet;

    icmp->type = ICMP_ECHO;
    icmp->code = 0;
    icmp->un.echo.id = htons(id);
    icmp->un.echo.sequence = htons(seq);
    memset(packet + sizeof(struct icmphdr), 0x61, datalen);  // 'a' как в ping

    icmp->checksum = 0;
    icmp->checksum = in_cksum((unsigned short *)packet, sizeof(struct icmphdr) + datalen);

    sendto(sock, packet, sizeof(struct icmphdr) + datalen, 0,
           (struct sockaddr *)to, sizeof(*to));
}

static int parse_size_arg(const char *arg, int *min_val, int *max_val) {
    const char *colon = strchr(arg, ':');
    if (colon) {
        char *end;
        long min = strtol(arg, &end, 10);
        if (end != colon || min < 0 || min > MAX_DATA_SIZE) return -1;
        long max = strtol(colon + 1, &end, 10);
        if (*end != '\0' || max < 0 || max > MAX_DATA_SIZE || max < min) return -1;
        *min_val = (int)min;
        *max_val = (int)max;
    } else {
        char *end;
        long val = strtol(arg, &end, 10);
        if (*end != '\0' || val < 0 || val > MAX_DATA_SIZE) return -1;
        *min_val = *max_val = (int)val;
    }
    return 0;
}

void usage() {
    printf("Использование: bping [-s размер] [-p посылок] [-b пачек] [-i интервал] host\n\n");
    printf("  -s размер     данные в байтах (по умолчанию 56, макс ~65k)\n");
    printf("               формат: -s 1472 (фиксированный) или -s 100:600 (диапазон)\n");
    printf("  -p посылок    пингов в одной пачке (по умолчанию 10, макс 10000)\n");
    printf("  -b пачек      количество пачек (0 = ∞, по умолчанию 0)\n");
    printf("  -i интервал   между пачками в секундах (0.001, 0.05 и т.д., по умолчанию 1.0)\n\n");
    printf("Внимание: требует cap_net_raw или root!\n");
    printf("Примеры:\n");
    printf("  sudo bping 8.8.8.8\n");
    printf("  bping -s 1472 -p 200 -i 0.05 1.1.1.1\n");
    printf("  bping -s 100:600 -p 500 -i 0.01 192.168.1.1\n");
    exit(1);
}

void sigint_handler(int sig) {
    printf("\n\n⛔ bping остановлен\n");
    exit(0);
}

int main(int argc, char **argv) {
    int opt;
    while ((opt = getopt(argc, argv, "s:p:b:i:h")) != -1) {
        switch (opt) {
            case 's':
                if (parse_size_arg(optarg, &data_size_min, &data_size_max) != 0) {
                    fprintf(stderr, "Ошибка: неверный формат размера. Используйте -s число или -s мин:макс\n");
                    usage();
                }
                break;
            case 'p': pings_per_burst = atoi(optarg); break;
            case 'b': bursts = atoi(optarg); break;
            case 'i': interval_sec = strtod(optarg, NULL); break;
            case 'h':
            case '?': usage();
        }
    }
    if (optind >= argc) usage();
    host = argv[optind];

    if (data_size_min < 0 || data_size_max > MAX_DATA_SIZE || pings_per_burst < 1 || pings_per_burst > MAX_PINGS_PER_BURST) {
        usage();
    }

    signal(SIGINT, sigint_handler);

    // Создаём raw сокет
    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) {
        if (errno == EPERM)
            fprintf(stderr, "Ошибка: нужен root или cap_net_raw+ep\n");
        else
            perror("socket");
        exit(1);
    }

    // Разрешаем hostname
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;

    if (inet_pton(AF_INET, host, &dest.sin_addr) <= 0) {
        struct hostent *he = gethostbyname(host);
        if (!he) {
            fprintf(stderr, "Не могу разрешить %s\n", host);
            close(sock);
            exit(1);
        }
        memcpy(&dest.sin_addr, he->h_addr, he->h_length);
    }

    uint16_t ident = getpid() & 0xFFFF;
    uint16_t global_seq = 0;

    printf("🚀 bping → %s  (raw ICMP)\n", host);
    if (data_size_min == data_size_max) {
        printf("Данные: %d байт | В пачке: %d | Пачек: %s | Интервал: %.3f сек\n\n",
               data_size_min, pings_per_burst, (bursts == 0 ? "∞" : "ограничено"), interval_sec);
    } else {
        printf("Данные: %d:%d байт (случайно) | В пачке: %d | Пачек: %s | Интервал: %.3f сек\n\n",
               data_size_min, data_size_max, pings_per_burst, (bursts == 0 ? "∞" : "ограничено"), interval_sec);
    }

    // Выделяем память один раз
    struct timeval *send_times = malloc(pings_per_burst * sizeof(struct timeval));
    uint16_t *seq_list = malloc(pings_per_burst * sizeof(uint16_t));
    if (!send_times || !seq_list) {
        fprintf(stderr, "malloc error\n");
        close(sock);
        exit(1);
    }

    int burst_num = 0;
    while (1) {
        burst_num++;
        printf("=== Пачка #%d ===\n", burst_num);

        // === Отправляем пачку максимально быстро ===
        int sent = 0;
        for (int i = 0; i < pings_per_burst; i++) {
            int pkt_size = data_size_min + (rand() % (data_size_max - data_size_min + 1));
            uint16_t seq = ++global_seq;
            seq_list[i] = seq;
            gettimeofday(&send_times[i], NULL);
            send_one_ping(sock, &dest, pkt_size, ident, seq);
            sent++;
        }

        // === Собираем ответы этой пачки ===
        int received = 0;
        time_t collect_start = time(NULL);
        char recv_buf[2048];

        while (received < sent && (time(NULL) - collect_start < RECV_TIMEOUT_SEC)) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(sock, &fds);

            struct timeval tv = {1, 0};  // 1 секунда на select
            int ret = select(sock + 1, &fds, NULL, NULL, &tv);

            if (ret > 0 && FD_ISSET(sock, &fds)) {
                struct sockaddr_in from;
                socklen_t from_len = sizeof(from);
                int len = recvfrom(sock, recv_buf, sizeof(recv_buf), 0,
                                   (struct sockaddr *)&from, &from_len);

                if (len > 28) {  // минимум IP + ICMP
                    struct iphdr *ip = (struct iphdr *)recv_buf;
                    int ip_hlen = ip->ihl * 4;
                    if (len < ip_hlen + sizeof(struct icmphdr)) continue;

                    struct icmphdr *icmp = (struct icmphdr *)(recv_buf + ip_hlen);
                    if (icmp->type == ICMP_ECHOREPLY) {
                        uint16_t recv_id = ntohs(icmp->un.echo.id);
                        uint16_t recv_seq = ntohs(icmp->un.echo.sequence);

                        if (recv_id == ident) {
                            // ищем в текущей пачке
                            for (int j = 0; j < sent; j++) {
                                if (seq_list[j] == recv_seq) {
                                    struct timeval now;
                                    gettimeofday(&now, NULL);
                                    double rtt = (now.tv_sec - send_times[j].tv_sec) * 1000.0 +
                                                 (now.tv_usec - send_times[j].tv_usec) / 1000.0;

                                    char from_ip[INET_ADDRSTRLEN];
                                    inet_ntop(AF_INET, &from.sin_addr, from_ip, sizeof(from_ip));

                                    printf("%d bytes from %s: icmp_seq=%d ttl=%d time=%.1f ms\n",
                                           len - ip_hlen, from_ip, recv_seq, ip->ttl, rtt);

                                    received++;
                                    seq_list[j] = 0;  // пометили как обработано
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }

        double loss = sent ? 100.0 * (sent - received) / sent : 0;
        printf("--- %d transmitted, %d received, %.0f%% loss ---\n\n", sent, received, loss);

        if (bursts > 0 && burst_num >= bursts) break;

        // интервал между пачками
        if (interval_sec > 0) {
            struct timespec ts;
            ts.tv_sec = (time_t)interval_sec;
            ts.tv_nsec = (long)((interval_sec - ts.tv_sec) * 1e9);
            nanosleep(&ts, NULL);
        }
    }

    free(send_times);
    free(seq_list);
    close(sock);
    printf("✅ bping завершён.\n");
    return 0;
}
