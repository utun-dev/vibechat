/* Прямой stream-протокол: канонический big-endian заголовок, пути и подписанный END.
 * IDs ETCP определены только в etcp_api.h. Все вызовы синхронны, памяти не выделяют.
 * Writers требуют проверенных полей, ненулевых указателей и достаточного wire-буфера; сами их не проверяют.
 * Parsers копируют поля, payload сообщения заимствован до освобождения кодограммы; при ошибке выходы неизменны. */
#ifndef STREAM_WIRE_H
#define STREAM_WIRE_H

#include "stream_proto.h"
#include "stream_metrics.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STREAM_WIRE_VERSION 16
#define STREAM_WIRE_GROUP_OFF 4
#define STREAM_WIRE_HEADER 100
#define STREAM_PATH_MAX 32
#define STREAM_END_LAST_OFF 82
#define STREAM_END_SIGNED_SIZE (STREAM_END_LAST_OFF + 8)
#define STREAM_END_SIZE (STREAM_END_SIGNED_SIZE + SC_SIGN_SIZE)
#define STREAM_METRIC_WIRE_SIZE 49
#define STREAM_INFO_METRIC_OFF 36
#define STREAM_INFO_OFFER_OFF (STREAM_INFO_METRIC_OFF + STREAM_METRIC_WIRE_SIZE)
#define STREAM_INFO_KEY_OFF (STREAM_INFO_OFFER_OFF + 25)
#define STREAM_INFO_REQUEST_SIZE 17 /* nonce:8, media:1, запрошенная позиция:8; UINT64_MAX — обзор головы. */
#define STREAM_INFO_POSITION_OFF (STREAM_INFO_KEY_OFF + 16)
#define STREAM_INFO_TIME_OFF (STREAM_INFO_POSITION_OFF + 25)
#define STREAM_INFO_WINDOW_OFF (STREAM_INFO_TIME_OFF + 24) /* Окно запросов получателя: uint16, независимо от DATA-слотов. */
#define STREAM_INFO_PATH_OFF (STREAM_INFO_WINDOW_OFF + 2)
#define STREAM_INFO_SIZE (STREAM_INFO_PATH_OFF + 1) /* nonce/границы/FEC/метрики/offer/KEY/точная позиция/PTS; затем path. */
#define STREAM_OVERLOAD_MAX 64
#define STREAM_OVERLOAD_HEADER 18 /* id:8, required_rate:8 (0 — дефицит слотов), required_count:1, candidates:1.
                                  * Оба порога обязательны: закрыть не меньше required_count и освободить required_rate. */
#define STREAM_FRAME_TIMING_SIZE 48
#define STREAM_DATA_HEAD_SIZE 8 /* После пути: последняя известная PTS публикации этой эпохи, UINT64_MAX — неизвестно. */
#define STREAM_TELEMETRY_PERIOD_MS 200
#define STREAM_TELEMETRY_STALE_MS 400
#define STREAM_TELEMETRY_ROW_SIZE 57

enum stream_command {
    STREAM_CMD_INFO_REQ = 1, STREAM_CMD_INFO_RESP, STREAM_CMD_SUBSCRIBE, STREAM_CMD_ACCEPT,
    STREAM_CMD_REJECT, STREAM_CMD_RENEW, STREAM_CMD_UNSUBSCRIBE, STREAM_CMD_CLOSED,
    STREAM_CMD_DATA, STREAM_CMD_GAP, STREAM_CMD_END, STREAM_CMD_REVERSE_REQ, STREAM_CMD_OVERLOAD, STREAM_CMD_OVERLOAD_SELECT,
    STREAM_CMD_RELAY_REQUEST, STREAM_CMD_RELAY_STATUS, STREAM_CMD_RELAY_CANCEL, STREAM_CMD_TELEMETRY, STREAM_CMD_OVERLOAD_END,
    STREAM_CMD_SEEK, STREAM_CMD_WINDOW_UPDATE, STREAM_CMD_WINDOW_APPLIED, STREAM_CMD_STATS
};
/* WINDOW_UPDATE/APPLIED: номер изменения:8, окно:2. APPLIED передаётся после прежних запросов, перед новыми.
 * Команды принадлежат паре peer-эпох; descriptor нужен только как сохранённый ключ сессии, локальный контент не требуется. */
