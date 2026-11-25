// serialize.c - Universal binary serialization with dynamic allocation
#include "serialize.h"
#include <string.h>

#define SERIALIZE_ERR_SIZE   -3  ///< превышен max_size схемы
#define SERIALIZE_ERR_NOTSUP -4  ///< неподдерживаемый тип

static uint16_t length_encode(uint8_t* out, uint16_t val) {
    if (out) {
        out[0] = (uint8_t)(val >> 8);
        out[1] = (uint8_t)val;
    }
    return 2;
}

static uint16_t length_decode(const uint8_t* in, size_t in_len, size_t* consumed) {
    if (!in || in_len < 2) {
        if (consumed) *consumed = 0;
        return 0;
    }
    if (consumed) *consumed = 2;
    return ((uint16_t)in[0] << 8) | in[1];
}

static uint32_t get_var_count(const uint8_t* data, const struct SerializeField* f) {
    if (!data || !f) return 0;
    const uint8_t* len_ptr = data + f->len_offset;
    if (f->type == SERIALIZE_TYPE_ARRAY_U8)  return *len_ptr;
    if (f->type == SERIALIZE_TYPE_ARRAY_U16) return *(const uint16_t*)len_ptr;
    if (f->type == SERIALIZE_TYPE_ARRAY_U32) return *(const uint32_t*)len_ptr;
    return 0;
}

static void set_var_count(uint8_t* data, const struct SerializeField* f, uint16_t count) {
    if (!data || !f) return;
    uint8_t* len_ptr = data + f->len_offset;
    if (f->type == SERIALIZE_TYPE_ARRAY_U8)  *len_ptr = (uint8_t)count;
    else if (f->type == SERIALIZE_TYPE_ARRAY_U16) *(uint16_t*)len_ptr = count;
    else if (f->type == SERIALIZE_TYPE_ARRAY_U32) *(uint32_t*)len_ptr = count;
}

