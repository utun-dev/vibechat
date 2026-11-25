# AGENTS.md — chatgui-android (Android P2P Chat)

Android-версия чатгуи: P2P чат на общем ядре ETCP/STCP, UI на Jetpack Compose (Kotlin).
`libutun_lite/utun_sources.cmake` собирает исходники `lib/` и `src/`, включая UDP/TCP,
NCD, группы/BGP и маршрутизатор. Исключения перечислены в CMake (например, `utun.c`);
голосовой стек собирается отдельно через `src/call/voice_sources.cmake`.

`instance_lite` создаёт ядро и вызывает `utun_core_start()` + `chat_service_start()` в отдельном uasync-потоке.
UTUN-сервис по умолчанию не запускается; `[gui] utun_enabled=yes` включает его явно.
ConfigProvider хранит этот флаг как `gui.utun_enabled` (0 по умолчанию).
При stop ядро освобождает чат и общие ресурсы; SQLite хранится в `db_path/chats.db`.
Владение ресурсами и порядок остановки: `../../doc/service_lifecycle.md`.

## Состав проекта (5 частей)

```
tools/chatgui-android/
├── libutun_lite/           # C-ядро (статическая библиотека)
│   ├── CMakeLists.txt
│   ├── utun_sources.cmake   # GLOB исходников, исключения и файлы обёртки
│   ├── instance_lite.h/c     # жизненный цикл (старт/стоп/рестарт, ключи)
│   ├── utun_config_api.h/c   # конфиг-провайдер (Kotlin → C)
│   ├── voice_recorder.h/c    # накопление PCM; voice/file подготовка через общий attachment_send
│   ├── attachment_sender.h/c # отправка обычных файлов в канал
│   ├── photo_sender.h/c      # отправка готовых изображений в канал
│   ├── video_sender.h/c      # готовый MP4 → attachment_send (канал/PM)
│   ├── standby.h/c           # фоновые ACTIVE/SLEEP интервалы и отменяемое ожидание
│   └── tests/test_standby.c   # тесты duty-cycle
│
├── headless/                # CLI для Linux (тестирование без Android)
│   ├── CMakeLists.txt
│   ├── headless_main.c       # отдельный каркас control-loop; сам ядро не запускает
│   └── headless_control.c/h  # управляющий TCP-сокет (JSON/text протокол)
│
├── jni_bridge/              # JNI прослойка C ↔ Kotlin
│   ├── android_jni_bridge.c/h  # C API; JNI-функции — только Android
│   └── android_udp_log.c/h     # UDP-лог: общий буфер, flush в uasync
│
├── app/                     # Android приложение (Gradle + NDK + Compose)
│   ├── build.gradle.kts
│   └── src/main/
│       ├── AndroidManifest.xml
│       ├── cpp/              # NDK: jni_bridge + libutun_lite → libutun.so
│       │   └── CMakeLists.txt
│       ├── java/com/utun/chat/
│       │   ├── ChatApplication.kt     # Application, инициализация
│       │   ├── MainActivity.kt        # Compose UI, навигация экранов
│       │   ├── data/
│       │   │   ├── NativeLib.kt       # JNI-обёртка
│       │   │   ├── ChatRepository.kt  # репозиторий (JNI + JSON)
│       │   │   ├── InviteLink.kt      # парсер utun://base64blob
│       │   │   ├── ConfigProvider.kt  # конфиг из DataStore
│       │   │   ├── LogManager.kt      # лог-менеджер
│       │   │   ├── ServerEntry.kt     # модель listen-сокета
│       │   │   ├── CallAudioEngine.kt # capture/playback звонка через native voice stack
│       │   │   ├── RadioAudioEngine.kt # capture/playback и PTT рации
│       │   │   └── UtunConnectionService.kt # Android Telecom ConnectionService для P2P-звонков
│       │   ├── viewmodel/
│       │   │   └── ChatViewModel.kt   # StateFlow для UI
│       │   ├── ui/screens/
│       │   │   ├── ChannelListScreen.kt  # список каналов + join/create
│       │   │   ├── ChatScreen.kt         # сообщения + ввод
│       │   │   ├── JoinChannelDialog.kt  # диалог подключения по invite
│       │   │   ├── QrScannerPreview.kt   # CameraX + ML Kit QR-сканер
│       │   │   ├── SettingsScreen.kt     # настройки
│       │   │   └── LogScreen.kt          # лог-просмотр
│       │   ├── ui/components/
│       │   │   └── MessageBubble.kt      # бабблы сообщений + InputBar
│       │   └── headless/
│       │       ├── HeadlessService.kt    # foreground-service фонового ядра
│       │       └── BootReceiver.kt       # запуск фонового сервиса после загрузки устройства
│       └── res/
│
└── doc/                     # документация
    ├── ARCHITECTURE.md      # подробная архитектура
    └── IMPL_PLAN.md         # план реализации по этапам
```
Этот `AGENTS.md` лежит в корне `tools/chatgui-android/`. Старые планы в `doc/` сверять с текущим кодом.

