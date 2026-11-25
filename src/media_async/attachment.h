/* attachment — общие метаданные file/voice/video/JPEG/PNG, без хранения файла, адресата и транспорта.
 * validate/decode возвращают 0 / -1, encode — число записанных байтов / -1.
 * Формат: kind:1, name_len:2, duration_ms:4, width:2, height:2, UTF-8 basename, waveform:100 только для voice.
 * Числа big-endian; имя — корректный UTF-8 basename длиной 1..255 байт, без '/'/'\\', ASCII C0/DEL и '.'/'..'.
 * Лишние wire-байты и NUL внутри переданного имени запрещены. Caller владеет struct/буферами;
 * вход и выход не пересекаются. DM шифрует эти метаданные, GUI получает разобранные поля. */
#ifndef ATTACHMENT_H
#define ATTACHMENT_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

enum attachment_kind { ATTACHMENT_FILE = 1, ATTACHMENT_VOICE, ATTACHMENT_VIDEO, ATTACHMENT_JPEG, ATTACHMENT_PNG };
#define ATTACHMENT_WAVE_SIZE 100
#define ATTACHMENT_WIRE_MAX (11 + 255 + ATTACHMENT_WAVE_SIZE)
#define ATTACHMENT_DURATION_MAX_MS 86400000u /* Общий максимум длительности voice/video: 24ч. */
struct attachment_info {
    enum attachment_kind kind; /* FILE/VOICE/VIDEO/JPEG/PNG; формат изображения не зависит от имени. */
    char name[256]; /* UTF-8 basename с завершающим NUL. */
    uint32_t duration_ms; /* VOICE/VIDEO: 1..ATTACHMENT_DURATION_MAX_MS; остальные: 0. */
    uint16_t width, height; /* VIDEO/JPEG/PNG: 1..16384, изображения с учётом EXIF; FILE/VOICE: 0. */
    char waveform[ATTACHMENT_WAVE_SIZE + 1]; /* 100 уровней из A-Za-z0-9+/; только voice. */
};
/* Проверить все поля и их согласованность с kind; info не меняется, NULL даёт -1. */
int attachment_validate(const struct attachment_info* info);
/* Записать полные проверенные метаданные в out ёмкостью capacity; ошибка не меняет out.
 * Вернуть точную длину wire или -1. out обязателен, ATTACHMENT_WIRE_MAX достаточно для любого kind. */
int attachment_encode(const struct attachment_info* info, uint8_t* out, size_t capacity);
/* Разобрать ровно length байт и проверить метаданные; data/info обязательны. 0/-1.
 * При ошибке info может быть изменена: результат используется только при возврате 0. */
int attachment_decode(const uint8_t* data, size_t length, struct attachment_info* info);
/* Постоянный MIME по kind: audio/opus, video/mp4, image/jpeg, image/png либо application/octet-stream. Не валидатор kind. */
const char* attachment_content_type(enum attachment_kind kind);
/* MIME готового вложения: VIDEO с именем .mkv → video/x-matroska, остальные — стандартный MIME kind. info обязателен и прошёл attachment_validate. */
const char* attachment_info_content_type(const struct attachment_info* info);

#ifdef __cplusplus
}
#endif
#endif
