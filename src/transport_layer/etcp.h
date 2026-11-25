/* etcp — надёжный зашифрованный транспорт с несколькими линками к одному пиру.
 * Хранит очереди, ACK, повторы и состояние транспортной сессии; normalizer
 * разбивает/собирает кодограммы. Сервисы используют etcp_api.h или etcp_router.h.
 * Владельцы прямых подключений работают через NCD. Функции этого заголовка —
 * внутренний API стека, выполняемый в потоке uasync экземпляра. */
#ifndef ETCP_H
#define ETCP_H

#include "etcp_connections.h"
#include "secure_channel.h"
#include "etcp_padding.h"
#include "etcp_tx.h"
#include "../lib/ll_queue.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "pkt_normalizer.h"

struct stcp_link;  // forward declaration

// Forward declarations
struct UTUN_INSTANCE;
struct ETCP_CONN;
struct etcp_cbk_entry; // defined in etcp_api.h
struct UASYNC;

/* Младшие 16 бит локального времени в единицах 0.1 ms (циклический timestamp). */
uint16_t get_current_timestamp(void);

// ETCP packet section types (from protocol spec)
#define ETCP_SECTION_PAYLOAD    0x00  // Data payload
#define ETCP_SECTION_ACK        0x01  // ACK section
#define ETCP_SECTION_TIMESTAMP  0x06  // Channel timestamp
#define ETCP_SECTION_MEAS_TS    0x07  // Measurement timestamp for bandwidth (burst packet)
#define ETCP_SECTION_MEAS_RESP  0x08  // Measurement response (burst result)
#define ETCP_SECTION_FILLER     0x09  // Filler/dummy data (discarded by receiver)
#define ETCP_SECTION_ACK_REQUEST 0x0a // запрос актуальной логической границы, без DATA/BBR sample

// Burst measurement constants
#define BURST_PACKET_COUNT      12   // число пакетов в burst
#define BURST_SKIP_COUNT         3   // сколько первых пакетов пропустить при замере gap
#define MIN_BURST_INTERVAL_TB  5000   // минимальный интервал между burst (500ms в 0.1ms)
#define BURST_RESP_TIMEOUT_TB 20000   // таймаут ожидания ответа на burst (2s в 0.1ms)

// Inflight phases
#define INFLIGHT_PHASE_SLOW_START       0
#define INFLIGHT_PHASE_CONG_AVOIDANCE   1

// Section sizes
#define MEAS_TS_SECTION_SIZE    9   // type(1) + burst_id(2) + flags(1) + seq(1) + ts_us(2) + pkt_sz(2)
#define MEAS_RESP_SECTION_SIZE 13   // type(1) + burst_id(2) + valid(1) + gap_avg(4) + gap_min(4) + pkt_count(1)
#define FILLER_HDR_SIZE         3   // type(1) + len(2)

// MEAS_TS flags
#define MEAS_FLAG_IS_FILLER  0x01   // пакет содержит FILLER вместо PAYLOAD
#define MEAS_FLAG_IS_LAST    0x02   // последний пакет в burst
#define MEAS_FLAG_IS_FIRST   0x04   // первый пакет в burst

// MEAS_RESP valid
#define MEAS_RESP_VALID      1

#define INFLIGHT_STATE_WAIT_ACK  0
#define INFLIGHT_STATE_WAIT_SEND 1
#define INFLIGHT_INITIAL_HASH_SIZE 1024
#define ETCP_LINK_HISTORY_BYTES 32 // по одному биту для локального слота линка 0..255
#define ETCP_ACK_DELAY_US 2000 // обычный ACK-only: срок от старейшей записи; piggyback и срочный ответ не ждут

#define MAX_INFLIGHT_SIZE 16384 // максимальное число элементов в inflight приёмной очереди (для предотвращения атак)
#define ETCP_ACK_GAP_MAX_PACKETS 8192 // предел кумулятивного ACK за приём: 8192 × PACKET_DATA_SIZE(2048) = 16 MB (защита от DoS)
#define ASM_BUF_MAX_SIZE (64 * 1024) // максимальный размер буфера сборки фрагментов

// в этот список пакет добавляется когда перемещается из input_queue в input_send_q, при этом к пакету добавляется struct INFLIGHT_PACKET из inflight_pool.
// пакет полностью удаляется когда приходит ACK (либо conn_reset/close)
struct INFLIGHT_PACKET {// выделяется из etcp->inflight_pool
    struct ll_entry ll;
    uint32_t seq;                  // packet seq (ID по документации)
    struct ETCP_LINK* last_link;   // Заимствованный курсор выбора следующего пути; очищается перед free линка
    uint64_t last_timestamp;       // Время последней принятой отправки, 0.1мс; только диагностика, не RTO/BBR
    uint64_t last_attempt_id;      // Внутренний ключ последней принятой попытки (seq<<16)|номер; 0 — отправок не было
    uint16_t attempt_count;        // Выданные номера этого seq, 1..65535, общие для всех линков; исчерпание меняет эпоху
    uint32_t send_count;           // Статистика отправок с насыщением, без wrap
    uint32_t retrans_req_count;    // Число запросов повтора с насыщением
    uint32_t active_attempts;      // Попытки этого seq, ещё занимающие окна каналов
    uint8_t state;                 // WAIT_SEND — готов к отправке; WAIT_ACK — логически ожидает доставки
    uint8_t send_hist[8];          // local_link_id последних попыток; send_count%8 — следующая позиция до насыщения счётчика
    uint8_t used_links[ETCP_LINK_HISTORY_BYTES]; // принятые отправки текущего обхода; слот очищается до удаления/повторной выдачи ID
};

/* Проверить текущий обход каналов пакета; бит описывает живой локальный слот, а не идентичность физической попытки. */
static inline int etcp_packet_link_used(const struct INFLIGHT_PACKET* packet, uint8_t link_id) {
    return !!(packet->used_links[link_id >> 3] & (1u << (link_id & 7)));
}

// Список пакетов для сборки. собирается в ll_queue (используем быстрый поиск с хешем)
struct ETCP_FRAGMENT {// выделяется из пула etcp->rx_pool
    struct ll_entry ll;
    uint32_t seq;
    uint16_t timestamp;
};

struct ACK_PACKET {
    struct ll_entry ll;
    uint64_t attempt_id;           // Внутренний индекс (seq<<16)|номер; по сети передаётся только номер u16
    uint32_t seq;                  // Логический фрагмент
    uint64_t received_at_us;       // Время обработки DATA ядром после transport/crypto; начало задержки ACK, не чтение сокета.
};

#define ETCP_TX_STATE_DATA_WAIT 1
#define ETCP_TX_STATE_LINK_WAIT 2

// ETCP connection structure (refactored)
struct ETCP_CONN {
    // state описывает объект в реестре; доступность транспорта определяется links_up.
    int state; // 0=pending, 1=индексирован по peer_node_id, 2=удалён (phase 1 завершена)
    int ref_count; // External reference count. >0 blocks deferred resource free.
                    // Take/free via etcp_conn_ref_take()/etcp_conn_ref_free().

    int mtu;

    struct UTUN_INSTANCE* instance;

    // Запись в реестре instance->connections; известный пир индексируется по node_id.
    struct ll_entry* conn_queue_entry; // entry в очереди instance
    struct ll_queue*  conn_queue;      // заимствованная очередь, содержащая conn_queue_entry

    // Links (channels) - linked list
    struct ETCP_LINK* links;
    struct ETCP_LINK* last_rr_link; // последняя принятая DATA-отправка; пробный выбор/ACK курсор не меняет
    uint8_t last_assigned_link_id;   // последний выданный link_id (для циклического перебора)
    
    struct ETCP_PADDING padding; // диапазон и независимый таймер всех линков этого соединения

    // Crypto and state
    struct secure_channel crypto_ctx;
    struct PKTNORM* normalizer;
    
    // Peer info
    uint64_t peer_node_id;         // Peer node ID
    uint8_t peer_ed25519_pubkey[SC_PUBKEY_SIZE];  // Ed25519 pubkey пира (из INIT)
    uint8_t peer_sleep_phase;      // 1 = пир в SLEEP-фазе (троттлить), 0 = ACTIVE-окно (обмен)
    uint16_t peer_active_remaining; // до SLEEP-фазы (x0.1с), из keepalive
    uint16_t peer_sleep_duration;   // длительность SLEEP-фазы (x0.1с), из keepalive
    void* peer_phase_timer;        // таймер смены фазы троттлинга

// ============ Processing incoming data to be sent by ETCP
    struct ll_queue* input_queue;              // Incoming packets to send (rx_pool -> ETCP_FRAGMENT)

