/* Автономный установившийся TCP: I/O направлений и общие с UDP пулы криптографии.
 * Копирует ключи/позиции потока после handshake. ETCP/STCP-объекты используются только callback в main.
 * TX принимает целый STCP payload; RX возвращает упорядоченные расшифрованные участки потока кадров.
 * close снимает регистрации и ждёт workers без poll(main); fd остаётся у вызывающего. */
#ifndef UTUN_STREAM_TRANSPORT_H
#define UTUN_STREAM_TRANSPORT_H
#include "packet_transport.h"
#include "reality_record.h"
struct stream_transport;
struct stream_transport_config {
    socket_t fd;
    struct UASYNC *main;
    struct transport_lane *tx, *rx;
    struct sc_stream_key send_key, recv_key;
    uint64_t send_offset, recv_offset, received_us;
    int reality;
    struct reality_traffic send_record, recv_record;
    const uint8_t *send_wire, *recv_cipher, *recv_wire; // Копируются create; хвосты handshake, могут быть NULL/0.
    size_t send_wire_size, recv_cipher_size, recv_wire_size;
};
enum stream_event_type { STREAM_TX, STREAM_RX, STREAM_END, STREAM_FINISHED };
struct stream_event {
    enum stream_event_type type;
    const uint8_t *data; // Заимствовано до возврата callback, только RX.
    size_t size;
    uint64_t received_us;
    int error; // 0: TX готов / RX данные / штатный EOF; иначе соединение требуется закрыть.
};
typedef void (*stream_callback)(void *arg, const struct stream_event *event);
/* NULL при ошибке с логом. callback только main; допускает close транспорта внутри callback. */
struct stream_transport *stream_transport_create(const struct stream_transport_config *config, stream_callback callback, void *arg);
/* main: копирует payload; 0 принято, -2 backpressure, -1 ошибка. padding — выбранная длина REALITY дополнения. */
int stream_transport_send(struct stream_transport *stream, const void *data, size_t size, unsigned padding);
int stream_transport_ready(struct stream_transport *stream);
/* main: приостанавливает доставку RX и чтение; готовые результаты сохраняют ограниченный кредит. */
void stream_transport_pause(struct stream_transport *stream, int paused);
/* main: дослать принятые пакеты и close_notify, затем STREAM_FINISHED; новых пакетов не принимает. */
void stream_transport_finish(struct stream_transport *stream);
/* main: отключить callback, завершить I/O/crypto и освободить. NULL допустим. */
void stream_transport_close(struct stream_transport *stream);
#endif
