/* Общая проверка независимого KEY (configuration + Annex B AU). Не переписывает поток.
 * Проверяет ограниченный набор для звонков: AVC Baseline/CB, HEVC Main с Main/High tier,
 * progressive 8-bit 4:2:0, один spatial/temporal layer, без reorder. DPB — по Annex A.
 * HEVC допускает dependent segments внутри IDR и явный pic_output_flag=1;
 * первый слайс независимый, адреса остальных — в границах CTB геометрии SPS.
 * Это проверка нужных полей parameter sets и заголовков IDR, без полного разбора entropy payload;
 * она не заменяет проверку реальным decoder. Результат публикуется только при успехе.
 * Все функции доступны worker-потокам; состояния и платформенных объектов нет. */
#ifndef CALL_VIDEO_BITSTREAM_H
#define CALL_VIDEO_BITSTREAM_H
#include "call_video.h"
#ifdef __cplusplus
extern "C" {
#endif
struct call_video_bitstream {
    unsigned width, height, level, profile, dpb; // Видимая геометрия после crop, wire level/profile и SPS DPB в кадрах.
};
/* codec=1/2, data — полный KEY envelope размером 9..CALL_VIDEO_MAX_FRAME.
 * 0 — заполнен out, -1 — отказ с логом; out на ошибке не меняется, data заимствован на время вызова. */
int call_video_inspect_key(int codec, const uint8_t *data, size_t length, struct call_video_bitstream *out);
/* Inspect + сравнение геометрии/profile и actual level <= format.level; 0/-1.
 * AVC CB принимается и в Baseline envelope. Сам format предварительно валидирует владелец. */
int call_video_check_key(const struct call_video_format *format, const uint8_t *data, size_t length);
/* KEY-проба 1280x720 либо 1920x1080, включая обратную ориентацию, fps=30 либо 60.
 * Буфер 8192 байт достаточен; возвращает размер или -1 с диагностикой. */
int call_video_probe_key(int codec, int profile, unsigned width, unsigned height, unsigned fps, uint8_t *out, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
