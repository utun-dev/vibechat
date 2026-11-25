// etcp_router.h — Сервисный слой маршрутизации поверх ETCP
// Маршрутизирует сервисные пакеты до целевой ноды, на промежуточных нодах ретранслирует
// Упрощённый TCP поверх ETCP: восстановление порядка, дедупликация, ретрансмиты, inflight-контроль.
// Подпись/шифрование вынесены в автономный модуль route_crypto (encode/decode).
// Все сетевые пакеты CHAT (данные, handshake, ACK, CLOSE) шифруются с подписью автора внутри.
// Router сам выбирает эту защиту, независимо от mode сервиса. Подпись покрывает заголовок
// и открытый payload; ключи берутся из MEMBERS группы. Транзит не расшифровывает пакет.
// mode сервиса управляет только прежним форматом UTUN; локальный loopback не шифруется.
// Логический канал задаётся (group_id, remote_ed25519, svc_id), физический транспорт берётся у топологии.
// Сервисы регистрируют приём через bind, отправляют через route_send и соблюдают backpressure.
// Незарегистрированный CHAT-сервис не создаёт входящий канал и не принимает DATA. Ответы control на исходящий канал разрешены.
// Транзит не требует локального bind.
// Все операции и callbacks — в потоке uasync. Закрытие группы удаляет её каналы и ожидающий транзит.
//
// Построение маршрутов описано в topo_group.h. Роутер берёт текущий живой next hop
// из таблицы группы; при ожидании свободной очереди проверяет выбор повторно.
// Во время сходимости BGP возможны временные петли. Источник записывает свой ID,
// каждый транзитный узел проверяет список пройденных узлов и дописывает свой ID.
// Повторное посещение запрещено для всех пакетов, включая ACK и handshake.
// Ожидание в очереди не добавляет ID повторно; ретрансмит источника начинает новый список.
//
// Формат SVC_ROUTE пакета:
//   [cmd:1] [group_key:32] [seq:4] [svc_id:1] [flags:1] [timestamp:2] [reset_id:8] [peer_reset_id:8] [challenge:8]
//   [src] [dst] [count:1] [visited × count] [body...]
//   Полный адрес занимает 33 байта (0x80 и Ed25519), однозначный short31 — 4 байта в сетевом порядке.
//   В DATA разрешены короткие адреса переходов, а при шифровании также src и dst. В служебных пакетах все адреса полные.
//   CHAT body = encrypt(payload || signature:64); UTUN body = payload/шифртекст || signature?:64.
//   count=1..ROUTER_MAX_VISITED; IDs идут от источника до отправителя текущего хопа.
//   Изменяемый список находится вне end-to-end подписи/шифрования. Получатель
//   проверяет его границы, уникальность, первый ID=src и последний ID=ETCP peer.
//   Это защита от ошибок маршрутизации, а не доказательство пути от недоверенного пира.
//   flags: bit7=START, bit6=RST, bit3=SIGNED, bit2=ENCRYPTED, bit1=CLOSE, bit0=RECEIVE_BUSY (ACK)
//   reset_id/peer_reset_id — идентификаторы отправителя/получателя во всех пакетах.
//   START=HELLO, RST=CHALLENGE, START|RST=CONFIRM; challenge используется только в handshake.
//   ACK-пакет: тот же заголовок, seq=rx_seq, payload_len=0; RECEIVE_BUSY поддерживает ожидание без продвижения seq.
// Локальная доставка через etcp_route_send выполняется синхронно без сетевого канала/crypto/receive_ready.
#ifndef ETCP_ROUTER_H
#define ETCP_ROUTER_H

