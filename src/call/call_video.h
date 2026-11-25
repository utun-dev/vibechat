/* Общий контракт видеотрека: чистые проверки/сериализация доступны любому потоку,
 * один receiver принадлежит одному владельцу, без внутренней синхронизации.
 * Сетевой transport использует receiver из uasync; codec workers используют проверки формата.
 * Кодеки: H.264 Baseline/Constrained Baseline, H.265 Main (Main/High tier), 8 bit,
 * 4:2:0, progressive, без B-кадров/reorder. DPB ограничен уровнем потока.
 * Поля format неизменны внутри generation; его увеличивают при смене формата/камеры.
 * Текущий bitrate encoder не входит в format и не меняет generation.
 * KEY: config_length:4BE + Annex B configuration + Annex B access unit (как uAV2).
 * Приём хранит один частичный кадр. Готовый view заимствован до следующего push/reset.
 * Очередью готовых кадров, decoder и показом по audio clock владеет вызывающий. */
#ifndef CALL_VIDEO_H
#define CALL_VIDEO_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define CALL_VIDEO_H264 1
#define CALL_VIDEO_H265 2
#define CALL_VIDEO_KEY 1
#define CALL_VIDEO_MAX_FRAME (2u * 1024u * 1024u)
#define CALL_VIDEO_FRAGMENT_BYTES 1200u
#define CALL_VIDEO_HEADER_BYTES 56u
#define CALL_VIDEO_CAPS_BYTES 64u
#define CALL_VIDEO_BASELINE 1u
#define CALL_VIDEO_CONSTRAINED_BASELINE 2u
#define CALL_VIDEO_MAIN 1u
#define CALL_VIDEO_MAIN_HIGH_TIER 2u

/* Нулевой limit означает отсутствие проверенного encoder/decoder данного профиля.
 * Индексы 0=H.264, 1=H.265. Лимиты независимы для каждого направления.
 * level — максимальный AVC level_idc / HEVC general_level_idc (31 / 93 для 3.1).
 * Поля — независимые верхние границы, не обещание поддержки их сочетания:
 * выбранный режим дополнительно проверяется открытием платформенного codec. */
struct call_video_limit {
    uint16_t width, height, fps; // Верхние границы осей (16..4096) и частоты (1..60); нулевой профиль — все поля нулевые.
    uint32_t pixels_per_second, maximum_bps; // Ограничение width*height*fps и целевого bitrate (300000..4000000 бит/с).
    uint8_t profiles; // Маска режимов: AVC Baseline/CB; HEVC Main с Main/High tier. Приём каждого проверяется отдельно.
    uint8_t level;
};
struct call_video_caps { struct call_video_limit encode[2], decode[2]; };
struct call_video_format {
    uint64_t generation; // Ненулевой, монотонно возрастающий номер формата в одном направлении звонка.
    uint16_t width, height, fps, rotation; // Чётные размеры 16..4096, 1..60 fps, поворот отображения 0/90/180/270 градусов.
    uint8_t codec, level; // CALL_VIDEO_H264/H265 и AVC level_idc / HEVC general_level_idc.
    uint32_t maximum_bps; // Предел целевого bitrate; не обещание мгновенного размера выхода/CPB.
    uint8_t profile; // Один режим 1/2, не маска: AVC Baseline/CB либо HEVC Main с Main/High tier.
};
struct call_video_frame {
    uint64_t call_id, frame_id; // Ненулевые ID; frame_id возрастает сквозь поколения, пропуск требует KEY.
    int64_t pts_us; // Capture MONOTONIC отправителя: 0..INT64_MAX/2, строго возрастает сквозь поколения.
    struct call_video_format format;
    uint32_t size, offset, length; // Размер всего кадра, смещение и размер данного фрагмента; полный кадр: offset=0, length=size.
    uint8_t flags; // Только CALL_VIDEO_KEY либо 0.
    const uint8_t *data; // Начало payload данного фрагмента, уже со смещением offset; владение задаёт вызывающий API.
    int64_t received_us; // Только локальная RX port метка MONOTONIC; по сети не передаётся.
};

/* 1 — все limits структурно корректны, включая полностью нулевые CAPS; 0 — ошибка/NULL.
 * Не проверяет реальные codec и совместимость сочетания независимых границ. */
