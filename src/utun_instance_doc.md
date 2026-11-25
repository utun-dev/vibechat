# utun_instance — ядро одного узла и независимые сервисы

Полностью сверены `utun_instance.c/.h` и этот документ, 26.10.2026.
Связанный документ [service_lifecycle.md](../doc/service_lifecycle.md) описывает также GUI,
Android и media workers; аудит instance не означает завершения всех его разделов.

## Использование и поток

`UTUN_INSTANCE` принадлежит одному узлу. Несколько экземпляров могут разделять
переданный `UASYNC`; он принадлежит вызывающему и должен жить до конца destroy.
Все операции выполняются в его потоке. Stop сервиса, reload и destroy выполняются
вне callbacks останавливаемых модулей. Межпоточные команды передаются в uasync;
прямые конкурентные обращения к instance недопустимы.

```c
struct UASYNC* ua = uasync_create();
if (!ua) return -1;
struct UTUN_INSTANCE* inst = utun_instance_create(ua, "utun.cfg");
if (!inst || utun_core_start(inst) < 0 || chat_service_start(inst) < 0) {
    utun_instance_destroy(inst);
    uasync_destroy(ua, 0);
    return -1;
}
while (inst->running) uasync_poll(ua, 100);
utun_instance_destroy(inst);
uasync_destroy(ua, 0);
```

Цикл запускает приложение; функции `utun_instance_run` нет.
`utun_instance_stop` только снимает `running` и вызывает wakeup. Это не stop сервисов.
Повторный core_start при уже установленном core_started не поднимает running после stop.
Решение о продолжении poll принимает владелец UASYNC, в том числе при нескольких instance.

## Конструкторы

| API | Действие и владение |
|---|---|
| `create(ua, path)` | Проверяет аргументы, запускает config updater, разбирает файл и создаёт ядро. Updater может создать/нормализовать файл, ключи и node_id |
| `create_from_config(ua, config)` | Использует готовую модель без updater. При успехе забирает config; при ошибке caller продолжает владеть им |
| `create_from_str(ua, text)` | Копирует ASCIIZ текст в mkstemp `/tmp/utun_cfg_XXXXXX`, вызывает файловый create и удаляет temp. Требует доступной ФС; текст не сохраняется |

Все возвращают NULL при ошибке и освобождают частично созданные ресурсы.
При `create_from_config` ключи должны быть заданы корректно; производный node_id
проверяется и может исправляться в модели даже при последующей ошибке.
Уровни debug меняют logger всего процесса, включая неудачный create.
`log_file` выбирает путь, только если файловый logger ещё не открыт; открытый сохраняет
первый путь. Выбранная строка копируется в постоянный буфер, чтобы пережить config.

Create уже выводит идентичность X25519/Ed25519, создаёт индекс сетей и соединений,
пулы, routing, обязательные UDP/TCP listeners, топологию/SQLite, мониторинг и
automatic sockets, firewall, router/multicast, NAT detection и настроенный proxy.
Это I/O с возможными ошибками bind, каталогов, БД и ресурсов; создание ядра
не ограничивается выделением структуры.

При включённой топологии SQLite открывается один раз: `db_path/chats.db`, без пути —
`:memory:`. Каталог подготавливается рекурсивно. Core/UTUN/chat ещё не запущены:
их флаги и `running` равны нулю. Неуспешный обязательный listen или proxy отклоняет
create. Недоступность ICMP raw socket по правам/поддержке отключает только ICMP exit с WARN.

## Запуск и остановка

| API | Ресурсы и результат |
|---|---|
| `utun_core_start` | db_sync, NCD/keepalive, постоянные транспорты `[client]`, control, время, broadcast/radio и STUN. Первый успех задаёт `core_started=running=1` |
| `utun_service_start` | Требует core и топологию. Создаёт UTUN-группу, DATA binding, опциональные TUN/маршруты/NAT и отдельные групповые запросы к `[client]` |
| `chat_service_start` | Объявлен в chat_core.h. Требует core и SQLite без чужой транзакции. Поднимает CHAT-синхронизацию, stream/media/DM/member_files/call/radio-call, headless и LAN |
| `utun_instance_init` | Последовательно core, затем UTUN при наличии топологии, затем чат при `chatserver_enabled` |