#ifdef __cplusplus
extern "C" {
#endif


#include <stdint.h>
#include "etcp_api.h"
#include "route_tx_stats.h"
#include "pkt_normalizer.h"
#include "secure_channel.h"

// ====================================================================
// 1. Протокол SVC_ROUTE (wire-формат и формат доставки в сервис)
// ====================================================================

#pragma pack(push, 1)
struct SVC_ROUTE_HDR {
    uint8_t  cmd;          // ETCP_ID_SVC_ROUTE (0x03)
    uint8_t group_key[32]; // полный адрес группы в подписанном заголовке
    uint32_t seq;          // data: tx_seq; ACK: rx_seq (ожидаемый seq)
    uint8_t  svc_id;       // идентификатор сервиса
    uint8_t  flags;        // ROUTER_FLAG_*; RECEIVE_BUSY допустим только в ACK без payload
    uint16_t timestamp;    // часы отправителя (0.1ms), для RTT; ACK: эхо последнего data-пакета
    uint64_t reset_id;     // случайный идентификатор локального экземпляра router_conn
    uint64_t peer_reset_id; // идентификатор экземпляра получателя (0 только в HELLO)
    uint64_t challenge;    // случайный запрос/эхо для подтверждения актуальности пира
    uint8_t src_ed[SC_PUBKEY_SIZE], dst_ed[SC_PUBKEY_SIZE]; // канонические адреса, включены в подпись
    uint64_t dst_node_id, src_node_id; // производные числа для внутренних вызовов, на проводе отсутствуют
    uint64_t group_id; // локальный индекс. Для подписи это поле всегда нулевое.
};
#pragma pack(pop)
#define SVC_ROUTE_HDR_SIZE      sizeof(struct SVC_ROUTE_HDR)
#define SVC_ROUTE_MAX_BINDINGS  256
#define ROUTER_MAX_VISITED      64  // предел размера списка, не замена проверки повторных ID
/* Бюджет пакета учитывает два полных адреса, все переходы и байт счётчика.
 * Постоянные поля исключают две позиции адресов из канонического заголовка. */
#define ROUTER_FIXED_FIELDS offsetof(struct SVC_ROUTE_HDR, src_ed)
#define ROUTER_FULL_ADDRESS_SIZE (1u + SC_PUBKEY_SIZE)
#define ROUTER_MAX_ENCODED_BODY (PKTNORM_MAX_DGRAM_SIZE - ROUTER_FIXED_FIELDS - (2u + ROUTER_MAX_VISITED) * ROUTER_FULL_ADDRESS_SIZE - 1u)
#define ROUTER_ENCRYPTION_OVERHEAD (SC_NONCE_SIZE + SC_CRC32_SIZE + SC_TAG_SIZE)
#define ROUTER_SIGNED_ENCRYPTED_LIMIT (ROUTER_MAX_ENCODED_BODY - ROUTER_ENCRYPTION_OVERHEAD - SC_SIGN_SIZE)

/* Максимум открытых данных одной сетевой DATA в байтах для эффективных SIGNED/ENCRYPTED
 * флагов. CHAT всегда использует оба флага. Заголовки маршрутизации в лимит не входят. */
size_t etcp_router_payload_limit(int mode);

// Единый формат доставки сервисной кодограммы (router → сервис):
//   [svc_id:1][rx_flags:1][group_id:8][src_ed:32][dst_ed:32][payload...]
// src/dst — реальные end-to-end узлы из SVC_ROUTE заголовка (не промежуточные).
// rx_flags — биты ROUTER_FLAG_ENCRYPTED/SIGNED, бывшие на wire (статус для сервиса).
// group_id — группа SVC_ROUTE-пакета (для per-group сервисов вроде conn_mgr).
#define ROUTER_SVC_FLAGS_OFF    1
#define ROUTER_SVC_GROUP_OFF    2
#define ROUTER_SVC_SRC_ED_OFF   10
#define ROUTER_SVC_DST_ED_OFF   42
#define ROUTER_SVC_PAYLOAD_OFF  74
#define ROUTER_SVC_HDR_SIZE     ROUTER_SVC_PAYLOAD_OFF

// Биты в flags
#define ROUTER_FLAG_START      0x80
#define ROUTER_FLAG_RST        0x40
#define ROUTER_FLAG_ENCRYPTED  0x04   // payload зашифрован AES-128-CCM (nonce+tag+crc)
#define ROUTER_FLAG_SIGNED     0x08   // Ed25519 подпись: CHAT внутри шифрования, UTUN после payload
#define ROUTER_FLAG_CLOSE      0x02   // нормальное закрытие conn
#define ROUTER_FLAG_RECEIVE_BUSY 0x01 // ACK: сервис приостановил приём, seq не подтверждает отложенные пакеты

// ====================================================================
// 2. Внутренние структуры и константы (etcp_router.c, control_server.c)
// ====================================================================

#define TRANSIT_QUEUE_HASH 256

/* Одноразовый запрос слота отправки. Обнулить перед использованием; живёт до callback/отмены.
 * Только uasync. Router снимает связь до callback; close/reset автоматически отменяет запрос. */
struct ETCP_ROUTER_CONN;
struct router_send_request {
    struct ETCP_ROUTER_CONN *conn; /* Владелец регистрации, NULL после снятия. */
    void (*ready)(struct ETCP_ROUTER_CONN*, void*); /* Формирует не более одной кодограммы через conn_send. */
    void *arg; /* Заимствованный контекст сервиса. */
    uint64_t since_us; /* Начало ожидания слота, без сброса при повторной регистрации. */
};

// Очередь транзитных пакетов для (group_id, src_ed, dst_ed).
// Создаётся когда не получается отправить пакет в ETCP напрямую,
// удаляется когда очередь становится пустой.
struct TRANSIT_QUEUE {
    struct ll_entry ll;                // Первые 72 байта data содержат group_id, src_ed и dst_ed для хеш-индекса.
    uint64_t group_id;                 // = data[0..7]
    uint8_t src_ed[SC_PUBKEY_SIZE], dst_ed[SC_PUBKEY_SIZE]; // ключ с group_id для индекса очереди
    uint64_t src_node_id, dst_node_id; // производные ID для журналов
    struct ll_queue* q;                // FIFO ещё не переданных транспорту пакетов
    struct queue_waiter_handle waiter; // ожидание свободной conn->send_input_q
    struct ETCP_CONN* conn;            // владелец очереди и waiter; текущий маршрут проверяется при выгрузке
    void* route_timer;                 // повторный выбор пути, даже если прежняя очередь остаётся занятой
};

// Состояние одного логического подключения (group_id + remote_ed + svc_id)
struct ETCP_ROUTER_CONN {
    struct ll_entry ll;
    uint64_t group_id;            // Первое поле ключа индекса
    uint8_t remote_ed[32];        // Ed25519 удалённого участника
    uint8_t svc_id;               // Последнее поле ключа индекса
    uint64_t remote_node_id;      // Производный адрес для потребителей числового API
    uint16_t last_dgram_ts;       // последний timestamp датаграммы (rx или tx, timebase 0.1ms)
    uint16_t rtt;                 // измеренный RTT (0.1ms)
    uint32_t rtt_jitter;          // EWMA |ΔRTT|, беззнаковый fixed-point *65536; монитор показывает удвоенный EWMA
    uint16_t max_inflight;        // настраиваемый рабочий максимум (default=ROUTER_MAX_INFLIGHT)
    uint16_t inflight_limit;      // текущий эффективный лимит (=max_inflight или MINRTT_PROBE_MAX_INFLIGHT)
    uint16_t minrtt;              // скользящее среднее minRTT (0.1ms), default=100 (10ms)
    uint16_t minrtt_window[10];   // окно 10 последних замеров minRTT
    uint64_t minrtt_window_tb[10];// таймстемпы замеров (timebase 0.1ms)
    uint8_t  minrtt_window_idx;   // cyclic index 0..9
    uint8_t  minrtt_window_count; // активных записей в окне (0..10)
    uint8_t  minrtt_probe;        // режим измерения (inflight capped at 4)
    uint64_t last_inflight_loaded_tb;   // когда inflight >= threshold
    uint64_t last_inflight_unloaded_tb; // когда inflight < threshold
    uint8_t  inflight_was_loaded;       // 1=канал загружен
    uint16_t last_recv_pkt_ts;    // timestamp пира из последнего data-пакета
    uint64_t last_recv_pkt_local_tb; // наше время приёма последнего data-пакета (0.1ms timebase)
    uint8_t  last_recv_updated;   // флаг: есть данные для эха в ACK

    uint32_t tx_seq;              // следующий seq для отправки
    uint32_t tx_sent;             // граница фактически отправленных пакетов (ACK <= tx_sent)
    uint32_t rx_seq;              // ожидаемый seq для сборки (next expected)
    uint32_t tx_acked;            // следующий seq после подтверждённых пакетов, модульный uint32, не накопленный счётчик
    uint32_t last_sent_ack_seq;   // последний отправленный ACK (= rx_seq на момент отправки)
    uint64_t last_ack_sent_tb;    // время отправки последнего ACK (timebase 0.1ms)

    struct UTUN_INSTANCE* inst;
    struct ETCP_CONN* recv_conn; // заимствованный транспорт последнего DATA; NULL после reset/уничтожения транспорта
    struct ll_queue* recv_q;     // reorder очередь: hash по seq (4 байта, offset 0)
    void*    ack_timer;           // периодический таймер (10ms при consumer_ack)
    void*    idle_ack_timer;      // idle таймер (дослать последний ack)

    struct ll_queue* send_q;     // очередь ожидающих отправки (inflight полон)
    struct router_send_request *send_request; /* Ленивый producer: payload ещё принадлежит сервису. */
    uint8_t send_producing;      /* Запрет рекурсивного kick внутри ready callback. */
    struct queue_waiter_handle send_waiter; // waiter на send_input_q (по 1 пакету, round-robin)
    struct ll_queue* send_waiter_q; // очередь регистрации, не результат повторного поиска маршрута
    uint32_t retrans_seq;         // следующий seq текущего прохода ретрансмитов
    uint32_t retrans_end;         // конец прохода (исключительно)
    void*    watchdog_timer;     // DEBUG-watchdog: медленная проверка инварианта drain
    uint8_t  send_blocked;       // 1 = inflight полон, ждём ack
    uint8_t  no_route;           // 1 = нет BGP-маршрута, передача приостановлена
    void*    no_route_timer;     // таймер 20ms проверки появления маршрута
    uint8_t  no_ack_count;       // проходы с реально отправленным повтором без прогресса ACK; насыщается
    uint8_t  retrans_started;    // текущий проход уже отправил хотя бы один повтор
    uint8_t  closed;             // 1 = в процессе закрытия, таймеры игнорируют
    uint64_t reset_id;           // собственный идентификатор; не меняется при рестарте пира
    uint64_t peer_reset_id;      // подтверждённый идентификатор пира (0 = handshake не завершён)
    uint64_t pending_peer_id;    // кандидат, ещё не имеющий права менять состояние сессии
    uint64_t pending_challenge; // случайный запрос подтверждения кандидату
    uint64_t challenge_started_tb; // начало ограниченного срока nonce; повторы не продлевают, 0.1ms
    uint64_t challenge_sent_tb; // последняя отправка того же nonce; ограничивает частоту повторов, 0.1ms
    void* handshake_timer;     // таймер повторов согласования
    uint8_t  start_sent;         // получен первый ACK данных

    // Ретрансмиты: inflight очередь
    struct ll_queue* inflight_q;        // хеш по seq (4 байта), непрерывный блок без SACK
    void*    retrans_timer;             // таймер проверки застоя ACK
    uint64_t last_ack_changed_tb;       // начало ожидания или продвижение ACK (0.1ms); обычные каналы учитывают также RECEIVE_BUSY
    uint64_t retrans_due_tb;            // срок следующего прохода; после отправки повтора учитывает backoff
    uint64_t retrans_srtt_tb;           // EWMA первого submit->ACK, без собственной send_q; 0 = нет измерений
    uint64_t retrans_rttvar_tb;         // EWMA отклонения submit->ACK; только пакеты без повторов

    // Очередь приёма входящего трафика (между сетью и recv_q)
    struct ll_queue* incoming_q;        // FIFO очередь входящих пакетов
    uint8_t  receive_blocked;           // сервис временно не принимает следующую кодограмму
    uint8_t  peer_receive_blocked;      // последний ACK пира объявил паузу приёма
    void* receive_pump;                 // отложенное продолжение приёма

    uint32_t c_pkts_sent;        // успешные отправки данных
    uint32_t c_pkts_send_err;    // ошибки отправки
    uint32_t c_pkts_rcvd;        // получено и доставлено данных
    uint32_t c_ack_sent;         // отправлено ACK
    uint32_t c_ack_recv;         // получено ACK (последовательных)
    uint32_t c_retrans_done;     // число выполненых ретрансмитов
    uint32_t c_dup_dropped;      // дропнуто дубликатов seq
    uint32_t c_oob_dropped;      // дропнуто out-of-bounds seq
    uint32_t c_stale_ack;        // ACK вне диапазона или без продвижения seq, включая допустимый повтор RECEIVE_BUSY
    uint32_t c_sign_fail;        // signature verification failures
    uint64_t delivery_rtt_us; // EWMA enqueue->ACK (1/8), включает send_q/нижние очереди/обратный путь.
    struct route_tx_meter delivery; // первые подтверждения исходного payload, без wire overhead
};

/* Заимствований нет. queued/inflight — исходный payload, а не размер криптограммы.
 * ACK подтверждает доставку callback сервиса; декодирование/показ сервис отслеживает отдельно. */
struct ETCP_ROUTER_TX_STATS {
    uint64_t local_epoch, peer_epoch, queued_bytes, inflight_bytes, acked_bytes, acked_packets;
    uint32_t queued_packets, inflight_packets;
    uint32_t ack_next_seq; // Первый неподтверждённый seq в текущей эпохе, с переходом через UINT32_MAX.
    uint64_t delivery_rtt_us, last_ack_us; // Полный RTT enqueue->ACK и монотонное время последнего продвижения ACK.
    uint64_t queued_age_us, inflight_age_us; // Возраст головы от исходного enqueue; 0 при пустой очереди.
    struct route_tx_window window;
    uint32_t rtt_us, baseline_rtt_us;
    int ready, no_route, receive_busy, probing;
};

/* Только uasync. 0 — снимок, 1 — канал отсутствует/закрыт (out обнулён), -1 — неверные аргументы/время.
 * now_us — монотонные микросекунды, не раньше последнего учтённого ACK. При -1 out не меняется; канал не создаётся.
 * ready означает подтверждённый peer_reset_id, отдельно от READY топологии или готовности физического транспорта. */
int etcp_router_tx_stats(struct UTUN_INSTANCE *inst, uint64_t group, uint64_t peer, uint8_t svc,
                         uint64_t now_us, struct ETCP_ROUTER_TX_STATS *out);
/* Снимок передачи по полной идентичности адресата; 0 — найден, 1 — отсутствует, -1 — ошибка. */
int etcp_router_tx_stats_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32], uint8_t svc,
                             uint64_t now_us, struct ETCP_ROUTER_TX_STATS* out);

