/*
 * logreceiver.c — UDP log receiver (Linux)
 *
 * Listens on IP:port, receives datagrams, writes to file or stdout.
 * Usage: ./logreceiver [-d] [-p pidfile] <bind_ip> <port> [output_file]
 *   -d        daemonize (fork to background)
 *   -p FILE   write PID to file
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define RECV_BUF_SIZE 65536

static volatile int g_stop = 0;

static void sig_handler(int sig) {
    (void)sig;
    g_stop = 1;
}

static void daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid > 0) _exit(0);

    if (setsid() < 0) { perror("setsid"); exit(1); }

    pid = fork();
    if (pid < 0) { perror("fork2"); exit(1); }
    if (pid > 0) _exit(0);

    chdir("/");
    umask(0);

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > 2) close(fd);
    }
}

static int write_pidfile(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { perror("fopen pidfile"); return -1; }
    fprintf(f, "%d\n", getpid());
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    int daemon = 0;
    const char *pidfile = NULL;
    int argi = 1;

    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "-d") == 0) { daemon = 1; argi++; }
        else if (strcmp(argv[argi], "-p") == 0 && argi + 1 < argc) {
            pidfile = argv[argi + 1]; argi += 2;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[argi]);
            return 1;
        }
    }

    if (argc - argi < 2) {
        fprintf(stderr, "Usage: %s [-d] [-p pidfile] <bind_ip> <port> [output_file]\n",
                argv[0] ? argv[0] : "logreceiver");
        return 1;
    }

    const char *bind_ip = argv[argi];
    int port = atoi(argv[argi + 1]);
    const char *out_path = (argc - argi >= 3) ? argv[argi + 2] : NULL;

    if (port < 1 || port > 65535) {
        fprintf(stderr, "Invalid port: %d\n", port);
        return 1;
    }

    FILE *out = stdout;
    if (out_path) {
        out = fopen(out_path, "a");
        if (!out) { perror("fopen"); return 1; }
        setvbuf(out, NULL, _IOLBF, 0);
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid IP: %s\n", bind_ip);
        close(fd);
        return 1;
    }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return 1;
    }

    if (daemon) {
        if (pidfile) write_pidfile(pidfile);
        daemonize();
    }

    if (!daemon) {
        printf("logreceiver: listening on %s:%d, output: %s\n",
               bind_ip, port, out_path ? out_path : "stdout");
    }

    char buf[RECV_BUF_SIZE];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);

    while (!g_stop) {
        ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&from, &from_len);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        buf[n] = '\0';
        fputs(buf, out);
        fflush(out);
    }

    if (!daemon) printf("\nlogreceiver: stopped.\n");
    if (out_path && out != stdout) fclose(out);
    close(fd);
    if (pidfile) unlink(pidfile);
    return 0;
}