    // Inflight очереди (2 шт) - пока пакет в статусе inflight - к нему прикрепляется struct INFLIGHT_PACKET
    struct memory_pool*     inflight_pool;     // память для inflight очередей
    struct memory_pool*     attempt_pool;      // метаданные физических попыток; payload принадлежат inflight_pool
    struct memory_pool*     io_pool;           // память для rx очередей

    struct ll_queue*        input_send_q;      // новые DATA/повторы; выбирается готовый повтор либо готовый новый пакет, не только голова
    struct ll_queue*        input_wait_ack;    // очередь ожидающих подтверждение (inflight_pool -> struct INFLIGHT_PACKET)

    struct ll_queue*        recv_q;            // очередь на сборку фрагментированных пакетов(rx_pool -> struct ETCP_FRAGMENT) + index [+0 SEQ]

    void (*link_ready_for_send_fn)(struct ETCP_CONN*);// функцию которую должен вызвать драйвер линка при готовности линка принимать данные
    void* tx_token;                // единственный call_soon либо retry-таймер после OOM
    uint8_t tx_delayed;             // 1 — tx_token отменяется как timeout; 0 — как call_soon
    uint8_t tx_running;             // объединяет пробуждения внутри прохода, запрещает рекурсивную отправку

    struct ll_queue* output_queue; // Assembled outgoing packets (storage: ETCP_FRAGMENT / rx_pool)
    struct ll_queue* transit_queues;   // ожидающий транзит, ключ (group_id:8, src_node_id:8, dst_node_id:8)
    struct ll_queue* send_input_q;     // единая входная очередь отправки (normalizer->input или tx_queue)


    // IDs and state
    uint32_t next_tx_id;           // Следующий seq, 1..UINT32_MAX; перед повторным использованием нужна новая эпоха
    uint32_t active_attempts;      // Все физические записи, включая резерв send; максимум 2*MAX_INFLIGHT_SIZE
    uint32_t last_rx_id;           // Last received ID
    uint32_t last_delivered_id;    // Last delivered to output_queue

    uint32_t rx_ack_till;// из ack пакета - по какой пакет получено и собрано на удаленной стороне

    // Metrics (RTT, jitter, etc.)
    // uint16_t retrans_delay;       // Not used
    uint16_t rtt_last;            // Минимум измеренных рабочих links, 0.1мс; 0 — неизвестен
    uint16_t tt_last;// transmission time (время пакета до меня)
//    uint16_t rxt_last;// recv relative time (бустро регаирует на изменение задержки rx и медленно на изменение задержки tx)
    uint16_t rtt_avg_10;           // Максимальный RTT среди links, в 0.1мс; имя историческое
    uint16_t jitter;               // Среднее по links от link->jitter/32768, в 0.1мс (удвоенный EWMA |ΔRTT|)
    uint32_t bytes_sent_total;
    // uint32_t bytes_received_total;  // Not used
    uint32_t retransmissions_count;
    uint32_t reinit_count;
    uint64_t setup_start_tb;     // время начала/рестарта подключения (0.1ms)
    uint32_t reset_count;
    // Диагностика за lifetime соединения: reset не обнуляет накопительные счётчики.
    uint64_t diagnostic_acked_bytes;    // Уникальные логические DATA-байты, подтверждённые пиром
    uint64_t diagnostic_wire_tx_bytes; // ETCP-кадры, принятые драйверами: включая ACK/повторы, без обрамления носителя
    uint64_t diagnostic_wire_rx_bytes; // ETCP-кадры от драйверов, до декодирования; без обрамления носителя
    uint64_t diagnostic_id;            // Постоянный случайный ID объекта; reset_id может меняться
    uint64_t diagnostic_next_link_id;  // Поколения линков этого объекта, не переиспользуются при освобождении local_link_id
    uint64_t diagnostic_ack_us;         // Последний реальный логический ACK; 0 — нет измерения текущей сессии
    uint64_t diagnostic_retries;        // Повторные DATA, действительно принятые транспортом
    uint64_t diagnostic_rtos;           // Физические DATA-попытки, снятые по RTO
    uint64_t tx_report_us;              // Начало окна секундной диагностики отправителя
    uint64_t tx_report_acked;           // Снимок diagnostic_acked_bytes на начало окна
//    uint32_t bytes_sent_norx;      // сколько отправили байт без единого ответного пакета (для детекции запроса реконнекта)

    // Window and inflight management
    uint32_t unacked_bytes;        // логические DATA-байты input_send_q + input_wait_ack; не физический flight копий
    uint32_t max_inflight;         // Max inflight cap from config (bbr_max_cwnd)
    uint32_t optimal_inflight;     // сумма индивидуальных окон; общий бюджет подгрузки новых DATA с учётом unacked_bytes

    // Timers
    void* ack_resp_timer;          // один таймер ближайшего срока ACK всех линков/логической границы
    uint64_t ack_resp_deadline_us; // абсолютный срок установленного таймера; 0 без таймера
    void* ack_window_timer;        // запрос потерянного till, только при заполненном приёмном окне
    uint64_t ack_till_deadline_us; // срок передачи текущей логической границы по рабочему пути; 0 — нет запроса
    uint8_t ack_request_pending;   // нужно запросить границу у пира
    void* down_timer;              // удаление после непрерывного DOWN, включая первоначальное ожидание
    uint64_t down_deadline_tb;     // сохраняется при наличии NCD handles после истечения таймера

    // Bandwidth measurement state
    // uint8_t burst_in_progress;     // Burst transmission flag - Not used
    // uint16_t burst_start_id;       // Start ID for burst - Not used

    // Statistics counters
    uint32_t ack_packets_count;      // Count of ACK packets received
    // uint16_t last_rx_ack_id;         // Last ACK ID received - Not used
    uint16_t rtt_history[10];        // RTT history for jitter calculation (RTT_HISTORY_SIZE=10)
    uint8_t rtt_history_idx;         // Current index in RTT history
    // uint32_t total_packets_sent;     // Total packets sent counter - Not used

    // Flags
    uint8_t got_initial_pkt;         // 
    uint8_t initialized;             // 1 — поток готов; 0 — создание или reset, в том числе ожидание подтверждения сессии
    uint64_t reset_id;               // локальная эпоха потока (меняется только при локальном reset)
    uint64_t peer_reset_id;          // эпоха пира: первая из handshake, смена — только после CONFIRM
    uint64_t session_candidate, session_challenge, session_started; // эпоха-кандидат, challenge, время начала (0.1 ms)
    void* session_timer;            // таймер согласования транспортной сессии
    uint8_t session_required;       // только подтверждение challenge может завершить локальный reset
    uint64_t session_data_errors;   // отвергнутые заголовки DATA; ограничивает повторяющиеся сообщения об ошибке
    uint8_t tx_state;                // 0 — не запускался; DATA_WAIT — нет работы; LINK_WAIT — ожидает ресурс/сессию
    uint8_t links_up;                // 0 - канал не готов для передачи, 1 - канал готов для передачи (хотя бы один линк не down)
    uint8_t reset_done;              // 0 - рукопожатие не завершено (реинит разрешён), 1 - соединение стабильно (реинит заблокирован)
    int callbacks_running;           // счётчик вложенности колбэк-цепочек (>0 — внутри цепочки, etcp_connection_close откладывается)

    uint8_t close_requested;         // новые операции запрещены, phase 1 может ждать выхода из callback
    uint8_t close_running;          // защита от повторного входа в close
    uint8_t delete_complete;        // detach и рассылка DELETE завершены
    uint8_t free_scheduled;         // освобождение уже поставлено в uasync
    void* close_token;              // отложенный close при вызове внутри callback

    // Unified callback chain with event mask (init/reinit/up/down/node_changed)
    struct etcp_cbk_entry* cbks;
    uint8_t reinit_pending;              // 1 = reinit в процессе, ждём завершения

    uint8_t fin_wait : 1;                            // close-pending: ожидаем подтверждение закрытия от remote
    void  (*fin_wait_clear_cb)(struct ETCP_CONN* conn, void* arg); // вызывается при сбросе fin_wait
    void* fin_wait_clear_arg;

    uint32_t cnt_ack_hit_inf;        // счетчик удлений из inflight
    uint32_t cnt_ack_hit_sndq;       // счетчик удалений inflight пакетов из sndq
    uint32_t cnt_ack_miss;           // счетчик не найденных ack
    uint32_t cnt_link_wait;          // счетчик переходов в ожидание когда link busy
    uint32_t rx_dup_count;           // счетчик принятых дубликатов (локальные)
    uint32_t tx_dup_count;           // счетчик дубликатов удаленной стороны (из ACK)
    uint32_t debug[8];               // 8 значений для дебага (live watch)

