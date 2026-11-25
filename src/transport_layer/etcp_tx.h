/* etcp_tx — учёт физических попыток DATA и ACK одного канала.
 * Логический seq/очереди доставки принадлежат ETCP_CONN, попытка — ETCP_LINK.
 * prepare резервирует окно; complete фиксирует принятую транспортом отправку либо откатывает резерв.
 * ACK возвращается своим путём, обновляет только его модель и отдельно подтверждает логический seq.
 * Все вызовы выполняются в uasync-потоке; DATA/ACK этого формата требуют согласованной сессии. */
#ifndef ETCP_TX_H
#define ETCP_TX_H

#include <stdint.h>
#include "../lib/ll_queue.h"

struct ETCP_CONN;
struct ETCP_LINK;
struct ETCP_DGRAM;
struct INFLIGHT_PACKET;
struct ETCP_ACK_TIMING;

/* Изменение задержки между двумя попытками одного пути/эпохи, без синхронизации часов. */
struct ETCP_ACK_TREND {
    uint32_t interval_us;          // Максимальный из четырёх интервалов; 0 — нет пригодной пары.
    uint32_t clock_error_us;       // Граница ошибки каждой дельты от заданного дрейфа и округления часов.
    int64_t forward_delta_us;      // Изменение DATA-задержки; не абсолютная задержка/длина очереди.
    int64_t backward_delta_us;     // Изменение ACK-задержки, отдельно от ожидания сборки ACK.
};

#define ETCP_ACK_CLOCK_DRIFT_PPM 140 // Запас для двух часов по ±6с/сутки (относительно до 138.9 ppm).

/* Сравнить previous/current одного пути и эпохи: разность интервалов убирает постоянное смещение.
 * max_interval_us ограничивает возраст базы; drift_ppm — граница относительного дрейфа часов узлов.
 * 1 — trend заполнен; 0 — очищен (первый sample, reorder, повтор или слишком старая база).
 * Меньшее clock_error_us изменение неотличимо от дрейфа. Не суммировать дельты без накопления ошибки;
 * скачок часов/смена пути требуют сброса базы вызывающим. BBR/RTO эта функция не изменяет. */
int etcp_ack_timing_trend(const struct ETCP_ACK_TIMING* previous, const struct ETCP_ACK_TIMING* current,
    uint32_t max_interval_us, uint32_t drift_ppm, struct ETCP_ACK_TREND* trend);

#define ETCP_TX_STALL_THRESHOLD 3 // три RTO физических попыток без ACK-прогресса, включая один пакетный вызов таймера

/* Внутренний ключ пары seq/номер без padding. Номер 1..65535; wire содержит seq:u32 и номер:u16 отдельно. */
static inline uint64_t etcp_tx_attempt_id(uint32_t seq, uint16_t number) {
    return ((uint64_t)seq << 16) | number;
}

/* Запись из attempt_pool соединения; индекс очереди линка — составной attempt_id, payload здесь не хранится. */
struct ETCP_TX_ATTEMPT {
    struct ll_entry ll;             // len — DATA-байты, dgram=NULL; одна очередь, владелец — линк
    uint64_t attempt_id;            // (seq<<16)|номер; пара не повторяется в одной эпохе, младшие 16 бит ненулевые
    uint32_t seq;                   // логический фрагмент; может быть уже освобождён после доставки
    uint64_t sent_at_us;            // время принятия транспортом; 0 — пока зарезервирован
    uint64_t delivered_at_send;     // доставленные DATA-байты этого канала на момент отправки
    uint64_t delivered_time_us;     // начало ACK-интервала оценки скорости
    uint64_t first_tx_us;           // начало интервала отправки
    uint64_t peer_rx_bytes_at_send; // точка приёма, известная при отправке этой попытки
    uint64_t peer_rx_time_at_send;  // время этой точки на часах получателя; 0 — новая/неизвестная база
    uint32_t inflight_at_send;      // окно после включения этой попытки
    uint8_t is_app_limited;         // источник не имел ожидающих данных либо идёт PROBE_RTT
    uint8_t is_retry;               // До этой принятой отправки seq уже отправлялся; не зависит от отказавших prepare/send.
};

/* Инициализировать очереди attempts/ACK линка. 0 — успех, -1 — OOM (с логом); при ошибке очереди очищены. */
int etcp_tx_link_init(struct ETCP_LINK* link);
/* Снять попытки/таймер/ACK/флаг застревания линка перед reset или free. retry=1 возвращает недоставленные DATA на отправку.
 * reset не меняет физический UP; destroy дополнительно освобождает очереди. */
void etcp_tx_link_reset(struct ETCP_LINK* link, int retry);
void etcp_tx_link_destroy(struct ETCP_LINK* link);
/* Базовый RTO = max(ceil(1.2*RTT + 1.5*jitter + 3000), 10000), все величины в мкс, насыщение UINT32_MAX.
 * RTT=max(DATA EWMA, последний RTT timestamp/keepalive) своего линка; jitter — только DATA, без RTT — 1с.
 * Timestamp-RTT задаёт нижнюю границу, чтобы рост задержки не удалял все попытки до их ACK и не замораживал DATA-измерения.
 * Это граница RTO; RTT/min_rtt и BW модели BBR по-прежнему обновляет только ACK действующей DATA-попытки. */
