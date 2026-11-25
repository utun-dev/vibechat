# chatgui-android — этапы реализации и проверки

Документ описывает текущие точки входа и критерии проверки. Наличие реализации
не означает, что соответствующий сценарий выполнен на устройстве.
Архитектура и контракты: [ARCHITECTURE.md](ARCHITECTURE.md), заголовки `libutun_lite/` и `jni_bridge/`.

## 1. Сборка общего ядра

Реализовано: `utun_sources.cmake` использует общие `lib/` и `src/`, включая ETCP,
STCP, NCD и группы/BGP. Android не требует отдельной реализации chat_sync и
не исключает UDP/маршрутизатор. Источники голосового стека собираются отдельно
через `src/call/voice_sources.cmake`. Итог NDK — `libutun.so`.

Проверять состав и исключения по `app/src/main/cpp/CMakeLists.txt`, а версии SDK,
NDK, JVM и ABI — по `app/build.gradle.kts`. Не создавать второй lite-instance или
локальную схему БД ради Android. UTUN-сервис управляется `[gui] utun_enabled`.

Сборка из корня репозитория:

```bash
./build.sh --no-gui -j4
cd tools/chatgui-android
./gradlew assembleDebug
```

Критерий: обе сборки завершились успешно; APK появился в
`app/build/outputs/apk/debug/app-debug.apk`. Сборка APK не подтверждает работу на устройстве.

## 2. Lifecycle и JNI

Реализовано: NativeLib.init сохраняет callbacks, start передаёт INI и создаёт worker,
SERVICE_STARTED подтверждает готовность. Restart пересоздаёт ядро; updateSockets
согласует `[server:*]` без рестарта. Код stop ждёт worker до двух секунд, после
таймаута возврат не подтверждает освобождение ресурсов.

Проверять последовательно: init → установка event callback → start → SERVICE_STARTED →
рабочая команда → результат → остановка media workers → stop. Отдельно проверить
restart и смену интерфейсов через updateSockets. Возврат nativeRestart/nativeUpdateSockets
не является подтверждением применения конфигурации.

Критерий: события приходят в Kotlin, нет обращений по старым inst/ua, stop/restart
не пересекаются с внешними I/O workers. Проверять категории sys/socket/connection/chat
и сообщения lifecycle; при таймауте остановки сначала установить причину по логам.

## 3. Каналы, приглашения и личные беседы

Реализовано: Compose UI, ChatRepository с JSON/JNI и ViewModel/StateFlow;
SQLite принадлежит общему ядру. Room/отдельный Kotlin SQL-reader не используются.
InviteLink.kt вызывает общий native-кодек для предпросмотра и передаёт исходную
ссылку JOIN-port с request ID. connectNode остался логирующей заглушкой.

Проверять на двух узлах: создать канал → запросить invite → дождаться готовой ссылки →
JOIN → CONNECT_RESULT → синхронизация участников и сообщений. Для сетевого baseline
на Linux использовать основной `src/utun` и `tools/chatcli`, описание команд —
`../../chatcli_commands.txt`. Android-каталог `headless/` не запускает ядро и не подходит
для такого end-to-end сценария.

Критерий: request результата совпадает, сообщение появляется на другом узле,
готовность JOIN и READY групповой сессии различается в диагностике.
Для DM отдельно проверить поколения беседы: поздняя история/подготовленное вложение
после удаления беседы не должны появиться в новом поколении.

## 4. Вложения, звонки, рация и трансляции

Реализовано: file/photo для каналов, общий attachment_send для voice/video каналов и DM,
DM files, native голосовой стек звонка/рации, MediaCodec и независимые stream workers.

При проверке файлов учитывать владение: file/photo caller сохраняет до завершения
копирования; принятые video/DM-file запросы забирают временный cache-файл и удаляют
также при ошибке/отмене. Отказ постановки оставляет источник caller. Успешный return
не подтверждает публикацию или доставку; смотреть события подготовки и отправки.

Для звонка/рации проверять capture/playback, PTT/auto, смену маршрута и остановку I/O
до освобождения DSP. Для stream — KEY/revision, отдельные viewers, demand/pull,
GAP/CLOSED, завершение аудио/видео и wake/освобождение signal.

Существующие native-проверки из корня репозитория:

```bash
make -C tests test_android_bridge test_android_replies test_android_udp_log test_android_stream test_standby test_standby_transport test_group_connect_standby
(cd tests && ./test_android_bridge && ./test_android_replies && ./test_android_udp_log && ./test_android_stream && ./test_standby && ./test_standby_transport && ./test_group_connect_standby)
```

test_android_bridge проверяет владение config callbacks, повторный init/destroy и
покадровое/батчевое декодирование настоящих Opus-файлов (пять частот, mono/stereo, EOF).
Эти тесты проверяют отдельные native-контракты, не весь Android lifecycle.
Устройство с собранным APK: сценарий задержки video worker на 500мс, EOF/reset и
независимых аппаратных аудиочасов — [stream_worker_smoke](../tests/stream_worker_smoke/README.md).
Критерии и зависимости заданы там; сетевой путь этим тестом не проверяется.

## 5. Фоновый сервис и диагностика устройства

Реализовано: HeadlessService как foreground service, сеть через NetworkChangeMonitor,
уведомления activity через NetworkActivityMonitor и native standby ACTIVE/SLEEP.
Отдельный Linux headless control socket не встроен в этот Android-сервис.
Общий headless API ядра требует явного `[chatserver] headless_control_bind`; параметр
controlPort у NativeLib.init listener не открывает.

Проверять: foreground → background → SLEEP → timer/ранний wake → foreground;
затем смену сети и restart. Уровни категорий задать до повторного прогона, чтобы
логи показывали причину перехода и согласованный набор сокетов.
UDP-лог настроить через `[log_udp]`; приёмник запускать по инструкции `tools/logreceiver/`.
После shutdown/restart не должно оставаться таймера старого uasync.

Критерий: ядро продолжает обслуживать события, standby-ожидания корректно будятся
или отменяются, UI получает новое состояние; при ошибке причина видна в логах.
Проверки на физическом устройстве выполняются отдельно от сборки и Linux-тестов.