#define ROUTER_CONN_HASH_SIZE     256
#define ROUTER_RECVQ_HASH_SIZE    1024
#define ROUTER_MAX_INFLIGHT       256    // макс пакетов в полёте (для контроля inflight)
#define ROUTER_ACK_INTERVAL_TB    100    // интервал ACK: 10ms в timebase (0.1ms)
#define ROUTER_ACK_IDLE_TB        5000   // idle таймаут: 500ms
#define ROUTER_ACK_BUSY_TB        1000   // 100ms: живой приёмник ждёт диск, а не потерял транспорт
#define ROUTER_MAX_SEND_Q_PACKETS 64     // порог backpressure на send_q
#define ROUTER_SEND_WATCHDOG_TB   5000   // DEBUG-watchdog: 500ms проверка инварианта drain

// Ретрансмиты
#define ROUTER_HANDSHAKE_TIMEOUT_TB 3000 // 300ms: согласование сессии, независимо от DATA
#define ROUTER_CHALLENGE_LIFETIME_TB 20000 // 2с: срок одного nonce, независимо от интервала повторов
#define ROUTER_RETRANS_MIN_TB     20000  // 2с: нижний транспорт уже обеспечивает надёжную доставку
#define ROUTER_RETRANS_BACKOFF_TB 300000 // 30с: предел backoff, но не меньше измеренного базового RTO
#define ROUTER_INFLIGHT_HASH_SIZE  1024
#define ROUTER_NO_ROUTE_RETRY_TB   200    // 20ms проверка появления маршрута
#define ROUTER_CALL_NO_ACK_TIMEOUT_TB 200000 // 20с без прогресса; увеличивается до 4 базовых RTO на медленных путях
#define ROUTER_NO_ACK_TIMEOUT_TB  50000  // 5с без прогресса; фактический срок не меньше 4 базовых RTO

// minRTT measurement
#define MINRTT_UNLOADED_MIN_TB     500    // 50ms минимальное время разгрузки канала
#define MINRTT_PROBE_TIMEOUT_TB    100000 // 10 сек без свежих замеров → probe
#define MINRTT_PROBE_MAX_INFLIGHT  4      // лимит inflight в режиме probe
#define MINRTT_WINDOW_SIZE         10
#define MINRTT_DEFAULT_TB          100    // default minRTT = 10ms (заменяется первым RTT)

// Inflight хранит подписанное/зашифрованное тело. Каждый ретрансмит добавляет новый список переходов.
struct ROUTER_INFLIGHT {
    struct ll_entry ll;         // индекс по seq (4 байта, offset 0)
    uint32_t seq;
    uint64_t enqueued_us;       // Исходное время постановки в send_q, неизменно при повторах.
    uint64_t last_sent_tb;      // время последней отправки (0.1ms)
    uint8_t  send_count;        // успешные отправки, включая первую; насыщается, чтобы повтор не стал чистым RTT-образцом
    uint8_t* dgram;             // неизменяемое тело [SVC_ROUTE_HDR][payload][sig?], без visited_ids/count
    size_t   dgram_len;
};

/* Политика отправителя для каналов одного сервиса. Действует только в uasync. */
struct router_tx_policy {
    uint32_t ack_timeout_tb; // срок без продвижения ACK, пока есть send_q/inflight; единица 0.1 мс
    void (*closed)(struct UTUN_INSTANCE*, uint64_t group_id, const uint8_t ed[32]); // необязательное уведомление отправителя о close/reset
};

