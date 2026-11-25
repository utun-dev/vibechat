/* Кольцо структурно проверенных частей одного запуска; подпись проверяется после восстановления.
 * Слоты резервируются по максимальному размеру, stored_bytes учитывает фактические длины.
 * Вытеснение целой старейшей пачки не закрепляет данные для медленного получателя.
 * Все вызовы — один uasync-поток. read возвращает память до следующего put/destroy.
 * public_key должен происходить из проверенной записи источника в группе. */
#ifndef STREAM_BUFFER_H
#define STREAM_BUFFER_H

#include "stream_block.h"
#include "stream_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

struct stream_buffer;
struct stream_buffer_delay {
    uint64_t frames, video_us, audio_us; // Непереданные кадры и длительности параллельных треков
    uint64_t first_pts, last_pts;       // Границы видео для объединения с ещё не упакованной очередью источника
    uint32_t period_us;                // Измеренный период последнего кадра
    int known;                         // 0 — отсутствует кадровая подсказка или ещё неизвестен период первого кадра
};
/* Сохранить неподписанную подсказку DATA для уже имеющейся пачки: OK/EXPIRED/ERR_*.
 * Повтор valid-подсказки должен совпасть по wire; на воспроизведение эти данные не влияют. */
int stream_buffer_set_timing(struct stream_buffer *buffer, uint64_t seq, const struct stream_frame_timing *timing);
/* Скопировать подсказку, включая valid=0: OK/EXPIRED/ERR_ARGUMENT; при отсутствии выход не меняется. */
int stream_buffer_get_timing(struct stream_buffer *buffer, uint64_t seq, struct stream_frame_timing *timing);
/* Очередь сохранённых частей одного subblock >= seq: OK/ERR_ARGUMENT. Даже пустая очередь даёт OK и нули.
 * frames считает начала кадров и один начатый ранее кадр; audio_us/video_us — параллельные длительности. */
int stream_buffer_get_delay(struct stream_buffer *buffer, uint64_t seq, uint8_t subblock, struct stream_buffer_delay *delay);
/* Скопировать последнюю телеметрию субпотока в кольцо: OK/ERR_ARGUMENT/ERR_MEMORY. Сохраняется после закрытия receive. */
int stream_buffer_set_telemetry(struct stream_buffer *buffer, uint8_t subblock, const struct stream_telemetry *telemetry);
/* Скопировать снимок с возрастом на now_ms: OK/WAIT/ERR_ARGUMENT; WAIT не меняет выход и сохранённый снимок. */
int stream_buffer_get_telemetry(struct stream_buffer *buffer, uint8_t subblock, uint64_t now_ms, struct stream_telemetry *telemetry);
/* Если peer — последний узел пути, пометить все строки STALE, последнюю также DOWN; числа сохранить. */
void stream_buffer_telemetry_down(struct stream_buffer *buffer, uint8_t subblock, struct stream_address peer);
/* Синхронное добавление/вытеснение части: added=1/0. Observer не выполняет I/O и не меняет кольцо;
 * wire заимствован лишь на callback. Destroy observer не вызывает. */
typedef void (*stream_buffer_event_fn)(void *arg, const uint8_t *wire, size_t length, int added);
/* Заменить observer/arg; observer=NULL отключает уведомления, прежнее arg модуль не освобождает. */
void stream_buffer_set_observer(struct stream_buffer *buffer, stream_buffer_event_fn observer, void *arg);
/* Точное число/сумма байтов частей >= seq и время старейшего принятия.
 * Нужен хотя бы один выход; count=bytes=NULL ищет только старейший, без полного обхода.
 * OK — найдены части; WAIT — выходы обнулены; ERR_ARGUMENT — выходы не меняются. */
int stream_buffer_pending(struct stream_buffer *buffer, uint64_t seq, uint8_t subblock,
                          uint64_t *count, uint64_t *bytes, uint64_t *oldest_ms);

struct stream_buffer_stats {
    size_t capacity_bytes;     // Зарезервированные слоты без индекса, путей и scratch декодирования
    size_t stored_bytes;       // Фактические wire-байты занятых слотов
    uint32_t blocks;           // Сохранённые RS-части
    uint32_t batches;          // Пачки, имеющие хотя бы одну часть
    uint64_t expired_before;   // Любой seq ниже этой границы уже вытеснен
    uint64_t highest_seq;      // Наибольший принятый seq; у пустого нового кольца 0
    uint64_t evicted_batches;  // Всего вытеснено целых пачек
    uint64_t duplicates;       // Повторные неизменные части
    uint64_t rejected_batches; // Пачки, отвергнутые при восстановлении/проверке подписи
};

