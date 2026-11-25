# lwip_pbuf

## 1. Назначение
Упрощённая реализация pbuf (packet buffer) для встроенного lwIP TCP стека — только PBUF_RAM.

В отличие от оригинального lwIP (где pbuf может быть RAM/ROM/Pool/Queue), здесь каждый pbuf — это единый блок `u_malloc`, содержащий: `struct pbuf` + зарезервированное место под заголовки (layer-зависимый offset) + полезные данные (payload). Поддерживаются цепочки pbuf через `next`, ref-counted управление памятью, сдвиг payload pointer через `pbuf_header`, цепочечное копирование данных.

Используется модулями `lwip_tcp`, `tcp_proxy_server`, `tcp_proxy_client` для формирования и разбора TCP-сегментов.

## 2. Как пользоваться

Типовой сценарий:
```c
// Создать pbuf с запасом под заголовки
struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, data_len);       // offset = 54 байта
// Записать заголовки (сдвиг payload назад в зарезервированную область)
pbuf_header(p, -54);                                         // payload теперь в начале блока
memcpy(p->payload, &tcp_header, 54);
pbuf_header(p, 54);                                          // вернуть payload на данные
// Заполнить данные
memcpy(p->payload, user_data, data_len);
// Прикрепить ещё сегментов
struct pbuf *tail = pbuf_alloc(PBUF_RAW, more_data_len);
memcpy(tail->payload, more_data, more_data_len);
pbuf_chain(p, tail);                                         // p->tot_len = data_len + more_data_len
// Отправить… (потом освободить)
pbuf_free(p);                                                // уменьшает ref, освобождает при ref==0
```

Ключевые концепции:
- **PBUF_RAM-only**: один `u_malloc` на pbuf (структура + зарезервированный offset + данные). Нет пулов, нет ROM-буферов.
- **Ref-counted**: `pbuf_ref()` +1, `pbuf_free()` -1, освобождение при 0. Каждое звено владеет ссылкой на следующее. Пока остаётся ссылка на звено, сохраняется весь его хвост.
- **pbuf_header(increment)**: сдвиг `payload`-указателя. Положительный increment — сужение данных (потребление заголовка). Отрицательный — расширение в зарезервированную область (создание заголовка). Проверяет границы через доступный offset (разницу между `payload` и `(uint8_t*)p + sizeof(struct pbuf)`).
- **pbuf_header_force()**: то же, но без проверки границ при положительном increment (форсированное переопределение границ).
- **Цепочка (chain)**: связный список через `p->next`. `tot_len` — суммарная длина всей цепочки, `len` — длина текущего сегмента. `pbuf_chain()` добавляет в конец, `pbuf_dechain()` отрезает первый от остальных и корректирует `tot_len`.
- **pbuf_copy_partial()/pbuf_take_at()**: работают с цепочкой прозрачно — пропускают сегменты по смещению, копируют чанками.

## 3. API

### Структуры

| Структура | Описание |
|-----------|----------|
| `struct pbuf` | Элемент пакетного буфера: `next` (цепочка), `payload` (указатель на данные), `len` (длина сегмента), `tot_len` (суммарная длина цепочки), `ref` (счётчик ссылок), `flags`, `type_internal` |
| `PBUF_FLAG_TCP_FIN` | Флаг (0x01) — маркер FIN-сегмента TCP в поле `flags` |

### Перечисления

| Enum | Значения | Описание |
|------|----------|----------|
| `enum pbuf_layer` | `PBUF_TRANSPORT=54`, `PBUF_IP=34`, `PBUF_LINK=14`, `PBUF_RAW_TX=54`, `PBUF_RAW=0` | Зарезервированный offset под заголовки перед payload |

### Функции

| Функция | Описание |
|---------|----------|
| `pbuf_alloc(layer, length)` | Выделить pbuf с зарезервированным offset (layer) и данными длины length. Возвращает NULL при ошибке. |
| `pbuf_free(p)` | Снять ссылку. При ref==0 освободить звено и продолжить со следующим. |
| `pbuf_ref(p)` | Инкремент счётчика ссылок. |
| `pbuf_header(p, increment)` | Сдвинуть `payload` на increment байт. +increment = освободить место под заголовок, -increment = добавить заголовок (негативный сдвиг). Проверяет границы выделенной памяти. Возвращает 0 / -1. |
| `pbuf_header_force(p, increment)` | То же, но без проверки границ при положительном increment (форсирование). |
| `pbuf_truncate(p, length)` | Сохраняет голову и первые length байт, обновляет tot_len у оставшихся звеньев и освобождает ссылку на отрезанный хвост. |
| `pbuf_chain(head, tail)` | Присоединить цепочку tail в конец head (head->tot_len не обновляется автоматически — caller делает это сам). |
| `pbuf_dechain(p)` | Отсоединить первый pbuf от цепочки: обнулить `next`, скорректировать `tot_len = len`. Возвращает остаток цепочки. |
| `pbuf_clen(p)` | Количество сегментов в цепочке. |
| `pbuf_copy_partial(p, buf, len, offset)` | Скопировать до len байт из цепочки p начиная со смещения offset в линейный буфер buf. Возвращает реально скопированное количество. |
| `pbuf_take(p, data, len)` | Записать len байт из data в цепочку p (начиная с offset 0). Обёртка над `pbuf_take_at`. |
| `pbuf_take_at(p, data, len, offset)` | Записать len байт из data в цепочку p начиная со смещения offset. Возвращает 0 при успехе, -1 если данных недостаточно. |
| `pbuf_get_contiguous(p, buf, len, offset)` | Скопировать len байт из цепочки в buf (как `pbuf_copy_partial`, но возвращает buf при успехе или NULL). |

### Зависимости
- `../../lib/mem.h` — `u_malloc`/`u_free` (wrappers с leak tracking)
- `string.h` — `memcpy`