// Bindings для сервисов внутри etcp_router (аналогично ETCP_BINDINGS)
struct ETCP_ROUTER_BINDINGS {
    etcp_recv_fn callbacks[SVC_ROUTE_MAX_BINDINGS];
    const struct router_tx_policy* tx_policy[SVC_ROUTE_MAX_BINDINGS]; // заимствованная политика, NULL — обычная отправка
    int (*authorize[SVC_ROUTE_MAX_BINDINGS])(struct UTUN_INSTANCE*, uint64_t, const uint8_t[32]); // Допуск group/Ed25519 пира; 1 — разрешено.
    int (*receive_ready[SVC_ROUTE_MAX_BINDINGS])(const struct ETCP_ROUTER_CONN*, const uint8_t*, size_t);
    uint8_t required_crypto[SVC_ROUTE_MAX_BINDINGS]; // Обязательные ENCRYPTED/SIGNED для сервисных данных; 0 — без требования.
    uint8_t chat_policy[SVC_ROUTE_MAX_BINDINGS]; // 1 — crypto/authorize не применяются к существующей UTUN-группе.
};

// ====================================================================
// 3. Публичное API (для сервисов)
// ====================================================================

// Первичная инициализация ядра: очистить bindings, создать router_conns, etcp_bind(ETCP_ID_SVC_ROUTE); 0/-1.
// Вызывать один раз до запуска сервисов; после destroy можно инициализировать заново.
int  etcp_router_init(struct UTUN_INSTANCE* inst);

