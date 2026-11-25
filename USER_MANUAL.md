# uTun — Руководство пользователя

uTun — децентрализованная VPN-система с шифрованием, надёжной доставкой данных, поддержкой mesh-сетей и встроенным TCP-прокси. Работает поверх UDP (основной транспорт) и TCP (STCP-резерв). Всё шифруется через X25519 + AES-128-CCM. Топология узлов обменивается автоматически (BGP-подобный протокол).

---

## 1. Быстрый старт

### 1.1. Сборка

```bash
git clone <репозиторий> && cd utun3
./build.sh --full -j4        # autoreconf + configure + make
```

Зависимости: `gcc`, `make`, `autotools` (autoconf/automake/libtool), `OpenSSL` (libssl-dev).

### 1.2. Генерация ключей

Ключи генерируются автоматически, если их нет в конфиге. Достаточно указать `my_node_name` — `config_updater` создаст пару X25519 и node_id:

```ini
[global]
my_node_name=my-first-node
```

После первого запуска ключи пропишутся в конфиг автоматически.

### 1.3. Первый запуск

```bash
sudo ./src/utun -f -c myconfig.conf
```

- `-f` — foreground (без демонизации, видно логи в консоли)
- `-c myconfig.conf` — путь к конфигу
- `-p /var/run/utun.pid` — PID-файл
- `-l /var/log/utun.log` — лог-файл

Без `-f` процесс уходит в демон (`fork` + `setsid`).

### 1.4. Проверка работы

```bash
ip addr show tun3                  # TUN-интерфейс создан?
ping 10.23.1.1                     # пингуем TUN-IP узла
ss -uln | grep 1333                # UDP-сокет слушает?
```

---

## 2. Архитектура (кратко)

uTun — **однопоточная асинхронная** система. Один event loop (`epoll`/`poll`) на поток. Никаких `sleep()`, никакого блокирующего I/O.

**Стек протоколов (5 уровней):**

| Уровень | Назначение |
|---------|------------|
| 1. UDP/TUN/STCP | Физический транспорт: UDP-сокеты, TUN-интерфейс, TCP-резерв |
| 2. secure_channel | X25519 ECDH + AES-128-CCM шифрование + Ed25519 подписи |
| 3. ETCP | Надёжный транспорт (inflight, ACK, ретрансмиссии, BBR congestion control) |
| 4. LoadBalancer | Multi-link балансировка (min inflight + round-robin) |
| 5. etcp_router | Сервисная маршрутизация (seq, dedup, reorder, transit forwarding) |

Два уровня надёжности:
- **ETCP** — per-connection, RTT-based ретрансмиссии
- **etcp_router** — per-service, 300ms ретрансмиссии с 17 попытками (~5 сек)

Node identity: `SHA256(private_key)` → первые 8 байт → 63-bit `node_id`.

---

## 3. Конфигурация

Формат: INI-style. Секции `[section]`, строки `key=value`, комментарии `#`.

### 3.1. `[global]` — Идентичность и базовые настройки

```ini
[global]
my_node_name=home-server                # имя узла (человекочитаемое)
# Адрес устройства — публичный Ed25519, вычисляется из my_private_key.
my_private_key=d065b784...4fe441        # X25519 приватный ключ (hex, 64 символа)
my_public_key=86b51a8b...6a2d19         # X25519 публичный ключ (hex, 64 символа)

tun_enabled=yes                         # создавать TUN-интерфейс (по умолчанию yes)
tun_ifname=tun3                         # имя TUN (по умолчанию tun0)
tun_ip=10.23.1.1                        # IP на TUN-интерфейсе
mtu=1500                                # MTU для всех соединений

debug_level=error                       # глобальный уровень отладки (none/error/warn/info/debug/trace)
log_file=/var/log/utun.log             # путь к лог-файлу
```

### 3.2. `[server:NAME]` — Локальные UDP-сокеты

```ini
[server: lan1]
addr=192.168.1.10:1333                 # IP:порт для приёма входящих
type=public                             # public / nat / private / local
transport=udp                           # udp (по умолчанию) или tcp (STCP)
mtu=1400                                # MTU для этого канала
so_mark=100                             # SO_MARK для policy routing
netif=eth0                              # привязка к интерфейсу
only_local=0                            # 1 = только локальные подключения
```

Типы адресов:
- `public` — узел на публичном IP (прямая связь)
- `nat` — узел за NAT (нужен NAT traversal)
- `private` — локальная сеть (только соседи в том же сегменте)
- `local` — только loopback/локальные подключения

