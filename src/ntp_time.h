/* ntp_time — асинхронные UDP NTP-запросы с DNS и периодическим повтором в uasync-потоке ядра.
 * Не меняет часы ОС: хранит local_time - ntp_time в instance->ntp.offset_us.
 * get_us/get_seconds возвращают скорректированное wall-clock время; до синхронизации — локальное.
 * В uasync-потоке вызвать init после создания config/ua, destroy — до освобождения ua, вне callbacks модуля.
 * Один цикл обходит серверы до первого успеха; на сервер — до трёх UDP-запросов с таймаутом 2 с.
 * Между завершением цикла и следующим запуском — resync_interval_sec. DNS выполняется асинхронно (IPv4).
 * enabled управляет только интернет-опросом; synced учитывает также коррекцию от ntp_node_time.
 * Приоритет: часы ОС < сохранённая поправка < свежая синхронизация пира < свой ответ NTP.
 * Каждый уровень сохраняется отдельно в общей SQLite через DB worker. После запуска все прочитанные значения — кэш;
 * среди них выбираем прежний NTP, затем пира. Свежий пир заменяет даже сохранённый NTP.
 * Пир с одним лишь кэшем не считается свежим. Сбой опроса не стирает полученные поправки.
 * Файловый кэш читается асинхронно; поздний ответ не заменяет измерения, полученные после init.
 * Обмен групп ждёт загрузки кэша, а при включённом NTP с серверами — первого успеха; общий предел 4 секунды.
 * Транспорт и обмен временем работают сразу; время пира не снимает ожидание, NTP продолжает опрос после него.
 * destroy отменяет DNS/сокет/таймеры, сохраняя synced, offset_us и last_sync_tb. Часы ОС не меняются. */
#ifndef NTP_TIME_H
#define NTP_TIME_H

#include <stdint.h>
#include <time.h>
#include "../lib/platform_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

struct UTUN_INSTANCE;

#define NTP_DEFAULT_INTERVAL_SEC 3600
#define NTP_FIRST_SYNC_DELAY_SEC 0
#define NTP_STARTUP_WAIT_SEC 4
#define NTP_TIME_LIMIT_US (INT64_MAX / 4) // запас для сложения времени, поправки RTT и разности двух измерений

/* Приоритет текущего времени. После перезапуска любые прочитанные из БД поправки — только SAVED. */
enum ntp_time_source {
    NTP_TIME_LOCAL = 0,
    NTP_TIME_SAVED = 1,
    NTP_TIME_PEER = 2,
    NTP_TIME_SERVER = 3
};

/* Последнее измерение своего уровня; fresh означает получение после текущего init, а не чтение из БД. */
struct ntp_time_offset {
    int valid, fresh;
    int64_t offset_us;          // local_time - reference_time, мкс
    int64_t received_us;        // часы ОС в момент получения; только диагностика, не сравнение источников
};

struct NTP_TIME {
    int enabled;            // интернет-опрос разрешён конфигом; 0 не запрещает коррекцию от пиров
    int synced;             // есть поправка от NTP или пира; не означает её свежесть/доступность интернета
    int reachable;          // успех последнего завершённого NTP-цикла; до первого результата = 1 (оптимистично)
    int64_t offset_us;      // local_time - reference_time, мкс; положительное = локальные часы спешат
    uint64_t last_sync_tb;  // монотонное время последнего принятого измерения, включая резерв; единицы 0,1 мс, 0 до получения
    enum ntp_time_source source; // активный приоритет: LOCAL -> SAVED -> PEER -> SERVER
    struct ntp_time_offset offsets[4]; // последние SAVED/PEER/SERVER отдельно; LOCAL имеет нулевой offset
    void* restore;          // уникальный контекст READ кэша; stop/reinit отсоединяют поздний ответ
    void* startup_timer;    // ожидание кэша/NTP перед обменом групп: до 4 секунд, NULL после успеха/таймаута
    int startup_wait;       // обмен групп удержан; транспорт и обмен временем работают
    void* timer;            // таймер следующего цикла, NULL во время опроса/после остановки/при ошибке таймера
    char **servers;         // принадлежащие модулю копии IPv4-адресов/имён серверов, освобождаются destroy
    int server_count;       // число элементов servers; после destroy = 0
    int server_current;     // начальный индекс следующего цикла; после успеха — следующий за ответившим
    int resync_interval_sec;// пауза после цикла, секунды; при активном опросе 1..INT_MAX/10000
    struct sockaddr_in test_addr; // sin_port != 0: единственный сервер цикла, без DNS/перехода к servers
    struct sockaddr_in test_dns;  // sin_port != 0: IPv4 DNS-сервер для тестов
    uint16_t test_port;           // порт запросов к servers в тестах: 0 = 123; не влияет на test_addr
    void* sync;                   // активный контекст синхронизации (struct ntp_sync*, opaque)
};