## Chat-подсистема (src/chat/)

Все модули из `src/chat/` компилируются в `libutun_lite` через `file(GLOB_RECURSE)`.
Описания каждого модуля — см. корневой `AGENTS.md`, секция «Chat (src/chat/)».

## Технологии

| Слой | Язык | Фреймворк |
|------|------|-----------|
| C-ядро | C99 | OpenSSL, SQLite3 |
| Bridge | C + JNI | Android NDK 29 |
| Android UI | Kotlin | Jetpack Compose, Material3, CameraX, ML Kit |
| Сборка C | CMake 3.16+ | GCC (Linux) / Clang (Android NDK) |
| Сборка APK | Gradle 8.13 | AGP 8.13.2, Kotlin/Compose compiler 2.2.20, CameraX 1.7.0-rc01 |

## Зависимости

### Для сборки APK (Android)

| Компонент | Версия | Установка |
|-----------|--------|-----------|
| JDK | 17–23 (Gradle 8.13) | `apt install openjdk-17-jdk` |
| CMake | 3.22+ | `apt install cmake` |
| Android SDK | API 36 | через [sdkmanager](https://developer.android.com/studio#command-line-tools-only) или Android Studio |
| Android NDK | 29.0.14206865 | `sdkmanager "ndk;29.0.14206865"` |
| Gradle | 8.13 (wrapper) | `./gradlew` скачает сам |
| Kotlin | 2.2.20 | встроен в Gradle плагин; Compose compiler plugin той же версии |
| adb | platform-tools | `apt install adb` (для прошивки) |

Обычный `build.sh` сам вызывает `prepare_build.sh --if-needed` до Gradle-сборки.
Подготовка отдельно от сборки APK: `bash tools/chatgui-android/prepare_build.sh --jobs 4` из корня репозитория.
Скрипт проверяет JDK, устанавливает SDK/NDK/CMake, создаёт `local.properties`, скачивает зависимости
и собирает OpenSSL для `arm64-v8a` и `x86_64` в `app/src/main/cpp/openssl/`.
Android OpenSSL и ONNX Runtime готовятся только здесь, desktop rlottie не скачивается.
Native-зависимости исключены из Git. `--if-needed` проверяет рецепт и SHA-256 результатов,
восстанавливает отсутствующие/изменённые файлы и пропускает неизменённые группы.

Фон `bg.webp` и иконка `icons/app_icon.webp` (256×256) общие с desktop, лежат в `tools/chatgui/resources/`.
Gradle-задача `prepareSharedImages` копирует их в `app/build/generated/sharedImages/res/`;
отдельную копию иконки в `app/src/main/res/` не храним.

### Для Headless (Linux, отладка без телефона)

| Компонент | Установка |
|-----------|-----------|
| CMake 3.16+ | `apt install cmake` |
| GCC / Clang | `apt install build-essential` |
| OpenSSL dev | `apt install libssl-dev` |
| pthread | встроен в libc |

### Настройка окружения

`prepare_build.sh` создаёт `local.properties` в корне `tools/chatgui-android/`; вручную путь можно задать так:

```properties
sdk.dir=/home/user/Android/Sdk
```

NDK выбирается по `ndkVersion` в Gradle и устанавливается в SDK скриптом подготовки.
Устаревший `ndk.dir` не нужен; при подготовке он удаляется из `local.properties`.

### Установка Android SDK (без Android Studio)

```bash
bash tools/chatgui-android/prepare_build.sh --sdk ~/Android/Sdk --jobs 4
```

При запросе `sdkmanager` прочитать и принять лицензии в терминале. Затем `build.sh noinstall` соберёт APK.

## Сборка

```
./build.sh              # clean + сборка + установка на все подключённые устройства
./build.sh noinstall     # только сборка, без прошивки
./build.sh <SN>          # clean + сборка + прошивка на конкретный девайс
./create_signing_key.sh  # однократно создать ключ релизной подписи
./build.sh --release noinstall # clean + сборка подписанного релизного APK
./build_release.sh      # clean + подписанный релиз + установка на все подключённые устройства
./build_release.sh <SN> # то же, установка на конкретный девайс
./publish_release.sh    # clean + подписанный релиз, версионная APK-ссылка в html/index.html и публикация на vc@vc
```

Обычный `./build.sh` собирает Debug с отладочной подписью. Для обновления установленного Release
использовать `./build.sh --release <SN>` или `./build_release.sh <SN>` с тем же релизным ключом.
`INSTALL_FAILED_UPDATE_INCOMPATIBLE` означает несовпадение подписей; `adb install -r` не заменяет подпись.
Если на подключённых устройствах установлены разные варианты, обновлять их отдельно с `--release` или без него.

Ключ RSA 3072 в PKCS12 (`signing/release.p12`) и случайный пароль
(`signing/keystore.properties`) создаются автоматически, без ввода пароля в командную строку.
Каталог `signing/` исключён из Git; права каталога — 700, файлов — 600.
Сохранить резервную копию **всего каталога `signing/`**: этот ключ нужен для дальнейших обновлений приложения.
Повторное создание поверх существующего каталога запрещено скриптом.
Для генерации нужны JDK 17+ (`keytool`, учитывается `JAVA_HOME`) и `openssl`.

Релизный APK: `app/build/outputs/apk/release/app-release.apk`.
Прямой вызов `./gradlew :app:assembleRelease` также подписывает APK этим ключом;
без настроек подписи release завершается ошибкой. Debug не требует релизного ключа.
`./build.sh --release <SN>` собирает и устанавливает релиз на выбранное устройство.

### APK (Android)

```bash
cd tools/chatgui-android
export ANDROID_HOME=/home/user/Android/Sdk
./gradlew assembleDebug
# APK: app/build/outputs/apk/debug/app-debug.apk
```

### Headless (Linux, для отладки сетевого стека)

Каталог `headless/` содержит отдельный каркас управления: его `main` не запускает
`instance_lite` и полноценный чат. Для сетевых сценариев использовать основной
`src/utun` с `[chatserver] headless_control_bind` и `tools/chatcli` (см. корневой `AGENTS.md`).
Команды сборки каркаса; требуется заранее собранный `lib/libopus/libopus_internal.a`:

```bash
cd tools/chatgui-android && mkdir -p build && cd build
cmake .. && make -j4
./headless/headless
```

## Прошивка на телефон

```bash
# Проверить доступные устройства
adb devices

# Установить APK
adb -s <serial> install -r app/build/outputs/apk/debug/app-debug.apk

# Смотреть логи
adb logcat -s utun:*

# Control-сокет (если его запуск включён в конфигурации приложения):
adb forward tcp:9999 tcp:9999
nc localhost 9999
> {"id":1,"cmd":"status"}
> {"id":2,"cmd":"connect","link":"utun://..."}
```

## Конфиг

Конфиг хранится в Android DataStore (`ConfigProvider.kt`) и передаётся в C-ядро
при старте как INI-текст через `nativeStart(configText)`.

Сокращённый пример INI; текущие поля и значения генератора — в `ConfigProvider.buildConfigText()`:

```ini
[global]
my_node_name=MyDevice
my_public_key=<64 hex chars X25519>
my_private_key=<64 hex chars Ed25519 seed>
db_path=/data/data/com.utun.chat/files
db_sync_enabled=1
chatserver_enabled=1
auto_sockets=android

# Секции [server:<имя>] генерируются из настроенных интерфейсов.

[allowed_keys]
allow_all=yes

[ntp]
enabled=no

[chat]
storage_autoload=1
storage_unit_size=10M
storage_total_size=1G
opus_codec_preset=1
compressor_enabled=0
compressor_max_gain_db=25
compressor_rise_rate=10
media_download_max_peers=3

[debug]
chat=info
chat_sync=info

# Опционально — UDP-лог
[log_udp]
ip=<ip хоста>
port=9999
```

Ключи (`my_public_key`, `my_private_key`) генерируются автоматически при первом запуске
и сохраняются в `keys.conf` в `db_path`. Имя узла (`my_node_name`) берётся из
настроек DataStore или `Build.MODEL`.

Вкладка «Рация»: `radio_keep_microphone_ready` — локальная настройка DataStore, по умолчанию включена.
Она удерживает подписку микрофона и DSP 3 минуты после включения рации/отбоя PTT, без передачи звука в ожидании.
Выключение экрана отменяет все удержания PTT и закрывает захват рации; пробуждение сохраняет исходный срок окна.
VAD и измеритель имеют отдельное основание захвата, также только при включённом экране. Общий микрофон закрывается
после последней подписки: трансляция может независимо продолжать пользоваться своим захватом.

## Логирование через logreceiver (только Android)

Debug-сообщения с телефона дублируются через UDP на приёмник `tools/logreceiver/`.
Отправка по умолчанию выключена. Галочка «Отправлять логи по сети (logreceiver)» в настройках отладки
сразу включает/выключает передачу; IP и порт сохраняются при выключении и сами по себе её не включают.

Запуск (всегда через скрипт, до старта utun):
```bash
tools/logreceiver/logreceiver_restart.sh
```
Скрипт ротирует логи (история 10 запусков: `.log` → `.log.1` → … → `.log.10`),
убивает старый демон и запускает новый на `0.0.0.0:9999`, вывод в `logreceiver_output.log`.
При каждом запуске логи фиксируются в истории и начинается свежий лог.

В INI-конфиге Android секция появляется только при включённой отправке и корректном IPv4/порте
(генерируется `ConfigProvider.buildConfigText`):
```ini
[log_udp]
ip=<ip хоста>
port=9999
```

## Ключевые файлы для доработок

### C-слой
- `libutun_lite/instance_lite.h/c` — Жизненный цикл uTun для Android: запуск/остановка C-ядра в отдельном потоке, перезапуск при смене конфига, генерация и обновление X25519-ключей, health-check
- `jni_bridge/android_jni_bridge.h/c` — JNI-прослойка Kotlin↔C: все операции из UI (отправка сообщений, вход в каналы, голосовые, статус, настройки) и обратные вызовы (логи, события). Здесь же — JNI-функции, компилируемые только для Android
- `../../src/chat/invite_link.c/h` — Общий C-код формата invite-ссылок; Android разбирает ссылки в `data/InviteLink.kt`.
- `libutun_lite/utun_config_api.h/c` — Поставщик конфигурации из Kotlin в C-ядро через callback-интерфейс (get_string, get_int64, get_int)
- `libutun_lite/voice_recorder.h/c` — Накопление PCM 48 kHz mono с компрессором; stop передаёт запись в общий attachment_send для фонового кодирования и публикации в канал/PM
- `src/call/call_audio.h/c` (+ `voice_jitter.cpp`, `call_tones.c`, SoundTouch) — единый голосовой стек звонка (`libutun_voice`), собирается через `src/call/voice_sources.cmake`; Opus encode (PCM→peer) и decode + адаптивный джиттер-буфер + time-stretch (PCM отдаётся через `nativeCallAudioPull`). Аудио I/O в Kotlin (AudioRecord/AudioTrack)
- `libutun_lite/attachment_sender.h/c` — Отправка файлов в канал: копирование в media-директорию и регистрация через chat_core (media_index → db_sync)
- `libutun_lite/photo_sender.h/c` — JPEG/PNG в канал/PM через attachment_send; MIME и размеры с EXIF проверяет Android
- `libutun_lite/video_sender.h/c` — Отправка подготовленного MP4 в канал/PM через attachment_send; задача владеет временным cache-файлом
- `libutun_lite/standby.h/c` — ACTIVE/SLEEP интервалы фонового режима, callbacks смены фазы и отменяемые wait handles
- `jni_bridge/android_udp_log.h/c` — Потокобезопасный буфер логов; timer flush привязан к текущему uasync и снимается при restart
- `libutun_lite/utun_sources.cmake` — Общий список источников: `lib/*.c` и рекурсивный `src/*.c` подхватываются автоматически;
  файлы самой обёртки перечислены в `_cfg_src`, исключения — через `list(FILTER/REMOVE_ITEM)`

### Kotlin-слой
- `data/NativeLib.kt` — Kotlin-обёртка над C-библиотекой: все вызовы из Kotlin транслируются в JNI-функции
- `data/CallAudioEngine.kt` — Аудио-движок звонка: AudioRecord/AudioTrack, audio-focus, потоки захвата (feed) / воспроизведения (pull из C-стека)
- `data/RadioAudioEngine.kt` — Аудио-движок рации: AudioRecord/AudioTrack, feed/pull native-стека и управление capture/PTT
- `data/RadioPttHolds.kt` — Отдельные владельцы ручного PTT: кнопка, overlay и volume-клавиши; отпускание одного не снимает остальные
- `data/RadioOverlayService.kt` — Плавающее окно PTT поверх других приложений
- `data/ConversationTarget.kt` — Типизированный target канала/PM для асинхронных операций
- `data/AudioRecorderManager.kt` — Android-захват PCM голосового сообщения и управление native voice_recorder
- `data/CallController.kt` / `CallState.kt` — Управление состоянием и действиями P2P-звонка
- `data/CallAudioRouter.kt` — Выбор аудиомаршрута звонка: earpiece/speaker/headset
- `data/TelecomCallManager.kt` / `UtunConnection.kt` / `UtunConnectionService.kt` — Интеграция P2P-звонков с Android Telecom
- `data/ChatRepository.kt` — Хранилище данных: буферизация сообщений и каналов между C-ядром и UI через StateFlow
- `data/InviteLink.kt` — Разбор invite-ссылок: извлекает ID канала, публичный ключ, адреса для подключения
- `data/ConfigProvider.kt` — Настройки DataStore и генерация INI-текста для запуска C-ядра
- `data/LogManager.kt` — Сбор и хранение логов из C-ядра через log-callback
- `viewmodel/ChatViewModel.kt` — ViewModel: StateFlow-состояние для UI (список каналов, сообщения, статус)
- `ui/screens/ChannelListScreen.kt` — Главный экран: список каналов, кнопки Join (по invite-ссылке) и Create
- `ui/screens/ChatScreen.kt` — Экран чата: список сообщений + поле ввода
- `ui/screens/DmChatScreen.kt` — Экран личной беседы
- `ui/screens/MemberListScreen.kt` — Участники канала и действия над выбранным участником
- `ui/screens/CallScreen.kt` / `IncomingCallDialog.kt` — Активный и входящий звонок
- `ui/screens/CameraRecordScreen.kt` — Общая CameraX-съёмка фото/видео для канала и PM; владеет cache до передачи ViewModel
- `ui/screens/PhotoViewerScreen.kt` / `VideoPlayerScreen.kt` — Просмотр локальных изображений и видео
- `ui/screens/InviteLinkDialog.kt` — Выдача invite-ссылки и QR
- `ui/screens/FirstLaunchScreen.kt` — Первичная настройка приложения
- `ui/screens/JoinChannelDialog.kt` — Диалог подключения к каналу: ввод/вставка invite-ссылки, предпросмотр, кнопка Connect
- `ui/screens/QrScannerPreview.kt` — QR-сканер для invite-ссылок (CameraX + ML Kit)
- `MainActivity.kt` — Точка входа Android-приложения: навигация между экранами, QrScan → JoinDialog flow

### Headless
- `headless/headless_main.c` — Отдельный select/poll-цикл управления; запуск ядра в этом main не реализован
- `headless/headless_control.c/h` — Каркас TCP-управления. Рабочий headless API общего чата находится в `../../src/chat/chat_headless_control.c/h`

## Invite-ссылки (механика подключения к каналу)

Создаваемые ссылки имеют версию `0x08`: `utun://` + base64(версия, тип, реквизиты,
REALITY-параметры, блоки ключа и адресов). Реквизиты CHANNEL — `join_key` и Ed25519 пригласившего,
SERVER — `add_key`. У адреса есть socketId,
proto, IP и port. Точный формат: `../../src/chat/invite_link.c` и `data/InviteLink.kt`.
Node ID получает Kotlin через `NativeLib.deriveNodeId`, вызывающий `sc_derive_node_id_from_pubkey()`.

Ссылка готова после сохранения join_key у пригласившего. Узел подключения пересылает
кадры между прямым транспортом вступающего и BGP-маршрутом к пригласившему.
Первый MEMBERS идёт по тому же пути через ETCP или BLE. Допуск появляется после
проверки и сохранения состава на вступающем узле.
JOIN_READY протокола добавления и READY групповой сессии — разные состояния.
Полный контракт: `../../src/chat/chat_join.h` и `../../src/routing_layer/topo_group.h`.

## Правила разработки

- Все C-файлы: C99, стиль как в корневом `AGENTS.md` (4 пробела, snake_case, DEBUG_* макросы)
- Для новых C-файлов проверить включение через `libutun_lite/utun_sources.cmake`; файлы обёртки добавить в `_cfg_src`
- Все новые Kotlin-файлы — Compose, Material3, coroutines/StateFlow
- ViewModel не держит ссылки на Context
- Логи в C — через `debug_set_log_hook` → bridge → Kotlin `LogManager`
- Основной запуск: Kotlin передаёт INI-текст через `nativeStart(configText)`; парсинг выполняет общий `config_parser`
- Перед коммитом: головная C-сборка + `./gradlew assembleDebug`


## Языки интерфейса

- Английский — полный набор `app/src/main/res/values/strings.xml`; русский — `values-ru/strings.xml`.
- Список языков в `res/xml/locales_config.xml` общий для `LanguageSetting` и системных настроек Android 13+.
- Новый язык: добавить `values-<код>/strings.xml` и `<locale android:name="<код>" />` в `locales_config.xml`.
- Compose получает текст через `stringResource` / `pluralStringResource`; callbacks и сервисы — через `AppLanguage.text`.
- Выбор хранит Android/AppCompat (`autoStoreLocales`), пустой tag означает язык системы. В DataStore язык не дублировать.
- В моделях и статусах хранить значения или ID ресурсов, а не переведённые строки. Протокольные поля и логи не переводить.
- Переключение пересоздаёт Activity; важное UI-состояние хранить в ViewModel или `rememberSaveable`. Сервисы не перезапускать.
