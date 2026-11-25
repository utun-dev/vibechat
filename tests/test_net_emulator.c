/* Контракты конфига и ресурсов отдельного CLI-эмулятора, включая реальную UDP-передачу. */
#define _GNU_SOURCE
#define main net_emulator_cli_main
#include "../net_emulator/net_emulator.c"
#undef main
#include "test_clock.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #c); exit(1); } } while (0)
static int fail_registration;

void* __real_uasync_add_socket(struct UASYNC*, int, socket_callback_t, socket_callback_t, socket_callback_t, const char*, void*);
void* __wrap_uasync_add_socket(struct UASYNC* ua, int fd, socket_callback_t r, socket_callback_t w,
                              socket_callback_t e, const char* name, void* arg) {
    if (fail_registration && name && strcmp(name, "emulator") == 0) return NULL;
    return __real_uasync_add_socket(ua, fd, r, w, e, name, arg);
}

static int load_config(net_emulator_t* em, const char* text) {
    char path[] = "/tmp/utun-netem-config-XXXXXX";
    int fd = mkstemp(path); CHECK(fd >= 0);
    FILE* file = fdopen(fd, "w"); CHECK(file);
    CHECK(fputs(text, file) >= 0); CHECK(fclose(file) == 0);
    int result = parse_emulator_config(path, em);
    CHECK(unlink(path) == 0);
    return result;
}

static void test_config(void) {
    net_emulator_t em = {0};
    CHECK(load_config(&em, "[127.0.0.1:20000]\nprob=40,delay=1-3\nprob=200,delay=0\nprob=20,delay=0\n") == 0);
    CHECK(em.endpoint_count == 1 && em.endpoints[0].rule_count == 2);
    CHECK(em.endpoints[0].total_probability == 60 && em.endpoints[0].sockfd == -1);
    CHECK(ntohs(em.endpoints[0].target_addr.sin_port) == 10000);
    CHECK(em.endpoints[0].rules[0].delay.min_ms == 1 && em.endpoints[0].rules[0].delay.max_ms == 3);
    CHECK(load_config(&em, "[127.0.0.1:9999]\nprob=100,delay=0\n") == 0);
    CHECK(em.endpoint_count == 2 && ntohs(em.endpoints[1].target_addr.sin_port) == 9999);
    net_emulator_cleanup(&em);
    em = (net_emulator_t){0};
    CHECK(load_config(&em, "[127.0.0.1:10000]\n") == 0);
    CHECK(ntohs(em.endpoints[0].target_addr.sin_port) == 0);
    em = (net_emulator_t){0};
    CHECK(load_config(&em, "[127.0.0.1:20000]\nprob=60,delay=0\nprob=50,delay=0\n") == -1);
    CHECK(em.endpoint_count == 1 && em.endpoints[0].rule_count == 2);
    em = (net_emulator_t){0};
    CHECK(load_config(&em, "[not-an-ip:20000]\n") == -1);
    CHECK(parse_emulator_config(NULL, &em) == -1);
    puts("PASS net_emulator configuration: ports, probabilities, invalid-rule skip, partial result");
}

static void test_partial_init(void) {
    net_emulator_t em = {0};
    CHECK(load_config(&em, "[127.0.0.1:20000]\n[127.0.0.1:20001]\n") == 0);
    em.endpoints[0].listen_addr.sin_port = 0;
    em.endpoints[1].listen_addr.sin_family = AF_INET6; /* Несовместимое с IPv4-сокетом семейство. */
    int sentinel = open("/dev/null", O_RDONLY); CHECK(sentinel >= 0);
    CHECK(net_emulator_init(&em) == -1 && !em.ua && !em.running);
    int created = em.endpoints[0].sockfd; CHECK(created >= 0);
    CHECK(em.endpoints[1].sockfd == -1);
    net_emulator_cleanup(&em); net_emulator_cleanup(&em);
    CHECK(em.endpoints[0].sockfd == -1);
    CHECK(fcntl(created, F_GETFD) == -1 && errno == EBADF);
    CHECK(fcntl(sentinel, F_GETFD) >= 0); CHECK(close(sentinel) == 0);
    em = (net_emulator_t){0};
    CHECK(load_config(&em, "[127.0.0.1:20000]\n") == 0);
    em.endpoints[0].listen_addr.sin_port = 0;
    fail_registration = 1;
    CHECK(net_emulator_init(&em) == -1 && !em.ua && !em.running && em.endpoints[0].sockfd == -1);
    fail_registration = 0;
    net_emulator_cleanup(&em);
    puts("PASS net_emulator partial init: socket cleanup, untouched descriptors, registration failure");
}

static void test_forwarding(void) {
    net_emulator_t em = {0};
    CHECK(load_config(&em, "[127.0.0.1:20000]\nprob=100,delay=3\n") == 0);
    em.endpoints[0].listen_addr.sin_port = 0;
    CHECK(net_emulator_init(&em) == 0);
    struct sockaddr_in listen_addr;
    socklen_t len = sizeof(listen_addr);
    CHECK(getsockname(em.endpoints[0].sockfd, (struct sockaddr*)&listen_addr, &len) == 0);
    socket_t sender = socket_create_udp(AF_INET), receiver = socket_create_udp(AF_INET);
    CHECK(sender != SOCKET_INVALID && receiver != SOCKET_INVALID);
    CHECK(socket_set_nonblocking(receiver) == 0);
    struct sockaddr_in target = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(bind(receiver, (struct sockaddr*)&target, sizeof(target)) == 0);
    len = sizeof(target); CHECK(getsockname(receiver, (struct sockaddr*)&target, &len) == 0);
    em.endpoints[0].target_addr = target;
    const uint8_t data[] = {1, 0, 2, 0xff};
    CHECK(sendto(sender, data, sizeof(data), 0, (struct sockaddr*)&listen_addr, sizeof(listen_addr)) == sizeof(data));
    test_clock_run_for(em.ua, 1000);
    uint8_t received[16];
    CHECK(recv(receiver, received, sizeof(received), 0) == sizeof(data));
    CHECK(memcmp(data, received, sizeof(data)) == 0);
    em.endpoints[0].rule_count = em.endpoints[0].total_probability = 0;
    CHECK(sendto(sender, data, sizeof(data), 0, (struct sockaddr*)&listen_addr, sizeof(listen_addr)) == sizeof(data));
    test_clock_run_for(em.ua, 1000);
    CHECK(recv(receiver, received, sizeof(received), 0) == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
    uasync_stop(em.ua); net_emulator_run(&em);
    int wakeup_fd = em.ua->wakeup_pipe[0], endpoint_fd = em.endpoints[0].sockfd;
    net_emulator_cleanup(&em); net_emulator_cleanup(&em);
    CHECK(!em.ua && !em.running && em.endpoints[0].sockfd == -1);
    CHECK(fcntl(wakeup_fd, F_GETFD) == -1 && errno == EBADF);
    CHECK(fcntl(endpoint_fd, F_GETFD) == -1 && errno == EBADF);
    CHECK(socket_close_wrapper(sender) == 0 && socket_close_wrapper(receiver) == 0);
    puts("PASS net_emulator UDP: delayed forwarding, residual drop, stop and cleanup");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    size_t baseline = u_get_allocated_count();
    test_clock_init(1000);
    test_config(); test_partial_init(); test_forwarding();
    test_clock_finish();
    CHECK(u_get_allocated_count() == baseline);
    puts("PASS net_emulator resource balance");
    return 0;
}