### 3.3. `[client:NAME]` — Подключение к удалённым узлам

```ini
[client: aeza]
link=lan1:85.192.42.96:1333            # server_name:ip:port (можно несколько link=)
link=lan1:85.192.42.96:1433            # второй канал к тому же узлу
peer_public_key=0206dbee...fbcc16      # публичный ключ удалённого узла
keepalive=1                             # интервал keepalive (сек)
```

Формат `link`: `локальный_сервер:удалённый_ip:удалённый_порт`. Серверная секция должна быть объявлена выше по файлу.

### 3.4. `[routing]` — Маршруты

```ini
[routing]
my_subnet=10.23.1.0/24                  # своя подсеть — анонсируется соседям через BGP
route_subnet=10.23.0.0/16               # добавляется в системную таблицу маршрутов через TUN
route_subnet=fd00:1234::/32             # IPv6-подсеть
```

### 3.5. `[debug]` — Настройка отладки

Формат: `категория=уровень`. Уровни: `none`, `error`, `warn`, `info`, `debug`, `trace`.

```ini
[debug]
etcp=trace                              # всё про ETCP
crypto=info                             # криптографические операции
bgp=info                                # обмен топологией
connection=info                         # сокеты и линки
general=info                            # общая информация
```

Основные категории (всего 26): `uasync`, `ll_queue`, `connection`, `etcp`, `crypto`, `memory`, `timing`, `config`, `tun`, `routing`, `timers`, `normalizer`, `bgp`, `socket`, `control`, `dump`, `traffic`, `debug`, `general`, `nat`, `keepalive`, `etcпroute`, `bbr`, `etcp_dump`.

### 3.6. `[firewall]` — Правила фильтрации

```ini
[firewall]
allow=all                               # разрешить всё (bypass)
allow=192.168.1.10                      # разрешить весь IP
allow=10.0.0.1:80                       # разрешить конкретный IP:порт
```

Фильтр основного TUN проверяет IP и TCP/UDP-порт назначения одинаково в обоих направлениях.
Без разрешений весь трафик этого интерфейса запрещён; ответы требуют отдельных разрешений.
CIDR-подсети не поддерживаются. Ошибочное правило отменяет загрузку нового конфига.

### 3.7. `[nat]` — EIM NAT

```ini
[nat]
tun_ifname=tun_nat_client
tun_ip=100.64.1.1
nat_via=dd9d33df43dfd02381612a908ff3e1296e4862b7121d590a12edefbae7d73d28
```

Секция включает NAT. На клиенте в `nat_via` укажите публичный Ed25519 шлюза. Пример конфигурации шлюза приведён в разделе 4.4.

### 3.8. `[tcp_proxy_client]` / `[tcp_proxy_server]` — TCP-прокси

**Клиент (выход через exit-узел):**

```ini
[tcp_proxy_client]
enabled=yes
tun_name=tun_proxy                      # имя TUN для прокси-трафика
tun_ip=10.200.30.1                      # IP клиентского TUN
via_node=dd9d33df43dfd02381612a908ff3e1296e4862b7121d590a12edefbae7d73d28               # node_id exit-узла
group=utun                              # группа клиента, по умолчанию utun

socks_enabled=yes                       # встроенный SOCKS5-прокси
socks_addr=127.0.0.1:1082               # слушать SOCKS5 здесь

http_proxy_enabled=yes                  # встроенный HTTP CONNECT-прокси
http_proxy_addr=127.0.0.1:8080
```

При закрытии SOCKS5- или HTTP-туннеля у прокси есть 10 секунд, чтобы дописать данные локальному клиенту.
Отсчёт начинается при получении CLOSE или после обмена FIN в обоих направлениях. Один FIN оставляет встречное направление открытым.
Для обычного HTTP-ответа с закрытием соединения тот же срок действует после получения ответа целиком.

**Exit-узел (сервер):**

```ini
[tcp_proxy_server]
allowed_group=utun
max_flows=1024
max_flows_per_node=256
```

Сервер по умолчанию обслуживает только `utun`. Чтобы разрешить также CHAT, добавьте
отдельную строку `allowed_group` с публичным Ed25519 группы (64 HEX). Явные строки задают полный
список. Оба узла должны быть участниками CHAT. Штатный клиент пока поддерживает только `group=utun`.

`max_flows` ограничивает весь exit, `max_flows_per_node` — один исходный узел во всех группах.
Каждый TCP-поток, UDP-поток и ICMP-запрос в ожидании ответа занимает одно место.
ICMP-запрос освобождает место при получении ответа или через 5 секунд после отправки.
UDP-поток освобождает место после 30 секунд без запроса клиента или ответа адресата.
Обе опции принимают положительные целые числа. Выше показаны значения по умолчанию.
При достижении любого предела новый TCP CONNECT получает ERROR, новые UDP-потоки и ICMP-запросы
отбрасываются. Ранее открытые потоки продолжают передавать данные.

Клиент настраивает браузер на SOCKS5 `127.0.0.1:1082` — весь TCP-трафик идёт через exit-узел.

### 3.9. `[control]` — Сервер мониторинга

```ini
[control]
control_ip=192.168.29.117               # IP для приёма подключений etcpmon
control_port=9090
control_allow=192.168.0.0/16            # разрешённые подсети (можно несколько)
```

### 3.10. `[allowed_keys]` — Ограничение подключений

```ini
[allowed_keys]
allow_all=1                             # разрешить все ключи
# key=86b51a8b...6a2d19                # разрешить конкретный pubkey
```

### 3.11. `[ntp]` — Синхронизация времени

```ini
[ntp]
enabled=yes
server=pool.ntp.org
server=time.google.com
interval=3600                           # интервал синхронизации (сек)
```

### 3.12. `[network:NAME]` — Именованные сети

```ini
[network: mynet]
pubkey=86b51a8b...6a2d19                # полный адрес сети (32 байта HEX)
signing_key=d065b784...4fe441           # ключ подписи (Ed25519)
```

### 3.13. Серверный vs клиентский конфиг

| Параметр | Сервер | Клиент |
|----------|--------|--------|
| `[server]` секция | Обязательна (свои сокеты) | Обязательна (свои сокеты) |
| `[client]` секция | Нет | Обязательна (к кому подключаться) |
| `peer_public_key` | Нет | Обязателен в `[client]` |
| `my_private_key` | Свой | Свой |
| `my_public_key` | Свой | Свой |

Важно: `[server]` обязателен в **обоих** случаях — это рабочий сокет узла. Клиент дополнительно указывает `[client]` с `peer_public_key` и `link`.

---

## 4. Сценарии использования

### 4.1. VPN между двумя узлами

**Сервер** (публичный IP `85.192.42.96`):

```ini
[global]
my_node_name=server
my_private_key=<hex>
my_public_key=<hex>
tun_ip=10.23.1.1

[server: main]
addr=85.192.42.96:1333
type=public

[routing]
my_subnet=10.23.1.0/24
```

**Клиент** (за NAT, подключается к серверу):

```ini
[global]
my_node_name=client
my_private_key=<hex>
my_public_key=<hex>
tun_ip=10.23.2.1

[server: uplink]
addr=0.0.0.0:1333                       # можно 0.0.0.0 — клиенту не нужен фиксированный порт
type=nat

[client: server]
link=uplink:85.192.42.96:1333
peer_public_key=<pubkey_сервера>
keepalive=1

[routing]
my_subnet=10.23.2.0/24
```

**Проверка связности:**

```bash
# На сервере
ping 10.23.2.1

# На клиенте
ping 10.23.1.1
```

### 4.2. Mesh-сеть из нескольких узлов

Каждый узел = сервер (свои сокеты) + клиент (подключения к соседям). Пример для узла B, который подключается к A и C:

```ini
[global]
my_node_name=node-b
my_private_key=<hex>
my_public_key=<hex>
tun_ip=10.23.2.1

[server: main]
addr=0.0.0.0:1333
type=public

[client: node-a]
link=main:85.192.42.96:1333
peer_public_key=<pubkey_A>

[client: node-c]
link=main:203.0.113.10:1333
peer_public_key=<pubkey_C>

[routing]
my_subnet=10.23.2.0/24
```

Что происходит автоматически:
1. B устанавливает ETCP-соединения с A и C
2. Через BGP A узнаёт о подсетях C (и наоборот)
3. Узел A может отправлять пакеты к подсети C через B (transit forwarding)
4. `etcp_router` обеспечивает надёжную маршрутизацию через промежуточные узлы

### 4.3. Exit-прокси (выход в интернет через удалённый узел)

**Exit-узел** (с прямым выходом в интернет):