int serialize_encode(const void* structure,
                     const struct SerializeSchema* schema,
                     const uint8_t* header,
                     uint8_t** out_buf,
                     size_t* out_len) {
    if (!schema || !structure || !out_buf || !out_len) return SERIALIZE_ERR_NULL;

    const uint8_t* data = (const uint8_t*)structure;
    size_t total = schema->header_len;

    for (size_t i = 0; i < schema->field_count; i++) {
        const struct SerializeField* f = &schema->fields[i];

        if (f->type == SERIALIZE_TYPE_FIXED || f->type == SERIALIZE_TYPE_ARRAY_FIXED) {
            total += f->elem_size;
        } else if (f->type == SERIALIZE_TYPE_ASCIIZ ||
                   f->type == SERIALIZE_TYPE_ARRAY_U8 ||
                   f->type == SERIALIZE_TYPE_ARRAY_U16 ||
                   f->type == SERIALIZE_TYPE_ARRAY_U32) {
            uint32_t count = 0;
            if (f->type == SERIALIZE_TYPE_ASCIIZ) {
                const char* str = *(const char**)(data + f->offset);
                count = str ? (uint32_t)strlen(str) : 0;
            } else {
                count = get_var_count(data, f);
            }
            uint16_t len = (count > 0xFFFFu) ? 0xFFFFu : (uint16_t)count;
            total += 2;
            if (f->elem_size == 1) {
                total += (size_t)len + 1; // данные + '\0'
            } else {
                total += (size_t)len * f->elem_size;
            }
        } else if (f->type == SERIALIZE_TYPE_LINKED) {
            // подсчёт узлов (максимум 65535)
            void* cur = *(void**)(data + f->offset);
            uint16_t count = 0;
            while (cur && count < 0xFFFFu) {
                count++;
                cur = *(void**)((uint8_t*)cur + f->len_offset);
            }
            total += 2 + (size_t)count * f->elem_size;
        } else {
            return SERIALIZE_ERR_NOTSUP;
        }
    }

    if (schema->max_size > 0 && total > schema->max_size) {
        return SERIALIZE_ERR_SIZE;
    }

    uint8_t* buf = u_malloc(total);
    if (!buf) return SERIALIZE_ERR_BUF;

    uint8_t* ptr = buf;
    if (header && schema->header_len > 0) {
        memcpy(ptr, header, schema->header_len);
        ptr += schema->header_len;
    }

    for (size_t i = 0; i < schema->field_count; i++) {
        const struct SerializeField* f = &schema->fields[i];
        const uint8_t* fld = data + f->offset;

        if (f->type == SERIALIZE_TYPE_FIXED) {
            if (f->elem_size > 0) {
                memcpy(ptr, fld, f->elem_size);
                ptr += f->elem_size;
            }
        } else if (f->type == SERIALIZE_TYPE_ARRAY_FIXED) {
            const uint8_t* arr = *(const uint8_t**)fld;
            if (f->elem_size > 0) {
                if (arr) memcpy(ptr, arr, f->elem_size);
                else     memset(ptr, 0, f->elem_size);
            }
            ptr += f->elem_size;
        } else if (f->type == SERIALIZE_TYPE_ASCIIZ ||
                   f->type == SERIALIZE_TYPE_ARRAY_U8 ||
                   f->type == SERIALIZE_TYPE_ARRAY_U16 ||
                   f->type == SERIALIZE_TYPE_ARRAY_U32) {
            uint32_t raw_count = 0;
            if (f->type == SERIALIZE_TYPE_ASCIIZ) {
                const char* str = *(const char**)fld;
                raw_count = str ? (uint32_t)strlen(str) : 0;
            } else {
                raw_count = get_var_count(data, f);
            }
            uint16_t len = (raw_count > 0xFFFFu) ? 0xFFFFu : (uint16_t)raw_count;
            if (len > 0 && f->type == SERIALIZE_TYPE_ASCIIZ) {
                if (!*(const char**)fld) len = 0;
            } else if (len > 0 && f->type != SERIALIZE_TYPE_ASCIIZ) {
                if (!*(const uint8_t**)fld) len = 0;
            }

            ptr += length_encode(ptr, len);

            if (f->elem_size == 1) { // ASCIIZ
                if (len > 0) {
                    const char* str = *(const char**)fld;
                    if (str) memcpy(ptr, str, len);
                    ptr += len;
                }
                *ptr++ = '\0';
            } else { // array
                if (len > 0) {
                    const uint8_t* arr = *(const uint8_t**)fld;
                    if (arr) memcpy(ptr, arr, (size_t)len * f->elem_size);
                    ptr += (size_t)len * f->elem_size;
                }
            }
        } else if (f->type == SERIALIZE_TYPE_LINKED) {
            void* cur = *(void**)(data + f->offset);
            uint16_t count = 0;
            while (cur && count < 0xFFFFu) {
                count++;
                cur = *(void**)((uint8_t*)cur + f->len_offset);
            }

            ptr += length_encode(ptr, count);

            cur = *(void**)(data + f->offset);
            for (uint16_t i = 0; i < count; i++) {
                if (!cur) break;
                memcpy(ptr, cur, f->elem_size);
                ptr += f->elem_size;
                cur = *(void**)((uint8_t*)cur + f->len_offset);
            }
        }
    }

    *out_buf = buf;
    *out_len = ptr - buf;
    return SERIALIZE_ERR_OK;
}

