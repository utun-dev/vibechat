/* IPv4 UDP-эмулятор одного потока uasync: правила случайной задержки и остаточная вероятность потерь.
 * Обнулить состояние → parse_emulator_config → init → run → cleanup после возврата из run.
 * Конфиг: [IPv4:port], строки prob=N,delay=min-max (мс); до 100 endpoint, до 10 правил на endpoint.
 * Для каждого пакета выбирается одно правило по накопленной вероятности; остаток до 100% означает drop.
 * Разные задержки могут менять порядок пакетов; отдельного механизма перестановки по reorder_probability нет. */
#ifndef NET_EMULATOR_H
#define NET_EMULATOR_H

#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "u_async.h"

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of rules per endpoint
#define MAX_RULES_PER_ENDPOINT 10
#define MAX_ENDPOINTS 100
#define MAX_PACKET_SIZE 65536

// Forward declaration
typedef struct net_emulator_s net_emulator_t;

// Delay range (milliseconds)
typedef struct {
    int min_ms;                    // Минимальная задержка >=0
    int max_ms;                    // Верхняя граница включительно; max >= min, значение <= INT_MAX/10 для timebase
} delay_range_t;

// Rule: probability and delay
typedef struct {
    int probability_percent;  // 0-100
    delay_range_t delay;
} rule_t;

// Network endpoint configuration
typedef struct {
    struct sockaddr_in listen_addr;  // Address to listen on
    struct sockaddr_in target_addr;  // Тот же IP; порт listen-10000 при listen>=10000, иначе исходный порт
    rule_t rules[MAX_RULES_PER_ENDPOINT];
    int rule_count;
    int total_probability;  // Сумма 0..100%; вероятность потери — 100-total_probability
    
    int reorder_probability;  // Не используется и не читается из конфигурации
    
    // Socket descriptor
    int sockfd;                       // Созданный endpoint-сокет; -1 до создания/после cleanup
    net_emulator_t* emulator;  // Parent emulator instance
} endpoint_t;

// Main emulator state
struct net_emulator_s {
    endpoint_t endpoints[MAX_ENDPOINTS];
    int endpoint_count;
    int running;                     // init завершён; сам флаг не останавливает mainloop, нужен uasync_stop(ua)
    uasync_t* ua;                     // Принадлежит эмулятору, создаётся init и уничтожается cleanup
};

/**
 * @brief Читает endpoint и правила; перед первым вызовом обнулить emulator, живой экземпляр не перенастраивать.
 * @param filename путь к конфигу; строки с ';' и пустые пропускаются
 * @param emulator состояние, в которое добавляются endpoint; прежние записи не очищаются
 * @return 0 после разбора; -1 при NULL, ошибке открытия, неверном endpoint, превышении лимитов/суммы вероятностей
 * Ошибочное правило логируется и пропускается без -1; прочие нераспознанные строки игнорируются.
 * При -1 уже разобранная часть сохраняется. Диапазон задержек должен помещаться в INT_MAX/10 мс.
 */
int parse_emulator_config(const char *filename, net_emulator_t *emulator);

/**
 * @brief Создаёт собственный uasync и неблокирующие UDP-сокеты настроенных endpoint.
 * @param emulator состояние после parse, без живого ua; endpoint/rule counts должны помещаться в массивы
 * @return 0 — сокеты созданы и зарегистрированы, -1 — NULL/ошибка ресурсов; после ошибки вызвать cleanup
 * При ручном заполнении endpoint до init установить sockfd=-1. Ошибка init может оставить созданные сокеты до cleanup.
 */
int net_emulator_init(net_emulator_t *emulator);

/**
 * @brief Запускает блокирующий mainloop, возвращается после uasync_stop(emulator->ua).
 * @param emulator успешно инициализированное состояние; NULL/нет ua/running=0 ничего не делает
 * Остановка и cleanup выполняются при согласованном времени жизни состояния, cleanup — вне callbacks.
 */
void net_emulator_run(net_emulator_t *emulator);

/**
 * @brief Закрывает созданные endpoint-сокеты и уничтожает собственный ua, сбрасывает running.
 * @param emulator состояние после init (в том числе неудачного); NULL и повторный cleanup допустимы
 * Перед cleanup завершить отложенные отправки: уничтожение таймеров uasync само не освобождает их packet-аргументы.
 */
void net_emulator_cleanup(net_emulator_t *emulator);

#ifdef __cplusplus
}
#endif

#endif /* NET_EMULATOR_H */
