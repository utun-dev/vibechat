/* etcp_connections — сокеты и физические линки прямого ETCP-соединения с пиром.
 * Принимает/отправляет пакеты, выполняет INIT, PING и подключает UDP/STCP-линки к общему ETCP_CONN.
 * Ядро создаёт сокеты через init_sockets/init_connections; NCD управляет линками к конкретным узлам.
 * Сервисы открывают транспорт через node_conn_direct.h, обмениваются данными через etcp_api.h или etcp_router.h.
 * Этот заголовок содержит внутренние структуры и API стека. Все операции — uasync-поток instance.
 * Сокет обслуживает несколько пиров; линк соединяет один сокет с одним адресом пира. */
#ifndef ETCP_CONNECTIONS_H
#define ETCP_CONNECTIONS_H

#ifdef __cplusplus
extern "C" {
#endif


#include "secure_channel.h"
#include "etcp_session.h"
#include "reality.h"
#include "utun_instance.h"
#include "etcp_bbr.h"
#include "../routing_layer/address_profile.h"
#include "../lib/socket_compat.h"
#include "../lib/ll_queue.h"
#include <stdint.h>
#include <stddef.h>

#define IP_UDP_OVERHEAD_V4 (20 + 8)            // IP(20)+UDP(8)
#define IP_UDP_OVERHEAD_V6 (40 + 8)            // IP(40)+UDP(8)
#define SC_ENCRYPT_OVERHEAD (ETCP_ENCRYPTED_HDR_SIZE + ETCP_SESSION_HEADER_SIZE + SC_NONCE_SIZE + SC_CRC32_SIZE + SC_TAG_SIZE)
#define UDP_SC_HDR_SIZE (SC_NONCE_SIZE + SC_TAG_SIZE + SC_CRC32_SIZE + 5)
#define ACK_REZERV 100// сколько байт резервировать под ack и прочие заголовки
#define WIRE_OVERHEAD(fam) ((fam) == AF_INET6 ? IP_UDP_OVERHEAD_V6 + SC_ENCRYPT_OVERHEAD : IP_UDP_OVERHEAD_V4 + SC_ENCRYPT_OVERHEAD)

#define INFLIGHT_LIM_MIN  8192      // минимально допустимый bbr_max_cwnd в конфиге; не минимум текущего окна BBR
#define INFLIGHT_LIM_MAX  1048576   // 1M

#define PACKET_DATA_SIZE        2048
// Wire-MTU кап: пакет на проводе не должен превышать 1500 (согласовано с TUN_MAX_PACKET_SIZE).
// PACKET_DATA_SIZE (2048) — это внутренний буфер аллокации/приёма, а не wire-MTU.
#define PACKET_DATA_MAX_MTU     1500
#define ETCP_PAYLOAD_HEADER_SIZE 7  // type + seq:u32 + attempt_no:u16 (LE, 1..65535, общий номер для всех линков seq)
// Запас normalizer: длина u16 + хотя бы 1 байт кодограммы; меньший MTU даёт нулевой/переполненный frag_size.
#define ETCP_MIN_UDP_MTU(fam) (WIRE_OVERHEAD(fam) + ETCP_PAYLOAD_HEADER_SIZE + ACK_REZERV + 3)
#define ETCP_ACK_ENTRY_SIZE      10 // seq:u32 + attempt_no:u16 + receiver_delay_us:u32; все числа LE
#define ETCP_MAX_PAYLOAD_SIZE   (PACKET_DATA_SIZE - ETCP_ACK_BASE_SIZE - ETCP_PAYLOAD_HEADER_SIZE)
#define ETCP_RFC791_MIN_MTU     576

// Типы кодограмм протокола
#define ETCP_INIT_REQUEST       0x02
#define ETCP_INIT_RESPONSE      0x03
#define ETCP_INIT_REQUEST_NOINIT    0x04
#define ETCP_INIT_RESPONSE_NOINIT   0x05
#define ETCP_PING               0x06
#define ETCP_PONG               0x07
#define ETCP_KEEPALIVE          0x08
#define ETCP_TCP_ADDRESS        0x0d  // STCP control: code + family(4/6) + observed IP(16), без порта

#define ETCP_PING_FLAG_WANT_RTT 0x01  // в PONG: хочу получить RTT от пингера
#define ETCP_PING_FLAG_SEND_RTT 0x02  // в PING: несу RTT для получателя
#define ETCP_PING_FLAG_OBSERVE 0x04   // PONG несёт фактически наблюдаемый IP:port
#define ETCP_PING_FLAG_ADDRESS_CHECK 0x08 // прямое входящее испытание endpoint
#define ETCP_PING_FLAG_NO_REPLY 0x10 // NAT-проба: результат через router, без автоматического UDP-ответа

// Адаптивный keepalive
#define KA_PERIOD_MIN_MS   200
#define KA_PERIOD_MAX_MS   10000
#define KA_TIMEOUT_MULT    10       /* timeout = period * mult */


#pragma pack(push, 1)
struct ETCP_DGRAM {// пакет (незашифрованный)
     struct ETCP_LINK* link;// откуда получена или куда отправялем
     uint16_t data_len;// длина data, включая открытый хвост; без timestamp/flag_up
     uint16_t noencrypt_len;// число байт (с конца) которые не надо шифровать. для передачи pubkey
     uint16_t payload_len; // локальные метаданные request_pkt: DATA-байты; 0 — только ACK/control/FILLER, вне DATA-pacing
     struct ETCP_TX_ATTEMPT* attempt; // локальный резерв окна; complete снимает/фиксирует его после send
     uint8_t burst_packet; // Пакет создан измерительной серией; не зависит от последующей смены burst_active
     uint8_t ack_count; // Число ACK, снимаемых из головы ack_q только после принятия send
     uint8_t commit_timestamp, commit_burst_response; // Подготовленные служебные секции
     uint8_t ack_request; // Пакет содержит запрос границы окна; сбрасывается только после успешного send
     uint64_t received_us; // Чтение сокета: часы RX-полосы/burst, не RTT; 0 — синхронный драйвер без ранней метки.
     uint16_t timestamp;// timestamp отправляющего узла при отправке пакета
     uint8_t flag_up:1;// bit0 = up/down (recv_keepalive)
     uint8_t data[];// данные пакета (без timestamp)
};
#pragma pack(pop)

// Размер зашифрованного заголовка ETCP_DGRAM: timestamp(2) + flag_up(1).
#define ETCP_ENCRYPTED_HDR_SIZE (offsetof(struct ETCP_DGRAM, data) - offsetof(struct ETCP_DGRAM, timestamp))

// Размер базового ACK: type(1) + count(1) + last_delivered_id(4) + rx_dup_count(2)
#define ETCP_ACK_BASE_SIZE (2 + 4 + 2)
#define ETCP_ACK_RX_POINT_SIZE 24 // При count>0: RX bytes:u64, RX socket time:u64, ACK snapshot time:u64 (LE, мкс).

// Размер ACK-секции для count=0..255; пустой ACK не содержит точки приёма и остаётся 8-байтовым.
static inline unsigned etcp_ack_section_size(uint8_t count) {
    return ETCP_ACK_BASE_SIZE + (unsigned)count * ETCP_ACK_ENTRY_SIZE + (count ? ETCP_ACK_RX_POINT_SIZE : 0);
}

/* Последняя достоверная DATA/ACK-попытка: две пары монотонных часов, без предположения RTT/2.
 * peer_receive_us восстанавливается из общей метки ACK минус delay конкретного entry.
 * clock_us содержат неизвестное смещение часов: использовать их изменения, не абсолютную one-way задержку.
 * Постоянное смещение сокращается в разностях samples; дрейф часов и смена пути требуют отдельной модели. */
struct ETCP_ACK_TIMING {
    uint64_t attempt_id;           // (seq<<16)|номер; 0 — достоверного измерения ещё нет.
    uint64_t local_send_us;        // Принятие отправки DATA локальным транспортом.
    uint64_t peer_receive_us;      // Приём этого DATA ядром пира, после transport/crypto.
    uint64_t peer_ack_us;          // Общий снимок времени при сборке ACK, до transport/crypto отправки.
    uint64_t local_ack_us;         // Обработка ACK локальным ядром, после transport/crypto.
    int64_t forward_clock_us;      // peer_receive_us-local_send_us: прямая задержка плюс смещение часов.
    int64_t backward_clock_us;     // local_ack_us-peer_ack_us: обратная задержка минус смещение часов.
};

// INIT внутри шифрованного тела, до padding: запрос — 59 байт, ответ — 56 байт.
// Каждая сторона передаёт свою эпоху reset_id; смена известной эпохи подтверждается через etcp_session.
// В обоих направлениях до изменения состояния отклоняются link_id=0 и mtu<ETCP_MIN_UDP_MTU(family); mtu>1500 ограничивается.

// INIT REQUEST: client → server. До изменения транспорта проверить полноту тела и соответствие Ed25519 транспортному X25519; padding допустим.
struct ETCP_INIT_REQUEST_PKT {
    uint8_t  code;              // 0:  ETCP_INIT_REQUEST (0x02) или ETCP_INIT_REQUEST_NOINIT (0x04)
    uint8_t  ed25519_pubkey[SC_PUBKEY_SIZE];  // 1: канонический адрес отправителя
    uint8_t  reset_id[8];       // 33:  локальная эпоха потока клиента (big-endian)
    uint8_t  mtu[2];            // 41: client MTU (big-endian)
    uint8_t  keepalive[2];      // 43: keepalive interval (big-endian)
    uint8_t  recovery[2];       // 45: recovery interval/100 (big-endian)
    uint8_t  link_id;           // 47: client's local link id, 1..255
    uint8_t  socket_id;         // 48: client's socket id
    uint8_t  only_local;        // 49: client only_local flag
    uint8_t  type;              // 50: client socket type (CFG_SERVER_TYPE_*)
    uint8_t  src_ipv4[4];       // 51: client interface_addr IPv4 (big-endian, 0 if N/A)
    uint8_t  src_port[2];       // 55: client interface_addr port (big-endian)
    uint8_t  collision;         // 57: 1 = cross-connect, remote claims master
    uint8_t  device_type;       // 58: CLIENT_TYPE_SERVER/DESKTOP/MOBILE
} __attribute__((packed));

#define ETCP_INIT_REQ_SIZE sizeof(struct ETCP_INIT_REQUEST_PKT)

// INIT RESPONSE: server → client. Обязательное тело — 56 байт без timestamp/flag_up и padding.
// Полнота тела и соответствие ключей проверяются до изменения RX/эпохи/параметров линка.
struct ETCP_INIT_RESPONSE_PKT {
    uint8_t  code;              // 0:  ETCP_INIT_RESPONSE (0x03) или ETCP_INIT_RESPONSE_NOINIT (0x05)
    uint8_t  ed25519_pubkey[SC_PUBKEY_SIZE];  // 1: канонический адрес отправителя
    uint8_t  reset_id[8];       // 33:  локальная эпоха потока сервера (big-endian), не эхо запроса
    uint8_t  mtu[2];            // 41: server MTU (big-endian)
    uint8_t  link_id;           // 43: server's local link id, 1..255
    uint8_t  remote_socket_id;  // 44: ID серверного сокета, отправляющего ответ
    uint8_t  only_local;        // 45: server only_local flag
    uint8_t  type;              // 46: server socket type (CFG_SERVER_TYPE_*)
    uint8_t  peer_ipv4[4];      // 47: client's external NAT address (big-endian)
    uint8_t  peer_port[2];      // 51: client's external NAT port (big-endian)
    uint8_t  device_type;       // 53: CLIENT_TYPE_SERVER/DESKTOP/MOBILE
    uint8_t  keepalive[2];      // 54: server keepalive interval (big-endian)
} __attribute__((packed));

#define ETCP_INIT_RESP_SIZE sizeof(struct ETCP_INIT_RESPONSE_PKT)

/* Обязательный заголовок UDP PING/PONG. После него идут size байт данных и padding.
 * ed25519 проверяется по X25519, которым аутентифицирован пакет. Числа в сетевом порядке. */
struct ETCP_PING_HDR {
    uint8_t code, flags;
    uint8_t ed25519[SC_PUBKEY_SIZE];
    uint8_t nonce[8], size[2];
};

typedef void (*etcp_ping_callback_t)(int success, uint16_t rtt, void* arg, uint64_t nonce,
                                      const uint8_t* resp_data, size_t resp_data_len);

struct PING_CONTEXT {
    struct PING_CONTEXT* next;
    struct UTUN_INSTANCE* instance;
    etcp_ping_callback_t cb;
    void* arg;
    uint64_t nonce;
    void* timeout_timer;
    uint64_t send_time;     // время отправки пинга в 0.1ms
    uint8_t peer_pubkey[SC_PUBKEY_SIZE];
    uint8_t socket_id;
    uint64_t socket_generation;
    struct sockaddr_storage peer_addr;
    uint8_t* user_data;
    size_t user_data_len;
};

#define LINK_ADDR_KEY_SIZE 19  // port(2) + addr(16) + family(1)

struct link_queue_entry {
    uint8_t key[LINK_ADDR_KEY_SIZE];
    struct ETCP_LINK* link;
};

// список активных подключений которые обслуживает сокет. каждый сокет может обслуживать много подключений
struct ETCP_SOCKET {
    struct ETCP_SOCKET* next;  // Linked list для всех соединений
    struct UTUN_INSTANCE* instance;
    char     name[MAX_CONN_NAME_LEN];
    socket_t fd;                        // UDP socket / TCP listen socket
    int      mtu;

    uint8_t  type;                      // CFG_SERVER_TYPE_*
    uint8_t  sock_id;                   // unique id (0-255) for NAT matching
    uint8_t  is_tcp;                    // 1=TCP (STCP), 0=UDP
    uint8_t  probe_only;                // вспомогательный UDP-порт наблюдателя, не транспорт NCD
    uint64_t generation;               // случайная эпоха endpoint; sock_id может переиспользоваться
    uint8_t  reality_enabled;           // 1=TCP-порт с REALITY-камуфляжем
    uint8_t  only_local;                // 1 = only local, no forwarding (UDP)
    uint32_t netif_index;               // для sin6_scope_id
    struct sockaddr_storage local_addr;
    struct sockaddr_storage interface_addr;
    struct sockaddr_storage nat_addr;

    // ── uasync handle (UDP read_cb / TCP accept_cb) ──
    void*    socket_id;

    // ── UDP-only ──
    struct ll_queue* links_queue;       // hash-очередь линков
    int      errorcode;
    size_t   pkt_format_errors;
    struct address_profile address_profile;
    uint32_t local_defaultroute_ip;     // auto-detected IPv4 (network byte order)
    uint8_t  local_defaultroute_ip6[16];
    struct   socks_udp* socks_udp;      // SOCKS5 UDP ASSOCIATE (NULL = напрямую)
    struct packet_socket* transport;   // Заимствованный fd обслуживают TX/RX; закрыть handle до fd.
};

#define LINK_STATE_HANDSHAKE 1
#define LINK_STATE_TRY_RECONNECT 2
#define LINK_STATE_CONNECTED 3

// Тип функции-перехватчика отправки UDP.
// etcp_udp_send вызывает hook вместо socket_sendto; SOCKS5-релей сокета имеет приоритет над hook.
struct ETCP_LINK;
typedef ssize_t (*etcp_udp_send_fn_t)(socket_t fd, const void* buf, size_t len,
                                       const struct sockaddr* addr, socklen_t addr_len,
                                       struct ETCP_LINK* link, void* context);

struct socks_udp;


// ETCP Link - одно динамическое соединение (один путь)
struct ETCP_LINK {
    struct ll_entry* link_queue_entry;  // элемент в socket->links_queue
    struct ETCP_LINK* next;         // Linked list подключений для ETCP_CONN (каждое подключение это child для ETCP_CONN)

    struct ETCP_CONN* etcp;         // подключение (parent)
    struct ETCP_SOCKET* conn;       // сокет через который работаем
    const struct etcp_link_codec* codec; /* Формат ETCP-пакетов, независимо от API драйвера. */
    struct etcp_link_io* io;        // Владеющий пакетный адаптер; закрывается до native-транспорта
    // Путь соединения
    struct sockaddr_storage remote_addr;    // Удалённый адрес
    uint64_t lan_generation;                // ненулевое поколение неподтверждённой LAN-пробы; 0 = обычный линк
    uint32_t lan_interface;                 // локальный интерфейс, через который подтверждён LAN-кандидат
    
    // Параметры соединения
    uint16_t mtu;                   // MTU общий
    uint16_t mtu_local;             // MTU локального узла
    uint16_t mtu_remote;            // MTU удаленного узла
    uint16_t keepalive_interval;    // Keepalive интервал (x1ms)
    uint32_t recovery_interval;     // recovery интервал (x0.1ms)
    uint8_t is_server;              // инициирует подключение клиент
    uint8_t initialized;            // Флаг инициализации (1=подтверждено или получен request)
    uint8_t require_known_node;     // Допуск только этого линка: штатный BLE=1, сокеты UTUN=0; общий ETCP_CONN политики не имеет.
    uint8_t peer_admitted;          // Положительный результат проверки после аутентифицированного handshake; сбрасывается при новом INIT.
    uint8_t closing;                // Close начат: повторный вход из callback игнорируется до окончательного free
    uint32_t active_users;          // Внутренние стеки конструктора/RX/событий; память закрытого линка живёт до выхода последнего
    uint8_t local_link_id;          // id моего линка
    uint64_t diagnostic_id;         // Неповторяющееся поколение внутри ETCP_CONN, независимо от local_link_id
    uint64_t diagnostic_wire_tx_bytes, diagnostic_wire_rx_bytes; // Lifetime ETCP-байты на границе пакетного драйвера
    struct {
        uint64_t sample_us, pace_due_us, acked_bytes, wire_bytes;
        uint64_t io_us, cwnd_us, pacing_us, late_us, ready_us;
        unsigned flight_max;
        unsigned gate;                 // Состояние с очередью DATA на выходе из прохода TX
    } tx_report;
    uint8_t remote_link_id;         // id этого линка на peer (устанавливается в момент initialized)
    uint8_t remote_socket_id;       // socket id peer
    uint8_t remote_only_local;      // only_local flag from peer
    uint8_t remote_type;            // CFG_SERVER_TYPE_* (type of peer's socket)
    uint8_t recv_keepalive;         // Принимаем допустимые пакеты этого линка; INIT также обновляет RX и его deadline
    uint8_t remote_keepalive;       // Входной flag_up: пир принимает наши пакеты; INIT(0) не сохраняет прежнюю единицу
    uint8_t link_status;            // 1 - up, 0 - down (итоговый статус - если есть проблемы на любой стороне - линк down)
    uint8_t link_state;             // 0 - just init, 1 - handshake, 2 - try reconnect, 3 - connected
    
    // Состояние установки соединения (только для клиентов)
    void* init_timer;               // Таймер для повторов INIT (NULL=не подключается)
    uint16_t init_timeout;          // Интервал повторов INIT, x0.1 мс: от 500 (50 мс) до 50000 (5 с)
    uint16_t init_retry_count;      // Счетчик попыток
    
//    uint64_t last_activity;         // Время последней активности
    uint64_t last_recv_local_time;  // x0.1 ms
    uint64_t rx_deadline_tb;        // конечный срок ожидания RX, включая объявленный сон
    void* rx_timeout_timer;        // проверка жизни независимо от отправки keepalive
    uint16_t last_recv_timestamp;
    uint8_t last_recv_updated; // =1 при обновлении timestamp, =0 при отправке (чтобы не дублировать отправки при отсутствии обновлений)

    uint64_t shaper_load_time_tb; // основной в 0.1 мс units
    uint64_t shaper_sub_nanotime; // sub 0-999999 для 0.1 нс - 0.1 мс
    uint8_t shaper_state;
    void* shaper_timer;
    struct ll_queue* attempts;      // Физические попытки, внутренний ключ (seq<<16)|attempt_no; ll.len учитывается в окне
    struct ll_queue* ack_q;         // ACK только DATA этого линка, тот же составной ключ
    uint8_t ack_reply_pending;      // срочный ACK: ответ till тем же путём либо fallback при OOM таймера; обходит ACK_DELAY
    void* retrans_timer;            // Ближайший срок принятой транспортом попытки этого канала
    uint64_t retrans_deadline_us;   // Уже запланированная проверка: более ранний таймер можно сохранить
    uint32_t data_rtt_us;           // Сглаженный RTT DATA->ACK этого пути; 0 до первого измерения
    uint32_t data_jitter_us;        // Сглаженное абсолютное отклонение собственного DATA RTT
    uint32_t single_path_rto_min_us; // Нижний DATA RTO без другого UP-пути; BLE задаёт 1с, повторы — вдвое больше; 0 — обычный.
    uint8_t single_path_rto_active; // Последний применённый режим: нужен для переноса уже назначенного срока при смене UP-путей.
    uint8_t data_rto_streak;        // 0..3: RTO попыток без подтверждённого прогресса; после3 tx_stalled=1
    uint8_t tx_stalled;             // исключить из повторов при наличии незастрявшего UP-пути; новые DATA/UP не меняются
    uint64_t tx_stalled_since_us;   // момент установки флага; снять его может ACK принятой после этого DATA-попытки

    uint32_t inflight_bytes;        // DATA-байты физических попыток этого линка, включая синхронный резерв send
    uint32_t inflight_packets;      // число таких пакетов
    uint32_t inflight_lim_bytes;    // индивидуальный лимит DATA в байтах, включая целиком следующий фрагмент
    uint8_t cwnd_limited;           // DATA не поместился в окно; сохраняется до передачи этого факта BBR по ACK

    // statistics
    size_t encrypt_errors;
    size_t decrypt_errors;
    size_t send_errors;
    size_t recv_errors;
    size_t total_encrypted;
    size_t total_decrypted;
    uint8_t transport_key_ready, transport_key[SC_SESSION_KEY_SIZE]; // Последний опубликованный RX-снимок этого адреса.
    uint32_t total_retransmissions;
    uint64_t acked_bytes;               // подтверждённых байт на этом линке
    uint64_t acked_packets;             // подтверждённых пакетов на этом линке
    
    uint16_t rtt_last;                 // Последний timestamp-RTT, 0.1мс (0 до измерения); нижняя граница RTT для RTO
    uint32_t jitter;                   // Current jitter [>>16] x0.1 ms
    // rtt_avg10 удалён — заменён BBR (bbr->min_rtt_us/100)
    uint32_t recv_dt_avg_tx;           // EWMA поправки TX timestamp, Q16 в единицах 0.1 мс, арифметика по модулю 2^32
    uint32_t recv_dt_avg_rx;           // EWMA поправки RX timestamp, тот же формат; используется для оценки rt_last

    uint16_t tt_last;                // transmit time (время доставки отправленных пакетов)
    uint16_t rt_last;                // recv time (время доставки принятых пакетов)
    uint32_t bandwidth;              // Отображение pacing в Kbit/s (вверх); shaper использует точный bbr_pacing_rate
    
    // NAT address tracking (from INIT_RESPONSE)
    struct sockaddr_storage local_as_seen_by_peer; // только наш endpoint, наблюдаемый удалённым пиром
    uint32_t nat_changes_count;      // Counter of NAT address changes
    uint32_t nat_hits_count;         // Counter of NAT address matches (new init response with same IP:port)
    
    // Keepalive state
    void* keepalive_timer;           // Таймер обычного keepalive (только в normal-режиме)
    uint32_t keepalive_timeout;      // таймаут (ms)
    uint16_t ka_period_ms;           // текущий период отправки keepalive
    uint16_t ka_sleep_period_ms;     // период keepalive в спячке (мс; настройка клиента / ka_period_ms пира)
    uint8_t ka_my_sleeping;          // 1 = я объявил сон (standby)
    uint8_t ka_peer_sleeping;        // 1 = пир объявил сон
    uint8_t ka_sleep_acked;          // получен ACK моего анонса сна
    uint16_t ka_active_remaining;    // сколько до SLEEP-фазы (x0.1с), 0 = уже спим
    uint16_t ka_sleep_duration;      // длительность SLEEP-фазы (x0.1с)
    void* ka_sleep_announce_timer;   // таймер ретрая анонса спячки (ACTIVE-окно)
    uint8_t ka_announce_attempts;    // попыток анонса в текущем окне
    uint8_t ka_announce_max_attempts; // лимит попыток на окно (сколько влезет, но ≥ min)
    uint8_t ka_fail_count;           // кумулятивные провалы (сброс по ACK), ≥ KA_SLEEP_MAX_FAILS → down
    uint8_t pkt_sent_since_keepalive; // Флаг: был ли отправлен пакет с последнего keepalive тика
    uint32_t keepalive_sent_count;  // Счётчик отправленных keepalive
    uint32_t keepalive_recv_count;  // Счётчик полученных keepalive
    uint8_t peer_device_type;       // CLIENT_TYPE_* удалённой стороны (из handshake)
    uint16_t handshake_minsize;      // минимальный размер udp при handshake
    uint16_t handshake_maxsize;      // мax размер udp при handshake (выбирает рандом)

    // === Burst-измерение bandwidth ===
    // Sender-сторона
    uint16_t burst_id;              // монотонно возрастающий ID burst
    uint8_t  burst_active;          // 1 = burst в процессе отправки
    uint8_t  burst_seq;             // текущий номер пакета в burst (0..burst_count-1)
    uint8_t  burst_count;           // общее число пакетов в burst
    uint64_t burst_last_time_tb;    // время последнего burst (0.1ms), для MIN_BURST_INTERVAL
    void*    burst_resp_timer;      // таймер ожидания ответа на burst
    uint32_t burst_target_bdp;      // Диагностический wire-BDP серии; 0 при неизвестном RTT, не управляет BBR
    uint16_t burst_pkt_size;        // сохранённый размер пакета для вычисления BW

    // Receiver-сторона
    uint16_t burst_recv_id;         // ID отслеживаемого burst
    uint8_t  burst_recv_count;      // ожидаемое число пакетов в burst
    uint8_t  burst_recv_next_seq;   // следующий ожидаемый seq
    uint8_t  burst_recv_valid;      // 1 = порядок пакетов не нарушен
    uint8_t  burst_recv_received;   // сколько пакетов уже принято
    uint64_t burst_recv_times[16];  // время прихода каждого пакета (µs)
    uint16_t burst_recv_pkt_size;   // размер пакета из MEAS_TS (для ответа)
    uint8_t  burst_resp_pending;    // 1 = есть готовый ответ для piggyback
    uint32_t burst_resp_gap_avg;    // средний inter-packet gap (µs)
    uint32_t burst_resp_gap_min;    // минимальный inter-packet gap (µs)
    uint8_t  burst_resp_pkt_count;  // число пакетов в измерении
    uint8_t  burst_resp_valid;      // валидность измерения

    // === BBR congestion control ===
    struct bbr* bbr;                   // BBR state (per-link, создаётся в etcp_link_new)
    uint64_t    delivered_bytes;        // кумулятивно delivered bytes на линке (для round detection)
    uint64_t    last_ack_time_us;       // время последней доставки/начала первого flight в микросекундах
    uint64_t    first_tx_time_us;       // начало интервала отправок, продвигается подтверждёнными попытками
    uint64_t    rx_data_bytes;          // допустимые физические DATA-байты пути, включая копии; сбрасывается с потоком
    uint64_t    rx_data_time_us;        // время последнего увеличения rx_data_bytes, локальные монотонные мкс; 0 — точки нет
    uint64_t    peer_rx_data_bytes;     // последняя достоверная точка из ACK действующей попытки этого пути
    uint64_t    peer_rx_data_time_us;   // часы получателя, не сравниваются с локальными; 0 — нет базы текущего flight
    struct ETCP_ACK_TIMING ack_timing; // Последний валидный ACK действующей попытки; старые/повторные ACK не заменяют его.
    uint32_t    bbr_pacing_rate;        // Точный pacing B/s; менять через set_pacing_rate для пересчёта долга/таймера
    uint32_t    bbr_loss_since_ack;     // счётчик потерь на линке с последнего ACK (для rs->lost)

    etcp_udp_send_fn_t send_hook;    // Перехват прямого UDP; NULL = socket_sendto, SOCKS5 имеет приоритет
    void*              send_hook_ctx;
    void (*send_hook_release)(struct ETCP_LINK*, void*); // Необязательная отвязка ctx при close; три поля очищаются до вызова

    struct sockaddr_storage local_bound_addr; // локальный адрес к которому привязан линк (interface_addr на момент создания)
    uint8_t  is_tcp;                 // 1 = TCP транспорт (STCP)
    uint8_t  reality_set;            // 1 = link->reality валиден (реклама пира из BGP)
    struct reality_client_config reality;  // reality-параметры пира (для исходящего TCP)
    struct stcp_link *tcp_link;      // STCP линк (для TCP)
    void *tcp_reconnect_timer;       // таймер реконнекта (TCP клиент)
    uint32_t tcp_reconnect_delay_ms; // 0 после READY; ошибки: 1000, 2000, 4000, 8000, 16000, затем 30000 мс
};

// ── Внутренняя отправка пакетов ──

// Отправить UDP: SOCKS5 сокета, иначе send_hook, иначе socket_sendto. Возвращает число байт или -1.
ssize_t etcp_udp_send(struct ETCP_LINK* link, socket_t fd, const void* buf, size_t len,
                       const struct sockaddr* addr, socklen_t addr_len);

// ── Запуск транспорта ядра ──

// Создать listen-сокеты из [server]; при auto_sockets их создаёт отдельный модуль.
int init_sockets(struct UTUN_INSTANCE* instance);

// Подготовить listen-сокеты, NCD control и keepalive; исходящие запросы UTUN запускаются отдельно. 0/-1.
int init_connections(struct UTUN_INSTANCE* instance);

// Заменить транспорты [client] ядра и запросы работающего UTUN. clients=NULL закрывает все; новые готовятся до закрытия старых. 0/-1.
int init_client_connections(struct UTUN_INSTANCE* instance, struct CFG_CLIENT* clients);
// Открыть UTUN-запросы поверх настроенных транспортов ядра; группа уже создана. 0/-1, частичный запуск очищает leave_utun.
int client_connections_join_utun(struct UTUN_INSTANCE* instance);
// Закрыть только UTUN-запросы, сохраняя транспорты и повторы ядра; NULL допустим.
void client_connections_leave_utun(struct UTUN_INSTANCE* instance);
// Переприменить сохранённые endpoints к новым сокетам, включая bootstrap без подписанного NODEINFO; объединяет повторные события.
void client_connections_network_changed(struct UTUN_INSTANCE* instance);

// ── Сокеты UDP и TCP ──

// Создать UDP-сокет приёма/отправки из server; один сокет обслуживает линки нескольких пиров.
struct ETCP_SOCKET* etcp_socket_add(struct UTUN_INSTANCE* instance, struct CFG_SERVER* server);

// Закрыть UDP-сокет и все привязанные к нему линки, освободить ETCP_SOCKET.
void etcp_socket_remove(struct ETCP_SOCKET* conn);
/* Создать общие TX/RX при первом UDP/TCP endpoint; 0/-1, только uasync ядра. */
int etcp_transport_start(struct UTUN_INSTANCE* instance);

// TCP socket functions (uses unified ETCP_SOCKET with is_tcp=1)
struct ETCP_SOCKET* tcp_socket_add(struct UTUN_INSTANCE* instance, struct CFG_SERVER* server);

void tcp_socket_remove(struct ETCP_SOCKET* sock);

// Создаёт один серверный сокет (UDP или TCP) из CFG_SERVER. Линки не добавляет.
int etcp_create_server_socket(struct UTUN_INSTANCE* instance, struct CFG_SERVER* server, struct ETCP_SOCKET** out_sock);

struct stcp_link;
struct stcp_client;
/* Присоединить принятый STCP к ETCP. При отказе закрывает адаптер; новый conn удаляет только без других линков и NCD handles.
 * Callbacks могут передать conn другому владельцу или создать путь: отмена этого линка их не закрывает. */
void tcp_server_on_link(struct stcp_link *link, struct ETCP_SOCKET *tcp_sock);

// ── Создание, поиск и закрытие линков ──

/* Создать линк etcp через conn к remote_addr; conn=NULL выбирает исходящий TCP без bind, is_server задаёт роль.
 * Уведомляет о создании, для исходящего UDP запускает INIT. Ошибка/close из callback — NULL без живого линка.
 * Конструктор удерживает память до возврата; отмена сразу закрывает ресурсы. */
/* Добавить произвольный пакетный драйвер к conn с настроенным ключом пира.
 * require_known_node=1 проверяет известность ключа при handshake только этого пути; 0 — без CHAT-фильтра.
 * На канале используется защищённый ETCP INIT/DATA без IP-заголовков. OPEN драйвера ещё не UP ETCP.
 * Успех копирует API и забирает ctx; отказ оставляет ctx непривязанным у вызывающего.
 * Начальный handshake/callbacks отложены. Все операции — conn->instance->ua. */
struct packet_link_driver;
struct ETCP_LINK* etcp_link_add(struct ETCP_CONN* conn, const struct packet_link_driver* driver, int require_known_node);

struct ETCP_LINK* etcp_link_new(struct ETCP_CONN* etcp, struct ETCP_SOCKET* conn,
                                const struct sockaddr_storage* remote_addr, uint8_t is_server);

// Применить окно DATA new_lim>0 (байты), ограничить сверху max_inflight и разбудить отправитель.
// Минимум выбирает BBR; ноль отклоняется с логом без изменения окна. Неверный link игнорируется.
void etcp_link_update_inflight_lim(struct ETCP_LINK* link, uint32_t new_lim);

/* Снимает линк из списков и отменяет таймеры/STCP; handle сразу становится недействителен.
 * Память освобождается после выхода всех активных внутренних обработчиков, иначе до возврата.
 * Из callback его закрытия повторный close допустим; сохранять handle для последующих событий нельзя.
 * Callback может закрыть ETCP_CONN или соседний линк: вызывающий не должен хранить next через уведомление. */
void etcp_link_close(struct ETCP_LINK* link);

/* Внутренний release после active_users++: последний обработчик освобождает закрытый link.
 * Возврат не гарантирует сохранность указателя. Новых таймеров/выделений памяти нет. */
void etcp_link_release(struct ETCP_LINK* link);
/* Проверить после callback, что conn не закрывается и тот же link ещё в его списке.
 * Вызывающий удерживает conn через callbacks_running и link через active_users, исключая повторное использование адреса. */
int etcp_link_is_alive(struct ETCP_CONN* conn, struct ETCP_LINK* link);

//int etcp_input_cbk(struct packet_buffer* pkt, struct ETCP_SOCKET* conn);// получает расшифрованный пакет
int etcp_encrypt_send(struct ETCP_DGRAM* dgram);// зашифровывает и отправляет пакет

// find link by address
struct ETCP_LINK* etcp_link_find_by_addr(struct ETCP_SOCKET* e_sock, const struct sockaddr_storage* addr, int is_tcp);

// Выбор исходящего линка для collision INIT по принимающему сокету и адресу отправителя.
// Линк через другой локальный сокет не подходит. NULL — встречной попытки на этой паре нет.
struct ETCP_LINK* etcp_select_collision_link(struct ETCP_CONN* conn, struct ETCP_SOCKET* e_sock,
                                             const struct sockaddr_storage* addr);

// Выбрать свободный local_link_id в 1..255 циклически после last_assigned_link_id; 0 зарезервирован.
// Помечает ID имеющихся линков в битовой карте; возвращает ID или -1, если свободных нет.
int etcp_find_free_local_link_id(struct ETCP_CONN* etcp);

// ── Внутренние измерения пропускной способности ──
void etcp_link_burst_start(struct ETCP_LINK* link);

void etcp_link_burst_check(struct ETCP_LINK* link);

void etcp_link_burst_finish(struct ETCP_LINK* link);

// ── Внутренние переходы состояния и приём ──

void etcp_link_enter_init(struct ETCP_LINK *link);

void etcp_link_enter_reinit(struct ETCP_LINK *link);

/* Публикует готовность STCP и запускает согласование ETCP-сессии. Close из callback прекращает переход. */
void etcp_link_enter_ready_tcp(struct ETCP_LINK *link);

void etcp_tcp_link_start_connect(struct ETCP_LINK *link, struct sockaddr_storage *addr, uint16_t port);

/* Только исходящий TCP: закрывает текущий адаптер и ставит один backoff-таймер; повторный вызов его не сдвигает.
 * Ошибка таймера закрывает ETCP_CONN. Close линка/соединения отменяет попытку; READY сбрасывает backoff. */
void etcp_tcp_link_start_reconnect(struct ETCP_LINK *link);

/* Принять plaintext установленного линка: session/формат проверяются до соответствующей обработки RX.
 * pkt_len включает timestamp/flag_up; длина тела после этих трёх байт записывается в pkt->data_len.
 * 0 — pkt потреблён; ненулевой код — pkt остаётся вызывающему. RX/handshake callbacks могут запросить close;
 * callbacks_running удерживает teardown до выхода, сброс сессии прекращает обработку старого потока. */
int etcp_packet_decrypted(struct ETCP_SOCKET* e_sock, struct ETCP_DGRAM* pkt,
                           struct ETCP_LINK* link, size_t pkt_len);

// ── Пробы доступности и RTT ──

int etcp_send_ping(struct UTUN_INSTANCE* instance, const uint8_t* peer_pubkey_bin,
                   const struct sockaddr_storage* addr, int timeout_ms,
                   etcp_ping_callback_t cb, void* user_arg,
                   const uint8_t* user_data, size_t user_data_len);

/* Шлёт UDP PING на addr с peer_pubkey_bin, копируя user_data[user_data_len].
 * 0 — принят, callback позже по ответу/таймауту; <0 — ошибка без callback и без pending-записи.
 * Отмена/удаление сокета также не вызывает callback. flags: ETCP_PING_FLAG_SEND_RTT — данные содержат RTT для peer. */
int etcp_send_ping_to_socket(struct UTUN_INSTANCE* instance, struct ETCP_SOCKET* e_sock,
                               const uint8_t* peer_pubkey_bin, const struct sockaddr_storage* addr,
                               int timeout_ms, etcp_ping_callback_t cb, void* user_arg,
                               const uint8_t* user_data, size_t user_data_len,
                               uint8_t flags);

// Отмена без callback: вызывающий владеет arg и освобождает его после отмены.
void etcp_cancel_pings(struct UTUN_INSTANCE *instance, etcp_ping_callback_t callback, void *arg);

// TCP-ping: подключение к TCP-адресу peer + хендшейк с флагом PING. RTT меряется
// по хендшейку (send→response). cb вызывается один раз (success=1/rtt или success=0).
// out_cli (опционально) — возвращает хендл stcp_client для досрочной отмены (stcp_client_destroy).
int etcp_send_tcp_ping(struct UTUN_INSTANCE* instance, const uint8_t* peer_pubkey_bin,
                       const struct sockaddr_storage* addr, int timeout_ms,
                       etcp_ping_callback_t cb, void* user_arg,
                       struct stcp_client** out_cli, const struct reality_client_config *reality);

// ── Внутренний callback uasync ──

void etcp_connections_read_callback_socket(socket_t sock, void* arg);


#ifdef __cplusplus
}
#endif
#endif // ETCP_CONNECTIONS_H