Start возвращает 0 при успехе или повторном запуске, -1 при ошибке. Готовность
локальных ресурсов не гарантирует UP транспорта или READY группы.
Ошибка старта UTUN/чата вызывает stop этого сервиса. Core start оставляет уже
подготовленные ресурсы для retry/destroy; NTP и STUN нефатальны.
`init` не откатывает ранее запущенные сервисы при отказе последующего.
chat_core заимствует SQLite ядра;схема/идентичность/media_base фиксируются одной
транзакцией после установки обязательных подписок. Активация сохранённых каналов
идёт после commit;её отказ откатывает runtime сервиса и допускает повтор старта.
DB_READY относится к готовности chat_core,не всего chat_service_start.

UTUN stop снимает DATA binding, TUN/NAT/маршруты, свои групповые запросы и UTUN-группу.
Постоянные `[client]`-транспорты ядра и чат сохраняются.
Chat stop прекращает LAN/headless/stream и фоновые задачи, дожидается workers,
затем закрывает медиа/DM/синхронизацию/CHAT-группы и chat core. SQLite остаётся;
следующий start загружает сохранённые каналы. Stop обоих сервисов идемпотентен.

Три headless bind-поля принимают IPv4 literal и десятичный порт 0..65535.
Пустая строка отключает listener; 0 выбирает порт ОС, доступный через getter.
Плохой bind или ошибка listener логируется и отключает только его; чат остаётся запущенным.
Call/radio stop отменяет таймеры, снимает клиентов с UASYNC, освобождает кодеки и контекст.
После 11-байтового wire-заголовка PCM копируется в выровненный int16_t буфер перед Opus.

Глобальный переключатель `set_tun_init_enabled` по умолчанию выключен; влияет
на последующий UTUN start и proxy test mode при create. Дополнительно нужен
`config.global.tun_enabled`. `set_topo_group_enabled` по умолчанию включён;
влияет на последующие create, включая общую БД. Это общие настройки процесса,
а не операции включения/выключения ресурсов существующего instance.

## Reload

Всегда сохранять возвращённый указатель:

```c
inst = utun_instance_reload(inst, ua, "utun.cfg");
if (!inst) { /* прежнее ядро уже уничтожено; обработать ошибку */ }
```

Нужны существующий instance, его же UASYNC и непустой путь. Сначала переоткрывается
лог, updater проверяет файл, затем готовятся новая модель и firewall.
Ошибка подготовки возвращает прежний instance; updater и logger могли уже изменить
файл/глобальные настройки, поэтому общей транзакции с ними нет.

Полный restart выбирается при изменении негорячих global-настроек, списков
my_subnet/route_subnet, а также server-конфига в automatic режиме.
Сюда входят идентичность, имя, db_path, TUN/NAT/proxy, control, keepalive и режим сокетов.
Горячими global-полями считаются firewall, уровни debug и STUN.
Списки сравниваются по содержимому; различия порядка и padding модели могут
консервативно выбрать restart. Повторяющиеся server-имена также выбирают полный путь,
поскольку parser сохраняет такие секции отдельно и поимённая замена неоднозначна.

Полный путь уничтожает старое ядро, создаёт новое из уже разобранной модели
и восстанавливает прежние `core_started/utun_started/chat_started/running`.
Изменение config-флага само по себе не включает ранее остановленный сервис.
Ошибки создания/восстановления после destroy возвращают NULL. In-memory SQLite
теряет данные, дисковая БД сохраняется.

Частичный путь заранее готовит отдельный индекс сетей. В ручном режиме endpoints
сопоставляются по имени `[server:name]` и всем параметрам: адрес/порт/IPv6 scope,
UDP/TCP, интерфейс, mark/FIB, MTU, type, REALITY/SOCKS, IPv6 mode и only_local.
Неизменённый endpoint сохраняет socket ID, generation и выбранный ОС порт при port=0.
IPv6 temporary/permanent разрешается при создании и меняет адрес в модели config;
повторный reload такого селектора может консервативно пересоздать endpoint.
Удаление TCP закрывает listener и TCP-модель, UDP использует собственный teardown.
Probe и endpoints других владельцев не удаляются только из-за отсутствия в server-конфиге.