```ini
[global]
my_node_name=exit-node
my_private_key=<hex>
my_public_key=<hex>

[server: main]
addr=85.192.42.96:1333
type=public

[tcp_proxy_server]
allowed_group=utun
```

**Клиент** (выходит в интернет через exit-узел):

```ini
[global]
my_node_name=client
my_private_key=<hex>
my_public_key=<hex>

[server: main]
addr=0.0.0.0:1333

[client: exit]
link=main:85.192.42.96:1333
peer_public_key=<pubkey_exit>

[tcp_proxy_client]
enabled=yes
tun_name=tun_proxy
tun_ip=10.200.30.1
via_node=<публичный_Ed25519_exit_64_HEX>
socks_enabled=yes
socks_addr=127.0.0.1:1082
```

Настройка браузера: SOCKS5 proxy `127.0.0.1:1082`. TCP-трафик идёт: браузер → SOCKS5 → lwIP → ETCP → exit-узел → интернет.

### 4.4. EIM NAT (раздача интернета через NAT)

```ini
[nat]
tun_ifname=tun_nat
tun_ip=100.64.0.1
port_start=20000
port_end=30000
forward=tcp:e6a1785a9fa59bf5278e4d14c716dbe9782343092f301e02e8a6554998f13d64:192.168.1.100:80:8080
forward=udp:e6a1785a9fa59bf5278e4d14c716dbe9782343092f301e02e8a6554998f13d64:192.168.1.100:53:53
```

### 4.5. Чат (chatgui)

Chatgui — десктопный GUI-чат (Qt 6) со встроенным uTun-узлом. Вся связь — напрямую между узлами (P2P), без центральных серверов.

**Сборка:**

```bash
sudo apt install librlottie-dev zlib1g-dev qt6-base-dev
cd tools/chatgui && mkdir -p build && cd build
cmake .. && make -j4
./chatgui
```

**Запуск:**

```bash
setsid env DISPLAY=:1.0 QT_ACCESSIBILITY=1 ./chatgui & disown
```

Chatgui работает в двух потоках:
- **GUI-поток** — Qt, отрисовка, чтение из SQLite
- **uasync-поток** — uTun-узел, сеть, криптография, ETCP, запись в SQLite

**Основные возможности:**
- Создание каналов и групп
- Обмен сообщениями с анимированными эмодзи (TGS)
- Invite-ссылки (`utun://...`) с QR-кодами
- P2P синхронизация через Merkle-деревья
- Авто-подключение к известным узлам из БД

---

## 5. Топология сети

### 5.1. Типы адресов

| Тип | Описание | Пример |
|-----|----------|--------|
| `INTERFACE` | Адрес на сетевом интерфейсе | `eth0: 192.168.1.10` |
| `NAT` | Внешний адрес после NAT (детектируется) | `85.192.42.96:54321` |
| `REAL` | Публичный адрес (совпадает interface == NAT) | `85.192.42.96:1333` |

### 5.2. Как узлы находят друг друга

1. **Прямое подключение** — через `[client]` секцию с явным IP:портом
2. **BGP-обмен** — при подключении к одному узлу, узнаёшь о всех его соседях
3. **Локальное сканирование** — `conn_mgr` периодически сканирует локальную сеть
4. **Invite-ссылки** — `utun://` ссылки с закодированными адресами и ключами

### 5.3. NAT traversal

`conn_mgr` реализует трёхфазное подключение:

1. **DIRECT** (5 сек) — прямое INIT-рукопожатие со всеми известными адресами
2. **REVERSE** (15 сек) — если клиент за NAT, сервер с прямым IP: клиент шлёт свои адреса через BGP, сервер подключается сам
3. **INDIRECT** (15 сек) — оба за NAT: выбираются общие кандидаты-посредники по min RTT, трафик идёт через `etcp_router`

Типы NAT, которые определяет uTun:
- **EIM** (Endpoint-Independent Mapping) — один внешний порт для всех назначений
- **Strict** (Address/Restricted/Symmetric) — разные порты

### 5.4. Группы

Два типа групп:
- **UTUN** (`group_id=0x8000000000000000`) — VPN-сеть: обмен подсетями, маршруты
- **CHAT** — чат-группа: только узлы, без подсетей, персистентность в SQLite

Узлы разных групп изолированы.

---

## 6. Мониторинг и отладка

### 6.1. Уровни отладки

```
none < error < warn < info < debug < trace
```

Настройка через `-d` в CLI (имеет приоритет) или секцию `[debug]` в конфиге:

