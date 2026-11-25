# TCP proxy client

`tcp_proxy_client.c/h` объединяет TUN/lwIP, SOCKS5 CONNECT и HTTP forward proxy/CONNECT,
передавая потоки к общему exit `via_ed`. Необязательная настройка `group=utun` в `[tcp_proxy_client]` принимает единственное значение `utun`. Форматы и общие правила —
[doc/proxy_protocol.md](../../doc/proxy_protocol.md), контракт API и полей —
[tcp_proxy_client.h](tcp_proxy_client.h), локальные listeners —
[socks_proxy_doc.md](socks_proxy_doc.md).

## Создание и владение

`tcp_proxy_client_create` вызывается в uasync с живым `ua`. Для TUN нужны **оба**
непустых `tun_name` и `tun_ip`; задание только одного — ошибка. `tun_init_nat` создаёт
отдельный proxy TUN (test_mode выбирает тестовый интерфейс); MTU<=0 заменяется на 1500,
MTU TUN передаётся в lwIP. Имя tun_init_nat не означает включение eim_nat на ingress.
Без TUN необходим хотя бы флаг SOCKS или HTTP; эти listeners могут работать без lwIP.

`inst/ua/mappings` заимствуются до destroy; результат принадлежит вызывающему коду.
Создание само не заполняет `inst->tcp_proxy_client`: это делает владелец, например
`utun_instance`. Для автономного TUN smoke test допускается inst=NULL, но для
обмена через router нужен instance. Mappings содержат порты в host byte order,
адрес назначения — IPv4-строку.

Все заданные SOCKS/HTTP listeners, TUN mappings, TUN/lwIP/пулы и TCP router binding
обязательны. Ошибка любого шага возвращает NULL и освобождает уже созданные ресурсы.
Повторное создание клиента или занятой binding отклоняются. UDP/ICMP создаёт владелец
instance после успешного создания TCP-модулей, если есть TUN либо включён exit.

Клиент привязывается к `ETCP_RT_ID_TCP_PROXY_CLIENT` (0x08), сервер — к
`ETCP_RT_ID_TCP_PROXY_SERVER` (0x04). Генератор stream_id общий для TUN/SOCKS/HTTP,
изначально равен 1, перед выдачей инкрементируется. На exit ключ потока включает группу, peer и sid.

## TUN → exit

Output queue TUN содержит однобайтовый префикс перед IP-пакетом. Обработчик проверяет
IPv4 version/IHL/total length и отвергает фрагменты (MF/offset) с WARN.
Для UDP проверяется длина из заголовка и передаётся ровно указанное тело. Для ICMP Echo проверяется наличие восьмибайтового заголовка.
TCP-сегменты передаются в `lwip_tcp_input`. Один listener обслуживает все порты назначения.
Перед выделением PCB для SYN проверяется общий лимит потоков. Состояние SYN_RCVD занимает один слот до accept, сброса или тайм-аута.

При accept выбирается первый mapping с совпадающим локальным портом; если его нет —
исходные `pcb->local_ip/local_port`. Создаются `tcp_proxy_client_conn`, `proxy_flow`
и очереди `tx_queue` (lwIP→exit) и `to_lwip` (exit→lwIP), отправляется CONNECT.
Локальный TCP handshake завершает lwIP; его успех не подтверждает connect назначения.
DATA ожидает CONNECTED от exit.

Recv callback копирует pbuf в блоки до 4096 байт в tx_queue. Drain отправляет через
`proxy_flow_send`; `tcp_recved` возвращает окно локальному клиенту только после
успешного принятия DATA router. Без кредита поток ожидает WINDOW. Готовые к отправке потоки
обслуживает общий планировщик. Уведомление router ставит встроенный callback клиента в FIFO.
Если отправка всё ещё заблокирована, зарезервированный при создании клиента таймер повторяет её через 50 мс.
Обе операции обходятся без выделения памяти. Неотправленная запись остаётся первой в очереди потока.

## Exit → TUN

Router callback проверяет peer==via_ed и group==TOPO_GROUP_UTUN, включая пустые
уведомления router. Сначала предлагает stream SOCKS/HTTP спискам, затем ищет lwIP-поток.

CONNECTED открывает `flow.ready`; WINDOW проверяет и увеличивает кредит, возобновляя
очереди. DATA после проверки окна помещается в to_lwip. `feed_from_transport` пишет
часть entry по доступному `tcp_sndbuf` с `TCP_WRITE_FLAG_COPY`, оставляя хвост в очереди.
Кредит возвращается после успешного `tcp_write`, затем вызывается `tcp_output`.
Дальнейший feed запускают DATA, WINDOW/wake, sent и poll callbacks.
Выходные IPv4-пакеты lwIP записываются через `tun_write`.

## Завершение

Локальный FIN устанавливает fin_local. FIN к exit ставится только после опустошения
tx_queue; общий control retry ждёт ready и повторяет неудавшуюся отправку.
Удалённый FIN устанавливает fin_remote/flow.fin_received и fin_deferred.
После дренажа to_lwip выполняется `tcp_shutdown(pcb, 0, 1)`; при неудаче попытка повторяется.

После подтверждения этого FIN клиент в состоянии FIN_WAIT_2 продолжает принимать данные браузера.
Приём данных или передача накопленных данных в exit продлевает ожидание встречного FIN.
Через 20 секунд простоя ближайший lwIP poll прерывает TCP, освобождает PCB и уведомляет exit через ERROR.
Пустые ACK оставляют дедлайн в силе.

Когда обе половины завершены, PCB отсоединяется от callbacks: lwIP может самостоятельно
освободить его по TIME_WAIT без err callback. Поток удаляется после отправленного FIN,
полученного FIN, снятого fin_deferred и пустых tx_queue/to_lwip. Штатное завершение
не требует дополнительного обмена CLOSE.

Входящий CLOSE прекращает поток через `tcp_close` и очистку, ERROR вызывает abort.
Ошибка lwIP обнуляет PCB, который уже освобождает стек; остальная очистка отменяет
задачи потока в общем планировщике и освобождает обе очереди.
Если при освобождении потока остаётся неотправленный CLOSE/ERROR, общий планировщик
сохраняет команду и stream_id. Через 30 секунд ожидания отправки он закрывает канал к exit.

Пустое уведомление router (`ROUTER_SVC_HDR_SIZE=26`, без TCP command/sid) удаляет
все lwIP/SOCKS/HTTP потоки клиента к настроенному exit. Это не 9-байтовый wire CLOSE_ALL.

`destroy` снимает только собственный TCP binding, закрывает listeners/клиентов,
освобождает lwIP/TUN/пул. Общие UDP/ICMP принадлежат instance и сохраняются.
Владелец затем сам обнуляет `inst->tcp_proxy_client`. Все callbacks и очистка выполняются
в uasync; уничтожать клиента следует вне его активного callback.
