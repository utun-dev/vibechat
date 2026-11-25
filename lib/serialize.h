/**
 * @file serialize.h
 * @brief Бинарная сериализация структур с динамическим выделением памяти
 *
 * Библиотека преобразует C-структуру, содержащую:
 * - фиксированные поля,
 * - ASCIIZ-строки (char*),
 * - динамические массивы (uint8_t* / char*),
 * - singly-linked списки
 * в компактный бинарный буфер и обратно.
 *
 * Поддерживаемые типы полей (см. SERIALIZE_TYPE_*):
 *   0 — фиксированные данные (встроены в структуру)
 *   1 — ASCIIZ-строка (char*, длина = strlen)
 *   2 — массив ровно из 1 элемента (указатель)
 *   3 — массив, количество элементов в UINT8  (по len_offset)
 *   4 — массив, количество элементов в UINT16 (по len_offset)
 *   5 — массив, количество элементов в UINT32 (по len_offset)
 *   6 — singly-linked list (next-указатель по смещению len_offset)
 *
 * Особенности:
 * • Все динамические данные (строки, массивы, списки) выделяются через u_malloc.
 * • При encode длина переменных полей кодируется всегда 2 байтами (uint16_t, big-endian).
 * • Для linked list в буфере сохраняется только количество узлов + сырые данные узлов
 *   (включая байты next/padding; decode заменяет next восстановленными ссылками).
 * • serialize_decode принимает буфер БЕЗ заголовка.
 * • serialize_free освобождает структуру, динамические поля схемы и узлы списков; обнуляет указатель на структуру.
 *   Указатели внутри элементов массивов и узлов списков рекурсивно не освобождаются.
 * • Поля кодируются в порядке schema.fields; фиксированные данные копируются без смены endian.
 * • max_size (если > 0) — лимит encode; header при header_len > 0 обязателен.
 * • При encode counts больше 65535 молча усекаются: сохраняются первые 65535 элементов массива/списка или байтов строки.
 * • elem_size=1 добавляет NUL и для byte-массивов.
 * • Linked nodes копируются целиком, без рекурсивного кодирования вложенных указателей.
 * • Этот формат зависит от ABI сырых полей; сериализация не нормализует размер/выравнивание структур.
 * • Схема доверенная: offsets, типы, размеры и выравнивание полей/next проверяет её автор, не decode.
 * • Независимые операции не используют общее изменяемое состояние; доступ к одной структуре согласует владелец.
 *
 * @example
 * // 1. Описание структуры
 * typedef struct Node {
 *     uint32_t value;
 *     struct Node* next;   // положение задаётся offsetof(Node, next)
 * } Node;
 *
 * typedef struct {
 *     uint8_t      id;
 *     char*        name;
 *     uint8_t      name_len;
 *     uint8_t*     data;
 *     uint16_t     data_cnt;
 *     Node*        list;   // linked list
 * } MyStruct;
 *
 * // 2. Схема
 * static const struct SerializeField my_fields[] = {
 *     {offsetof(MyStruct, id),      1, SERIALIZE_TYPE_FIXED,      0},
 *     {offsetof(MyStruct, name),    1, SERIALIZE_TYPE_ARRAY_U8,   offsetof(MyStruct, name_len)},
 *     {offsetof(MyStruct, data),    1, SERIALIZE_TYPE_ARRAY_U16,  offsetof(MyStruct, data_cnt)},
 *     {offsetof(MyStruct, list),    sizeof(Node), SERIALIZE_TYPE_LINKED, offsetof(Node, next)}
 * };
 *
 * static const struct SerializeSchema my_schema = {
 *     .field_count = 4,
 *     .struct_size = sizeof(MyStruct),
 *     .max_size    = 4096,        // опционально
 *     .header_len  = 4,
 *     .fields      = my_fields
 * };
 *
 * // 3. Использование
 * uint8_t header[] = {0x01, 0x02, 0x03, 0x04};
 * uint8_t *buf = NULL;
 * size_t len;
 *
 * serialize_encode(&orig, &my_schema, header, &buf, &len);
 *
 * MyStruct *restored = NULL;
 * uint8_t *data = buf + my_schema.header_len;
 * serialize_decode(data, len - my_schema.header_len, &my_schema, (void**)&restored);
 *
 * serialize_free(&my_schema, (void**)&restored);
 * u_free(buf);
 */
