# ETCP Packet Section Dump (etcp_debug)

## 1. Назначение
Вывод в лог человекочитаемой сводки по ETCP-пакету: направление (SEND/RECV), timestamp, и содержимое каждой секции — ACK (last_delivered, count, rx_dup), CH_TS (возвращаемый и принимаемый channel timestamp), PAYLOAD (размер и seq). Используется для отладки трафика на уровне отдельных пакетов.

## 2. Как пользоваться

```c
#include "etcp_debug.h"

// В обработчике отправки/приёма пакета:
etcp_dump_pkt_sections(pkt, link, 1);  // is_send=1 — отправка
etcp_dump_pkt_sections(pkt, link, 0);  // is_send=0 — приём
```

Вывод идёт через `DEBUG_DEBUG(DEBUG_CATEGORY_DUMP, ...)`, сборка строки также
пропускается при выключенном фильтре. Настройка INI:
```ini
[debug]
dump=debug
```

### Вспомогательная функция
```c
IP_STR ip = ip_to_string(0x0A000001);
// ip.a == "10.0.0.1"
```

## 3. API

**`IP_STR ip_to_string(uint32_t ip)`** — печатает байты uint32_t в порядке `[3],[2],[1],[0]`.
На используемых little-endian платформах это числовой IPv4 в host order:
`0x0A000001 → 10.0.0.1`. Значение `sin_addr.s_addr` сначала преобразуют через ntohl.
Результат — структура по значению с буфером 16 байт.

**`void etcp_dump_pkt_sections(struct ETCP_DGRAM* pkt, struct ETCP_LINK* link, int is_send)`** —
читает секции из pkt->data без внешней SESSION_DATA-обёртки и выводит ACK
(last_delivered, count, rx_dup_count, точку приёма bytes/time и общий ACKtime), TIMESTAMP,
ACK_REQUEST и PAYLOAD (размер, seq, attempt_no). Направление: is_send!=0 — SEND, иначе RECV.
Пакет не изменяется; это диагностика после снятия защиты, а не проверка подлинности.
Длинная строка безопасно усекается, неизвестная секция завершает разбор. Пример:
```
ETCP PKT: SEND ts=12345 ack=100/0 dup=0 ch_ts=200/150 payload=100(seq=101 attempt=1)
```

### Зависимости
- `etcp_connections.h` — `struct ETCP_DGRAM`, `struct ETCP_LINK`
- `etcp.h` — константы `ETCP_SECTION_*`
- `lib/debug_config.h` — `DEBUG_DEBUG(DEBUG_CATEGORY_DUMP, ...)`
