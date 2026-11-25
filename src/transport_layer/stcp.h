/* stcp — зашифрованное фреймирование поверх TCP: handshake ключей/эпох, AES-CTR поток и CRC кадров.
 * Этот заголовок — внутренние структуры и I/O; создание исходящих/входящих соединений — client/server,
 * интеграция с ETCP_LINK — stcp_link.h. Все операции одного соединения выполняются в uasync-потоке.
 * do_close закрывает I/O и вызывает on_close; окончательное освобождение зависит от владельца conn.
 * recv_on_chunk получает заимствованный буфер только на время callback; rx_queue получает отдельную копию. */
#ifndef STCP_H
#define STCP_H

#ifdef __cplusplus
extern "C" {
#endif


#include "secure_channel.h"
#include "etcp_padding.h"
#include "reality.h"
#include "reality_io.h"
#include "crc32.h"
#include "../lib/u_async.h"
#include "../lib/socket_compat.h"
#include "../lib/ll_queue.h"
#include "../lib/debug_config.h"
#include <stdint.h>

struct UTUN_INSTANCE;
struct ETCP_CONN;
struct stcp_server;
struct stream_transport;

typedef void (*stcp_ping_cb)(int success, uint16_t rtt, void *arg);

#define STCP_MAX_MSG_SIZE   65535
#define STCP_RECV_BUF_INIT  8192
#define STCP_RECV_BUF_MAX   131072
#define STCP_HS_TIMEOUT     50000   // 5s in 0.1ms timebase units
#define STCP_CONNECT_TIMEOUT 100000  // 10s in 0.1ms timebase units

#define STCP_HS_CLIENT_MIN  (SC_PUBKEY_ENC_SIZE + STCP_HS_ENC_CLIENT)  // 40+51=91
#define STCP_HS_SERVER_MIN  (SC_PUBKEY_ENC_SIZE + STCP_HS_ENC_SERVER)  // 40+51=91
#define STCP_HS_ENC_CLIENT  51  // ed25519_pubkey(32) + got_initial_pkt(1) + reset_id(8) + padding_size(2) + device_type(1) + keepalive(2) + flags(1) + CRC32(4)
#define STCP_HS_ENC_SERVER  51  // ed25519_pubkey(32) + got_initial_pkt(1) + reset_id(8) + padding_size(2) + device_type(1) + keepalive(2) + flags(1) + CRC32(4)

// handshake plain layout (before CRC):
//   [0..31]  ed25519_pubkey
//   [32]     got_initial_pkt
//   [33..40] reset_id (epoch)
//   [41..42] padding_size
//   [43]     device_type
//   [44..45] keepalive_interval
//   [46]     flags
#define STCP_HS_PLAIN_SIZE  47  // без CRC32
#define STCP_HS_PADDING_MAX ETCP_PADDING_MAX

#define STCP_HANDSHAKE_FLAG_PING 0x01

#define STCP_STREAM_CLIENT_SEND 0
#define STCP_STREAM_SERVER_SEND 1

#define STCP_RX_QUEUE_MAX_PACKETS 64
#define STCP_RX_QUEUE_MAX_BYTES   (256 * 1024)

enum stcp_state {
    STCP_STATE_INIT,
    STCP_STATE_HS_CLIENT_SENT,
    STCP_STATE_HS_SERVER_WAIT,
    STCP_STATE_DATA,
    STCP_STATE_CLOSED,
    STCP_STATE_ERROR
};

struct pending_entry {
    struct pending_entry *next;
    uint8_t *data;
    size_t len;
};

struct stcp_conn {
    socket_t sock;
    struct UASYNC *ua;
    void *socket_id;
    struct stream_transport *transport; // Установившийся сокет/crypto в фоновых направлениях; API остаётся в main.
    uint64_t last_read_us; // Время получения байтов, до ожидания дешифровки/основного цикла.
    void (*tx_ready)(void *arg); // main: кредит автономного TX снова доступен.
    void *tx_ready_arg;
    // Снимок назначения сохраняется и при неудачном connect, когда getpeername уже недоступен.
    char log_target[320];
    uint64_t log_started_tb;

    struct UTUN_INSTANCE *inst;    // сервер: диапазон padding и поиск ETCP_CONN по ключу пира
    struct ETCP_CONN *etcp_conn;   // клиент: родительский conn для live-чтения gop/reset_id при handshake

    enum stcp_state state;
    uint8_t is_server;
    uint8_t allocated;       // 1 = allocated by u_calloc, free in stcp_conn_free

    uint8_t session_key[SC_SESSION_KEY_SIZE];
    struct sc_stream_state stream_send;
    struct sc_stream_state stream_recv;

    struct SC_MYKEYS my_keys;
    uint8_t peer_pubkey[SC_PUBKEY_SIZE];
    uint8_t peer_pubkey_set;

    uint8_t peer_ed25519_pubkey[SC_PUBKEY_SIZE];
    uint8_t peer_ed25519_set;
    uint8_t my_ed25519_pubkey[SC_PUBKEY_SIZE];

    uint8_t got_initial_pkt;       // local, sent to peer during handshake
    uint8_t peer_got_initial_pkt;  // remote, received from peer during handshake
    uint64_t reset_id;             // local, sent to peer during handshake (reset epoch)
    uint64_t peer_reset_id;        // remote, received from peer during handshake
    uint8_t  device_type;          // CLIENT_TYPE_* sent to peer during handshake
    uint16_t keepalive_interval;   // keepalive interval sent to peer during handshake
    uint8_t  peer_device_type;      // CLIENT_TYPE_* from peer handshake
    uint16_t peer_keepalive_interval; // keepalive interval from peer handshake
    uint8_t  hs_flags;              // client: handshake flags to send (STCP_HANDSHAKE_FLAG_*)
    uint8_t  peer_flags;            // flags received from peer handshake
    uint64_t hs_send_time;          // client: handshake send time (0.1ms tb), for ping RTT
    uint8_t  finish_requested;
    uint8_t  close_after_send;      // server: graceful close once pending send is flushed

    // Внешний поток REALITY и копия серверной конфигурации для accept/fallback.
    struct reality_io *reality_io;
    struct reality_owner *reality_owner;
    struct   reality_server_config reality_srv;   // конфиг сервера (копия)
    char     reality_dest[REALITY_DEST_MAX];      // "host:port" для релея неавторизованных

    struct ll_queue *rx_queue;
    struct ll_queue *tx_queue;
    void (*tx_cb)(struct ll_queue *q, void *arg);

    uint8_t *recv_buf;
    size_t recv_buf_len;
    size_t recv_buf_cap;

    uint8_t *send_buf;
    size_t send_len;
    size_t send_offset;

    struct pending_entry *wire_head, *wire_tail;
    size_t wire_bytes;
    uint8_t recv_active, close_callback_active, free_requested, free_scheduled;

    struct pending_entry *pending_head;
    struct pending_entry *pending_tail;

    void (*on_ready)(struct stcp_conn *conn, void *arg);
    void *ready_arg;

    void (*on_close)(struct stcp_conn *conn, int err, void *arg);
    void *close_arg;

    void *hs_timer;          // handshake timeout handle, NULL when not active
    void *free_on_close;     // если не NULL — uasync_call_soon для u_free(arg) при закрытии
    void *close_call_soon;   // handle отложенного do_close при write-error; NULL = не запланирован
    int   close_err;         // сохранённый err для отложенного close

    // recv state machine (stcp_recv_try)
    size_t  recv_need;       // expected next chunk size (0 = disabled)
    uint8_t recv_streaming;  // 0=raw (handshake), 1=stream (DATA, auto-XOR+CRC)
    uint8_t recv_in_meta;    // stream: waiting for 2-byte header
    uint16_t recv_msg_size;  // stream: size from decoded header
    void (*recv_on_chunk)(struct stcp_conn *c, uint8_t *data, size_t len);
    uint8_t rx_paused;       // reading paused due to rx_queue overflow

    struct stcp_server *srv;     // владеющий сервер (accept-сторона), NULL для клиентских коннектов
    struct stcp_conn *srv_next;  // звено в списке принятых коннектов сервера
};

void stcp_conn_free(struct stcp_conn *c);
/* Адрес пира соединения для логов (статический буфер). "?" если недоступен. */
const char* stcp_conn_peer_str(struct stcp_conn *c);
struct stcp_log_context { char text[768]; };
struct stcp_log_context stcp_conn_describe(const struct stcp_conn *c);
void stcp_conn_set_tx_queue(struct stcp_conn *c, struct ll_queue *q);
void stcp_conn_set_rx_queue(struct stcp_conn *c, struct ll_queue *q);
void stcp_conn_set_on_close(struct stcp_conn *c, void (*cb)(struct stcp_conn *conn, int err, void *arg), void *arg);

// tx: encrypts frame, XOR covers len+data+CRC (continuous stream)
int  stcp_frame_encrypt(struct stcp_conn *c, const uint8_t *data, size_t data_len, uint8_t *output, size_t *output_len);
// decrypt: XOR-in-place data+CRC blob (handshake only; DATA uses stcp_recv_try stream mode)
int  stcp_frame_decrypt(uint8_t *data, size_t len, struct sc_stream_state *stream, size_t *out_len);

void stcp_pending_queue(struct stcp_conn *c, const uint8_t *data, size_t len);
void stcp_pending_clear(struct stcp_conn *c);

// data из u_malloc: 0 — отправлен и освобождён, 1 — принят для досылки; -1 оставляет data вызывающему.
int  stcp_send_raw(struct stcp_conn *c, uint8_t *data, size_t len);
int  stcp_try_send(struct stcp_conn *c, uint8_t *data, size_t len);
void stcp_write_cb(socket_t sock, void *arg);
void stcp_flush_pending(struct stcp_conn *c);

// Читает TCP в recv_buf: 1 — новые байты, 0 — ждать, -1 — ошибка/закрыто. Разбор через stcp_recv_try вызывает владелец.
int  stcp_conn_read(struct stcp_conn *c);

// set next expected chunk: need=0+streaming=1 enters DATA stream mode
void stcp_recv_set(struct stcp_conn *c, size_t need, int streaming,
                    void (*on_chunk)(struct stcp_conn *c, uint8_t *data, size_t len));
// try to process buffered data according to recv_need/streaming/on_chunk
void stcp_recv_try(struct stcp_conn *c);
/* После выхода из read/recv callbacks: передать установившийся сокет автономному транспорту, если workers включены. */
void stcp_transport_attach(struct stcp_conn *c);

/* Мягкое закрытие: запрещает RX, досылает накопленный TX и REALITY close_notify.
 * Завершение или таймаут 1 с вызывают do_close. Не освобождает внешние RX/TX очереди. */
void stcp_conn_finish(struct stcp_conn *c);
/* Немедленно закрывает сокет, отменяет таймеры, освобождает wire/recv/send буферы и потоки шифрования.
 * on_close вызывается один раз; повторное закрытие игнорируется. Внешние RX/TX очереди остаются у владельца.
 * Серверный allocated conn и free_on_close освобождаются отложенно; клиентский conn встроен в client handle. */
void stcp_conn_do_close(struct stcp_conn *c, int err);

// снимает принятый коннект из списка владеющего сервера (реализация в stcp_server.c)
void stcp_server_remove_conn(struct stcp_conn *c);

// handshake timeout callback (for use with uasync_set_timeout)
void hs_timeout_cb(void *arg);

// unified tx queue callback (for use as tx_cb)
void stcp_tx_queue_cb(struct ll_queue *q, void *arg);

// push decoded data to rx_queue; pauses reading if queue over threshold
void stcp_rx_push(struct stcp_conn *c, uint8_t *data, size_t len);
// call after consumer dequeues from rx_queue; resumes reading when below threshold
void stcp_rx_resume_if_needed(struct stcp_conn *c);


#ifdef __cplusplus
}
#endif
#endif