uint32_t etcp_tx_rto_us(const struct ETCP_LINK* link);
/* Пересчитать ближайший DATA-срок. Единственный UP-путь с single_path_rto_min_us ждёт не меньше этого значения,
 * повторная принятая отправка — вдвое дольше. Без попыток снимает таймер, при закрытии действий нет.
 * Более раннюю проверку можно сохранить, сокращённый срок назначается сразу. При OOM закрывает conn с ERROR.
 * Timestamp handler вызывает после обновления метрик; отправка/ACK/RTO вызывают после изменения attempts. */
void etcp_tx_update_rto(struct ETCP_LINK* link);
/* После изменения состава согласованных UP-путей пересчитать таймеры линков с особой политикой, без внешних callbacks. */
void etcp_tx_conn_update_rto(struct ETCP_CONN* conn);
/* Резервировать окно для packet; NULL — отказ с логом. OOM/бюджет сохраняют packet в очереди,
 * исчерпание номера меняет эпоху и удаляет прежний поток, включая packet.
 * Физические записи ограничены MAX_INFLIGHT_SIZE на линк и 2*MAX_INFLIGHT_SIZE на соединение.
 * Исчерпание бюджета блокирует выбор DATA до ACK/RTO/закрытия; ACK-очередь линка ограничена MAX_INFLIGHT_SIZE.
 * Номер общий для всех линков одного seq; выданный номер не возвращается при отказе send.
 * После номера 65535 новый prepare сбрасывает поток со сменой эпохи, не оборачивает счётчик. */
struct ETCP_TX_ATTEMPT* etcp_tx_prepare(struct ETCP_LINK* link, struct INFLIGHT_PACKET* packet);
/* Завершить синхронный send: success=1 принят транспортом, 0 — снять резерв без сетевой потери.
 * Принятие DATA фиксирует историю каналов и RR; повторно использованный слот начинает новый обход.
 * После потребления ACK пересчитывает ближайший срок оставшихся очередей; успешный пакет снимает pending till/urgent этого пути.
 * Вызвать ровно один раз до освобождения dgram и до следующего poll; dgram->attempt обнуляется. */
void etcp_tx_complete(struct ETCP_DGRAM* dgram, int success);
/* Локальная ошибка уже принятой попытки: ускорить повтор без BBR/RTO-потери; старый ID игнорируется. Только uasync. */
void etcp_tx_failed(struct ETCP_LINK* link, uint64_t attempt_id, int error);
/* Принятый ACK: seq подтверждает логическую доставку; внутренний attempt_id=(seq<<16)|номер обновляет только свой линк.
 * Неизвестный старый ID не создаёт измерение и не вычитает окно. UINT32_MAX delay — RTT недостоверен.
 * До застревания действующий ACK сбрасывает серию RTO; после — только ACK отправки после установки флага сбрасывает серию/флаг.
 * peer_rx_bytes/time_us — согласованная точка приёма пути из ACK; 0/0 — неизвестна.
 * При достоверных положительных дельтах interval=max(send_us,ceil(delivered*delta_rx_us/delta_rx_bytes));
 * дельты от снимка при отправке. Темп приёма заменяет ACK clock, исключая и сжатие, и растяжение обратной очередью.
 * send-интервал сохраняет защиту от сжатия RX clock. Без приёмной базы/прогресса используется max(send_us,ack_us).
 * RTT, min_rtt, логическая доставка и счётчики подтверждённых байтов от выбора часов BW не зависят.
 * Используется неубывающая точка пути; старый/чужой attempt не меняет её, перестановка ACK не откатывает.
 * Непредставимый интервал не создаёт оценку BW. Пустой flight заново начинает приёмную базу, исключая простой. */
void etcp_tx_ack(struct ETCP_LINK* link, uint32_t seq, uint64_t attempt_id, uint32_t receiver_delay_us,
                 uint64_t peer_rx_bytes, uint64_t peer_rx_time_us);
/* Тот же ACK с общей меткой сборки на часах пира и локальным временем обработки; processed_us=0 — текущий вызов.
 * peer_ack_us=0 или UINT32_MAX delay исключают направленное измерение, но не подтверждение доставки.
 * Валидный ACK действующей попытки сохраняет четыре времени в link->ack_timing; BBR/RTO от него не меняются.
 * Время чтения сокета использовать нельзя: окно занято до обработки ACK, включая очереди транспортных workers. */
void etcp_tx_ack_at(struct ETCP_LINK* link, uint32_t seq, uint64_t attempt_id, uint32_t receiver_delay_us,
    uint64_t peer_rx_bytes, uint64_t peer_rx_time_us, uint64_t peer_ack_us, uint64_t processed_us);
/* Поставить ACK уже сохранённого/доставленного DATA в очередь этого линка; 0/-1, ошибка логируется.
 * received_at_us — время приёма DATA ядром после transport/crypto, начало задержки формирования ACK.
 * Вызывающий затем запускает etcp_conn_schedule_ack(). Очередь дешифрования остаётся частью RTT. */
int etcp_tx_queue_ack(struct ETCP_LINK* link, uint32_t seq, uint64_t attempt_id);

#endif
