/* crc32 — табличный CRC-32 с отражённым полиномом 0xEDB88320.
 * calc для непустого блока начинает с UINT32_MAX и инвертирует результат.
 * calc_ex/update возвращают внутренний CRC без финальной инверсии: для потока начать с UINT32_MAX,
 * последовательно update и в конце инвертировать. NULL/пустой calc → UINT32_MAX, update → исходный CRC.
 * Таблица общая, без блокировки: при нескольких потоках выполнить init до их запуска. */
#ifndef CRC32_H
#define CRC32_H

#ifdef __cplusplus
extern "C" {
#endif


#include <stdint.h>
#include <stddef.h>

/* Инициализирует общую таблицу (повторный вызов ничего не меняет).
 * Вызывается автоматически из calc/calc_ex; до параллельных вызовов выполнить в одном потоке. */
void crc32_init(void);

/* data/len — блок байтов, выравнивание не требуется. Для непустого блока возвращает финальный CRC-32;
 * при data==NULL или len==0 возвращает UINT32_MAX, а не стандартный CRC пустого блока (0). */
uint32_t crc32_calc(const uint8_t *data, size_t len);

/* Обновляет initial_crc байтами data[0..len), возвращает CRC без финальной инверсии.
 * При data==NULL или len==0 возвращает initial_crc. Для обычного CRC начать с UINT32_MAX. */
uint32_t crc32_calc_ex(const uint8_t *data, size_t len, uint32_t initial_crc);

/* Эквивалент crc32_calc_ex(data, len, crc); финальная инверсия — ответственность вызывающего. */
uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len);


#ifdef __cplusplus
}
#endif
#endif // CRC32_H
