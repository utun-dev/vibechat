# Radix Tree (Patricia Trie) для IP-маршрутизации

## 1. Назначение

Бинарное radix-дерево (Patricia trie) для хранения маршрутов и поиска Longest Prefix Match (LPM). Производное от BSD `net/radix.c`. Используется в `route_lib.c` / `route6_lib.c` для таблиц маршрутизации IPv4/IPv6.

Каждый маршрут — пара `(key, mask)`. Ключи — sockaddr-подобные структуры: первый байт хранит длину всей структуры, остальное — IP-адрес. Максимальная длина ключа — 32 байта (`RADIX_MAX_KEY_LEN`).

Потокобезопасность: замки — заглушки (no-op). Подразумевается однопоточное использование с `u_async`.

## 2. Как пользоваться

```c
#include "../lib/radix.h"

// --- Инициализация ---
struct radix_node_head *rnh = NULL;
rn_inithead((void**)&rnh, 1);  // off=1 пропускает байт длины в сравнениях

// --- Структура данных маршрута ---
struct route_data {
    struct radix_node rn_nodes[2]; // ПАМЯТЬ ДЛЯ ДЕРЕВА — первые 2 поля!
    uint8_t  key[32];
    uint8_t  mask[32];
    // ... пользовательские поля ...
    int metric;
};

// --- Добавление маршрута ---
struct route_data *rd = u_calloc(sizeof(*rd), 1);
rd->key[0]  = 17;               // длина (1 + 16 байт IPv6)
memcpy(rd->key + 1, addr, 16);
rd->mask[0] = 17;
make_mask(rd->mask + 1, plen);
rn_addroute(rd->key, rd->mask, &rnh->rh, rd->rn_nodes);

// --- Longest Prefix Match ---
uint8_t search_key[17];
search_key[0] = 17;
memcpy(search_key + 1, dst_addr, 16);
struct radix_node *leaf = rn_match(search_key, &rnh->rh);
if (leaf && !(leaf->rn_flags & RNF_ROOT)) {
    // leaf = &rd->rn_nodes[0] — восстанавливаем указатель на данные:
    struct route_data *found = (struct route_data *)(
        (char *)leaf - offsetof(struct route_data, rn_nodes));
}

// --- Точный поиск (key + mask) ---
struct radix_node *node = rn_lookup(key, mask, &rnh->rh);

// --- Удаление ---
rn_delete(rd->key, rd->mask, &rnh->rh);

// --- Обход всех маршрутов ---
static int walk_cb(struct radix_node *rn, void *arg) {
    if (rn->rn_flags & RNF_ROOT) return 0;
    struct route_data *rd = (struct route_data *)((char *)rn - offsetof(...));
    // ... обработать rd ...
    return 0; // 0 = продолжить, !=0 = прервать
}
rn_walktree(&rnh->rh, walk_cb, NULL);

// --- Обход с нижней границы (mask-фильтр) ---
uint8_t base_key[...], base_mask[...];
rn_walktree_from(&rnh->rh, base_key, base_mask, walk_cb, NULL);

// --- Завершение ---
rn_detachhead((void**)&rnh);
```

**Ключевые нюансы:**
- `rn_nodes[2]` **должны быть первыми полями** в структуре данных — дерево использует их для хранения.
- `rn_addroute` возвращает `&rd->rn_nodes[0]` при успехе. Для извлечения структуры используйте `offsetof()`.
- `rn_match` возвращает самый специфичный (longest prefix) лист либо `NULL`. Если `RNF_ROOT` — совпадений нет.
- Максимальный размер ключа — 32 байта (включая байт длины).
- Все динамические выделения через `R_Malloc`/`R_Zalloc`/`R_Free` (макросы на `u_malloc`/`u_free`).
- Потокобезопасность не реализована — замки (макросы `RADIX_NODE_HEAD_LOCK` и т.д.) пустые.

## 3. API

| Функция | Назначение |
|---------|-----------|
| `rn_inithead(void **head, int off)` | Выделяет и инициализирует `radix_node_head`. `off` — смещение (байт длины sockaddr). |
| `rn_detachhead(void **head)` | Освобождает дерево и все узлы (через `rn_walktree` + `rn_delete`). |
| `rn_match(key, head)` | Longest Prefix Match. Возвращает лист-победитель или NULL. |
| `rn_lookup(key, mask, head)` | Точный поиск (key + mask). Возвращает узел или NULL. |
| `rn_addroute(key, mask, head, nodes[2])` | Добавляет маршрут. `nodes` — память из структуры данных. |
| `rn_delete(key, mask, head)` | Удаляет маршрут. Возвращает удалённый узел для освобождения. |
| `rn_walktree(head, callback, arg)` | Обход всего дерева (in-order), вызывает `callback(rn, arg)` для каждого листа. |
| `rn_walktree_from(head, base_key, base_mask, callback, arg)` | Обход поддерева, ограниченного `base_mask`. |
| `rn_refines(mask1, mask2)` | Проверяет, является ли `mask1` более специфичной, чем `mask2`. |
| `rn_nextprefix(rn)` | Переход к следующему dupedkey-узлу с тем же ключом. |

**Структуры:**
- `struct radix_node_head` — корень дерева с таблицей функций (`rnh_matchaddr`, `rnh_addaddr`, ...).
- `struct radix_node` — узел: внутренний (bit-offset, left/right) или лист (key, mask, dupedkey).
- `struct radix_mask` — аннотация маски для нетривиальных (non-normal) масок в поддеревьях.
