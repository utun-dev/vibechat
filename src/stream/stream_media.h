/* Медиаблок внутри одной FEC-пачки: big-endian, uAV2, fixed header(32),
 * video descriptors(32 each), audio descriptors(16 each), video bytes, audio bytes.
 * video descriptor: frame_id:8, PTS_us:8, frame_size:4, fragment_offset:4, length:4, flags:1, reserved:3.
 * audio descriptor: PTS_us:8, length:4, samples:2, reserved:2. Opus: 48kHz mono/stereo, 960 frames/channel.
 * KEY payload: config_length:4BE, codec configuration, access unit. Остальные кадры — access unit.
 * Видео меняется по revision; аудиотрек фиксирован на source_epoch. Конфигурация защищена общей подписью/RS.
 * first_frame_offset относится к video bytes; UINT32_MAX означает отсутствие начала кадра.
 * Parser проверяет весь блок до публикации view, указатели view заимствованы из входного блока.
 * Сборка кадров и восстановление после GAP/revision выполняются store/cursor из stream_media_buffer.h. */
#ifndef STREAM_MEDIA_H
#define STREAM_MEDIA_H

#include "stream_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STREAM_MEDIA_HEADER_SIZE 32
#define STREAM_MEDIA_VIDEO_SIZE 32
#define STREAM_MEDIA_AUDIO_SIZE 16
#define STREAM_MEDIA_MAX_ENTRIES 16
#define STREAM_MEDIA_MAX_FRAME (2u * 1024u * 1024u)
#define STREAM_MEDIA_MAX_BLOCK 65536u
#define STREAM_MEDIA_MAX_OPUS 1275u
#define STREAM_MEDIA_KEY 1
#define STREAM_MEDIA_H264 1
#define STREAM_MEDIA_H265 2
#define STREAM_MEDIA_VP9 3
#define STREAM_MEDIA_AV1 4
#define STREAM_MEDIA_H266 5
#define STREAM_MEDIA_MAX_CONFIG 65536u
#define STREAM_MEDIA_OPUS 1
#define STREAM_MEDIA_OPUS_STEREO 2

struct stream_media_format {
    uint16_t width, height, rotation; /* Геометрия 1..4096, поворот 0/90/180/270. */
    uint8_t video_codec, audio_codec; /* H264..H266 и 0/OPUS/OPUS_STEREO; 0 означает отсутствие аудиотрека. */
    uint32_t revision;               /* Ненулевая версия видео; связь версий между пачками проверяет store. */
};
struct stream_media_video {
    uint64_t frame_id, pts_us;          /* Ненулевой ID кадра и PTS <= INT64_MAX в микросекундах источника. */
    uint32_t frame_size, offset, length; /* Полный размер <=2MiB, смещение/длина непустого фрагмента. */
    uint8_t flags;                     /* Только KEY; одинаковы у всех фрагментов кадра. */
    const uint8_t *data;               /* Заимствованные length байтов. */
};
struct stream_media_audio {
    uint64_t pts_us;      /* PTS <= INT64_MAX, микросекунды источника. */
    uint32_t length;      /* 1..1275 байтов одного Opus-пакета. */
    uint16_t samples;     /* Ровно 960 frames/channel (20мс при 48кГц). */
    const uint8_t *data;  /* Заимствованные length байтов; codec здесь не декодируется. */
};
struct stream_media_block {
    struct stream_media_format format;
    uint32_t first_frame_offset; /* Parser вычисляет и сверяет; encode вычисляет заново, поле входа игнорируется. */
    unsigned video_count, audio_count; /* Каждый <=16, сумма >0; видео/аудио внутри блока упорядочены по PTS. */
    struct stream_media_video video[STREAM_MEDIA_MAX_ENTRIES];
    struct stream_media_audio audio[STREAM_MEDIA_MAX_ENTRIES];
};

/* Encode: полный блок <=65536 байтов, данные не перекрывают output; output/length обязательны.
 * STREAM_OK, ERR_ARGUMENT для недостаточного capacity/выходов или ERR_FORMAT для неверного блока.
 * output/length не меняются при отказе; межблочную непрерывность/codec bitstream не проверяет. */
int stream_media_encode(const struct stream_media_block *block, uint8_t *output, size_t capacity, size_t *length);
/* Parse: проверить полный канонический блок и reserved, затем опубликовать заимствованные views в block.
 * STREAM_OK/ERR_FORMAT; block не меняется при отказе, data должен жить всё время использования views. */
int stream_media_parse(const uint8_t *data, size_t length, struct stream_media_block *block);
/* Разобрать только полностью собранный KEY: config допускает длину 0..65536, access unit обязателен.
 * Указатели заимствованы из data; все выходы обязательны и не меняются при ERR_FORMAT. Codec не проверяется. */
int stream_media_key_parse(const uint8_t *data, size_t length, const uint8_t **config, size_t *config_length,
                           const uint8_t **frame, size_t *frame_length);

#ifdef __cplusplus
}
#endif
#endif
