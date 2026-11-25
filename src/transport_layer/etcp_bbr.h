/* etcp_bbr — модель congestion control одного ETCP-линка: состояния BBR, окно и pacing rate.
 * ETCP собирает bbr_rate_sample из ACK/потерь и вызывает init/main/note_loss в uasync-потоке.
 * tx_start — отдельный helper возобновления после idle, текущая интеграция ETCP его не вызывает.
 * Модуль не отправляет пакеты: возвращённые лимиты применяет линк/loadbalancer.
 * cwnd/inflight/delivered — байты DATA, interval/rtt — микросекунды, pacing_rate — байты в секунду.
 * MSS — максимальный DATA-фрагмент normalizer, без заголовков; не wire-MTU и не размер кадра STCP.
 * Первый достоверный RTT в STARTUP инициализирует pacing от начальных 4*MSS, включая короткие служебные DATA.
 * Короткий RTT-минимум обновляется меньшим достоверным RTT либо измерением после истечения 5с.
 * min_rtt наследует его значение и метку; большее значение принимается после 10с возраста min_rtt.
 * Выход из PROBE_RTT возобновляет короткое окно. Копирование кеша сохраняет его метку, не задаёт текущее время.
 * Метки RTT-фильтров и срок hold — uint64 TB (100мкс); окна проверяются при обработке ACK.
 * Минимальное окно — 4*MSS; PROBE_RTT осушает до max(4*MSS, 0.5*BDP), затем ждёт >=200мс и раунд ACK.
 * Транспорт применяет окно с верхним ограничением конфига и не повышает его до собственного минимума.
 * Целевое окно PROBE_BW включает 2*BDP и ACK-резерв, в PROBE_UP дополнительно 2*MSS.
 * Общий предел этого окна — max(4*MSS, BW*(min_rtt+queue_budget_us)); init задаёт бюджет 20мс.
 * В STARTUP превышение min_rtt+бюджет всеми RTT целого раунда завершает разгон и включает DRAIN.
 * Это цель по оценке полосы, не гарантия физической задержки: STARTUP/DRAIN, минимум окна,
 * неизвестная/изменившаяся полоса и очереди вне наблюдаемого пути могут превышать бюджет. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BBR_SCALE  8
#define BBR_UNIT   (1 << BBR_SCALE)
#define BW_SCALE   16  // bw_*: байты/мкс Q16; диапазон до 65 GB/s, pacing API насыщается на UINT32_MAX B/s
#define BW_UNIT    (1 << BW_SCALE)

#define BBR_STARTUP_PACING_GAIN  (BBR_UNIT * 277 / 100 + 1)
#define BBR_STARTUP_CWND_GAIN    (BBR_UNIT * 2)
#define BBR_DRAIN_GAIN           (BBR_UNIT * 1000 / 2885)
#define BBR_CWND_GAIN            (BBR_UNIT * 2)
#define BBR_QUEUE_BUDGET_US      20000 // целевая добавочная очередь после STARTUP; минимум 4*MSS сохраняется
#define BBR_PROBE_RTT_CWND_GAIN  (BBR_UNIT * 1 / 2)
#define BBR_CWND_MIN_TARGET      4
#define BBR_FULL_BW_THRESH       (BBR_UNIT * 5 / 4)
#define BBR_FULL_BW_CNT          3
#define BBR_LOSS_THRESH          (BBR_UNIT * 2 / 100)
#define BBR_BETA                 (BBR_UNIT * 30 / 100)
#define BBR_INFLIGHT_HEADROOM    (BBR_UNIT * 15 / 100)
#define BBR_EXTRA_ACKED_GAIN     BBR_UNIT
#define BBR_EXTRA_ACKED_WIN_RTTS 5
#define BBR_ACK_EPOCH_ACKED_RESET_THRESH (1U << 20)
#define BBR_EXTRA_ACKED_MAX_US   (100 * 1000)
#define BBR_FULL_LOSS_CNT        6
#define BBR_BW_PROBE_MAX_ROUNDS  63
#define BBR_BW_PROBE_RAND_ROUNDS 2
#define BBR_BW_PROBE_BASE_US     (2 * 1000000)
#define BBR_BW_PROBE_RAND_US     (1 * 1000000)
#define BBR_PROBE_RTT_WIN_MS     5000
#define BBR_MIN_RTT_WIN_SEC      10
#define BBR_PROBE_RTT_MS         200

enum bbr_mode         { BBR_STARTUP = 0, BBR_DRAIN = 1, BBR_PROBE_BW = 2, BBR_PROBE_RTT = 3 };
enum bbr_bw_probe_cyc { BBR_BW_PROBE_UP = 0, BBR_BW_PROBE_DOWN = 1, BBR_BW_PROBE_CRUISE = 2, BBR_BW_PROBE_REFILL = 3 };
enum bbr_ack_phase    { BBR_ACKS_INIT = 0, BBR_ACKS_REFILLING, BBR_ACKS_PROBE_STARTING, BBR_ACKS_PROBE_FEEDBACK, BBR_ACKS_PROBE_STOPPING };

struct bbr_rate_sample {
    uint32_t delivered;        // байты, подтверждённые за измерительный интервал; не только текущим ACK
    uint32_t interval_us;      // мкс; ETCP: max(send,приёмная граница), без RX-базы max(send,ACK); 0 — нет оценки BW
    uint32_t rtt_us;           // RTT линка, мкс; 0 — пока неизвестен
    uint32_t acked_sacked;     // новые подтверждённые байты текущего ACK; только они увеличивают общий delivered
    uint32_t prior_delivered;  // счётчик delivered при отправке подтверждённого пакета, modulo 2^32
    uint32_t tx_in_flight;     // байты в пути при отправке
    int      lost;             // потерянные байты с предыдущего ACK
    int      is_app_limited;   // при отправке не было ожидающих данных приложения либо шёл PROBE_RTT
};

typedef void (*bbr_cwnd_update_fn)(void* ctx, uint32_t new_cwnd);

/* Последний расчёт окна: реальные значения между ограничителями, без повторного вычисления в диагностике.
 * target/model_cap=0 — соответствующий расчёт пропущен (fast path/нет новых ACK). Все размеры в DATA-байтах. */
