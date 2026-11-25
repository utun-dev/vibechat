# Проверки 2026-11-16

Текущий stream wire v11,16 отдельных процессов,1Мбит/с полезной opaque-нагрузки.
Первое сравнение FEC на одной топологии и seed20261011:

```sh
python3 tests/stream_netns/run.py --n 8 --r 0 --duration 120 --faults --fault-duration 30 --output /tmp/stream-fec-n8-r0-20261116
python3 tests/stream_netns/run.py --n 8 --r 2 --duration 120 --faults --fault-duration 30 --output /tmp/stream-fec-n8-r2-20261116
python3 tests/stream_netns/report.py /tmp/stream-fec-n8-r0-20261116 /tmp/stream-fec-n8-r2-20261116
```

Оба прогона PASS, главный namespace не изменён. После деградации WAN, восстановления и двух отписок
все13 оставшихся зрителей получили подписанный END. GAP=0, checksum/order errors=0.

| Метрика | 8/0 | 8/2 |
| --- | ---: | ---: |
| Проверенных пачек у автора | 2170 | 2892 |
| CPU16 процессов baseline, долей одного ядра | 2,432 | 2,861 |
| Исходящие байты всех eth0 за весь прогон | 591002255 | 743952254 |
| Максимум p50 отдельных зрителей,мс | 280 | 105 |
| Максимум p95/p99 отдельных зрителей,мс | 2815/3564 | >=4095/>=4095 |
| Максимальная задержка пачки,мс | 3867 | 8966 |
| Переносов / INFO-проб | 351/3122 | 656/4266 |

CPU включает все потоки процессов, но кодека в этом сценарии нет. Сетевые байты включают JOIN, control,
обрамление и повторы; IFB не считается повторно. Последняя корзина гистограммы — нижняя граница4095мс.
Распределение путей и тайминги процессов различаются даже при одном seed. Одна пара не доказывает причинность;
оснований увеличивать FEC по умолчанию нет. Порог8/0 сохранён; остальные результаты и ограничения — в `stream.md`.

Проверки Python-стенда: `python3 -m unittest discover -s tests/stream_netns -p 'test_*.py'` —9/9.
Пять проверок изоляции плюс четыре проверки подсчёта CPU/сетевых байтов.

Приход волнами и два отключения/возврата реальных промежуточных узлов:

```sh
python3 tests/stream_netns/run.py --join-wave-size 3 --churn-cycles 2 --duration 30 --seed 20261116 --output /tmp/stream-churn-waves-fixed-20261116
```

PASS:1926 пачек,3 оборота timestamp, END у всех15 зрителей, GAP=0, ошибок содержимого0, главный namespace неизменён.
Выбраны n12 и n02, участвовавшие в24 и20 наблюдаемых DATA-путях. Каждый отключён на35с и перешёл в waiting
с освобождением сетевых выдач; после возврата сети DATA возобновились за2,04/3,09с по секундным снимкам.
Максимальная задержка пачки37291мс включает догон истории отключённым opaque-получателем: это не media KEY-сценарий.
Первый прогон `/tmp/stream-churn-waves-20261116` остановился из-за ошибки стенда: link up не восстановил удалённый
Linux default route. Снимки подтвердили отсутствие маршрута, UDP сообщал error101. Исправлен возврат маршрута стендом.

Два одновременных автора и настоящая загрузка uasync, затем ограничение uplink:

```sh
python3 tests/stream_netns/run.py --second-publisher n08 --resources --duration 60 --seed 20261117 --output /tmp/stream-two-resources-20261116
python3 tests/stream_netns/run.py --second-publisher n08 --resources --faults --duration 300 --seed 20261118 --output /tmp/stream-steady-resources-20261116
python3 tests/stream_netns/report.py /tmp/stream-two-resources-20261116 /tmp/stream-steady-resources-20261116
```

Оба прогона PASS. Первый:1624/1445 пачек,15/15 конечных получателей,4 оборота timestamp.
Второй:5312/5120 пачек,13/15 получателей после отписок,12 оборотов. GAP0, checksum/order errors0 в обоих потоках;
сеть хоста неизменна. Публикация каждого автора имеет полезный bitrate1Мбит/с.