int call_video_caps_valid(const struct call_video_caps *caps);
/* После успешных платформенных проб width×height и height×width расширяет обе осевые
 * границы limit до длинной стороны, сохраняя pixel-rate/FPS/bitrate/profile/level.
 * codec — 1/2; limit — ненулевые проверенные возможности одного направления.
 * 0 — успех, -1 — неверный limit/codec (limit не меняется). */
int call_video_limit_orientations(struct call_video_limit *limit, int codec);
/* format задаёт generation/размер/FPS/rotation/maximum_bps, level=0, profile=0 для auto.
 * codec=0: H.265, затем H.264. Успех: codec (1/2), заполняются codec/profile и минимальный level.
 * 0 — нет совместимого режима, -1 — неверные аргументы; на отказе format не меняется. */
int call_video_choose(const struct call_video_caps *sender, const struct call_video_caps *receiver,
                      struct call_video_format *format);
/* 1 — формат допустим для sender.encode и receiver.decode, 0 — нет/неверный аргумент. */
int call_video_format_supported(const struct call_video_caps *sender, const struct call_video_caps *receiver,
                                const struct call_video_format *format);
/* 1/0: проверка полей и таблиц level по padded geometry/FPS/bitrate, без открытия codec. */
int call_video_format_valid(const struct call_video_format *format);
/* Нормативный предел DPB в кадрах по coded geometry и level; 0 при неверном уровне. */
unsigned call_video_dpb_limit(int codec, unsigned level, unsigned width, unsigned height);
/* 1/0: ненулевые HRD bitrate (бит/с) и cpb (биты) в пределах таблицы уровня.
 * nal!=0 включает NAL-множитель; для HEVC используется консервативная таблица Main tier. */
int call_video_hrd_valid(int codec, unsigned level, int nal, uint64_t bitrate, uint64_t cpb);
/* Сравнение всех полей format, включая generation; оба указателя обязательны. */
int call_video_same_format(const struct call_video_format *a, const struct call_video_format *b);
/* CAPS: четыре 16-байтовых limits (encode AVC/HEVC, decode AVC/HEVC), integers BE.
 * 0/-1; при ошибке выход не меняется. Parse требует ровно CALL_VIDEO_CAPS_BYTES. */
int call_video_caps_encode(const struct call_video_caps *caps, uint8_t out[CALL_VIDEO_CAPS_BYTES]);
int call_video_caps_parse(const uint8_t *data, size_t length, struct call_video_caps *out);
/* Header 56 байт BE + 1..1200 байт payload; size всего кадра <=2 МиБ.
 * Encode копирует fragment.data[0..length); parse возвращает view внутри data.
 * Успех 0; весь header проверяется до записи/публикации view. Отказ -1, выход не меняется. */
int call_video_fragment_encode(const struct call_video_frame *frame, uint8_t *out, size_t capacity, size_t *length);
int call_video_fragment_parse(const uint8_t *data, size_t length, struct call_video_frame *out);

struct call_video_receiver;
/* Создать пустую сборку, изначально требующую KEY; NULL — ошибка выделения. */
struct call_video_receiver *call_video_receiver_create(void);
/* Destroy/reset допускают NULL. Reset освобождает буфер и забывает ID/PTS/формат. */
void call_video_receiver_destroy(struct call_video_receiver *receiver);
void call_video_receiver_reset(struct call_video_receiver *receiver);
/* 1 — полный кадр, 0 — частичный/пропущенный, -1 — неверный поток; после пропуска нужен KEY.
 * fragment — валидный заголовок и payload одного фрагмента, call_id/peer/group проверяет владелец.
 * now_us — неубывающее локальное время. Частичная сборка сохраняется во время паузы.
 * Фрагменты одного кадра идут без разрывов offset; повтор/перестановка сбрасывают сборку.
 * KEY здесь проверяется только по envelope; NAL проверяет call_video_bitstream/платформа.
 * Новый формат принимается только с KEY. Старые готовые кадры вызывающий сохраняет до их PTS. */
int call_video_receiver_push(struct call_video_receiver *receiver, const struct call_video_frame *fragment,
                             uint64_t now_us, struct call_video_frame *complete);
/* 1 — нужен независимый KEY (также для NULL), 0 — цепочка сборки непрерывна. */
int call_video_receiver_needs_key(const struct call_video_receiver *receiver);
#ifdef __cplusplus
}
#endif
#endif