struct bbr_cwnd_snapshot {
    uint32_t input, target, after_target, model_cap, after_model, output;
};

struct bbr {
    uint32_t min_rtt_us;           // принятый долгий минимум RTT, мкс; UINT32_MAX — измерений ещё нет
    uint32_t bw_lo;
    uint32_t bw_hi[2];
    uint32_t inflight_lo;
    uint32_t inflight_hi;
    uint32_t bw_latest;
    uint32_t inflight_latest;
    uint32_t queue_budget_us;      // целевое время очереди в PROBE_BW, мкс; init задаёт 20мс, 0 отключает ограничение
    uint32_t queue_cwnd_cap;       // последний предел DATA: max(4*MSS, BW*(min_rtt+budget)); 0 — ограничение не активно
    struct bbr_cwnd_snapshot cwnd_snapshot; // фактическая цепочка input→target→inflight model→queue budget
    uint32_t startup_rtt_min_us;   // минимум достоверных RTT текущего ACK-раунда STARTUP; UINT32_MAX — измерений нет
    uint8_t  mode;
    uint8_t  cycle_idx;
    uint16_t pacing_gain;
    uint16_t cwnd_gain;
    uint32_t next_rtt_delivered;
    uint8_t  round_start     : 1;
    uint8_t  full_bw_reached : 1;
    uint8_t  loss_in_round   : 1;
    uint8_t  loss_in_cycle   : 1;
    uint8_t  idle_restart    : 1;
    uint8_t  try_fast_path   : 1;
    uint8_t  has_seen_rtt    : 1;
    uint8_t  initialized     : 1;
    uint32_t loss_round_delivered;
    uint32_t bw_probe_up_cnt;
    uint32_t bw_probe_up_acks;
    uint8_t  bw_probe_up_rounds;
    uint8_t  bw_probe_samples   : 1;
    uint8_t  prev_probe_too_high: 1;
    uint8_t  stopped_risky_probe: 1;
    uint8_t  rounds_since_probe;
    uint32_t probe_wait_us;
    uint64_t cycle_mstamp_tb;
    uint32_t probe_rtt_min_us;     // короткий RTT-минимум, мкс; UINT32_MAX — измерений ещё нет
    uint64_t probe_rtt_min_stamp_tb; // метка короткого фильтра; обновляется его измерением и завершением PROBE_RTT
    uint64_t probe_rtt_done_stamp_tb; // полный срок hold в TB; 0 — hold ещё не начат; используется в PROBE_RTT
    uint8_t  probe_rtt_round_done : 1;
    uint32_t extra_acked[2];       // избыточно подтверждённые байты, без усечения на 64KiB
    uint32_t ack_epoch_acked;
    uint64_t ack_epoch_mstamp_tb;
    uint8_t  extra_acked_win_idx  : 1;
    uint8_t  extra_acked_win_rtts : 5;
    uint32_t prior_cwnd;
    uint8_t  init_cwnd     : 7;     // начальное окно в DATA-фрагментах; init задаёт BBR_CWND_MIN_TARGET
    uint8_t  full_bw_cnt   : 2;
    uint8_t  ack_phase     : 3;
    uint8_t  loss_events_in_round : 4;
    uint32_t delivered;
    uint32_t full_bw;
    uint64_t min_rtt_stamp_tb;     // метка короткого фильтра, принятая вместе с min_rtt_us; окно 10с
    uint8_t  loss_round_start : 1;
    uint8_t  full_bw_now      : 1;
    uint8_t  pad_unused       : 6;
    uint8_t  link_id;              // local_link_id for debug logging
    uint64_t now_tb;               // 0 = real get_time_tb(); >0 = test-controlled time
    bbr_cwnd_update_fn on_cwnd_update;
    void* cwnd_update_ctx;
};

// Перед первым init обнулить структуру; init сохраняет now_tb/link_id, задаёт STARTUP и начальное окно 4 DATA-фрагмента.
void bbr_init(struct bbr* bbr);
/* Обработать ACK rs и обновить входные/выходные cwnd_out (байты DATA) и pacing_rate_out (B/s).
 * mss>0 — frag_size normalizer; inflight_bytes — текущий занятый DATA-flight после ACK;
 * is_cwnd_limited — отправитель упирался в окно до ACK. Все указатели обязательны.
 * При неверных аргументах логирует ошибку и сохраняет выходы; void. */
void bbr_main(struct bbr* bbr, const struct bbr_rate_sample* rs,
              uint32_t* cwnd_out, uint32_t* pacing_rate_out,
              uint32_t mss, uint32_t inflight_bytes, int is_cwnd_limited);
void bbr_note_loss(struct bbr* bbr);
void bbr_tx_start(struct bbr* bbr);

#ifdef __cplusplus
}
#endif
