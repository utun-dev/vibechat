/* opus_codec — владеющие обёртки encoder/decoder libopus для int16 interleaved PCM.
 * create принимает частоту и 1/2 канала; неположительная частота → 48000, неверные каналы → моно.
 * Encoder/decoder имеют состояние потока; каждому нужен один последовательный владелец.
 * frame_samples — отсчёты НА КАНАЛ: PCM вмещает frame_samples*channels элементов.
 * encode возвращает байты Opus, decode — отсчёты на канал; отрицательное значение означает ошибку.
 * out_cap — байты; decode с data=NULL/len=0 использует PLC libopus. Освобождение — соответствующим destroy. */
#ifndef OPUS_CODEC_H
#define OPUS_CODEC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct opus_codec_encoder opus_codec_encoder_t;
typedef struct opus_codec_decoder opus_codec_decoder_t;

/* Режим кодирования Opus: голос (VOIP, default) или музыка (AUDIO). */
#define OPUS_CODEC_APP_VOIP  2048   /* OPUS_APPLICATION_VOIP */
#define OPUS_CODEC_APP_AUDIO 2049   /* OPUS_APPLICATION_AUDIO */

opus_codec_encoder_t* opus_codec_encoder_create(int sample_rate, int channels);
void opus_codec_encoder_destroy(opus_codec_encoder_t* enc);
int opus_codec_encode(opus_codec_encoder_t* enc, const int16_t* pcm, int frame_samples,
        uint8_t* out, int out_cap);

opus_codec_decoder_t* opus_codec_decoder_create(int sample_rate, int channels);
void opus_codec_decoder_destroy(opus_codec_decoder_t* dec);
int opus_codec_decode(opus_codec_decoder_t* dec, const uint8_t* data, int len,
        int16_t* pcm, int frame_samples);

int opus_codec_encoder_bitrate_set(opus_codec_encoder_t* enc, int bitrate_bps);
int opus_codec_encoder_complexity_set(opus_codec_encoder_t* enc, int complexity);
int opus_codec_encoder_signal_set(opus_codec_encoder_t* enc, int signal_type);
int opus_codec_encoder_application_set(opus_codec_encoder_t* enc, int application);
/* Задержка codec в сэмплах на канал; запрашивать после настройки encoder, не задавать константой. */
int opus_codec_encoder_delay(opus_codec_encoder_t* enc, int* samples);

const char* opus_codec_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif
