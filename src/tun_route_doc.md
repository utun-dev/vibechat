# TUN Route (tun_route)

## 1. Назначение
Модуль управления маршрутами в системной таблице маршрутизации. Добавляет маршруты к заданным подсетям через TUN-интерфейс, чтобы исходящий трафик на эти подсети направлялся в виртуальный интерфейс uTun. Удаляет их при завершении работы. Кроссплатформенный: использует разные механизмы ОС с fallback-стратегией.

## 2. Как пользоваться
```c
// Добавить один маршрут
tun_route_add(ifindex, "tun0", 0x0A000000, 8);   // 10.0.0.0/8

// Добавить все маршруты из конфига
struct tun_route* installed = NULL;
int result = tun_route_add_all(ifindex, "tun0", cfg.routes, &installed);

// Удалить все маршруты из конфига
tun_route_del_all(ifindex, "tun0", &installed); // также при result < 0

// Удалить все маршруты через интерфейс (очистка)
tun_route_flush("tun0");
```

**Network передаётся в host byte order** (внутри конвертируется в network byte order).
`tun_route_add_all` читает `CFG_ROUTE_ENTRY`, выделяет запись владения до операции
и сохраняет только успешно добавленные маршруты. При ошибке возвращает `-1`,
оставляя созданный список для отката. `tun_route_del_all` удаляет и освобождает
этот список независимо от дальнейших изменений конфига. IPv6 пока не реализован:
его присутствие в `route_subnet` возвращает ошибку, а не молча пропускается.

**Root-права:** требуются для изменения системной таблицы маршрутизации.

## 3. API

### Функции
- **`tun_route_add(ifindex, ifname, network, prefix_len)`** — добавляет маршрут к подсети `network/prefix_len` через интерфейс `ifindex`. Пытается использовать нативный механизм платформы, при неудаче — fallback на shell-команду
- **`tun_route_del(ifindex, ifname, network, prefix_len)`** — удаляет аналогичный маршрут
- **`tun_route_add_all(ifindex, ifname, routes, installed)`** — `0`, если установлены все маршруты, `-1` при первой ошибке; `installed` изначально пуст
- **`tun_route_del_all(ifindex, ifname, installed)`** — удаляет принадлежащие сервису маршруты; `0` при полном успехе, `-1` при любой ошибке; обнуляет список
- **`tun_route_flush(ifname)`** — удаление всех маршрутов, привязанных к интерфейсу `ifname`, без привязки к конфигу

### Платформенные механизмы

**Linux:**
1. Netlink socket (`AF_NETLINK`, `NETLINK_ROUTE`) на одну операцию с автоматически назначенным port ID, без подписок. Работает независимо от уже открытых сокетов мониторинга.
2. `RTM_NEWROUTE`/`RTM_DELROUTE` с `RTA_DST` и `RTA_OIF`; успех только после корректного ACK ядра с совпавшим sequence и `error=0`. Общий срок ожидания — 1с, включая EINTR и посторонние ответы. Add/delete не используют shell-fallback.
3. `tun_route_flush`: отдельный API `ip route flush dev <ifname>`; UTUN stop/rollback используют список владения, а не flush.

**Windows:**
1. `netsh interface ip add/delete route` через `system()` (первичный метод)
2. Fallback: IP Helper API — `CreateIpForwardEntry`/`DeleteIpForwardEntry` с заполнением `MIB_IPFORWARDROW`
3. `tun_route_flush`: `GetIpForwardTable` → перебор записей по `dwForwardIfIndex` → `DeleteIpForwardEntry`

**BSD/macOS:**
1. Routing socket (`PF_ROUTE`, `SOCK_RAW`): сообщения `RTM_ADD`/`RTM_DELETE`, адреса RTA_DST + RTA_NETMASK + RTA_IFP (sockaddr_dl)
2. Fallback: `route add/delete -net <net>/<prefix> -interface <ifname>` через `system()`
3. `tun_route_flush`: `route -n show -interface <ifname>` → парсинг awk → `route delete`
