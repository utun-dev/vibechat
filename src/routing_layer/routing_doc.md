# routing – централизованный модуль маршрутизации

## 1. Назначение
Связывает TUN-интерфейс и ETCP-транспорт в единый маршрутизатор. Принимает IP-пакеты из TUN, по IP-адресу назначения ищет маршрут в таблице (`route_lib`) и отправляет пакет через ETCP нужному узлу. В обратную сторону принимает пакеты из ETCP (callback `routing_pkt_from_etcp_cb`) и отдаёт в TUN локально.

IPv6-пакеты на данный момент дропаются на этапе `extract_dst_ip` (возвращает 0).

Модуль НЕ реализует таблицу маршрутизации сам — он пользуется `route_lib` (`instance->rt`) и BGP-обменом (`topo_node` / `topo_group`) для наполнения таблицы.

## 2. Как пользоваться
**Порядок инициализации в `utun_instance.c`:**
1. `routing_create()` — создаёт `ROUTE_TABLE` в `instance->rt`
2. `routing_bind()` — биндит коллбек `routing_pkt_from_etcp_cb` через `etcp_router_bind(ETCP_RT_ID_DATA)` — принимать пакеты из ETCP
3. `routing_set_tun()` — вешает коллбек `routing_pkt_from_tun_cb` на `tun->output_queue` — забирать пакеты из TUN

**Основной поток данных:**
- **TUN → сеть:** пакет попадает в `tun->output_queue` → `routing_pkt_from_tun_cb` → `route_pkt` → `etcp_route_send_key`
- **сеть → TUN:** пакет приходит через ETCP → `etcp_router` вызывает `routing_pkt_from_etcp_cb` → `route_pkt` → `queue_data_put(tun->input_queue)`

**Ключевые нюансы:**
- `routing_add_conn` / `routing_del_conn` — **deprecated** (no-op), маршруты теперь управляются через BGP и таблицу `route_lib`
- Все не-IPv4 пакеты (IPv6, unknown) дропаются в `extract_dst_ip`
- Multicast (224.0.0.0/4) и limited broadcast (255.255.255.255) отбрасываются с DEBUG-логом routing
- Пакеты без маршрута дропаются с инкрементом `instance->dropped_packets`
- `route_pkt` забирает entry и dgram при любом исходе; ожидает `[cmd:1][IPv4]` и готовые `rt`/`tun`.
- Удалённая доставка идёт через `TOPO_GROUP_UTUN`; локальная — в `tun->input_queue`.
  Firewall проверяется перед маршрутизацией выхода из TUN и перед локальной доставкой в TUN.
- Все вызовы выполняются в потоке ядра. `routing_destroy` снимает DATA binding и освобождает таблицу;
  callback очереди TUN снимается при остановке самого TUN/UTUN-сервиса.

## 3. API

| Функция | Описание |
|---------|----------|
| `routing_create(instance)` | Создаёт `ROUTE_TABLE` в `instance->rt`. -1 при ошибке |
| `routing_bind(instance)` | Биндит обработчик входящих ETCP-пакетов через `etcp_router_bind` |
| `routing_destroy(instance)` | Отвязывает ETCP-обработчик, уничтожает `ROUTE_TABLE` |
| `routing_set_tun(instance)` | Вешает callback на `tun->output_queue` для перехвата пакетов из TUN |
| `routing_add_conn(etcp)` | **Deprecated**, ничего не делает |
| `routing_del_conn(etcp)` | **Deprecated**, ничего не делает |
| `route_pkt(instance, entry, src_ed)` | Ядро маршрутизации: извлекает dst IP → `route_lookup` → `etcp_route_send_key` или `queue_data_put(tun->input_queue)` для локального |

### Внутренние функции

| Функция | Описание |
|---------|----------|
| `extract_dst_ip(data, len)` | Извлекает dst IPv4 из заголовка пакета (offset 16, 4 байта). Возвращает 0 для не-IPv4 |
| `routing_pkt_from_etcp_cb(conn, pkt)` | Callback: приём пакета из ETCP-роутера, вызывает `route_pkt` |
| `routing_pkt_from_tun_cb(q, arg)` | Callback: приём пакета из TUN output_queue, вызывает `route_pkt` |
