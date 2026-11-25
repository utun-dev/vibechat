/* TUN: output_queue — ОС → ядро, input_queue — ядро → ОС; очереди содержат префикс(1) + IP.
 * tun_write/inject/read_packet работают с чистым IP. API — в потоке ua, close — вне callbacks TUN,
 * до уничтожения ua. POSIX test_mode использует socketpair; Windows требует Wintun. */
#ifndef TUN_IF_H
#define TUN_IF_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <pthread.h>
#include "../lib/ll_queue.h"

struct UASYNC;
struct utun_config;
struct tun_packet_data;
#ifdef __cplusplus
extern "C" {
#endif
#define TUN_MTU_DEFAULT     1500
#define TUN_MTU_MIN         68     // Минимальный IPv4 MTU.
#define TUN_MAX_PACKET_SIZE 1500

struct tun_if {
    char ifname[16];                  // Имя устройства ОС либо тестового интерфейса.
    int fd;                          // POSIX: устройство или socketpair; Windows: -1.
    void* platform_handle;           // Windows: WINTUN_SESSION_HANDLE.
    void* adapter_handle;            // Windows: WINTUN_ADAPTER_HANDLE.
    int platform_initialized;        // Успешный init передал cleanup общему коду.
    struct UASYNC* ua;               // Заимствованный цикл, живёт дольше TUN.
    void* socket_id;                  // Регистрация POSIX-чтения; NULL на Windows.
    struct memory_pool* pool;        // Пул entry, только поток ua.
    struct ll_queue* output_queue;   // ОС → ядро, следующий read ждёт пустой очереди.
    struct ll_queue* input_queue;    // Ядро → ОС, callback снимает префикс перед tun_write.
    struct queue_waiter_handle read_waiter; // Ожидание output_queue, отменяется до дренажа.
    int test_mode;                   // POSIX socketpair вместо устройства.
    int test_fd;                     // Заимствованный тестами peer socketpair, иначе -1.
    int mtu;                         // Размер IP-пакета: TUN_MTU_MIN..TUN_MAX_PACKET_SIZE.
    int closing;                     // Защищён post_lock при доступе worker.
    pthread_mutex_t post_lock;       // Защищает pending_packet и worker_read_errors.
    struct tun_packet_data* pending_packet; // Не более одной передачи worker → ua.
    uint32_t worker_read_errors;     // Ошибки worker, переносимые в read_errors под lock.
    uint64_t bytes_read;             // IP-байты, принятые output_queue; только поток ua.
    uint64_t bytes_written;          // Полностью записанные IP-байты; только поток ua.
    uint32_t packets_read;           // Пакеты, принятые output_queue.
    uint32_t packets_written;        // Полностью записанные пакеты.
    uint32_t read_errors;            // Ошибки чтения/доставки, включая отказ очереди.
    uint32_t write_errors;           // Ошибки аргументов/записи, включая неполную запись.
    uint32_t ifindex;                // Индекс ОС; 0 в тестовом backend.
#ifdef _WIN32
    HANDLE read_thread;              // Производитель, завершается до освобождения TUN.
    HANDLE stop_event;               // Прерывает ожидание данных и разрешения чтения.
    HANDLE read_permit;              // Auto-reset: разрешение принять следующий пакет.
    HANDLE read_event;               // Заимствованный event Wintun; не закрывать отдельно.
#endif
};

// Основной TUN по config: IPv4, MTU 1500. ua/config обязательны; NULL при ошибке.
struct tun_if* tun_init(struct UASYNC* ua, struct utun_config* config);
// Отдельный TUN: ifname, IPv4 без CIDR, явный MTU. test_mode только POSIX; NULL при ошибке.
struct tun_if* tun_init_nat(struct UASYNC* ua, const char* ifname, const char* ip_addr, int mtu, int test_mode);
// Остановить reader, обезвредить post, отменить ожидания и освободить ресурсы. NULL допустим.
void tun_close(struct tun_if* tun);
// Синхронно записать чистый IP без передачи владения buf: len при успехе, -1/errno при ошибке.
ssize_t tun_write(struct tun_if* tun, const uint8_t* buf, size_t len);
// Заимствованные очереди (префикс + IP), либо NULL при tun == NULL.
struct ll_queue* tun_get_input_queue(struct tun_if* tun);
struct ll_queue* tun_get_output_queue(struct tun_if* tun);
// Вернуть 0/1, при NULL — -1.
int tun_is_test_mode(struct tun_if* tun);
// Заимствованный POSIX peer socketpair для чистого IP; иначе -1. Самостоятельно не закрывать.
int tun_get_test_fd(struct tun_if* tun);
// Скопировать IP в свободную output_queue: 0 / -1; занятая очередь — EAGAIN, buf остаётся вызывающему.
int tun_inject_packet(struct tun_if* tun, const uint8_t* buf, size_t len);
// POSIX helper: чистый IP из test_fd; >0 — размер, 0 — пока пусто, -1/errno — ошибка.
// При малом buf (EMSGSIZE) пакет сохраняется. Счётчики не изменяются; один consumer test_fd.
ssize_t tun_read_packet(struct tun_if* tun, uint8_t* buf, size_t len);

/* Внутренний backend API: init сам откатывает ошибку; cleanup — после успешного init.
 * read/write передают чистый IP; poll_fd заимствован и регистрируется общим кодом. */
int tun_platform_init(struct tun_if* tun, const char* ifname, const char* ip_str, int mtu);
void tun_platform_cleanup(struct tun_if* tun);
ssize_t tun_platform_read(struct tun_if* tun, uint8_t* buf, size_t len);
ssize_t tun_platform_write(struct tun_if* tun, const uint8_t* buf, size_t len);
int tun_platform_get_poll_fd(struct tun_if* tun);
// Worker: скопировать пакет в резервированный post, 0 / -1. TUN/ua живы до join.
// После успеха следующий пакет разрешает read_permit; очереди и пул worker не использует.
int tun_post_packet(struct tun_if* tun, const uint8_t* buf, size_t len);
// Worker: логировать системную ошибку и увеличить защищённый счётчик; operation — описание операции.
void tun_reader_error(struct tun_if* tun, const char* operation, unsigned long error);
#ifdef _WIN32
// Подготовить events и запустить reader: 0 / -1; частичные handles освобождает tun_close.
int tun_windows_start_reader(struct tun_if* tun);
#endif
#ifdef __cplusplus
}
#endif
#endif