CPU нагружен вычислением SHA256 в uasync90мс с паузой10мс; срок ограничен10с. Измеренные97,199/97,605%
закрывают новые выдачи; DATA к работающим зрителям продолжаются. После прекращения работы допуск восстанавливается.
Далее TBF2Мбит/с на eth0 первого автора на30с: при CPU≤70% наблюдаются OVERLOAD и освобождение части выдач,
без закрытия общего CPU-допуска. После восстановления сети оба потока доходят до END.
Это реальная работа в потоке, но не проверка OS quota/thermal throttling и не нагрузка encoder.

В длинном прогоне максимум индивидуальных p50/p95/p99/max задержек пачек:
первый поток253/8945/14221/15475мс, второй243/9293/11208/11805мс.
Гистограмма этого прогона имеет предел60000мс; предыдущие4095мс остаются нижней границей.
Длительный прогон выполнен до pull4db75c268 с оптимизацией выключенных логов: его CPU-результат нельзя приписывать новой базе.
Оба сценария — opaque-данные: предел10с с переходом медиаплеера на KEY проверяется другими тестами.

После pull4db75c268 и добавления проверки k≤192 выполнен TCP-only со строгим NAT:

```sh
python3 tests/stream_netns/run.py --publisher n05 --tcp-only --duration 75 --seed 20261119 --output /tmp/stream-tcp-wire11-20261116
```

PASS:1030 пачек,1 оборот timestamp, все15 получателей получили END, GAP0/errors0, сеть хоста неизменна.
Максимумы индивидуальных p50/p95/p99/max:241/3107/4280/4458мс. CPU16 процессов baseline:2,181 доли ядра;
исходящие eth0 за весь прогон304736732 байта,145 переносов и1453 INFO-пробы. Эти числа относятся к TCP/seed20261119,
их нельзя трактовать как эффект оптимизации относительно UDP с другим seed.

# Проверки 2026-10-11

Дополнение 2026-10-12: резервный STUN по IP/netif.
`python3 tests/stream_netns/run.py --stun-only --output /tmp/stun-interface-netns-01` — PASS.
Настоящие RFC5780-серверы, отдельный тестовый порт и две рабочие подписанные записи одного IP/netif:
n03 STUN EIM/OPEN -> ETCP EIM/OPEN;
n14 STUN EIM/CONTACT_REQUIRED -> ETCP EIM/CONTACT_REQUIRED;
n05 STUN DEPENDENT/CONTACT_REQUIRED -> ETCP DEPENDENT/UNKNOWN.
Без пиров и с одним наблюдателем выбирается STUN; независимый помощник позволяет выбрать ETCP.
Mapped порт пробы не рекламируется для рабочего сокета. Главный namespace не изменён;
`/tmp/stun-interface-netns-01/host-check.json`, `stun-result.json`, `result.json`.

Финальные проверки после исправления порядка остановки и сохранения истории портов при reload:
`make -C tests check -j4` — 145 passed, 0 failed, 0 skipped (`/tmp/stun-fallback-final-check.log`).
В отдельной ASAN-сборке `test_stun_nat`, `test_nat_detection`, `test_route_ping`
прошли с `detect_leaks=1:halt_on_error=1`: ошибок памяти/утечек нет
(`/tmp/stun-fallback-asan-test_stun_nat.log`, `/tmp/stun-fallback-asan-test_nat_detection.log`,
`/tmp/stun-fallback-asan-test_route_ping.log`). Проверены группировка, TTL/refresh,
удаление представителя, reload, подпись/SQLite source, приоритет частичного ETCP и cleanup.
Пять проверок изоляции прошли. IPv6 STUN остаётся отдельным этапом.

Стенд: 16 процессов uTun, 10 NAT-клиентов, 3 страны. Основной network namespace не изменился во всех перечисленных запусках.

Результаты прежних стриминговых прогонов ниже подтверждали сетевые правила шлюзов
и доставку данных. Они не проверяли правильность классификации NAT внутри uTun.
Новая проверка владельца endpoint: `--nat-only --tcp`, артефакты
`/tmp/nat-owner-netns-02/nat-result.json`. Проверены все 16 UDP-профилей и 16 TCP-профилей:
6 UNCHANGED/OPEN, 4 EIM/OPEN, 2 EIM/CONTACT_REQUIRED, 4 DEPENDENT/UNKNOWN;
TCP: 6 публичных OPEN, 10 UNKNOWN/no_listener. Точные внешние адреса совпали с ожидаемыми.

