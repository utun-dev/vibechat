/* voice_file — кодирование PCM в общий контейнер Opus голосовых сообщений Qt/Android. Вызывать только в worker.
 * PCM принадлежит вызывающему; encode заполняет duration/waveform по реально записанным кадрам. Возврат 0 / -1.
 * path — собственный файл вызывающего: fopen(wb) перезаписывает прежнее содержимое;
 * после ошибки записи/close файл удаляется (отказ удаления логируется). fsync/атомарной публикации здесь нет.
 * count — число interleaved int16 отсчётов; rate — 8/12/16/24/48 kHz, channels — 1/2, preset — 0..2.
 * Пишутся полные кадры по 20мс (хвост отбрасывается), минимум 500мс, максимум ATTACHMENT_DURATION_MAX_MS.
 * Недопустимая длительность отвергается до создания encoder, открытия path и изменения info.
 * name задаёт caller; encode устанавливает kind=VOICE и обнуляет width/height.
 * info используется только при rc=0: при поздней ошибке close часть полей уже может быть изменена.
 * Формат native-endian: magic:uint32=0x5355504f, rate:uint32, channels:uint16, samples_per_channel:uint16;
 * затем (packet_len:uint16, Opus bytes) для каждого кадра и нулевая длина в конце. */
#ifndef VOICE_FILE_H
#define VOICE_FILE_H
#include "attachment.h"
#ifdef __cplusplus
extern "C" {
#endif
int voice_file_encode(const int16_t* pcm, size_t count, int rate, int channels, int preset,
                       const char* path, struct attachment_info* info);
#ifdef __cplusplus
}
#endif
#endif