    // Logging identifier (format: "XXXX→XXXX [name]" - last 4 digits of local and peer node_id + optional name)
    char log_name[256];
    char* name;                  // Connection name from config (e.g., "client_test1"), or NULL/empty
};


/* Создать транспортный объект в instance, name копируется (может быть NULL).
 * Жизненным циклом прикладного соединения управляет NCD. Возвращает объект либо NULL при ошибке/отмене из callback.
 * New-connection/NEW callbacks могут закрыть объект; после отмены конструктор завершает закрытие и не возвращает handle. */
struct ETCP_CONN* etcp_connection_create(struct UTUN_INSTANCE* instance, char* name);

#define ETCP_DOWN_TIMEOUT_TB 300000 /* 30 секунд в 0.1 ms */

/* Поставить удаление DOWN без NCD handles с учётом уже прошедшего срока; -1 при отказе таймера. */
int etcp_conn_schedule_down_close(struct ETCP_CONN* conn);

/**
 * @brief Закрыть соединение: фаза-1 detach + отложенная фаза-2 очистка.
 *
 * Порядок коллбэков при закрытии (строгая последовательность):
 *   1) если соединение ещё UP — links_up = 0 и финальный per-connection DOWN;
 *   2) state = 2;
 *   3) per-connection DELETE, затем instance-level DELETE;
 *   4) cleanup: таймеры, routing, линки, очередь, deferred u_free.
 *
 * При вызове внутри callback закрытие откладывается, close_requested ставится сразу.
 * После state==2 допустим только DELETE; ref_count задерживает освобождение памяти.
 */
void etcp_connection_close(struct ETCP_CONN* etcp);
/* Индексировать уже созданный conn по полному X25519 до уведомления о линках.
 * Переиспользует запись реестра без аллокаций и callbacks; 0 — успех, -1 — ошибка/дубликат.
 * При ошибке вызывающий закрывает conn. Не переводит транспорт в READY. */
int etcp_conn_index_peer(struct ETCP_CONN* conn);
/* Настроить 32-байтный X25519-ключ пира пустого conn до добавления линков; 0/-1.
 * Получает node_id из ключа, не публикует UP и не запускает I/O. Отказ не меняет conn. */
int etcp_connection_set_peer(struct ETCP_CONN* conn, const uint8_t* public_key);
/* Перевести pending в ready и уведомить INIT/UP; при ошибке индекса закрыть conn. */
void etcp_conn_queue_set_ready(struct ETCP_CONN* conn);

/**
 * @brief Take a reference on the connection (blocks deferred resource free).
 * @param conn connection
 * @return 0 on success, -1 if conn is NULL, closing or already deleted (state 2).
 *
 * Increments ref_count and retains the connection object and phase-2 resources.
 * Close still frees the normalizer and physical links during phase 1; this reference
 * does not keep those pointers valid. Active callbacks defer phase 1 via callbacks_running.
 * Always pair with etcp_conn_ref_free().
 */
int  etcp_conn_ref_take(struct ETCP_CONN* conn);

/**
 * @brief Release a reference. Schedules phase-2 free after close completes and the last ref/callback exits.
 * @param conn connection
 *
 * Decrements ref_count. If close phase 1 is complete, no references/callbacks remain,
 * and free has not been scheduled, schedules phase-2 cleanup via uasync_call_soon.
 * Safe to call on NULL.
 */
void etcp_conn_ref_free(struct ETCP_CONN* conn);

/* Очистить старый поток без выделений памяти/вызова callbacks очередей; DATA и ожидания отбрасываются.
 * Эпохи/физические каналы сохраняются; это внутренняя очистка, внешний перезапуск — fatal_reinit(). */
void etcp_conn_reset(struct ETCP_CONN* etcp);
void etcp_links_reset(struct ETCP_CONN* etcp);

/* Локальный reset: эквивалент fatal_reinit(); etcp — соединение, reason — причина для лога. Возврата нет. */
void etcp_conn_reinit(struct ETCP_CONN* etcp, const char* reason);
/* Сбросить поток etcp с заданной локальной эпохой reset_id; reason — причина для лога. Возврата нет.
 * Не генерирует эпоху и не запускает подтверждение. При подтверждённой смене эпохи пира передают текущий etcp->reset_id. */