```bash
./src/utun -f -c myconfig.conf -d "etcp=trace,crypto=info"
```

Или эквивалент в конфиге:

```ini
[debug]
etcp=trace
crypto=info
bgp=info
```

### 6.2. Категории отладки (26 шт)

| Категория | Что логирует |
|-----------|-------------|
| `etcp` | Протокол ETCP: очереди, retrans, ACK, RTT |
| `crypto` | Шифрование/дешифрование, ключи, nonce |
| `connection` | Сокеты, линки, INIT handshake, keepalive |
| `bgp` | Обмен топологией (NODEINFO, WITHDRAW) |
| `routing` | Таблица маршрутов, поиск путей |
| `tun` | TUN-интерфейс: чтение/запись |
| `traffic` | Данные трафика |
| `general` | Общая информация |
| `bbr` | BBR congestion control |
| `keepalive` | Адаптивный keepalive |
| `memory` | Выделение/освобождение памяти |
| `dump` | Hex-дампы пакетов |
| `etcp_dump` | Декодирование ETCP-пакетов |

### 6.3. etcpmon — Windows GUI монитор

Подключается к `control_server` узла по TCP (порт из `control_port`). Показывает:
- Список соединений и их состояние
- RTT, потери, inflight
- Графики метрик
- Дамп пакетов

### 6.4. control_server — TCP API

```ini
[control]
control_ip=0.0.0.0
control_port=9090
control_allow=192.168.0.0/16
```

Любой TCP-клиент может подключиться и получать события в реальном времени (JSON-подобный формат).

### 6.5. Логи и дампы

Лог-файл указывается через `-l`:

```bash
./src/utun -f -c myconfig.conf -l /tmp/utun.log
```

Hex-дамп пакетов включается категорией `dump=trace`:

```ini
[debug]
dump=trace
```

---

## 7. NAT и Firewall

### 7.1. Как uTun определяет тип NAT

1. Клиент подключается к серверу
2. Сервер видит внешний IP:порт клиента (из UDP-пакета)
3. Сервер через **третий узел** (посредник) пингует клиента на этот внешний IP:порт
4. Если клиент ответил — NAT EIM (один mapping для всех)
5. Если нет — NAT Strict

Результат сохраняется в `link->nat_type` и broadcast-ится всем соседям через BGP.

### 7.2. Firewall

```ini
[firewall]
allow=all                               # всё разрешено
allow=10.0.0.5                          # разрешить весь IP
allow=192.168.1.5:22                    # разрешить IP:порт
```

Без секции `[firewall]` весь входящий и исходящий трафик основного TUN запрещён.
`allow=IP` и `allow=IP:0` разрешают все протоколы IP; `allow=IP:port` — TCP/UDP указанного порта.
Для фрагментов IPv4 нужен весь IP либо `allow=all`. NAT и прокси используют отдельные TUN интерфейсы.

### 7.3. Проброс портов

```ini
[nat]
forward=tcp:e6a1785a9fa59bf5278e4d14c716dbe9782343092f301e02e8a6554998f13d64:192.168.1.100:80:8080
forward=udp:e6a1785a9fa59bf5278e4d14c716dbe9782343092f301e02e8a6554998f13d64:10.0.0.53:53:53
```

Второе поле — публичный Ed25519 узла, через который доступен внутренний IPv4. Последние два поля задают внутренний и внешний порты.

---

## 8. Устранение неполадок

### 8.1. Нет связи — проверка INIT handshake

Включите:

```ini
[debug]
connection=debug
crypto=debug
```

В логах ищите:
- `INIT_REQUEST sent to ...` / `INIT_RESPONSE received from ...` — handshake
- `session_ready=1` — ключи согласованы, шифрование работает
- `ETCP_KEEPALIVE` — keepalive-пакеты идут
- Если нет — проверьте firewall на портах, доступность IP

### 8.2. Пакеты теряются — проверка метрик

```bash
# В логах с etcp=debug смотрите:
grep "retrans" /tmp/utun.log            # ретрансмиссии
grep "rtt=" /tmp/utun.log               # задержки
grep "lost" /tmp/utun.log               # потери
```

Каждые 10 минут пишутся гистограммы метрик с RTT-бакетами и loss-бакетами.

### 8.3. Высокая задержка — keepalive

Адаптивный keepalive: период растёт от 200ms до 10s при отсутствии трафика, сбрасывается к 200ms при появлении. Таймаут = период × 10.

Проверьте:

```ini
[debug]
keepalive=debug
```

### 8.4. Утечки памяти — проверка пулов

```ini
[debug]
memory=debug
```

При завершении `u_report_unfreed_blocks()` показывает все неосвобождённые блоки.

### 8.5. Дамп состояния

ETCP предоставляет функцию `etcp_dump_all_conns()` для вывода полного состояния всех соединений (очереди, inflight, счётчики). Включается через `etcp_dump=trace`.

### 8.6. ASAN-сборка (поиск повреждений памяти)

```bash
./build.sh --asan -j4
ASAN_OPTIONS=detect_leaks=0:halt_on_error=0 ./src/utun -f -c myconfig.conf
```

Краш-логи ASAN пишутся в `/tmp/utun_asan.<pid>`.

---

## 9. Производительность

### 9.1. BBR congestion control

BBR работает per-link. Основные настройки:

```ini
[global]
bbr_max_cwnd=1048576                    # максимум окна перегрузки (1MB по умолчанию)
```

BBR автоматически измеряет пропускную способность через burst-измерения (12 пакетов пачкой, замер inter-packet gap). Периодичность burst — не чаще 500ms.

### 9.2. Memory pools

Три пула в `UTUN_INSTANCE`:
- `data_pool` — данные пакетов (payload)
- `pkt_pool` — `struct ETCP_DGRAM` (сетевые пакеты)
- `ack_pool` — `struct ACK_PACKET` (подтверждения)

Пулы аллоцируются upfront при старте. Статистику можно посмотреть через control_server.

### 9.3. Multi-link балансировка

LoadBalancer выбирает линк с минимальным inflight, с round-robin для равных. Traffic shaper ограничивает отправку до `optimal_inflight`:

```
timeout = (rtt_avg_10 * 32 + jitter * 32) / 16
```

Оптимальное окно: `optimal_inflight = sum(inflight_lim_bytes)`

### 9.4. Keepalive

Адаптивный режим:
- Начальный период = `keepalive` из `[client]` (сек)
- При трафике → сброс к начальному
- Без трафика → растёт ×1.05 до 10 секунд
- Таймаут = период × 10

---

## 10. CLI-справка

```
utun [опции]

Опции:
  -f            Foreground (без демонизации)
  -c FILE       Конфигурационный файл (по умолчанию: utun.conf)
  -p FILE       PID-файл (по умолчанию: /var/run/utun.pid)
  -l FILE       Лог-файл (по умолчанию: utun.log)
  -d CONFIG     Отладка: категория=уровень,... (переопределяет конфиг)
  -h            Справка
```

**Сигналы:**
- `SIGTERM` / `SIGINT` — graceful shutdown (закрытие соединений, освобождение ресурсов)
- `SIGHUP` — reload конфига (селективный: меняются только изменённые сокеты/клиенты; полный — при изменении ключей или TUN)

---

## 11. Типовые конфигурации

### Минимальный сервер

```ini
[global]
my_node_name=server
tun_ip=10.23.1.1

[server: main]
addr=0.0.0.0:1333
```

### Минимальный клиент

```ini
[global]
my_node_name=client
tun_ip=10.23.2.1

[server: main]
addr=0.0.0.0:1333

[client: server]
link=main:85.192.42.96:1333
peer_public_key=<pubkey_сервера>
```

### Mesh-узел с несколькими пирами

```ini
[global]
my_node_name=mesh-node-3
tun_ip=10.23.3.1

[server: main]
addr=0.0.0.0:1333

[client: node-1]
link=main:10.0.0.1:1333
peer_public_key=<pubkey_1>

[client: node-2]
link=main:10.0.0.2:1333
peer_public_key=<pubkey_2>

[routing]
my_subnet=10.23.3.0/24
```

### Exit-узел с TCP-прокси

```ini
[global]
my_node_name=exit-eu
tun_ip=10.23.100.1

[server: main]
addr=85.192.42.96:1333
type=public

[tcp_proxy_server]
allowed_group=utun

[routing]
my_subnet=10.23.100.0/24
```

### Клиент с SOCKS5-прокси

```ini
[global]
my_node_name=home-laptop

[server: main]
addr=0.0.0.0:1333

[client: exit]
link=main:85.192.42.96:1333
peer_public_key=<pubkey_exit>

[tcp_proxy_client]
enabled=yes
socks_enabled=yes
socks_addr=127.0.0.1:1082
via_node=<публичный_Ed25519_exit_64_HEX>
```