#define STREAM_BUFFER_POSITION_PEERS 16
/* Одна линия индикатора — получатель; next обозначает позицию передачи в ETCP, а не подтверждённой доставки. */
struct stream_buffer_peer_position {
    struct stream_address peer; // Ed25519 получателя
    uint64_t worst, best;      // Минимальный/максимальный следующий seq его субпотоков
    uint64_t worst_us, best_us; // Максимальная/минимальная длительность непереданных данных; UINT64_MAX — неизвестна
    unsigned subflows;         // Число действующих выдач получателю
};
struct stream_buffer_positions {
    uint64_t first, head;      // [first, head) — сохранённое окно; head — позиция после последней пачки
    unsigned count, total;    // Показанные/все получатели; показаны до 16 наиболее отставших
    struct stream_buffer_peer_position peers[STREAM_BUFFER_POSITION_PEERS]; // По возрастанию worst, затем peer
};

/* out заранее NULL; bytes=0 выбирает 32 МБ, меньший бюджет допускается для стенда.
 * Должна помещаться хотя бы одна полная пачка n блоков; верхний предел — STREAM_BUFFER_BYTES.
 * descriptor/public_key копируются; OK/ERR_* с неизменным out при ошибке. */
int stream_buffer_create(struct stream_buffer **out, const struct stream_descriptor *descriptor,
                         const uint8_t public_key[SC_PUBKEY_SIZE], size_t bytes);
/* Освободить кольцо и заимствованные read-данные; NULL допустим. */
void stream_buffer_destroy(struct stream_buffer *buffer);

/* Формат новой части проверяется до изменения кольца. Повтор неизменной части: STREAM_DUPLICATE.
 * STREAM_EXPIRED не меняет более свежие данные; конфликт возвращает отрицательный код.
 * Части ещё не подтверждены подписью источника и могут пересылаться до локального decode. */
int stream_buffer_put(struct stream_buffer *buffer, const uint8_t *wire, size_t length);
/* Как put с путём от источника до владельца кольца включительно; первый путь повтора остаётся неизменным.
 * put без явного пути сохраняет единственный узел source. */
int stream_buffer_put_path(struct stream_buffer *buffer, const uint8_t *wire, size_t length, const struct stream_path *path);
/* Скопировать путь части: OK/GAP/EXPIRED/ERR_ARGUMENT; без части выход не меняется. */
int stream_buffer_path(struct stream_buffer *buffer, uint64_t seq, uint8_t subblock, struct stream_path *path);
/* Найти ближайший имеющийся блок >= seq для скачка через GAP; WAIT при отсутствии. */
int stream_buffer_next_seq(struct stream_buffer *buffer, uint64_t seq, uint8_t subblock, uint64_t *next);
/* Ближайший seq >= заданного с хотя бы k частями: OK/WAIT/ERR_ARGUMENT. Подпись и REJECTED здесь не проверяются. */
int stream_buffer_next_decodable(struct stream_buffer *buffer, uint64_t seq, uint64_t *next);

/* seq=UINT64_MAX читает последний имеющийся блок именно запрошенного субпотока.
 * WAIT — блок впереди головы/пустое кольцо; GAP — дыра; EXPIRED — вытеснен.
 * При отсутствии блока выходные указатели/номер не меняются. */
int stream_buffer_read(struct stream_buffer *buffer, uint64_t seq, uint8_t subblock,
                       const uint8_t **wire, size_t *length, uint64_t *actual_seq);
/* Границы включительно, внутри допустимы дыры: OK/WAIT/ERR_ARGUMENT; без частей выходы не меняются. */
int stream_buffer_range(struct stream_buffer *buffer, uint8_t subblock, uint64_t *first, uint64_t *last);

/* Восстановить пачку и проверить одну подпись; достаточно capacity >= действительной длины.
 * После успеха фиксируется использованный набор k неизменяемых частей: повторное чтение не проверяет подпись заново
 * и не доверяет поздним частям, которые не участвовали в подтверждённом восстановлении.
 * STREAM_REJECTED означает неверную подпись/подписанные метаданные; такая пачка повторно не декодируется.
 * Выход не меняется при WAIT/GAP/EXPIRED/REJECTED/ошибке; отсутствующие части в кольцо не добавляются. */
int stream_buffer_decode(struct stream_buffer *buffer, uint64_t seq, uint8_t *output, size_t capacity, size_t *length);
/* Скопировать текущие счётчики без обхода слотов: OK/ERR_ARGUMENT. */
int stream_buffer_get_stats(const struct stream_buffer *buffer, struct stream_buffer_stats *stats);

#ifdef __cplusplus
}
#endif
#endif
