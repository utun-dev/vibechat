/**
 * @file dummynet.h
 * @brief Двунаправленный IPv4 UDP-эмулятор и inline-фильтр потерь для ETCP.
 *
 * FULL: create(ua, bind_ip, port), set_direction для обоих различных IP:порт, затем poll(ua).
 * Эмулирует задержку fixed+random, пропускную способность, потери и общие для обоих направлений микрофризы.
 * Принимает датаграммы 0..1500 байт; более длинные отбрасывает целиком. Только IPv4-литералы.
 * Неизвестные источники не пересылаются и не входят в статистику направлений.
 *
 * Архитектура:
 *   UDP recv -> size/DROP/loss/queue limit -> delay timer -> очередь -> shaper timer -> UDP send
 *
 * Поток данных:
 * - Слушаем на bind_ip:listen_port
 * - Пакеты от dest_backward -> отправляем на dest_forward (forward direction)
 * - Пакеты от dest_forward -> отправляем на dest_backward (backward direction)
 * - Оба направления настраиваются до начала трафика; IP и порты участников независимы
 * - Каждое направление имеет свою очередь и параметры эмуляции; random delay может менять порядок пакетов.
 * INLINE: filter_create/attach вставляет фильтр потерь и блокировок в один свободный UDP send_hook.
 * Он не создаёт сокета, не добавляет задержки/BWlim; закрытие линка автоматически отвязывает фильтр.
 * Все вызовы — в потоке ua. ua должен жить до destroy; статистика заимствована до destroy.
 * Потери используют rand(): seed задаёт вызывающий. Подробнее — dummynet_doc.md.
 */

#ifndef DUMMYNET_H
#define DUMMYNET_H

