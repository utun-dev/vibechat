# ETCP API

## 1. Назначение

Модуль предоставляет публичный API для отправки и приёма пакетов через ETCP-соединения. Это входная точка для всех вышележащих подсистем (роутер, маршрутизация, NAT, TCP-прокси, транспорт сообщений).

**Ключевая идея:** приёмный коллбэк `etcp_int_recv` диспетчеризует входящие пакеты по первому байту кодограммы (`cmd`). Любой модуль может зарегистрировать обработчик на свой `cmd` через `etcp_bind` и слать данные через `etcp_send`.

Формат кодограммы (сырой пакет внутри ETCP): `<cmd 1 байт> <данные N байт>`.

Все вызовы выполняются в uasync-потоке instance. Для отправки нужен UP-транспорт и свободная
`send_input_q`; владелец прямого соединения удерживает NCD handle. Принятие кодограммы
в очередь и транспортный ACK не подтверждают завершение операции сервиса: REINIT
отбрасывает прежний поток, и сервис завершает или повторяет свои операции.

## 2. Как пользоваться

### Типовой сценарий: модуль хочет принимать пакеты своего типа

```c
// Регистрируем коллбэк при инициализации модуля
etcp_bind(instance, ETCP_ID_DATA, my_data_recv_callback);

// Коллбэк вызывается для каждого входящего пакета с cmd=ETCP_ID_DATA
void my_data_recv_callback(struct ETCP_CONN* conn, struct ll_entry* entry) {
    // entry->dgram[0] = cmd (уже совпал с ETCP_ID_DATA)
    // entry->dgram[1..len-1] = данные
    process_data(conn, entry->dgram + 1, entry->len - 1);
    queue_dgram_free(entry);
    queue_entry_free(entry);
}
```

### Отправка пакета

```c
struct ll_entry* e = ll_alloc_lldgram(1 + payload_len);
if (!e) {
    DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Cannot allocate service datagram");
    return -1;
}
e->dgram[0] = my_cmd;
memcpy(e->dgram + 1, payload, payload_len);
e->len = 1 + payload_len;
if (etcp_send(conn, e) < 0) {
    queue_dgram_free(e); queue_entry_free(e);
    return -1;
}
// При успехе e уже может быть обработан и освобождён синхронно: больше его не читаем.
```

`ll_alloc_lldgram` выделяет и entry, и dgram. При ошибке `etcp_send` оба блока
остаются вызывающему; при успехе владение передаётся normalizer.

### Коллбэки состояния соединения

```c
void on_conn_event(struct ETCP_CONN* conn, int event, void* arg);
void on_new_conn(struct ETCP_CONN* conn, void* arg);

etcp_conn_add_cbk(conn, on_conn_event, my_data,
    ETCP_CBK_EVENT_INIT | ETCP_CBK_EVENT_UP | ETCP_CBK_EVENT_DOWN |
    ETCP_CBK_EVENT_REINIT | ETCP_CBK_EVENT_DELETE);
etcp_add_new_conn_cbk(instance, on_new_conn, my_data);

// Снять именно свою подписку; fn и arg идентифицируют запись.
etcp_conn_remove_cbk(conn, on_conn_event, my_data);
etcp_remove_new_conn_cbk(instance, on_new_conn, my_data);
```

`INIT` означает первую готовность потока, `UP/DOWN` — доступность транспорта,
`REINIT` — подтверждённую новую сессию. `DELETE` — терминальное событие, единственное
допустимое при `state==2`. Callback может закрыть соединение или снять подписку;
новые подписки не участвуют в уже начатом обходе.

Instance callback создания вызывается для входящих и исходящих объектов, ещё до
handshake: peer_node_id может быть неизвестен. Он не означает UP. Для общей
диагностики доступны `etcp_add_conn_status_cbk` и `etcp_add_link_status_cbk`.

### Сервисные ID

Полный реестр с назначением каждого ID находится в [etcp_api.h](etcp_api.h).
`ETCP_ID_*` используются для прямых кодограмм через `etcp_bind` / `etcp_send`;
`ETCP_RT_ID_*` — для сервисов через `etcp_router_bind` / `etcp_route_send`.
Роутер оборачивает сервисные данные в прямую кодограмму `ETCP_ID_SVC_ROUTE`.
Значения уникальны внутри каждой таблицы; совпадения между двумя таблицами допустимы.
Проверка реестра и регистраций выполняется автоматически при `make check`.

### Прямые соединения

Владелец открывает handle через `node_conn_direct_open/open_node`, ждёт `NCD_EVENT_UP`
и получает транспорт через `node_conn_direct_get_conn`. Handle удерживается на всё
время использования; его закрытие не отменяет владение других сервисов. Контракт — в `node_conn_direct.h`.

## 3. API

### Структуры

| Структура | Назначение |
|-----------|-----------|
| `ETCP_BINDINGS` | Per-instance массив `callbacks[256]` |

### Типы коллбэков

| Тип | Сигнатура | Назначение |
|-----|-----------|-----------|
| `etcp_recv_fn` | `void(conn, entry)` | Приём пакета — **обязан** освободить entry/dgram |
| `etcp_cbk_fn` | `void(conn, event, arg)` | События соединения по маске `ETCP_CBK_EVENT_*` |
| `etcp_inst_cbk_fn` | `void(conn, arg)` | Создание объекта соединения |
| `etcp_conn_status_fn` | `void(conn, status, arg)` | Статусы `ETCP_CONN_STATUS_*` на instance |
| `etcp_link_status_cbk_fn` | `void(conn, link, old_state, old_status, arg)` | Изменения физического линка |

### Функции

| Функция | Назначение |
|---------|-----------|
| `etcp_bind(inst, id, fn)` | Подписаться на пакеты с cmd=`id` |
| `etcp_unbind(inst, id)` | Отписаться |
| `etcp_send(conn, entry)` | Отправить пакет; владение entry/dgram передаётся только при результате 0 |
| `etcp_int_recv(queue, arg)` | **Внутренняя:** диспетчер из очереди normalizer'а, вызывает bound-коллбэк по cmd |
| `etcp_conn_add_cbk(conn, fn, arg, mask)` | Подписка на выбранные события соединения |
| `etcp_conn_remove_cbk(conn, fn, arg)` | Снять свою подписку |
| `etcp_conn_update_cbk_mask(conn, fn, arg, mask)` | Изменить маску существующей подписки |
| `etcp_add_new_conn_cbk/remove_new_conn_cbk` | Добавить/убрать из цепочки new_conn |
| `etcp_add_conn_status_cbk/remove_conn_status_cbk` | Добавить/снять instance-подписку на статусы соединений |
| `etcp_add_link_status_cbk/remove_link_status_cbk` | Добавить/снять instance-подписку на состояния линков |

### Командные ID (сырые, первый байт кодограммы)

| Константа | Значение | Назначение |
|-----------|----------|-----------|
| `ETCP_ID_DATA` | 0x00 | Данные для передачи адресату |
| `ETCP_ID_SVC_ROUTE` | 0x03 | Транспорт роутера (etcp_router) |