#ifndef SERIALIZE_H
#define SERIALIZE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include "../lib/mem.h"

#define SERIALIZE_TYPE_FIXED      0  ///< фиксированные данные, elem_size = размер
#define SERIALIZE_TYPE_ASCIIZ     1  ///< char* (ASCIIZ, длина = strlen)
#define SERIALIZE_TYPE_ARRAY_FIXED 2 ///< *array ровно 1 элемент
#define SERIALIZE_TYPE_ARRAY_U8   3  ///< *array, счётчик UINT8
#define SERIALIZE_TYPE_ARRAY_U16  4  ///< *array, счётчик UINT16
#define SERIALIZE_TYPE_ARRAY_U32  5  ///< *array, счётчик UINT32
#define SERIALIZE_TYPE_LINKED     6  ///< linked_list (next по смещению len_offset)

/**
 * @brief Описание одного поля структуры для сериализации
 */
struct SerializeField {
    uint16_t offset;     ///< смещение поля в структуре
    uint8_t  elem_size;  ///< размер элемента (для fixed = размер данных, для array/linked = размер одного элемента)
    uint8_t  type;       ///< SERIALIZE_TYPE_*
    uint16_t len_offset; ///< type 3–5: смещение счётчика в структуре; type 6: смещение next в узле; type 0–2: игнорируется
};

/**
 * @brief Схема структуры для сериализации
 */
struct SerializeSchema {
    uint16_t field_count; ///< число полей
    uint16_t struct_size; ///< размер основной структуры
    uint16_t max_size;    ///< максимальный размер сериализованных данных (0 = без ограничения)
    uint16_t header_len;  ///< длина заголовка
    const struct SerializeField* fields; ///< field_count описателей в порядке записи; схема заимствуется на время вызова
};

#define SERIALIZE_ERR_OK    0  ///< успех
#define SERIALIZE_ERR_BUF  -1  ///< нехватка памяти; decode также: обрезанные данные или неизвестный тип поля
#define SERIALIZE_ERR_NULL -2  ///< NULL-указатель
#define SERIALIZE_ERR_SIZE -3  ///< encode: превышен max_size
#define SERIALIZE_ERR_NOTSUP -4  ///< encode: неподдерживаемый тип поля

/* Кодирует structure по schema; при header_len > 0 header указывает минимум на header_len байт.
 * При успехе *out_buf — новый блок u_malloc (освободить u_free), *out_len — фактически записанная длина с header.
 * Входная структура и её вложенные данные остаются у владельца. На ошибке выходные значения не меняются.
 * Возврат: OK, NULL (обязательные аргументы), BUF (OOM), SIZE (max_size), NOTSUP (неизвестный тип).
 * ASCIIZ требует elem_size=1; порядок fields задаёт формат, сортировка по offset не требуется. */
int serialize_encode(const void* structure,
                     const struct SerializeSchema* schema,
                     const uint8_t* header,
                     uint8_t** out_buf,
                     size_t* out_len);

/* Декодирует in_len байт БЕЗ header: передавать buf + header_len, len - header_len.
 * При успехе *structure — новая структура с выделенными полями; очищать serialize_free с той же схемой.
 * При ошибке частично созданные объекты освобождаются, *structure = NULL. Старый *structure не освобождает.
 * max_size не ограничивает decode; хвост после последнего поля игнорируется.
 * Возврат: OK, NULL (аргументы), BUF (OOM, обрезанные данные или неизвестный тип). */
int serialize_decode(const uint8_t* in_buf, size_t in_len,
                     const struct SerializeSchema* schema,
                     void** structure);

// Освобождает выделенную через u_malloc структуру, поля схемы и узлы списков, обнуляет *structure; NULL допустим.
// Вложенные указатели внутри элементов массивов/узлов не обходит; к структуре на стеке применять нельзя.
void serialize_free(const struct SerializeSchema* schema, void** structure);


#ifdef __cplusplus
}
#endif
#endif // SERIALIZE_H