int serialize_decode(const uint8_t* in_buf,
                     size_t in_len,
                     const struct SerializeSchema* schema,
                     void** structure) {
    if (!in_buf || !schema || !structure) {
        if (structure) *structure = NULL;
        return SERIALIZE_ERR_NULL;
    }

    void* obj = u_malloc(schema->struct_size);
    if (!obj) {
        *structure = NULL;
        return SERIALIZE_ERR_BUF;
    }
    memset(obj, 0, schema->struct_size);

    const uint8_t* ptr = in_buf;
    const uint8_t* end = in_buf + in_len;
    uint8_t* data = (uint8_t*)obj;
    size_t consumed;

    for (size_t i = 0; i < schema->field_count; i++) {
        const struct SerializeField* f = &schema->fields[i];

        if (f->type == SERIALIZE_TYPE_FIXED) {
            if (ptr + f->elem_size > end) goto decode_error;
            if (f->elem_size > 0) {
                memcpy(data + f->offset, ptr, f->elem_size);
                ptr += f->elem_size;
            }
        } else if (f->type == SERIALIZE_TYPE_ARRAY_FIXED) {
            if (ptr + f->elem_size > end) goto decode_error;
            uint8_t* arr = NULL;
            if (f->elem_size > 0) {
                arr = u_malloc(f->elem_size);
                if (arr) memcpy(arr, ptr, f->elem_size);
            }
            *(uint8_t**)(data + f->offset) = arr;
            if (f->elem_size > 0 && !arr) goto decode_error;
            ptr += f->elem_size;
        } else if (f->type == SERIALIZE_TYPE_ASCIIZ ||
                   f->type == SERIALIZE_TYPE_ARRAY_U8 ||
                   f->type == SERIALIZE_TYPE_ARRAY_U16 ||
                   f->type == SERIALIZE_TYPE_ARRAY_U32) {
            uint16_t len = length_decode(ptr, end - ptr, &consumed);
            if (consumed == 0) goto decode_error;
            ptr += consumed;

            int is_str = (f->elem_size == 1);
            size_t needed = is_str ? (size_t)len + 1 : (size_t)len * f->elem_size;

            if ((size_t)(end - ptr) < needed) goto decode_error;

            if (is_str) {
                char* str = NULL;
                if (len == 0) {
                    str = u_malloc(1);
                    if (str) str[0] = '\0';
                } else {
                    str = u_malloc(len + 1);
                    if (str) {
                        memcpy(str, ptr, len);
                        str[len] = '\0';
                    }
                }
                *(char**)(data + f->offset) = str;
                if (!str) goto decode_error;
                ptr += needed;
            } else {
                uint8_t* arr = NULL;
                if (needed > 0) {
                    arr = u_malloc(needed);
                    if (arr) memcpy(arr, ptr, needed);
                }
                *(uint8_t**)(data + f->offset) = arr;
                if (needed > 0 && !arr) goto decode_error;
                ptr += needed;
            }

            if (f->type >= SERIALIZE_TYPE_ARRAY_U8 && f->type <= SERIALIZE_TYPE_ARRAY_U32) {
                set_var_count(data, f, len);
            }
        } else if (f->type == SERIALIZE_TYPE_LINKED) {
            uint16_t len = length_decode(ptr, end - ptr, &consumed);
            if (consumed == 0) goto decode_error;
            ptr += consumed;

            void* head = NULL;
            void* prev = NULL;

            for (uint16_t i = 0; i < len; i++) {
                if (ptr + f->elem_size > end) {
                    // освободить уже созданную часть списка
                    void* n = head;
                    while (n) {
                        void* nxt = *(void**)((uint8_t*)n + f->len_offset);
                        u_free(n);
                        n = nxt;
                    }
                    goto decode_error;
                }

                void* new_node = u_malloc(f->elem_size);
                if (!new_node) {
                    void* n = head;
                    while (n) {
                        void* nxt = *(void**)((uint8_t*)n + f->len_offset);
                        u_free(n);
                        n = nxt;
                    }
                    goto decode_error;
                }

                memcpy(new_node, ptr, f->elem_size);
                // обязательно обнуляем next в новом узле
                *(void**)((uint8_t*)new_node + f->len_offset) = NULL;

                if (!head) {
                    head = new_node;
                } else {
                    *(void**)((uint8_t*)prev + f->len_offset) = new_node;
                }
                prev = new_node;
                ptr += f->elem_size;
            }

            *(void**)(data + f->offset) = head;
        } else {
            goto decode_error;
        }
    }

    *structure = obj;
    return SERIALIZE_ERR_OK;

decode_error:
    serialize_free(schema, &obj);
    *structure = NULL;
    return SERIALIZE_ERR_BUF;
}

void serialize_free(const struct SerializeSchema* schema, void** structure) {
    if (!schema || !structure || !*structure) return;

    uint8_t* data = (uint8_t*)*structure;

    for (size_t i = 0; i < schema->field_count; i++) {
        const struct SerializeField* f = &schema->fields[i];

        if (f->type == SERIALIZE_TYPE_LINKED) {
            void** headp = (void**)(data + f->offset);
            void* node = *headp;
            while (node) {
                void* next = *(void**)((uint8_t*)node + f->len_offset);
                u_free(node);
                node = next;
            }
            *headp = NULL;
        } else if (f->type == SERIALIZE_TYPE_ASCIIZ ||
                   f->type == SERIALIZE_TYPE_ARRAY_FIXED ||
                   (f->type >= SERIALIZE_TYPE_ARRAY_U8 && f->type <= SERIALIZE_TYPE_ARRAY_U32)) {
            uint8_t** p = (uint8_t**)(data + f->offset);
            if (*p) {
                u_free(*p);
                *p = NULL;
            }
        }
    }

    u_free(*structure);
    *structure = NULL;
}