// Снять обработчик, закрыть каналы и очистить все транзитные очереди/таймеры на живых ETCP. Повторный destroy допустим.
// До вызова остановить производителей/ожидания сервисов; из callback останавливаемого сервиса ядро не уничтожать.
void etcp_router_destroy(struct UTUN_INSTANCE* inst);

// Зарегистрировать/заменить callback; 0/-1. Замена сохраняет receive_ready/crypto policy.
// callback=NULL снимает callback, receive_ready и policy, даже если сервиса не было; каналы не закрывает.
// Сервис получает владение entry: [26-byte ROUTER_SVC header][payload]. Пустой payload — уведомление close/reset.
// conn заимствован на время callback; при локальных событиях синтетический (instance, peer=self), отправлять через него нельзя.
// End-to-end src/dst/group читать из заголовка через memcpy; conn может обозначать промежуточного пира.
int  etcp_router_bind(struct UTUN_INSTANCE* inst, uint8_t svc_id, etcp_recv_fn callback);
// Снять обработчик; само по себе не закрывает логические каналы сервиса.
int  etcp_router_unbind(struct UTUN_INSTANCE* inst, uint8_t svc_id);

/* Для указанного сервиса назначения требовать живой путь перед enqueue и ограничить ожидание ACK.
 * policy заимствована до снятия (NULL) или destroy; ненулевой срок должен помещаться в int.
 * Регистрировать до отправки. Уведомление closed выполняется синхронно после снятия/сброса канала.
 * Локальная доставка работает без логического канала. 0 — успех, -1 — неверный instance или срок. */
int etcp_router_set_tx_policy(struct UTUN_INSTANCE* inst, uint8_t svc_id, const struct router_tx_policy* policy);

// Задать обязательные ROUTER_FLAG_ENCRYPTED/SIGNED для зарегистрированного сервиса, в обе стороны сетевого обмена.
// Входящие данные проверяются до состояния/очередей/receive_ready; SIGNED обязателен также для header-only control.
// Для UTUN control подписывается без шифрования пустого payload, данные требуют указанные биты.
// Для CHAT router всегда требует SIGNED|ENCRYPTED у control и данных, независимо от этой настройки.
// Доверенная синхронная доставка dst=self обходит crypto policy и даёт rx_flags=0.
// Работает в uasync; unbind очищает требование. Возвращает 0 при успехе, -1 при неверном instance/сервисе/маске.
int etcp_router_require_crypto(struct UTUN_INSTANCE* inst, uint8_t svc_id, uint8_t flags);

/* Допуск конечных участников сервиса; NULL снимает. Требует зарегистрированный сервис с обязательным SIGNED.
 * authorize(inst,group,peer) получает полный Ed25519 конечного участника и возвращает 1 при разрешении.
 * Вызывается в uasync: разрешены чтения БД, запрещены callbacks, мутации и закрытие router.
 * Проверяется при открытии, сетевом приёме, доставке очереди и отправке/ретрансмите, также при dst=self.
 * Транзит не проверяется. Unbind очищает callback. 0/-1 с диагностикой. */
int etcp_router_set_authorize(struct UTUN_INSTANCE* inst, uint8_t svc_id,
                              int (*authorize)(struct UTUN_INSTANCE*, uint64_t, const uint8_t[32]));
/* Установить crypto и authorize общему сервису только для CHAT, сохранив обмен существующих UTUN-групп.
 * До первого использования сервиса; требует bound callback, SIGNED, authorize и отсутствие прежней политики.
 * Неизвестная группа проходит authorize (обычно получает отказ); сам scope не даёт ей разрешения.
 * Ограничения чистоты authorize те же. Bind сохраняет scope, unbind очищает. 0/-1 с диагностикой. */
int etcp_router_set_chat_policy(struct UTUN_INSTANCE* inst, uint8_t svc_id, uint8_t flags,
                               int (*authorize)(struct UTUN_INSTANCE*, uint64_t, const uint8_t[32]));