/* SEEK: header.handle — новый ID, payload old_handle:8 + seq:8. Переносит существующую выдачу вперёд без нового слота.
 * ACCEPT подтверждает новый ID; старые DATA/control игнорируются. Повтор нового ID/seq идемпотентен, допуск новых выдач не нужен. */
/* OVERLOAD_END: id:8 + outcome:1. Завершает только этот или более старый запрос той же peer-эпохи; DATA не закрывает. */
enum stream_overload_outcome { STREAM_OVERLOAD_COMPLETED = 1, STREAM_OVERLOAD_DRAINED, STREAM_OVERLOAD_EXPIRED };
/* Routed помощь: handle — nonce запроса; эпохи запроса/сервиса независимы от raw ETCP-сессии. */
enum stream_relay_status { STREAM_RELAY_STARTING = 1, STREAM_RELAY_READY, STREAM_RELAY_BOUND,
                           STREAM_RELAY_REJECTED, STREAM_RELAY_ENDED, STREAM_RELAY_CANCELLED };
enum stream_reject {
    STREAM_REASON_STOPPED = 1, STREAM_REASON_POSITION, STREAM_REASON_EXPIRED, STREAM_REASON_STALE,
    STREAM_REASON_CONFLICT, STREAM_REASON_LIMIT, STREAM_REASON_LOOP, STREAM_REASON_TRANSPORT,
    STREAM_REASON_LEASE, STREAM_REASON_MEMBER, STREAM_REASON_PROTOCOL, STREAM_REASON_OVERLOAD
};
struct stream_path {
    struct stream_address nodes[STREAM_PATH_MAX]; // Порядок от источника до текущего отправителя/владельца
    uint8_t count;                  // 1..32; validate отдельно проверяет адреса и концы пути
};
/* Подсказка источника для учёта кадров без сборки FEC; только диагностика, не управление воспроизведением.
 * started — начала кадров в пачке; continuation исключает повторный учёт кадра на границе пачек. */
struct stream_frame_timing {
    uint64_t first_frame, last_frame, // IDs первого/последнего видео, включая продолженный кадр
        first_pts, last_pts;         // Их PTS в микросекундах
    uint32_t period_us,              // Последний измеренный период кадров; 0 — ещё неизвестен
        started,                    // Число начал кадров в пачке, 0..16
        audio_us;                   // Суммарная длительность аудио, 0..320000 мкс
    uint8_t valid,                   // 0 требует нулей во всех остальных полях
        video,                      // Пачка содержит видеоматериал
        continuation;               // Первый кадр начался в предшествующей пачке
};
enum stream_telemetry_flags { STREAM_TELEMETRY_KNOWN = 1, STREAM_TELEMETRY_STALE = 2, STREAM_TELEMETRY_DOWN = 4 };
struct stream_telemetry_row {
    struct stream_address node;
    uint64_t buffered_us, sample; // Узел, длительность очереди материала и номер его измерения
    uint32_t frames, age_ms;          // Кадры и накопленный возраст измерения; пересылка его не обнуляет
    uint8_t flags;                   // KNOWN, STALE, DOWN; неизвестное значение отличается от нуля
};
struct stream_telemetry {
    struct stream_telemetry_row rows[STREAM_PATH_MAX]; // Фактический stream-путь до отправителя включительно
    uint64_t received_ms; // Локальное монотонное время сохранения; по сети не передаётся
    uint8_t count;        // 1..32 строки без повторяющихся node
};
/* Записать ровно STREAM_FRAME_TIMING_SIZE байтов. Подсказка не подписана и не заменяет media index. */
void stream_frame_timing_write(uint8_t *wire, const struct stream_frame_timing *timing);
/* Проверить первые 48 байтов (length >= 48), флаги, резерв и согласованность полей: OK/ERR_FORMAT. */
int stream_frame_timing_read(const uint8_t *wire, size_t length, struct stream_frame_timing *timing);
/* Записать и вернуть 1+count*STREAM_TELEMETRY_ROW_SIZE байтов; received_ms не передаётся. */
size_t stream_telemetry_write(uint8_t *wire, const struct stream_telemetry *telemetry);
/* Требует точной длины, проверяет флаги/адреса/диапазоны: OK/ERR_FORMAT. received_ms в выходе равен 0. */
int stream_telemetry_read(const uint8_t *wire, size_t length, struct stream_telemetry *telemetry);
/* Прибавить неотрицательное now_ms-received_ms с насыщением age_ms, обновить received_ms;
 * возраст >= 400 мс выставляет STALE, ранее выставленные флаги сохраняются. */