void etcp_conn_reinit_id(struct ETCP_CONN* etcp, const char* reason, uint64_t reset_id);
/* Сгенерировать новую локальную эпоху, сбросить поток etcp и запустить подтверждение сессии.
 * reason — причина для лога; DATA ждёт подтверждения, физические линки сохраняются. Возврата нет.
 * Старые DATA, в том числе ACKed транспортом, не воспроизводятся. После согласования владельцы получают REINIT
 * и завершают/повторяют свои операции. При невозможности согласования (RNG/таймер/таймаут) соединение закрывается. */
void etcp_conn_fatal_reinit(struct ETCP_CONN* etcp, const char* reason);
/* Учесть эпоху peer_reset_id из handshake соединения conn: первую сохранить, смену предложить для challenge.
 * Нулевая и прежняя эпохи игнорируются. Сброс при смене выполняется после CONFIRM, своя эпоха сохраняется. Возврата нет. */
void etcp_conn_apply_peer_reset_id(struct ETCP_CONN* conn, uint64_t peer_reset_id);

// Отправка: используем api ll_queue для очереди ETCP_CONN.input_queue
// Прием: используем api ll_queue для очереди ETCP_CONN.output_queue
// для очередей используется формат 

// Input from etcp_connections (decrypted packet)
void etcp_conn_input(struct ETCP_DGRAM* pkt);

// Send data through ETCP connection
// Allocates memory from data_pool and places in input queue
// Returns: 0 on success, -1 on failure
int etcp_int_send(struct ETCP_CONN* etcp, const void* data, uint16_t len);

/* Подготовить следующий пакет из pkt_pool либо NULL при ожидании/ошибке/отсутствии работы.
 * Наступивший/срочный ACK своего пути имеет приоритет; готовый DATA этого пути забирает его piggyback.
 * Сканирует send_q: готовые повторы идут перед новыми DATA, заблокированные записи пропускаются.
 * Короткая измерительная burst-серия завершается до перехода к ACK другого пути.
 * При отсутствии кандидата допускает одну подгрузку в рамках общего/приёмного окна; callback может сделать reset/close.
 * Пакет с DATA содержит резерв попытки: etcp_loadbalancer_send завершает send/rollback и возвращает пакет в пул. */
struct ETCP_DGRAM* etcp_request_pkt(struct ETCP_CONN* etcp);

/* Запросить один отложенный проход отправителя; не меняет UP/DOWN. Все вызовы — в uasync.
 * Используется очередями, ACK/RTO, изменением окна и подтверждением сессии. close/reset отменяет запуск. */
void etcp_conn_wake_sender(struct ETCP_CONN* conn);

/* Пересчитать единственный ACK-таймер по головам очередей и ack_till_deadline_us; просроченную работу передать sender.
 * Вызвать после RX/продвижения till, завершения send и смены доступных путей. NULL/закрытый conn допустимы.
 * Piggyback не ждёт сроков. При OOM таймера существующие флаги срочного ACK обеспечивают немедленный fallback. */
void etcp_conn_schedule_ack(struct ETCP_CONN* conn);

// Process ACK receipt - remove acknowledged packet from inflight queues
void etcp_ack_recv(struct ETCP_CONN* etcp, uint32_t seq);

// Recalculate optimal_inflight and resume input queue after link inflight_lim change
void etcp_conn_on_inflight_lim_changed(struct ETCP_CONN* etcp);

// Fire connection status callback on instance's conn_status_cbks chain
void etcp_fire_conn_status(struct ETCP_CONN* conn, int status);

// Update log_name when peer_node_id becomes known
void etcp_update_log_name(struct ETCP_CONN* etcp);

// Вызывается стеком etcp когда соединение установлено (можно передавать данные)
void etcp_conn_ready(struct ETCP_CONN* conn);

/* Uasync: пересчитать минимум ненулевых RTT рабочих линков; без SQL/уведомлений. NULL допустим. */
void etcp_conn_update_rtt(struct ETCP_CONN* conn);
void etcp_on_link_down(struct ETCP_CONN* etcp, struct ETCP_LINK* down_link);

void etcp_update_mtu(struct ETCP_CONN* etcp);

#ifdef __cplusplus
}
#endif

#endif // ETCP_H