/* После commit изменения допуска: закрыть запрещённые каналы группы, уведомить сервисы, очистить очереди.
 * Не затрагивает общий ETCP и сервисы без authorize; callbacks могут менять список каналов. */
void etcp_router_revalidate_group(struct UTUN_INSTANCE* inst, uint64_t group_id);

// Проверяется до доставки/ACK. 0 оставляет пакет в recv_q, сохраняя порядок; CLOSE не блокируется.
// ready получает заимствованные канал с полным ключом пира и plaintext. Вызывается в uasync без I/O и изменения канала.
// NULL снимает ограничение; для уже приостановленных каналов вызвать resume_receive отдельно. Локальная доставка не проверяется.
void etcp_router_set_receive_ready(struct UTUN_INSTANCE* inst, uint8_t svc_id,
                                  int (*ready)(const struct ETCP_ROUTER_CONN*, const uint8_t*, size_t));
// Продолжение отложено; канал не создаётся, waiter/arg сервиса не сохраняются.
void etcp_router_resume_receive(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t peer, uint8_t svc_id);
/* Возобновить приём конкретного канала; NULL и уже закрытый канал допустимы. */
void etcp_router_conn_resume_receive(struct ETCP_ROUTER_CONN* rconn);

// Отправить сервисный пакет (авто-conn, seq, inflight-контроль через send_q).
// Принимает владение entry во всех исходах, включая ошибку.
// entry->dgram = svc_id[1] || непустой payload; mode должен содержать только допустимые биты.
// Для удалённого узла 0=принято в send_q, не подтверждение доставки. Предел включает wire-header, crypto и полный visited-list.
// Для dst=self callback вызывается синхронно, 0=доставлено callback, -1=размер/нет обработчика/ошибка памяти.
// В self-ветке нет канала, inflight, receive_ready и crypto; rx_flags=0, payload до UINT16_MAX-ROUTER_SVC_HDR_SIZE байт.
// force разрешает превысить лимит числа пакетов сетевой send_q; для self не используется.
// mode — допустимые биты ROUTE_CRYPTO_SIGN / ROUTE_CRYPTO_ENCRYPT; влияет на UTUN (0 = обычный пакет).
// CHAT автоматически использует обе защиты, включая mode=0.
int  etcp_route_send(struct UTUN_INSTANCE* inst, uint64_t group_id, uint64_t dst_node_id, struct ll_entry* entry, int force, int mode);
/* Доставка по полному Ed25519, включая очередь офлайн-узла. Владение entry передаётся при любом исходе. */
int etcp_route_send_key(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t ed[32], struct ll_entry* entry, int force, int mode);

// Найти/создать логический канал. Указатель заимствован до close/destroy, NULL при ошибке.
/* Открыть канал по полному Ed25519; адрес сохраняется при потере маршрута и при повторах.
 * Возвращает заимствованный канал либо NULL при неверном ключе, отказе допуска или ошибке выделения. */
/* Найти существующий канал по группе, полному Ed25519 и сервису; NULL, если канал отсутствует. */
struct ETCP_ROUTER_CONN* etcp_router_conn_lookup(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32], uint8_t svc);
/* Проверить живой путь к точному Ed25519 (peer — его производный индекс); 1 означает доступность. */
/* 1, когда доступный маршрут к полному ключу использует готовую сессию группы. */
int etcp_router_group_peer_ready(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32], uint8_t svc);
int etcp_router_peer_reachable(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32], uint8_t svc);

struct ETCP_ROUTER_CONN* etcp_router_conn_get_key(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t ed[32], uint8_t svc_id);
struct ETCP_ROUTER_CONN* etcp_router_conn_get(struct UTUN_INSTANCE* inst,
                                               uint64_t group_id, uint64_t remote_node_id, uint8_t svc_id);

// Есть ли сейчас физический маршрут до узла (прямой, indirect-посредник или глобальный).
// 1 = найден транспорт (для self всегда 1); это не гарантия UP/готовности логического канала.
int etcp_router_has_route(struct UTUN_INSTANCE* inst, uint64_t group_id, uint64_t dst_node_id);

// Снимок текущего пути отправки сервиса, только uasync-поток; без создания канала/изменения очередей.
// type — CONN_TYPE_*; отсутствие живого пути: NONE и next_hop_node_id=0.
struct ETCP_ROUTER_PATH {
    uint8_t type;
    uint64_t next_hop_node_id;
    uint8_t next_hop_ed[32];             // Публичный ключ непосредственного соседа; нули при NONE.
    uint64_t tx_epoch, wire_tx_bytes; // Эпоха и lifetime TX выбранного ETCP_CONN; для графика, не оценка доставки ACK.
};
struct ETCP_ROUTER_PATH etcp_router_get_path(struct UTUN_INSTANCE* inst, uint64_t group_id,
                                           uint64_t dst_node_id, uint8_t svc_id);
