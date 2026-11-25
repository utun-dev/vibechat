/* Настоящий CLI без -f: daemon должен создать disk worker после fork, завершиться
 * по SIGTERM и вернуть launcher ошибку, если PID-файл нельзя создать. Без настоящего TUN. */
#include "../lib/platform_compat.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "test_utils.h"
#include <stdio.h>
#include <string.h>
#ifdef __linux__
#include <dirent.h>
#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
extern char** environ;

static int launch(const char* config, const char* pidfile, const char* log) {
    char* argv[] = { UTUN_CLI_PATH, "-c", (char*)config, "-p", (char*)pidfile, "-l", (char*)log,
                     "-d", "sys=info,debug=trace", NULL };
    pid_t launcher;
    int error = posix_spawn(&launcher, UTUN_CLI_PATH, NULL, NULL, argv, environ);
    if (error) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "daemon test: spawn failed error=%d", error); return -1; }
    int status;
    while (waitpid(launcher, &status, 0) < 0) {
        if (errno == EINTR) continue;
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "daemon test: waitpid failed: %s", strerror(errno)); return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int thread_count(pid_t pid) {
    char path[128]; snprintf(path, sizeof(path), "/proc/%ld/task", (long)pid);
    DIR* dir = opendir(path);
    if (!dir) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "daemon test: task listing failed: %s", strerror(errno)); return -1; }
    int count = 0; struct dirent* entry;
    while ((entry = readdir(dir))) if (entry->d_name[0] != '.') count++;
    closedir(dir); return count;
}

static int stop(pid_t pid, const char* pidfile) {
    if (kill(pid, SIGTERM)) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "daemon test: SIGTERM failed: %s", strerror(errno)); return -1; }
    uint64_t deadline = get_time_tb() + 20000;
    while (access(pidfile, F_OK) == 0 && get_time_tb() < deadline) poll(NULL, 0, 10);
    if (access(pidfile, F_OK) == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "daemon test: shutdown stuck pid=%ld; killing own fixture", (long)pid);
        if (kill(pid, SIGKILL)) DEBUG_ERROR(DEBUG_CATEGORY_SYS, "daemon test: fixture cleanup failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    char dir[512] = "/tmp/utun_daemon_worker_XXXXXX";
    if (test_mkdtemp(dir)) return 1;
    char config[768], pidfile[768], log[768], bad_pid[768];
    snprintf(config, sizeof(config), "%s/utun.conf", dir); snprintf(pidfile, sizeof(pidfile), "%s/daemon.pid", dir);
    snprintf(log, sizeof(log), "%s/daemon.log", dir); snprintf(bad_pid, sizeof(bad_pid), "%s/missing/daemon.pid", dir);
    FILE* f = fopen(config, "w"); if (!f) return 1;
    fprintf(f, "[global]\nmy_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
               "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
               "tun_ip=192.0.2.1\ntun_test_mode=1\n"
               "[server:test]\naddr=127.0.0.1:0\ntype=private\n"
               "[chatserver]\ndb_path=%s/db\nstorage_autoload=0\ngroup_autoconnect=0\n", dir);
    if (fclose(f) || launch(config, pidfile, log)) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "daemon test: startup failed log=%s", log); return 1;
    }
    f = fopen(pidfile, "r"); long value = 0;
    if (!f || fscanf(f, "%ld", &value) != 1 || value <= 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "daemon test: invalid/missing pidfile=%s", pidfile); if (f) fclose(f); return 1;
    }
    fclose(f); pid_t daemon_pid = (pid_t)value;
    int threads = thread_count(daemon_pid);
    int stopped = stop(daemon_pid, pidfile);
    if (threads < 2 || stopped) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "daemon test: worker lost or shutdown failed threads=%d stopped=%d log=%s", threads, stopped, log);
        return 1;
    }
    if (launch(config, bad_pid, log) == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "daemon test: launcher accepted failed child initialization log=%s", log); return 1;
    }
    printf("daemon worker: PASS threads=%d, graceful shutdown, startup errors propagate\n", threads);
    return 0;
}
#else
int main(void) { printf("daemon worker: SKIP (requires Linux procfs)\n"); return 77; }
#endif