void stream_telemetry_age(struct stream_telemetry *telemetry, uint64_t now_ms);
/* Сумма buffered_us с насыщением UINT64_MAX, включая STALE/DOWN; только выбор худшего реального пути. */
uint64_t stream_telemetry_total(const struct stream_telemetry *telemetry);
struct stream_message {
    struct stream_descriptor descriptor; // Только group/source/epoch; FEC в общем заголовке отсутствует
    uint64_t sender_epoch,               // Ненулевая эпоха stream-сервиса отправителя
        receiver_epoch,                 // Эпоха stream-сервиса адресата; до согласования допустим 0
        handle;                         // Локальный nonce запроса/выдачи; смысл определяет command
    uint8_t command, subblock;           // Команда и RS-индекс; допустимость индекса проверяет диспетчер
    const uint8_t *payload;              // Заимствованные байты после общего заголовка
    size_t length;                       // Размер payload без STREAM_WIRE_HEADER
};

/* Проверяет минимальный размер, ETCP ID, версию, ненулевые ключи/sender_epoch и command: OK/ERR_FORMAT.
 * descriptor.group_key заполняется из сети, локальный group_id присваивает диспетчер группы.
 * Payload, receiver_epoch, handle и subblock разбирает диспетчер конкретной команды. */
int stream_wire_parse(const uint8_t *wire, size_t length, struct stream_message *message);
/* Записать 100 байтов заголовка из descriptor.group_key; group_id, payload/length и FEC-поля descriptor не используются. */
void stream_wire_header(uint8_t *wire, const struct stream_message *message);
/* Путь без нулей/повторов, начинается источником и заканчивается отправителем; forbidden не входит в него. */
int stream_path_validate(const struct stream_path *path, struct stream_address source, struct stream_address sender, struct stream_address forbidden);
/* Прочитать count и IDs из начала wire, used=1+8*count: OK/ERR_FORMAT. Проверка значений IDs — через validate. */
int stream_path_parse(const uint8_t *wire, size_t length, struct stream_path *path, size_t *used);
/* Записать и вернуть 1+8*count байтов. */
size_t stream_path_write(uint8_t *wire, const struct stream_path *path);
/* Записать 49 байтов: флаг valid и шесть uint64; числа пишутся также при valid=0. */
void stream_metric_write(uint8_t *wire, const struct stream_metric_snapshot *snapshot, int valid);
/* Указатели ненулевые, доступны 49 байтов; проверяет флаги, числа копирует без интерпретации: OK/ERR_FORMAT. */
int stream_metric_read(const uint8_t *wire, struct stream_metric_snapshot *snapshot, int *valid);
/* Подписать Ed25519 private key: domain+group/source/epoch+last, затем подпись, всего 130 байтов.
 * last=UINT64_MAX — пустой поток. OK/ERR_ARGUMENT/ERR_CRYPTO; при crypto-ошибке wire уже изменён. */
int stream_end_sign(const struct stream_descriptor *descriptor, uint64_t last, const uint8_t key[32], uint8_t wire[STREAM_END_SIZE]);
/* Проверить точный размер, ключ запуска и подпись Ed25519 public key; last меняется только при OK.
 * Любая ошибка, включая аргументы, возвращает ERR_SIGNATURE; FEC-поля descriptor не подписаны END. */
int stream_end_verify(const struct stream_descriptor *descriptor, const uint8_t key[32], const uint8_t *wire, size_t length,
                      uint64_t *last);
#ifdef __cplusplus
}
#endif
#endif