Новые endpoints создаёт общая UDP/TCP factory. При изменении servers/clients
перестраиваются постоянные `[client]`; неизменённый список сохраняет handles/таймеры.
Копии клиентских параметров не ссылаются на прежний config.
Далее заменяются config, networks и firewall, перепубликуются изменившиеся адреса
и перезапускается STUN при работающем core.

Это не атомарная замена sockets: отказ нового endpoint сохраняет новый config
и отсутствие endpoint для следующей попытки reload. Ошибка подготовки клиентов
сохраняет их прежние самостоятельные handles. Ошибки выводятся в лог;
`socket_init_status=-1` означает отказ хотя бы одного endpoint, 0 — успех.

## Подписки активности и сна пира

Activity и peer-sleep подписки принадлежат instance; fn/arg заимствованы.
Пара fn+arg уникальна; повторный add ничего не меняет. Новые записи идут в голову,
события доставляются синхронно в обратном порядке добавления.
NULL instance/fn и remove отсутствующей записи — no-op; OOM add логируется.

В callback разрешено удалить себя/соседа и добавить подписку.
Удалённые записи немедленно пропускаются, память удерживается до конца внешней
рассылки обоих видов событий. Новая подписка не участвует в текущем обходе,
но может получить вложенное событие. Вложенный peer-sleep исполняется до продолжения
внешнего; новая эффективная activity прекращает остаток старой activity-рассылки.
Reload/destroy внутри таких callbacks запрещён.

`utun_set_client_activity` нормализует requested mode, учитывает stream hold,
применяет Android standby при соответствующей сборке и затем рассылает изменение.
Повтор согласует режим без повторного события; ошибка standby откатывает состояние.
SERVER возвращает 0 без изменения; phone_state этим API не меняется.

## Уничтожение и диагностика

Destroy снимает running/core_started и запрещает повторный вход, останавливает
STUN/NAT detection/padding, NTP/control/proxy, чат/UTUN и core-client handles,
затем мониторинг/automatic sockets и топологию/broadcast/radio/multicast.
Далее закрывает endpoints с учётом UDP/TCP, db_sync, STCP/REALITY, ETCP и pending pings,
routing/media/router/firewall, сети, config, пулы и оставшиеся подписки.
Списки сокетов и соединений обходятся заново после закрывающих callbacks.
Deferred callbacks выкачиваются, пока UASYNC жив.

Общая SQLite закрывается последней. Caller обязан завершить заимствованные
statements/backups до destroy. BUSY логируется, close_v2 откладывает физическое
освобождение БД до закрытия оставшихся SQL-объектов; instance после возврата уже недействителен.
Ни logger процесса, ни UASYNC destroy ядра не уничтожает.

`utun_instance_diagnose_leaks` печатает sys/info счётчики и простые sanity-проверки
списков. Надпись POTENTIAL LEAK о живом ресурсе до cleanup не доказывает утечку;
проверка произвольных повреждённых указателей не гарантируется.
`instance_find_conn` ищет node_id в основном ETCP и старом TCP-реестре, возвращает
заимствованный указатель либо NULL, включая NULL instance; готовность UP/READY не обещает.

Основные поля: `name[MAX_CONN_NAME_LEN]` (64), собственный config, node_id/ключи,
заимствованный ua, три *_started и running, topo_groups/SQLite, conn_mgr (у UTUN-группы),
etcp_sockets (UDP/TCP), stcp_servers (список), config_conn_handles, pools и networks.
Системные маршруты принадлежат отдельному `tun_routes`, а не списку config.
Подробные поля/типы описаны в заголовке.

Проверки и платформенные ограничения фиксируются в
[utun_instance_audit_findings.txt](../doc/utun_instance_audit_findings.txt).