Полная проверка с новым детектором:
`python3 tests/stream_netns/run.py --nat-check --publisher n05 --tcp --duration 75 --rate 2000000 --faults --fault-duration 15 --output /tmp/nat-owner-stream-integration-02`.
PASS: сначала все 16 UDP/TCP-профилей, затем реальные JOIN/Merkle/BGP, 4041 пачка,
перегрузка WAN, recovery, две отписки и END у оставшихся 13 подписчиков.
Ошибок payload/checksum/порядка нет, GAP полностью учитывается. Главный namespace неизменён.
`make -C tests check -j4`: 144 passed, 0 failed, 0 skipped (`/tmp/nat-owner-regressions-02.log`).

В отдельной копии исходников собраны ядро и целевые `test_nat_detection` / `test_route_ping`
с одинаковыми ASAN-флагами. Оба теста прошли с `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`;
ошибок памяти и утечек нет (`/tmp/nat-owner-asan-detector.log`, `/tmp/nat-owner-asan-route.log`).

Калибровка дополнена переходом «незапрошенный вход → исходящий контакт → вход».
`/tmp/nat-owner-calibration-before-guard` воспроизвёл блокировку последующего UDP
для restricted NAT даже без uTun. После фильтрации запрещённого входа до conntrack
`/tmp/nat-owner-calibration-after-guard` прошёл. Главный namespace не изменился.

| Сценарий | Команда из корня репозитория | Результат |
| --- | --- | --- |
| Публичный UDP-источник, длинный прогон и перегрузка | `python3 tests/stream_netns/run.py --duration 120 --rate 2000000 --faults --output /tmp/stream-netns-final-01` | PASS: 5996 пачек, 3 оборота timestamp, вытеснение из кольца, WAN recovery, unsubscribe, END |
| Источник за строгим NAT, только TCP | `python3 tests/stream_netns/run.py --publisher n05 --tcp-only --duration 20 --output /tmp/stream-netns-tcp-only-fixed-01` | PASS: 608 пачек, END у всех 15 получателей; после исправления раннего END |
| Источник за строгим NAT, чистые mappings/conntrack, UDP+TCP | `python3 tests/stream_netns/run.py --publisher n05 --tcp --duration 75 --rate 2000000 --faults --fault-duration 15 --output /tmp/stream-netns-clean-nat-fixed-01` | PASS: 4172 пачки, 2 оборота timestamp, 946 вытесненных пачек у источника; после обоих исправлений |

В последнем запуске median LAN RTT: A=2,64 мс, B=2,72 мс, C=2,54 мс; WAN A–B=51,79 мс, A–C=100,85 мс, B–C=150,31 мс. Все 10 NAT-профилей прошли отдельную проверку. После калибровки создана новая сеть без ее conntrack/mappings.

У источника в последнем запуске сохранено 33 550 400 байт подписанных блоков при capacity 33 554 300 байт (в пределах 32 MiB). Все оставшиеся после unsubscribe 13 получателей дошли до cursor=4172 и получили подписанный END. Ошибок checksum/порядка/повторной DATA нет. GAP и увеличенные задержки при перегрузке отражены в `summary.csv` и `samples.jsonl`; PASS не означает доставку без пропусков.

Обнаружены и исправлены два дефекта:

1. END первого поставщика выключал оптимизатор до дочитывания неполной FEC-пачки. Проверка до исправления: `/tmp/stream-netns-early-end-before-02.log`; после: `/tmp/stream-netns-early-end-after.log`.
2. Restart через отстающего поставщика повторно выдавал DATA той же эпохи. Прогон `/tmp/stream-netns-clean-nat-01` зафиксировал seq=2864 при ожидаемом 2865 с верным checksum. Исправление сохраняет уже выданный cursor и отложенный GAP; регрессии: `/tmp/stream-netns-restart-before.log` и `/tmp/stream-netns-restart-after-02.log`.

После последнего исправления `make -C tests check -j4`: **144 passed, 0 failed, 0 skipped** (`/tmp/stream-netns-final-unit-check.log`). `test_stream_service` включает ранний END, старый bootstrap после restart, два restart до GAP и проверку отсутствия оставшихся отслеживаемых аллокаций. Пять проверок изоляции `test_isolation.py` прошли; C-агент проверен с `-Wall -Wextra -Wformat=2` без предупреждений.

Файлы `/tmp` — результаты конкретных запусков; при очистке системы они могут быть удалены. Команды выше позволяют воспроизвести сценарии с новым каталогом `--output`; распределение потерь и расписание процессов могут менять количество GAP и задержки.
