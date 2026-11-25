# ETCP Connection State Dump (etcp_dump)

## 1. Назначение
Снимок состояния ETCP-соединений и сокетов: параметры, очереди, пулы, normalizer,
физические попытки, ACK/RTO и метрики линков. Пишется в настроенные выходы системы
отладки на уровне DEBUG категории `etcp_dump`. Все функции вызываются в uasync-потоке instance.

## 2. Как пользоваться

```c
#include "etcp_dump.h"

debug_set_category_level(DEBUG_CATEGORY_ETCP_DUMP, DEBUG_LEVEL_DEBUG);

// Дамп одного соединения:
etcp_dump_conn_state(conn);

// Дамп всех соединений инстанса:
etcp_dump_all_conns(instance);

// Сокеты либо весь снимок:
etcp_dump_sockets(instance);
etcp_dump_all(instance);
```

Для INI используется секция `[debug]` со строкой `etcp_dump=debug`.
Дамп не меняет состояние и не приобретает владение объектами.

## 3. API

**`void etcp_dump_conn_state(struct ETCP_CONN* conn)`** — параметры одного conn,
логические очереди и normalizer, статистика пулов и список линков. У каждого линка
выводятся его attempts/ACK, DATA RTT/jitter, RTO, окно/pacing, keepalive, ошибки,
наблюдаемый локальный endpoint и burst. Таймер повторов находится у каждого линка.

**`void etcp_dump_all_conns(struct UTUN_INSTANCE* instance)`** — обход реестра
instance->connections, включая pending-соединения; вызывает conn_state для каждой записи.

**`void etcp_dump_sockets(struct UTUN_INSTANCE* instance)`** — сокеты, профили
mapping/inbound, MTU, число UDP-линков и сводные счётчики TCP.

**`void etcp_dump_all(struct UTUN_INSTANCE* instance)`** — последовательно sockets и all_conns.

### Зависимости
- `etcp.h` — `struct ETCP_CONN`, поля состояния, очереди, normalizer
- `etcp_connections.h` — `struct ETCP_LINK`
- `pkt_normalizer.h` — `struct PKTNORM`
- `lib/ll_queue.h` — `queue_entry_count()`, `queue_total_bytes()`
- `lib/memory_pool.h` — `memory_pool_get_stats()`
- `lib/socket_compat.h` — `AF_INET`, `sockaddr_in`, `ntohs`
- `utun_instance.h` — `struct UTUN_INSTANCE`