// Читает config, копирует серверы, ставит READ кэша в DB worker; для интернет-NTP — таймер опроса 0.
// Группы ждут кэш/первый NTP, максимум4 с; :memory: без worker восстанавливается сразу. Общая схема должна существовать.
// instance/config/ua обязательны, состояние исходно нулевое либо после destroy; повторный активный init запрещён.
// 0 — успех (включая отключённый опрос/пустой список), -1 — ошибка с освобождением новых ресурсов.
// Ошибка повторного init сохраняет прежнее состояние. Новый init сбрасывает свежесть и test_*, перечитывает кэш.
int ntp_time_init(struct UTUN_INSTANCE* instance);

// Применить измерение своего уровня в RAM и сохранить отдельно в общей SQLite: RW worker через db_exec.
// До общего RW-перехода RO owner допускает прежнего единственного писателя; без owner — только :memory:.
// source=SAVED — время пира только из его кэша; PEER — свежая синхронизация пира; SERVER — свой NTP-ответ.
// 1 — активное время изменилось, 0 — сохранён только резерв/значение прежнее, -1 — неверные аргументы.
// Ошибка сохранения логируется; полученное время всё равно применяется в текущем запуске.
int ntp_time_record_offset(struct UTUN_INSTANCE* instance, enum ntp_time_source source, int64_t offset_us);
// Отменяет опрос/ожидание, отсоединяет поздний READ и освобождает серверы без ожидания БД.
// Запись уже полученной поправки завершается независимо; поправку/reachable сохраняет, enabled сбрасывает. NULL допустим.
void ntp_time_destroy(struct UTUN_INSTANCE* instance);

// После init, до первого poll задать IPv4-литерал ip и порт сервера; port=0 отключает подмену.
// Неверный ip сбрасывает test_addr; NULL ntp/ip игнорируется. Требуется непустой список серверов в конфиге.
void ntp_time_set_test_addr(struct NTP_TIME* ntp, const char* ip, uint16_t port);
// После init, до poll задать IPv4-литерал ip, dns_port (0 отключает подмену DNS) и ntp_port (0 = 123).
// Неверный ip сбрасывает test_dns и test_port; NULL ntp/ip игнорируется. test_addr имеет приоритет.
void ntp_time_set_test_dns(struct NTP_TIME* ntp, const char* ip, uint16_t dns_port, uint16_t ntp_port);

// Unix wall-clock в мкс: local - offset_us при synced, иначе локальное время; NULL возвращает локальное.
int64_t ntp_time_get_us(struct UTUN_INSTANCE* instance);
// То же время в целых секундах: get_us()/1000000 с приведением к time_t; NULL допустим.
time_t ntp_time_get_seconds(struct UTUN_INSTANCE* instance);
// Ненулевое значение, если есть поправка от NTP/пира (включая после destroy); для NULL — 0.
int ntp_time_is_synced(struct UTUN_INSTANCE* instance);

#ifdef __cplusplus
}
#endif

#endif // NTP_TIME_H