#ifdef __cplusplus
extern "C" {
#endif


#include <stdint.h>
#include <stddef.h>
#include "../lib/socket_compat.h"

// Forward declarations
struct UASYNC;
struct ll_queue;
struct ll_entry;

/* Направления передачи */
#define DUMMYNET_FORWARD  0   // client -> server
#define DUMMYNET_BACKWARD 1   // server -> client
#define DUMMYNET_DIR_COUNT 2

/* Максимальный размер UDP пакета */
#define DUMMYNET_MAX_PKT_SIZE 1500

/* Максимальное время задержки (10 секунд) */
#define DUMMYNET_MAX_DELAY_MS 10000

/* Максимальное значение loss_permille (1000 = 100%) */
#define DUMMYNET_MAX_LOSS_PERMILLE 1000

#define DUMMYNET_MAX_FREEZE_TYPES 8

/* Один независимый источник помех: случайные моменты начала и равномерная длительность 0..max_duration_ms. */
struct dummynet_freeze_spec {
    double events_per_second;    // Средняя частота, 0.001..1000/с; сумма частот <=1000/с
    uint32_t max_duration_ms;    // Верхняя граница длительности, 1..10000 мс, включительно
};

/* Снимок общих помех с последнего set_microfreezes; направление не дублирует эти счётчики. */
struct dummynet_freeze_stats {
    uint64_t events[DUMMYNET_MAX_FREEZE_TYPES]; // События по типам, включая нулевую длительность
    uint64_t overlaps;           // Ненулевое событие началось во время действующей паузы
    uint64_t paused_us;          // Объединённое время пауз, включая текущую до момента снимка
    uint64_t active_since_us;    // Начало текущей непрерывной паузы; 0 — передача разрешена
    uint64_t until_us;           // Запланированный конец паузы; фактический конец зависит от poll
    uint64_t timer_late_max_us;  // Максимальное опоздание таймера помех
    unsigned type_count;        // Число типов в действующей конфигурации; 0 — выключено
};

enum dummynet_mode {
    DUMMYNET_RUN,            // обычная передача
    DUMMYNET_DROP,           // потеря новых и уже задержанных пакетов
    DUMMYNET_HOLD            // задержанные пакеты сохраняются; очередь ограничена max_queue_pkts
};

/**
 * @brief Статистика направления
 */
struct dummynet_stats {
    uint64_t recv;           // FULL: пакеты распознанного направления, включая слишком большие; INLINE: вызовы hook
    uint64_t sent;           // Отправлено пакетов
    uint64_t dropped;        // Слишком большой пакет, переполнение очереди, отказ выделения/таймера или ошибка отправки
    uint64_t lost;           // Случайные потери; FULL: DROP, INLINE: блокировки адреса
    uint64_t recv_bytes;     // Принято UDP-байт, без IP/UDP заголовков
    uint64_t sent_bytes;     // Передано UDP-байт, без IP/UDP заголовков; пустой пакет увеличивает sent, но не sent_bytes
    uint64_t blocked;        // FULL: режим DROP; INLINE: блокировка IP:порта; включены в lost
    uint64_t max_queue_delay_us; // максимум времени до отправки минус заданная propagation delay; включает сериализацию/HOLD
    uint32_t queue_size;     // Все ожидающие пакеты, включая delay и HOLD
    uint32_t queue_max;      // Максимальный размер очереди (пиковый)
};

/**
 * @brief Контекст dummynet
 */
struct dummynet;

/**
 * @brief Создаёт dummynet контекст
 *
 * @param ua Экземпляр uasync (обязателен)
 * @param bind_ip IP адрес для привязки (NULL или "0.0.0.0" для всех интерфейсов)
 * @param listen_port Порт для прослушивания (0 выбирает ОС; фактический порт — get_listen_port)
 * @return Указатель на контекст или NULL при ошибке
 */
struct dummynet* dummynet_create(struct UASYNC* ua, const char* bind_ip, uint16_t listen_port);

/**
 * @brief Уничтожает dummynet контекст
 *
 * Освобождает пакеты, очереди и пул, закрывает сокет, отменяет все delay/shaper-таймеры.
 * ua не освобождается. NULL допустим; после вызова контекст и указатели на статистику недействительны.
 *
 * @param dn Контекст dummynet
 */
void dummynet_destroy(struct dummynet* dn);

/**
 * @brief Настраивает параметры направления
 *
 * Ошибка сохраняет прежнюю конфигурацию целиком. Сумма fixed+random <= 10000 мс;
 * max_queue_pkts <= INT_MAX. loss > 1000 ограничивается до 1000 с WARN.
 * Одинаковые endpoints и dest_ip=0.0.0.0 запрещены. Смена endpoint требует пустых обоих направлений.
 * Остальные параметры можно менять с очередью: delay/loss применяются к новым входящим пакетам,
 * BW — к следующей сериализации (уже начатая завершается по прежнему сроку), лимит — к новым поступлениям.
 *
 * @param dn Контекст dummynet
 * @param direction Направление: DUMMYNET_FORWARD или DUMMYNET_BACKWARD
 * @param delay_fixed_ms Фиксированная задержка (мс)
 * @param delay_random_ms Случайная составляющая задержки (0..random_ms, мс)
 * @param bandwidth_kbps Пропускная способность (кбит/с, 0 = unlimited)
 * @param max_queue_pkts Максимум всех ожидающих пакетов, включая delay/HOLD; 0 отбрасывает все
 * @param loss_permille Вероятность потери пакета (0.1%, 0-1000)
 * @param dest_ip IP адрес назначения
 * @param dest_port Порт назначения
 * @return 0 при успехе, -1 при ошибке
 */
int dummynet_set_direction(struct dummynet* dn, int direction,
                           uint32_t delay_fixed_ms, uint32_t delay_random_ms,
                           uint32_t bandwidth_kbps, uint32_t max_queue_pkts,
                           uint32_t loss_permille,
                           const char* dest_ip, uint16_t dest_port);

/* Меняет режим направления; 0 / -1. HOLD отменяет текущую сериализацию; RUN начинает её заново
 * для головы очереди и выпускает накопленное с BWlim, без мгновенного сброса всей очереди.
 * DROP удаляет также пакеты в delay. Все вызовы — в потоке ua. BWlim=0 означает unlimited. */
int dummynet_set_mode(struct dummynet* dn, int direction, enum dummynet_mode mode);

/* Задать сразу несколько источников помех для ОБОИХ направлений FULL-прокси; 0 / -1.
 * Начала — пуассоновский поток с суммой частот, тип выбирается пропорционально частоте.
 * Длительность — целое число мс 0..max; перекрытия объединяются. Точность — таймер ua (около 1 мс).
 * Фриз приостанавливает уже начатую сериализацию: после паузы сохраняется оставшаяся работа,
 * время простоя не создаёт кредит BW. Delay/loss и ограниченная очередь продолжают работать.
 * specs копируются; count=0 выключает помехи и завершает текущую паузу; иначе count<=MAX_FREEZE_TYPES.
 * seed задаёт отдельный PRNG, независимый от потерь/jitter/числа пакетов. Ошибка сохраняет старую настройку.
 * Успех сбрасывает только freeze_stats. HOLD/DROP направления не отключают источник помех.
 * Отказ таймера во время работы отключает помехи с ERROR и освобождает паузу. */
int dummynet_set_microfreezes(struct dummynet* dn, const struct dummynet_freeze_spec* specs, unsigned count, uint32_t seed);

/* Копирует текущий снимок помех в out, включая незавершённую паузу; 0 / -1 (NULL логируется). */
int dummynet_get_freeze_stats(struct dummynet* dn, struct dummynet_freeze_stats* out);

/**
 * @brief Получает статистику направления
 *
 * @param dn Контекст dummynet
 * @param direction Направление
 * @return Указатель на статистику или NULL
 */
const struct dummynet_stats* dummynet_get_stats(struct dummynet* dn, int direction);

/**
 * @brief Сбрасывает статистику направления
 *
 * @param dn Контекст dummynet
 * @param direction 0/1 или -1 для обоих. Текущий queue_size сохраняется, queue_max становится равным ему;
 * отладочные счётчики таймеров не сбрасываются. Остальные значения/NULL логируются без изменения состояния.
 */
void dummynet_reset_stats(struct dummynet* dn, int direction);

/**
 * @brief Возвращает файловый дескриптор сокета (для отладки)
 *
 * @param dn Контекст dummynet
 * @return socket_t или SOCKET_INVALID
 */
socket_t dummynet_get_socket(struct dummynet* dn);

/**
 * @brief Возвращает listen_port
 *
 * @param dn Контекст dummynet
 * @return Порт или 0
 */
uint16_t dummynet_get_listen_port(struct dummynet *dn);

/**
 * @brief Возвращает указатель на UASYNC (для запуска poll)
 *
 * @param dn Контекст dummynet
 * @return Указатель на UASYNC или NULL
 */
struct UASYNC *dummynet_get_uasync(struct dummynet *dn);

/**
 * @brief Возвращает размер очереди направления
 *
 * @param dn Контекст dummynet
 * @param direction Направление
 * @return Число всех ожидающих пакетов (delay + очередь + сериализация/HOLD) или -1
 */
int dummynet_get_queue_size(struct dummynet *dn, int direction);

/**
 * @brief Получает отладочные счетчики направления
 *
 * @param dn Контекст dummynet
 * @param direction Направление
 * Выходы необязательны (NULL пропускается); неверный контекст/направление даёт нули с WARN.
 * @param delay_set Выход: число успешных установок delay таймеров
 * @param delay_fire Выход: число срабатываний delay таймеров
 * @param shaper_set Выход: число установок shaper таймеров
 * @param shaper_fire Выход: число срабатываний shaper таймеров
 */
void dummynet_get_debug_counters(struct dummynet *dn, int direction,
                                  uint64_t *delay_set, uint64_t *delay_fire,
                                  uint64_t *shaper_set, uint64_t *shaper_fire);

/* INLINE: перехват native UDP ETCP_LINK. SOCKS5 и TCP не поддерживаются.
 * Потеря возвращает len как успешную отправку; фактически socket_sendto не вызывается.
 * Реальная ошибка отправки возвращается вызывающему и учитывается в dropped.
 * Размер UDP здесь задаёт ETCP; лимит 1500 относится только к FULL. */

struct ETCP_LINK;
struct dummynet_filter;

/* Создать фильтр для ua (обязателен); NULL при ошибке. ua заимствован, сокетов/таймеров у фильтра нет. */
struct dummynet_filter* dummynet_filter_create(struct UASYNC* ua);
/* Установить вероятность 0..1000; превышение ограничивается до 1000 с WARN. */
void dummynet_filter_set_loss(struct dummynet_filter* df, uint32_t loss_permille);
/* Присоединить к живому native UDP link того же ua; 0/-1. Занятый hook не заменяется.
 * Повтор к своему link идемпотентен. При переходе к другому прежний отвязывается после проверки нового.
 * Фильтр не владеет линком; link close отвязывает фильтр, сохраняя сам фильтр/статистику для повторного attach. */
int dummynet_filter_attach(struct dummynet_filter* df, struct ETCP_LINK* link);
/* Снять свой hook; NULL/уже отвязанный допустимы. При внешней подмене сохранить чужие fn/ctx и выдать WARN. */
void dummynet_filter_detach(struct dummynet_filter* df);
/* Отвязать и освободить фильтр; NULL допустим. Линк/ядро могли быть уже уничтожены. */
void dummynet_filter_destroy(struct dummynet_filter* df);
/* Заимствованная статистика или NULL. INLINE: queue_size/queue_max/max_queue_delay_us всегда 0. */
const struct dummynet_stats* dummynet_filter_get_stats(struct dummynet_filter* df);
/* Добавить/снять блокировку IPv4 IP:порта в network byte order, оба ненулевые.
 * 0 — выполнено/повтор, -1 — неверный аргумент или заполнены 4 различных слота (только block).
 * Block проверяется до случайной потери. Отсутствующий unblock идемпотентен. */
int dummynet_filter_block_addr(struct dummynet_filter* df, uint32_t ip, uint16_t port);
int dummynet_filter_unblock_addr(struct dummynet_filter* df, uint32_t ip, uint16_t port);


#ifdef __cplusplus
}
#endif
#endif // DUMMYNET_H