/* Снимок фактического пути до Ed25519 адресата; пустой снимок при отсутствии пути. */
struct ETCP_ROUTER_PATH etcp_router_get_path_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32], uint8_t svc);

// Отправить данные с авто-seq и контролем inflight
// data: payload без svc_id; mode задаёт защиту UTUN, для CHAT автоматически SIGN|ENCRYPT.
// Копирует непустой data; 0=принято в очередь, <0=ошибка. Доставка подтверждается отдельно протоколом.
// Это сетевой API rconn; для синхронной локальной доставки использовать etcp_route_send.
int  etcp_router_conn_send(struct ETCP_ROUTER_CONN* rconn,
                           const uint8_t* data, size_t len, int mode);

// Снять канал с реестра, затем синхронно уведомить сервис и очистить ресурсы; память освобождается отложенно.
// Callback может закрывать другие каналы/группу или открыть замену с тем же ключом. После close rconn использовать нельзя.
void etcp_router_conn_close(struct ETCP_ROUTER_CONN* rconn);

// Установить max_inflight (минимум 4), пересчитать inflight_limit и state machine.
// В minRTT probe лимит равен 4; reset сессии возвращает default=256.
void etcp_router_set_max_inflight(struct ETCP_ROUTER_CONN* rconn, uint16_t new_max);

/* Запросить готовность handshake/маршрута/окна/ETCP при пустой send_q. 0/-1 с логом.
 * Callback всегда отложен, разделяет round-robin с обычными очередями, может отменить/перезапустить канал.
 * Один запрос на канал; повтор того же запроса не меняет место в очереди. В callback можно поставить
 * одну кодограмму через conn_send и зарегистрировать следующий запрос. Вызов conn_send в этом callback
 * только принимает копию: не вызывает сервисы синхронно. Нельзя сохранять готовность после возврата.
 * h и arg должны жить до callback/отмены; reset/close обнуляет h->conn без вызова ready. */
int etcp_router_request_send(struct ETCP_ROUTER_CONN* rconn, struct router_send_request* h,
                             void (*ready)(struct ETCP_ROUTER_CONN*, void*), void* arg);
/* Отменить исходную регистрацию; обнулённый h допустим, callback после возврата не придёт. */
void etcp_router_cancel_request(struct router_send_request* h);

// Отмена относится к исходной очереди, даже если логический канал уже перезапущен.
void etcp_router_cancel_send_ready(struct queue_waiter_handle* h);

/* Проверить место в очереди конкретного канала. Возвращает 0 для отсутствующего или закрытого канала. */
int etcp_router_conn_send_q_has_room(const struct ETCP_ROUTER_CONN* rconn);
/* Перенести регистрацию h на очередь канала. Предыдущий waiter отменяется, callback получает arg при готовности. */
void etcp_router_conn_on_send_ready(struct ETCP_ROUTER_CONN* rconn, struct queue_waiter_handle* h,
                                    queue_threshold_callback_fn callback, void* arg);

// ====================================================================
// 4. Служебные функции (для каркаса: etcp.c, topo_group.c)
// ====================================================================

// При сбросе транспорта пира ed отвязать отправку от его очереди, сохранив inflight и ретрансмиты.
void etcp_router_pause_retrans_for_node(struct UTUN_INSTANCE* inst, const uint8_t ed[32]);

// Закрыть все каналы и ожидающий транзит группы. Сначала сервисы отменяют свои producers/waiters.
void etcp_router_close_group(struct UTUN_INSTANCE* inst, uint64_t group_id);
// Закрыть логические каналы группы к пиру с ключом ed.
void etcp_router_conn_close_all_for_node(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t ed[32]);

// Сбросить существующий peer+svc: новая локальная эпоха, пустые очереди/seq/метрики, затем уведомление сервиса и HELLO.
// Отсутствующий канал не создаётся. Данные, отправленные сервисом из уведомления, относятся к новой сессии.
void etcp_router_conn_restart(struct UTUN_INSTANCE* inst, uint64_t group_id, uint64_t remote_node_id, uint8_t svc_id);

// Удалить все транзитные очереди соединения (вызывается при закрытии ETCP_CONN)
void etcp_router_transit_queues_destroy(struct ETCP_CONN* conn);

// Сбросить recv_conn у всех ROUTER_CONN, ссылающихся на уничтожаемый ETCP_CONN.
void etcp_router_conn_destroyed(struct ETCP_CONN* conn);


#ifdef __cplusplus
}
#endif
#endif // ETCP_ROUTER_H
