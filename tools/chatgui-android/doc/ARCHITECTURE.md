# chatgui-android — архитектура

## Состав и сборка

Android-клиент использует общее ядро из `lib/` и `src/`: ETCP с UDP-линками и
STCP-линками поверх TCP, NCD, группы/BGP, ETCP-router, чат, DM и media-сервисы.
Отдельной Android-реализации chat_sync/merkle_sync и отдельного lite-instance нет.
STCP передаёт ETCP-пакеты; его потоковое шифрование — AES-CTR, пакетное ETCP — AES-CCM.
Контракты транспорта описаны в `../../../src/transport_layer/stcp.h` и `etcp.h`.

`libutun_lite/utun_sources.cmake` собирает `lib/*.c` и рекурсивно `src/*.c`,
исключая `utun.c`, каталоги lwip_orig/uip/transport_layer/BBR и файлы отдельного
голосового стека. Это источник актуального состава, а не список «только TCP».
Файлы Android-обёртки перечислены в `_cfg_src`. Для ссылок-приглашений используется общий кодек `src/chat/invite_link.c`.

NDK-сборка в `app/src/main/cpp/CMakeLists.txt` создаёт OBJECT-библиотеку `utun_core`
и итоговую `libutun.so`: ядро, JNI, UDP-лог, отдельная статическая `utun_voice`,
OpenSSL Crypto, Opus, SpeexDSP, ONNX Runtime. JNI распределён между
`android_jni_bridge.c`, `android_stream_jni.c`, `android_call_video_jni.c` и
`android_call_output_jni.c`. Голосовой стек собирается по `src/call/voice_sources.cmake`.

Параметры приложения определяет `app/build.gradle.kts`: minSdk 26, compile/targetSdk 36,
NDK 29.0.14206865, JVM 17, ABI arm64-v8a/x86_64. UI — Kotlin, Jetpack Compose и Material3;
CameraX отвечает за захват фото/видео, MediaCodec — за live video, AudioRecord/AudioTrack — за аудио.
Сборка использует Gradle 8.13, AGP 8.13.2, Kotlin/Compose compiler 2.2.20 и CameraX 1.7.0-rc01;
минимальный Android приложения остаётся API 26.

`CameraRecordScreen` объединяет режимы «Фото / Видео» для канала и PM. Адресат,
поколение PM и канальный ответ фиксируются до открытия камеры. Фото запрашивает
только CAMERA; RECORD_AUDIO требуется в режиме видео. Привязывается Preview и
один выбранный use case (ImageCapture либо VideoCapture). После снимка доступны
просмотр и пересъёмка; переключение режима разрешено в состоянии готовности.
Экран удаляет отменённые cache-файлы и игнорирует запоздавшие callbacks. При отправке
владение файлом переходит ViewModel, а после принятия JPEG/PNG — `attachment_send`.

Общие с трансляцией иконки открывают меню камеры, разрешения, кодека и битрейта.
Видеозапись использует H.264/H.265 и AAC в MP4; доступные кодеки и разрешения
проверяются через `Recorder.getVideoCapabilities(cameraInfo, mime)` для выбранной камеры.
Разрешение и битрейт независимы. Перед готовностью/отправкой `MediaExtractor`
проверяет, что фактический MIME видеодорожки совпадает с выбранным кодеком.
В фото отдельное меню JPEG quality (10..100); `ImageCapture.setJpegQuality()`
задаёт сжатие при съёмке. Предпросмотр читает полученный JPEG с EXIF и сохраняет
артефакты сжатия; масштабирование позволяет рассмотреть их. Декодирование ограничено
4096 по большей стороне, на SM-A525F штатные снимки читаются без уменьшения.

JPG/JPEG/PNG из выбора файла и камеры проходят общий `sendPhotoMessage`:
фактический MIME и размеры с учётом EXIF читаются в IO, затем передаются через JNI.
В PM метаданные JPEG/PNG шифруются вместе с остальными метаданными вложений.
Превью декодируется с ограничением размера; подпись содержит разрешение и размер
файла. Каналы и PM используют `PhotoViewerScreen` с масштабированием и перемещением.

## Владение ядром и потоки

`NativeLib.start(dbPath, controlPort, configText)` под lifecycle mutex инициализирует
config/log/event callbacks и voice_recorder на IO, затем передаёт INI в `instance_lite_start()`.
Инициализация выполняется до первого worker либо после подтверждённого stop/join;
ещё запускающийся worker сохраняет прежние callbacks. controlPort в bridge только логируется,
control-listener включается конфигурацией ядра. Event callback переживает destroy в Kotlin
и повторно устанавливается после создания новых JNI global refs.

Один worker pthread создаёт uasync и `UTUN_INSTANCE`, принудительно задаёт
CLIENT_TYPE_MOBILE и запускает `utun_core_start()` + `chat_service_start()`.
UTUN-сервис включается отдельно через `[gui] utun_enabled=1`, по умолчанию выключен.
Наличие TUN/routing/NAT-кода в сборке само по себе сервис не включает.

До `CHAT_EVT_SERVICE_STARTED` worker привязывает stream bridge и call_video_port,
а при UTUN_WITH_VOICE — call_audio. Успех start означает создание потока либо уже
существующий запуск; он не подтверждает готовность сервисов. Готовность проверяется
по событию и `instance_lite_is_running()`.

Команды UI копируют аргументы и передают их владельцу worker. JNI JSON-getters и короткие проверки
читают SQLite/живые структуры через `instance_lite_call`; C getters и `utun_bridge_set_member_nick()`
исполняются только в потоке владельца. JNI nativeSetChatSetting использует отдельный trampoline.
Реестры команд освобождают также PCM/compressor и собственные cache-файлы при отмене до исполнения.
JNI строки преобразуются через обычный UTF-8 byte[] в обе стороны: имена, JSON и пути сохраняют emoji.
NUL во входной строке отклоняется, ошибки выделения возвращают NULL/отказ, а не усечённую строку.
SQL/память в channels/messages/members возвращает NULL, отдельно от подтверждённого пустого массива.
Массивы сокетов/линков строятся целиком с экранированием имён; при ошибке прежний UI-снимок сохраняется.
JNI event callback удерживает local ref на время вызова, независимо от замены global ref.
Ошибка копирования не доставляет повреждённый payload; исключение callback описывается/снимается для следующих событий.
Заимствованные inst/ua не удерживают ресурс; они пригодны только в потоке владельца.
`instance_lite_post` публикует команду под защитой времени жизни worker и заполняет её поле instance.
Аргумент принадлежит callback, а при отмене — discard; отдельный реестр переживает удаление posted_task.
`instance_lite_call` выполняет короткое чтение в worker и ждёт подтверждения либо отмены при stop/restart.
Вложенный call из worker исполняется сразу; после отмены callback уже не может обратиться к стеку caller.

JNI получает JNIEnv для native-потока и вызывает Kotlin `onEvent(type, ByteArray?)`.
Payload копируется в byte array; его формат зависит от CHAT_EVT_* из `src/chat/chat_event.h`.
Это не универсальный JSON. Отдельный C `utun_event_fn` не является обработчиком
бинарных событий instance_lite. ViewModel переносит обновление UI в свой coroutine context.

Start/stop/restart/regenerate/resetDatabase сериализованы одним Kotlin lifecycle mutex.
Перед stop/restart Kotlin закрывает новый media capture, сбрасывает controllers и ждёт
radio cleanup, recorder owners, CallService, stream/player workers и общий MediaAudioController.
SDK-очистка выполняется на main/собственных workers, ожидание Future/join — на IO.
Неподтверждённый release запрещает native teardown; CoreShutdown повторяет stop.
Worker снимает media-привязки, уничтожает instance, снимает standby/UDP-таймеры и уничтожает uasync.
`instance_lite_stop()` ждёт полный cleanup и pthread_join, включая startup до публикации instance;
каждые две секунды ожидания пишет предупреждение, поток не отсоединяет. Повторный stop безопасен.
Restart пересоздаёт instance и uasync. Возврат restart/updateSockets подтверждает принятие копии INI,
ошибка очереди возвращается Kotlin; готовность нового запуска подтверждается SERVICE_STARTED.
Принятие restart сразу закрывает ready для новых frontend/media запросов. Ошибка подготовки возвращает
ready прежнему instance и повторяет SERVICE_STARTED для восстановления frontend.
Событие означает готовность, а не обязательно новый instance; смену идентичности подтверждает KEYS_GENERATED.
Ранее принятые команды могут завершиться в прежнем worker до его снятия;
после снятия instance они отменяются, даже если uasync обрабатывает оставшиеся callbacks при teardown.
Fatal signal handlers принадлежат платформе: worker не заменяет их обработчиком с небезопасными locks/JNI/backtrace.
Общие правила освобождения: `../../../doc/service_lifecycle.md`.

## Данные и конфигурация

MainActivity читает начальные настройки на IO до открытия setup/основного UI; ошибка оставляет повтор загрузки.
Сохранение setup принадлежит очереди Application, поэтому пересоздание Activity не отменяет принятый commit.
Отказ запуска foreground service показывается с повтором. Activity не читает DataStore при каждом нажатии volume:
PTT использует последний подтверждённый снимок Flow. NotificationSound получает enabled/muted из одного снимка
DataStore в Application и остаётся выключенным до первого успешного чтения; последующая ошибка сохраняет снимок.
AppEventHandler независимо доставляет событие каждому владельцу на main и публикует его текущим подписчикам
без replay; новая ViewModel восстанавливается запросом снимков. Ошибка одного обработчика не прерывает остальные.

Ядро владеет общей SQLite `db_path/chats.db`; путь без db_path даёт in-memory БД.
Chat использует тот же handle, что и топология. Схему создают общие модули
`src/routing_layer/topo_node_sqlite.c`, `src/chat/`, `src/dm/` и media-подсистемы.
Room/DbRepository и отдельного Kotlin SQL-соединения в приложении нет:
ChatRepository получает JSON через JNI и разбирает события, ViewModel хранит StateFlow.
Имена/форматы таблиц не следует дублировать в Android-коде.

ConfigProvider хранит настройки в DataStore, читает ключи и формирует общий INI
через buildConfigText/buildConfigFromServers. Callback-ключи вроде `node.name` и `db.path`
не являются именами INI-секций. Native config API копирует таблицу callbacks по значению;
функции и их окружение остаются у провайдера. Init без config и destroy снимают прежнюю
регистрацию; смену провайдера нужно сериализовать с cfg_get_*. Минимальный пример INI:

```ini
[global]
my_node_name=Android
# При отсутствии ключей начальный запуск сгенерирует X25519-пару.
db_path=/data/user/0/com.utun.chat/files
db_sync_enabled=1
chatserver_enabled=1
auto_sockets=android

[gui]
utun_enabled=0

[allowed_keys]
allow_all=yes

[ntp]
enabled=no

[debug]
chat=info
socket=info
```

Секции `[server:<имя>]` с addr/type/transport/netif строятся из интерфейсов устройства.
В manual пустой список остаётся пустым; автоматическое обнаружение не включает отключённые пользователем сокеты.
Ошибка перечисления интерфейсов отклоняет новый снимок до замены файла и сохраняет прежний runtime config.
Флаг временного IPv6 сопоставляется по 16 байтам адреса с /proc; недоступность /proc на Android
логируется один раз и оставляет пригодные IPv6-адреса. Кэши overlay/microphone/stereo меняются после DataStore commit.
NetworkChangeMonitor передаёт новый INI в updateSockets: uasync согласует сокеты через
`auto_socket_reload_config`, сохраняя ядро/чат. В auto_sockets=android общий
`auto_socket_on_network_change()` не запускает scan: нужен именно reload из Kotlin.
Настройки голоса, рации, stream, standby и хранения передаются в `[chat]`,
UDP-лог — в `[log_udp] ip/port`; полный набор задаёт ConfigProvider.
При наличии keys.conf startup/restart строго проверяет сохранённую пару; ошибка запрещает запуск
с событием STARTUP_FAILED. Android предлагает явную замену при непригодном ключе и повтор при ошибке доступа.
Без файла используются ключи INI либо для пустого профиля атомарно сохраняется новая пара.
Для восстановления Service дожидается stop/join, затем native атомарно заменяет keys.conf в выбранном профиле.
После записи повтор запуска использует сохранённый ключ. При выключенном сервисе замена в настройках заканчивается записью.
`instance_lite_regenerate_keys()` принимает полный INI: worker сохраняет новую пару и пересоздаёт
ядро/сервисы. Ошибка до teardown сохраняет прежнее ядро; новый node_id не применяется к старым соединениям.

Сброс БД принадлежит SettingsOperations приложения. NativeLib держит тот же lifecycle mutex
на всём stop/join → очистка UI → удаление chats.db/-wal/-shm в явно выбранном каталоге → start.
При частичном отказе удаления возвращается ошибка; прежние ключи сохраняются.
UI убирает личные беседы, ответы/редактор, историю, участников и ожидания отправок удалённой БД.
Перед восстановлением проверяется живой foreground service, чтобы не создавать ядро после его остановки.

## Язык интерфейса

`AppLanguage.supportedTags` читает общий `res/xml/locales_config.xml`: имена
нормализуются в BCP47, пустые/повторные/некорректные записи отклоняются атомарно.
Parser закрывается также при ошибке. Успешный каталог кэшируется; провал lazy
логируется и не запрещает следующую попытку чтения.

`selectedTag()` возвращает requested список Android/AppCompat через запятую,
пустая строка означает системный выбор. Effective ресурсы получают отдельно
через `context()`/`text()`, в том числе вне Activity: для этого используется
[ContextCompat.getContextForLanguage](https://developer.android.com/reference/androidx/core/content/ContextCompat#getContextForLanguage(android.content.Context)).
`nativeName()` учитывает приоритетный список и именует каждый тег на его языке.

`select()` вызывается на main после создания Activity, принимает один тег
каталога либо пустую строку. true означает уже выбранный/принятый запрос,
false — отказ, а не подтверждение отката или завершения смены ресурсов.
Выбор хранит Android/AppCompat с `autoStoreLocales`, без дублирования в DataStore;
механизм хранения/пересоздания описан в
[AppCompatDelegate.setApplicationLocales](https://developer.android.com/reference/androidx/appcompat/app/AppCompatDelegate#setApplicationLocales(androidx.core.os.LocaleListCompat)).
UI перечитывает requested состояние после попытки, включая частичный отказ;
ошибки чтения и выбора показываются отдельно с возможностью повторить действие.

`applied()` на main получает effective locale и при каждом сигнале конфигурации/
создании Activity заново запрашивает обновление четырёх владельцев уведомлений:
сеть, входящий звонок/рация, текущий звонок и radio overlay. Кэша «этот locale
уже применён» нет: владелец может обработать отказ внутри себя или стать активным
после прежнего сигнала. Обновляются каналы/тексты; ядро, media и сигналы вызова
не перезапускаются. Ошибка одного владельца не прерывает остальные. При отказе
чтения конфигурации сигнал логируется и пропускается до следующего вызова.

## Каналы, приглашения и DM

Создание канала ставится в uasync; channel_id, переданный старому bridge API создания,
игнорируется: ядро генерирует ID. `connect_node(address,port,pubkey)` остался заглушкой.
Для подключения канала/участника используются общие chat_core/topo_group API.

`InviteLink.kt` получает предпросмотр через `NativeLib.decodeInvite()` — JNI вызывает
общий `src/chat/invite_link.c`. Ссылка `utun://...` содержит версию `0x07` и тип.
CHANNEL несёт `join_key` и X25519 пригласившего, SERVER — 32-байтовый `add_key`.
Далее идут REALITY-параметры и блоки публичного ключа/адресов. В Kotlin-preview
SERVER имеет `joinKey=0`. `add_key` остаётся в исходной ссылке.
JOIN передаёт исходную ссылку и request ID через потокобезопасный chat_join_port;
принятый запрос завершается CONNECT_RESULT с тем же request. Отрицательный возврат
submit означает отказ без события. Детали: `src/chat/chat_join.h`, `chat_join_port.h`.

Регистрация join_key подтверждается до выдачи ссылки. JOIN добавляет подписанного мембера,
Merkle распространяет запись; доступ к группе определяется локальной таблицей участников.
JOIN_READY добавления и READY групповой сессии — разные состояния.

LAN-приглашения хранит app-scoped `LocalDiscovery`; AppEventHandler передаёт ему
события напрямую на main, без replay ViewModel. Список устройств 61 заменяется
целиком; входящая канальная ссылка 62 только ставится в очередь показа, без JOIN.
Кадры 61..63 имеют BE-поля ID/длин, 64 содержит байт активности. Принимаются
не более 128 устройств, имена 1..127 UTF-8 байтов без ASCII control, ссылка до
1100 байтов; входящая UI-очередь ограничена 8 приглашениями. Повреждённые кадры,
невалидный UTF-8, нулевые/повторные ID устройств не заменяют прежнее состояние.

Одна исходящая LAN-отправка на приложение хранит request и снимок адресата.
Событие 63 применяется только к pending с совпавшими request и peer; статусы
0/1/2 означают failed/received/forbidden. ACK подтверждает приём ссылки, не JOIN.
Отказ/исключение JNI завершает pending ошибкой. Stop очищает устройства и завершает pending ошибкой,
сохраняет уже полученные приглашения; поздний ACK не возобновляет завершённый запрос.

Android MulticastLock следует эффективной активности native discovery, а не
видимости Activity. Владелец сохраняется до configure/acquire и при ошибке
query/release: следующий ACTIVE/STOP повторяет операцию с тем же lock.
В логе указаны точный этап и сохранён ли владелец; на повторном ACTIVE удерживаемый
lock не захватывается ещё раз. Постоянный таймер повторов здесь не запускается.

`InviteLinkDialog` получает candidates/link/pending от владельца генерации.
Он показывает QR или текст, запрашивает LAN-снимок и отображает результат
последней отправки вместе с её адресатом. Закрытие не отменяет pending-доставку.
Ошибка QR логируется без ссылки, текст остаётся доступен; ошибка clipboard
показывается в диалоге. Смена ссылки сбрасывает подтверждение копирования.

`JoinChannelDialog` автоматически передаёт валидную ссылку из текста/QR/clipboard
в callback запуска; сетевой результат показывает владелец join. Ошибка clipboard
не блокирует ручной ввод, ошибка запуска разрешает повторить ту же ссылку.
QR preview владеет пассивной `QrScannerSession` до вызова start. Она снимает только
свои CameraX use cases; ImageProxy закрывается после завершения ML Kit Task,
включая отказ регистрации listener. Dispose отменяет доставку результата, но ждёт
завершения распознавания перед закрытием scanner. Ошибка SDK cleanup сохраняет
владельца независимо от composition и повторяется на main с backoff 250 мс..30 с.

DM-команды используют conv_id и поколение беседы. История приходит событием
CHAT_EVT_DM_MESSAGES, список — CHAT_EVT_DM_CONVERSATIONS. Frontend отбрасывает
ответы старого поколения/другой беседы. Voice/video принимают target `dm:<conv_id>`;
отдельные dm_* функции принимают conv_id без префикса. Общее устройство DM:
`../../../doc/dm_arch.md`.

## Вложения и media

Канальные text/file/photo используют chat_core_submit_trampoline. reply_to — ровно
32 байта исходного message_id, нули означают отсутствие ответа. request_id связывает
публикацию с CHAT_EVT_MSG_SUBMITTED; успешная постановка не означает доставку пиру.
Обычный файл, MP4 и JPEG/PNG передают владение временным cache общему worker;
он удаляет источник также при отказе подготовки. Дополнительные GIF/BMP/WebP канала
остаются у caller до результата асинхронной индексации. Фото здесь не декодируется и не масштабируется.

voice_recorder принимает int16 mono 48kHz, отсоединяет PCM/compressor при stop и
передаёт общий attachment_send. Запись короче 500мс по монотонным часам отбрасывается.
Готовый MP4 передаётся без транскода; при принятом video-запросе cache-файл принадлежит
задаче и удаляется также при ошибке/отмене. То же правило у отдельной DM-отправки файла.
При отказе постановки файл остаётся caller. Generation защищает публикацию в DM;
reply_to канала и request_id канала/PM фиксируются до начала подготовки.
ConversationComposer сохраняет draft, URI и тип вложения через пересоздание Activity;
поколение PM входит в сохранённый target. Возврат picker и восстановленный выбор
проверяются против текущего адресата. UI владеет cache до передачи задачи ViewModel,
отмена копирования удаляет непереданный cache в NonCancellable. Ошибка edit/picker
показывается ресурсом интерфейса; ошибка cleanup логируется отдельно.
После передачи ViewModel владеет всем пакетом «вложение + текст», продолжает обе части
после уничтожения Activity и сохраняет отдельные результаты. Composer восстанавливает token,
ревизию текста и ID выбора: подтверждение очищает только свой прежний draft/URI.
Reply снимается после сохранения обеих частей. Повторный пакет тому же target до завершения запрещён.
SAF export в канале и PM сообщает об отказе запуска picker/записи. Null output stream
не считается успешным сохранением; ожидающий путь переживает пересоздание Activity.
PM text передаётся JNI как обычные UTF-8 bytes, generation и ненулевой request ID.
Успешный enqueue не очищает draft: composer ждёт MSG_SUBMITTED после транзакции истории/outbox.
При stale generation/SQL error draft остаётся, отказ очереди завершает ожидание сразу.
Тот же MSG_SUBMITTED подтверждает commit PM-файла, фото, видео и голоса; не готовность
первого этапа подготовки и не доставку пиру. Ошибка/отмена каждого этапа даёт отрицательный результат.

ChatRepository применяет авторитетные JSON-снимки целиком: null означает отказ,
а пустой список — корректный пустой снимок. ViewModel сохраняет прежние каналы,
историю и сокеты при ошибке и проверяет владельца/revision после фоновых запросов.
SERVICE_STOPPED инвалидирует старый repository до следующего DB_READY.

Звонок и рация используют общий C голосовой стек. Kotlin владеет AudioRecord/AudioTrack,
feed/pull workers и маршрутом Android Telecom; DSP и сетевой сигналинг разделены.
Вызовы lifecycle DSP выполняются после остановки его внешних feed/pull callers.
Контракты PCM/PTS: `src/call/call_audio.h`, `src/radio/radio_audio.h`, `jni_bridge/android_jni_bridge.h`.

CameraRecordScreen сохраняет CameraCaptureResources до захвата и снимает только свои
use cases. Cache фото живёт до OnImageSaved/onError, видео — до Finalize; discard/выход
UI сначала запрашивает закрытие записи и ждёт writer перед удалением файла.
Ошибки Recording.close, unbind, отключения ориентации и удаления независимы и сохраняются
в process registry с retry. Поздний callback не восстанавливает UI и не удаляет переданный sender файл.
Pause/resume подтверждаются событиями CameraX; разрешения перечитываются при ON_RESUME.
VideoFileTracks проверяет MIME/геометрию готового MP4; MediaExtractor остаётся до release ACK,
ошибка чтения сохраняется при дополнительной ошибке cleanup. Контракты CameraX:
[Recording.close](https://developer.android.com/reference/androidx/camera/video/Recording#close()),
[PendingRecording.start](https://developer.android.com/reference/androidx/camera/video/PendingRecording#start(java.util.concurrent.Executor,androidx.core.util.Consumer)).

Локальный `VideoPlayerScreen` сохраняет пассивный VideoFilePlayer до start. UI отражает
результаты MediaPlayer start/pause/seek, ошибка команды завершает playback и оставляет
доступным закрытие экрана. Пользовательская пауза сохраняется при пересоздании Surface.
TextureView и player держат отдельные leases output; неудавшийся release MediaPlayer
сохраняет его ссылки до SDK ACK с retry 250 мс..30 с. Поздний destroy прежнего texture
не отключает новый output, callbacks после dispose не запускают playback/навигацию.
Metadata читаются в IO через отдельный владелец retriever; ошибка release сохраняет
его для повтора и не подменяет исходную ошибку read. Поворот и панели окна возвращаются
к прежним значениям при выходе. VideoFilePlayerTest/VideoFileMetadataTest проверяют
эти контракты на управляемых SDK-границах; аппаратное декодирование в JVM не выполняется.

ConfigProvider владеет SettingsOperations: изменения с неизменным снимком аргументов
принимаются на main, последовательно сохраняются и затем отправляют runtime-команды.
Закрытие/пересоздание Settings не отменяет уже принятые записи. Back перед restart и
открытие вкладки ждут очередь; ошибка записи остаётся видимой и не отправляет live-обновление.
Ошибка Flow чтения сохраняет последнее значение и повторяет подписку с backoff.
Первоначальный отказ чтения не открывает редактор с defaults; тяжёлые audio/network чтения идут через IO.
Тест микрофона принадлежит своей вкладке: dispose запрашивает асинхронный cleanup,
поздняя выдача разрешения не запускает захват после ухода UI. Очистка теста не отменяет запись сообщения.

TelecomCallManager на main регистрирует self-managed account и connections по call ID; lookup разрешён workers.
Поздний/дублирующий Connection не заменяет живого владельца и не возвращается framework как новый активный вызов.
Ошибки markDisconnected не прерывают destroy. Неудавшийся destroy сохраняет отдельного retiring owner
до ACK; Handler повторяет очистку, явный remove/requireReleased работают также после отказа очереди retry.
Этот barrier запрещает новое media/core teardown. Восстановление рации ждёт освобождения сервиса и Telecom.
ConnectionService проверяет ненулевые call/peer IDs и ловит ошибки extras/SDK создания; controller получает failure по известному ID.
UtunConnection подтверждает маршрут callbacks SDK: request outcome не заменяет фактический endpoint.
Последнее желание пользователя сохраняется до завершения запроса, включая позднее подтверждение старого маршрута.
После disconnect системный ввод, routes и старые outcomes игнорируются; ошибки callbacks не выходят в SDK.
Hold не рекламируется, поскольку приложение его не реализует. Решение о fallback/завершении при Telecom failure принимает controller.

Прямое превью Camera2 передаёт UI, ожидающей команде и каждой camera session отдельный
`StreamPreviewTarget` на общий Surface/SurfaceTexture. retain создаёт новую ссылку,
release одной ссылки идемпотентен; идентичность источника проверяется через sameSource.
Ошибка listener отменяет предложенную ссылку, queued receiver пропускает released
target. Detach/close продолжают освобождение независимо от ошибки доставки команды;
поздний update не активирует закрытое превью. Последняя ссылка передаёт физическую
очистку process-владельцу: Surface и texture подтверждаются независимо, retry идёт
через 250 мс..30 с. При отказе Handler повторный release завершает её явно.

ProximityManager сохраняет прежний sensor listener
и его manager при отказе unregister: start ждёт cleanup, Handler повторяет его через
250 мс..30 с, повторный stop работает после отказа таймера. Старая регистрация и
повреждённые показания не дают ложного FAR; после lookup failure start повторяет поиск.

`CallAudioRouter` сохраняет device callback и каждый полученный Bluetooth proxy до
успешного unregister/close. stop инвалидирует callbacks и снимки устройств сразу,
очищает независимые ресурсы и бросает ошибку при неподтверждённом результате.
Собственные communication device/SCO-заявки отмечаются до API и снимаются отдельно;
перед передачей в Telecom глобальный маршрут должен быть освобождён. Повтор start
сначала завершает прежнюю очистку, сохраняя заранее настроенную Telecom Connection.
Отказы удерживаются process-ссылкой; Handler повторяет очистку через 250 мс..30 с.
При отказе Handler владелец сохраняется для явного stop/start. Принятый запрос
getProfileProxy ожидает callback; поздний proxy после stop также получает владельца
и повтор close при ошибке. При активном новом маршруте очищаются лишь старые proxy.
Дубликат callback освобождённого/заменённого proxy не делает его текущим снова.

`MediaAudioSession.close` требует отсутствия потребителей и сразу запрещает новые
подписки, outputs и refresh. Hardware и router очищаются независимо; ошибка оставляет
close повторяемым и сохраняет executor до подтверждения обеих частей. Пока close
не завершён, awaitIdle также отвергает передачу ресурсов звонку.

`RadioCallController` на main управляет адресным приглашением участника в обычную
рацию канала. Это отдельный сигналинг `src/radio/radio_call.h`, без отдельного
аудиозвонка. START резервирует busy до первого события ядра: быстрое повторное
нажатие не создаёт второй запрос, а CoreActivity удерживает активность также
на этом промежутке. Отказ очереди освобождает резерв, reset отменяет pending.
Неготовое ядро сообщает недоступность; занятая попытка или P2P-звонок — занятость.

Событие 65 содержит ровно 27 little-endian байтов: id/group/peer по 8, затем
phase/reason/incoming по 1. ID ненулевые; Long хранит все биты uint64, строка
канала — десятичный uint64. Проверяются диапазоны и сочетания роли/фазы/причины,
а у своей попытки — вся тройка и incoming. Посторонние/устаревшие события не
меняют живую попытку и не публикуют результат от старой. При недоступных именах
repository UI использует ID, READY по-прежнему отправляется.

Входящий сначала публикует оповещение и отправляет READY. Ответ из уведомления
ждёт onStart Activity, разрешение микрофона перепроверяется; callbacks старой
попытки не принимают новую. Accept публикует PREPARING/activation, Activity
включает обычную рацию и возвращает activated(id, success). Только тогда входящий
отправляет ACCEPT либо FAIL и ждёт FINISHED. У исходящего ACCEPTED запрашивает
локальную активацию; её успешное завершение сразу снимает карточку. Поздние
RINGING/WAIT_DECISION/повторный FINISHED не откатывают исходящий PREPARING.
CONTINUE разрешён только своей WAIT_DECISION-попытке и сохраняет id.

Отказ/исключение JNI логируется с action/id/group/peer. Неотправленный READY,
ACCEPT/FAIL или CONTINUE завершает локальную попытку; DECLINE/CANCEL очищает UI
независимо от очереди. Блокирующая захват трансляция отправляет FAIL и закрывает
оповещение. Ошибка CoreActivity не прерывает сигналинг и независимую очистку:
CoreActivity сохраняет намерение для следующего sync. Deadline приглашения
остаётся в ядре; приложение владеет только отменяемой задержкой 2,5 с удаления
ENDED-карточки. Новая попытка/reset отменяет её, callback проверяет сам снимок.

Завершение приглашения не выключает обычную рацию. StateFlow повторяет текущие
state/activation для пересозданной Activity; история завершённых вызовов не
восстанавливается. RadioCallDialog отображает этот снимок и передаёт действия
контроллеру, не управляя deadline/аудио. Результат видимой Activity показывается
Toast, в фоне — отдельным уведомлением 1006, без замены уведомлений сервисов.
Отказ уведомления/локализации не мешает удалению карточки.

`RadioPttHolds` на main хранит по одному ручному удержанию BUTTON/OVERLAY/VOLUME.
Повтор press того же источника идемпотентен; несколько volume-клавиш агрегирует
Activity и отпускает общий VOLUME после последней клавиши. Release возвращает
true только при удалении последнего владельца; неизвестное отпускание не гасит
чужую передачу. Cancel возвращает отсутствие владельцев также для уже пустого
набора: это позволяет отменить хвост передачи после release. Null снимает всех.
Агрегатор не управляет VAD, аудио или таймерами; их владелец — RadioAudioEngine.

Ручные talkBegin/talkEnd/cancelTalk в RadioAudioEngine проверяют main-поток до
изменения владельцев/таймера. Неактивная рация отказывает с WARN; отсутствие
RECORD_AUDIO запрашивает разрешение без сохранения удержания. После выдачи
пользователь нажимает снова. Принятый PTT — асинхронный запрос, не подтверждение
первого PCM/TX: эти этапы видны отдельно в логах capture/native и transmittingGroup.

Последний talkEnd оставляет 250 мс хвоста; новое нажатие снимает его таймер,
cancel последнего владельца снимает хвост сразу. Если postDelayed отказал,
ERROR содержит source/group/generation; движок немедленно снимает ручной запрос
и ставит согласование capture в io. Сообщение scheduled появляется только при
успешной постановке. Когда захват больше не нужен, подписка закрывается и native
capture останавливается. При работающем измерителе/автоматике захват сохраняется,
а отпускание PTT проходит общей очередью управления/PCM до native radioTalkEnd.
Снятие ручного PTT само по себе не выключает автоматическую передачу.
Stop/смена группы сбрасывает удержания и прежний хвост; io проверяет generation.

Start/stop рации также вызываются на main. Start отвергает нулевую группу,
проверяет размер буфера AudioTrack и выполняет на io сначала очистку прежних
ресурсов, затем setup. Даже отказ первоначальной очистки завершает onReady(false)
на main. Отложенный результат перепроверяет generation: он не подтверждает и
не очищает новый запрос. Ошибка текущего запуска снимает activeGroup после
callback, если callback не начал новый start/stop. Это позволяет restoreAfterCall
сначала выключить native-членство неудачно восстановленной группы. Исключение
пользовательского callback логируется и передаётся main-caller. Если main-очередь
отвергла доставку, callback на io не вызывается, новые аудиоресурсы очищаются с ERROR.

Stop снимает логическое участие сразу; его Future завершается после io-очистки
либо возвращает первый критический отказ. Ждать Future нужно вне main. Receiver,
capture, output lease/AudioTrack и native DSP очищаются независимо. Перед остановкой
DSP движок дожидается своих capture/play workers; interruption не пропускает join,
флаг восстанавливается после очистки. Ссылка на отказавший receiver/lease/track
сохраняется для следующего stop/start. При ошибке detach lease трек ещё принадлежит
MediaAudioController и не получает release. Успешно очищенные ссылки сбрасываются;
native audio stop остаётся идемпотентной завершающей попыткой после join.
Новая группа не заменяет владельцев при неподтверждённой очистке.

Capture сохраняет подписку до native start и отдельно помнит возможное владение
encoder. Даже исключение после частичного эффекта API получает rollback; неудачный
native capture stop сохраняется для повторной попытки независимо от закрытой
PCM-подписки. Подтверждённый capture/audio stop сбрасывает наблюдаемый TX и режим
детектора. Отказ автоматического детектора закрывает автоматический захват;
ручной PTT или измеритель могут продолжить работу в режиме OFF.

Оба worker сохраняются до Thread.start. Capture принимает ровно 960 PCM frames
на канал; старые кадры отбрасываются с границей перед следующим свежим кадром.
Ошибка poll, PTT-команды, границы или feed логируется с этапом/группой/кадром,
после чего io закрывает захват, сохраняя приём. False от feed означает отказ
native-захвата; это не потеря сетевого пакета. Новый пользовательский запрос
может повторить захват после устранения причины. Очистка проверяет подписку и
generation; rollback старого запуска не снимает PTT нового канала.

Playback использует переданные при запуске track/группу/поколение. Pull допускает
0..1920 стереосэмплов с выравниванием по двум каналам, дополняя короткий результат
тишиной. Положительные частичные write передают остаток того же кадра; ноль,
отрицательный, слишком большой либо нечётный результат работающего write — ошибка.
Ошибка head/pull/write/sleep освобождает свою сессию на io до уведомления main;
отказ main-очереди не удерживает ресурсы. Identity трека защищает новую сессию
от запоздалой очистки. Interruption при штатном stop и результат разблокированного
им write считаются отменой; interruption работающего worker логируется как ошибка.

Screen receiver удерживается до вызова registerReceiver, включая частичный отказ
IPC. IllegalArgumentException от unregister подтверждает отсутствие регистрации;
прочие ошибки сохраняют владельца. Доставленный после замены receiver проверяет
identity и исходное поколение до изменения состояния экрана. Проверка settings ACK
имеет собственный отменяемый Runnable: ACK, новая настройка и stop снимают его.
Отказ постановки ACK-timeout логируется, захват по-прежнему ждёт подтверждения.
Отказ таймера фоновой автоматики закрывает её окно; ручной PTT/измеритель могут
сохранить захват в режиме OFF. Логи содержат группу, поколение, этап отказа и join.

Reconfigure инвалидирует прежний settings request до чтения новых настроек.
Ошибка чтения/применения/обновления capture логируется с этапом; приём остаётся
включённым, capture ждёт повторной перенастройки и нового ACK. Прежний ACK не
возобновляет захват после отказа. Частично созданный capture получает rollback.

`RadioTalkState` хранит отдельный снимок говорящих для активной группы. Включение/
смена канала создаёт новый снимок, выключение устанавливает groupId=0. Событие 43
разбирается строго как 19 little-endian байтов: group/node/stream/on; ID ненулевые,
stream=0..65535, on=0/1. Неверное событие возвращает null и логируется ViewModel.
START/STOP обновляет только набор node ID своей группы; чужая группа и повтор
состояния сохраняют прежний объект. В C radio_talker_on заменяет stream одного
узла без STOP старого потока, поэтому stream не является ключом UI-набора.
Индикатор речи не зависит от обновления таблицы участников или локального mute;
выключенная рация игнорирует события. Эти снимки не управляют ручным PTT.

Live stream bridge владеет одним publisher и независимыми viewers поверх общих
`stream_publisher`/`stream_viewer`. UI использует session IDs, не inst/ua.
Данные идут через pull, а каталог источников — через CHAT_EVT_STREAM_SOURCES.
Pull возвращает owned буфер записей с 28-байтовым BE-заголовком; освобождение — u_free.
Demand ограничивает батч viewer; GAP/CLOSED требуют сбросить прежний frontend-буфер.
Точный формат, лимиты и срок жизни signal: `libutun_lite/stream_bridge.h`.

Viewer сохраняет пассивные video/DSP owners до их start. GAP очищает прежние записи
в StreamReadQueue, сохраняя последующий KEY того же batch. Ошибка reset не теряет старый DSP handle.
Video close ждёт worker до 250 мс и подтверждает release decoder; отказ запрещает fallback на другой codec
и передачу Surface обратно UI. Audio worker независимо повторяет native stop, video/DSP cleanup,
затем закрывает signal и выполняет callbacks освобождения Surface. Успешные операции не повторяются.
Незавершённый viewer остаётся в process-реестре; native stop/restart/destroy запрещены до его cleanup.
Микшер удерживает единственный AudioTrack до успешного close, включая частичную initialize/play.
Новые inputs ждут освобождения прежнего выхода; awaitIdle ограничивает join 250 мс за попытку.
Ошибки listener detach сохраняют отдельную неактивную регистрацию до SDK ACK, включая failed attach без lease;
поздний routing callback не читает released track. Повтор очистки выполняет media control executor.
Viewer UI передаёт физический Surface/texture cleanup идемпотентному StreamPreviewTarget.
Прямой CameraPreviewView снимает свои leases при onRelease, не ожидая destroy TextureView;
отдельная camera lease сохраняет output до ACK сессии. Поздний destroy возвращает false
и не закрывает texture средствами framework. Смена имени источника обновляет плитку без смены reader.

Publisher service сохраняет пассивный capture до start, CPU lock — до acquire, foreground-флаг — до IPC.
Capture stop=true подтверждает camera/session callbacks, encoder/input Surface, ориентацию, подписку микрофона и Opus worker.
False сохраняет engine у сервиса; ошибки SDK/native DSP повторяются независимо. Таймаут camera close не подменяет ACK.
Смена/восстановление режима ждут полного video cleanup; stop отменяет продолжение pending switch.
FGS cleanup переживает onDestroy, сохраняет ближайший retry и только после capture ACK отправляет native FIN.
Обычная остановка ждёт END до 4 секунд; abort/destroy переходят сразу к освобождению lock/tokens/foreground/stopSelf.
Controller остаётся STOPPING до подтверждения сервиса, включая незавершённый старт FGS; native teardown проверяет этот barrier.
Null от startForegroundService считается отказом. Старые IDs callbacks/notification не управляют новой публикацией.
Malformed TX snapshot сохраняет прошлую статистику; ошибка poll завершает publisher через тот же cleanup.

## Фон и диагностика

`CallService` на main владеет audio/video движками, PARTIAL_WAKE_LOCK, power receiver,
foreground-уведомлением 1002 и собственными PendingIntent. Запуск сначала
подтверждает `phoneCall|microphone`, CPU lock и receiver, затем сообщает serviceReady.
Ошибка канала в onCreate логируется; onStart повторяет её до захвата ресурсов.
Ошибка диагностического чтения канала не отменяет успешный create. Null от
startForegroundService считается отказом, а отменённый запрос завершает
foreground-handshake без микрофона/CPU lock и останавливается.

Владельцы сохраняются до IPC: частичные acquire/register/promote получают rollback.
startAudio проверяет свой ID и CPU lock; повтор не заменяет живые движки.
Ошибка audio start/video constructor откатывает сервис. Неудачный camera promote
сохраняет последний подтверждённый флаг; caller не меняет capture при false.
Notification update/язык и power diagnostics ловят ошибки независимо от media.
Токены open/END имеют identity своего звонка, повторно используются при обновлении
и отменяются при shutdown. Старый END не завершает другой звонок; после stop
уведомление не публикуется. Chronometer использует startedAtElapsedRealtime из controller.

Shutdown независимо пытается остановить video/audio, снять receiver, CPU lock и
токены. Успешные операции не повторяются, отказавшие владельцы сохраняются.
IllegalArgumentException от unregister подтверждает отсутствие receiver; отказ
release после эффекта подтверждается следующим isHeld=false. Foreground и
stopSelf(lastStartId) снимаются после успешных stop API обоих движков. Последний
owned call ID сохраняется отдельно для serviceLost после локального освобождения.
Audio stop снимает active и callbacks, пытается разблокировать capture и ждёт каждый
worker до 250 мс за попытку; playback также ждёт свой reporter. Живые потоки сохраняются,
main не ждёт device lock занятого worker. Маршрут и AEC откладываются до освобождения
lock с проверкой call ID, диагностика владельцев не обращается к SDK устройств.
устройства освобождаются после завершения обоих workers. Ошибки release оставляют
AudioRecord/CallAudioOutput у движка, включая частичный запуск и отказ после эффекта.
Остановка router, удаление communication listener и восстановление собственного AudioManager mode/speaker
повторяются независимо; после передачи режима в Telecom fallback его не восстанавливает.
Незавершённая очистка бросает ошибку сервису и запрещает новый start. Native audio stop
вызывается после подтверждения этих операций, его отказ также допускает повтор.
Сервис сначала сохраняет пассивный CallVideoEngine, затем вызывает start: частичный запуск имеет владельца для rollback.
Video stop возвращает true только после RX join/освобождения decoder, playout и Surface,
SDK-подтверждения закрытия камеры/сессий, освобождения Images/readers/encoder и native close.
До этого false сохраняет engine у сервиса; исключения RX/post допускают повтор stop.
Отказ RX не прерывает независимую очистку TX, но запрещает закрывать native port.
TX хранит ресурсы до подтверждения SDK, повторяет отказы с backoff и лишь затем завершает worker.
onConfigureFailed уже подтверждает закрытие сессии: ждать дополнительного onClosed нельзя.
Если создание сессии бросило исключение, её выходы сохраняются до callback сессии или закрытия устройства.
VideoCodecResources отдельно удерживает codec и input Images при отказах release,
включая ошибку конструктора и аппаратной пробы. Его retry не обращается к native-ядру.
RX join ограничен 250 мс за попытку; decoder не теряется при отказе release, его Surface
остаётся до подтверждённого освобождения codec. Остальные Surface и playout очищаются
независимо. Очередь Surface владеет callback release до успешного возврата, включая
позднюю передачу после stop; callback должен допускать повтор после исключения.
CallVideoTexture передаёт callback идемпотентного StreamPreviewTarget: частичный отказ
физического release не теряет второй ресурс. Старый destroy не отсоединяет новый output.
CallScreen фиксирует ориентацию окна, immersive возвращает прежние панели;
Bluetooth permission связан с call ID, camera permission — с конкретным engine.
Process-ссылка и Handler сохраняют cleanup после выхода RX, без переноса уже назначенного
раннего таймера. Отказ post не теряет владельцев и допускает явный stop.

Shutdown возвращает Deferred, успешный результат означает подтверждение всех
владельцев сервиса. При отказе process-ссылка удерживает экземпляр даже после
onDestroy, Handler повторяет попытку с задержкой 250 мс..30 с. Отказ post не теряет
владельца: shutdown/requireReleased повторяет очистку явно. Отмена Deferred
не подтверждает освобождение. CallController снимает карточку независимо от
retry, blocksMedia запрещает другой audio, а восстановление рации ждёт Deferred
и mediaPreparation. NativeLib перед stop/restart требует подтверждённый сервис
и завершённую подготовку media; иначе возвращает ошибку без разрушения ядра.

CallController проверяет приём каждой команды сигналинга в native-очередь.
Отказ start/accept завершает локальный запуск, отказ continue оставляет выбор для повтора.
Непринятые decline/hangup сохраняются по call ID и повторяются с задержкой 250 мс..30 с;
CALL_ENDED/DECLINED отменяет повтор. До приёма завершающей команды blocksMedia запрещает
новый вызов, восстановление рации ждёт также эту очередь и завершение Telecom.
Reset очищает повторы до остановки ядра. Ошибки записи маршрута и показа результата
логируются независимо от завершения звонка и очистки сервиса.

`RadioOverlayService` на main обеспечивает foreground-режим микрофона при активной
рации и включённом оверлее либо автоматике. Сам PCM захватывает RadioAudioEngine;
разрешённый экран/фоновое окно по-прежнему проверяются движком. Старт FGS
откладывается до видимой Activity. Разрешения запрашиваются только явной командой
пользователя; отказ launcher снимает ожидание и допускает повтор. Ошибки настроек,
проверки разрешения и обновления языка логируются с причиной.

Starting удерживается от запроса ОС до onCreate; stop в этом промежутке ставит
pendingStop. Сервис сначала завершает foreground-handshake, затем запрашивает
остановку. Новый запрос во время stop ждёт onDestroy. Отказ stopService/исключение
не фиксирует pendingStop навсегда и допускает повтор. Ошибки создания канала,
promote и получения WindowManager попадают в общий rollback запуска. Уведомление
оверлея имеет отдельный ID 1005, без замены звонкового FGS (1002), трансляции (1004)
и результатов вызовов (1003/1006). Обновление языка не публикует уведомление остановленного владельца.

Окно и LayoutParams сохраняются до addView: исключение после эффекта API получает
remove. Успешный remove либо IllegalArgumentException подтверждает отсутствие окна;
прочие отказы сохраняют ссылку и отключают обработку касаний. IsShown означает
добавленное либо ещё не подтверждённо удалённое окно. После onDestroy такой
владелец остаётся в currentService, и sync повторяет очистку до замены новым.
Повторный/поздний onDestroy не гасит новый сервис. Touch/layout callbacks проверяют
identity окна и владельца; старый callback позиционирования не сбрасывает флаг нового.

Обычное отпускание OVERLAY оставляет хвост 250 мс. Два коротких тапа фиксируют PTT;
следующее нажатие снимает latch. Drag начинается только в первые 300 мс удержания,
снимает PTT с обычным хвостом и сохраняет выбранную позицию до нового окна.
ACTION_CANCEL, ошибка перемещения, remove и stop отменяют OVERLAY сразу, сохраняя
BUTTON/VOLUME. Ошибка окна и отзыв разрешения также останавливают ненужный FGS,
если автоматическая передача его не удерживает. При разрешённой автоматике
сервис может продолжать работу без плавающей кнопки. Ошибка observer не уходит
в необработанное исключение: свой PTT/окно закрываются, сервис запрашивает stop.

`IncomingCallManager` на main владеет одним входящим оповещением P2P/рации.
Повтор `show` с теми же ID/каналом/пиром/видом обновляет только уведомление.
Замена сначала снимает прежнее оповещение. Ошибки wake lock и уведомления
не мешают запуску `RingtonePlayer`; обновление языка не повторяет звук,
пробуждение или full-screen intent.

`stopRinging` сразу снимает `active`, затем вызывает остановку `RingtonePlayer`
и независимо очищает wake lock, кнопки и уведомление. Receiver проверяет полный
64-битный call ID и вид P2P/рация: уже доставленный DECLINE после stop/замены
игнорируется. PendingIntent различаются URI с видом/ID, а не усечённым requestCode.

Wake lock один, без подсчёта ссылок, с таймаутом 30 секунд. Менеджер хранит его
до успешного `isHeld`/`release`; если ОС уже сняла lock, второй release не нужен.
При отказе старый lock не перезаписывается новым: следующий звонок получает
звук/уведомление, но пропускает новое пробуждение до очистки прежнего lock.
Каждый PendingIntent хранится с владельцем до успешного `cancel`. Необходимость
отмены уведомления отмечается до вызова `notify`: исключение может прийти после
эффекта в ОС, поэтому ошибка публикации не освобождает от последующей отмены.

Повтор `stopRinging`, `init` или `refreshLanguage` без `active` повторяет
незавершённую очистку, не трогая уже успешно очищенные ресурсы. Пока канал
не подготовлен, `init`/`refreshLanguage` также отменяет возможное уведомление
прежнего процесса; отказ создания канала не блокирует очистку. Отдельного
таймера повторных попыток нет. При активном оповещении обновление языка
сохраняет его кнопки, звук и lock. Логи `InCall` содержат ID/вид вызова для
кнопок, фазу/идентификатор lock и признак оставшейся очистки после stop.

`RingtonePlayer` читает системный режим звонка при `play`: NORMAL даёт звук и
вибрацию, VIBRATE — вибрацию, SILENT — ни одного эффекта. API 28+ использует
зацикленный Ringtone, API 26–27 — MediaPlayer с профилем звонка после выдачи
audio focus. Отказ одного эффекта не запрещает другой. Потеря focus запрашивает
остановку звука, сохраняя вибрацию; последующий GAIN не перезапускает мелодию.

`stop` сначала запрещает callbacks звука и снимает таймер вибрации. Владельцы
Ringtone/MediaPlayer/Vibrator остаются до успешного stop/release/cancel,
а исходная пара AudioManager/AudioFocusRequest — до GRANTED от
[abandonAudioFocusRequest](https://developer.android.com/reference/android/media/AudioManager#abandonAudioFocusRequest(android.media.AudioFocusRequest)).
Новый звук не запускается, пока остаётся старый плеер или focus; новый импульс
не запускается при незавершённой отмене вибрации. Повтор `stop`/начало `play`
повторяет только незавершённую очистку. IncomingCallManager вызывает её также
при повторных stop/init/обновлении языка без активного оповещения.

Вибрация состоит из конечных импульсов 1000 мс с повторным запуском через 2000 мс.
Отказ импульса или постановки таймера отключает повторение и запрашивает cancel.
Даже при ошибке cancel новые импульсы не ставятся в очередь. Ошибка API после
возможного эффекта сохраняет владельца для следующей попытки; отдельного таймера
очистки нет. Логи `Ringtone` показывают идентификатор ресурса, причину отказа,
успешное освобождение и оставшиеся sound/focus/vibration после запроса stop.

`CallRingback` на main владеет одним зацикленным MediaPlayer с профилем MEDIA:
connecting и ringing выбирают разные ресурсы. Повтор активной фазы сохраняет
плеер. Смена фазы, stop, error и неожиданный completion сначала запрещают
callbacks, затем запрашивают release. При исключении ссылка и прежний resource ID
остаются для следующего stop/start. Пока release не подтверждён, новая фаза
пропускается с WARN; запоздалые callbacks не повторяют очистку и не трогают
замену. После восстановления start может заново запустить ту же фазу.
Пропущенный start автоматически не воспроизводится; таймера повторов нет.

`NotificationSound` на main запускает отдельный однократный плеер с профилем
уведомлений; сигналы разных сообщений могут перекрываться. Объект приложения
сильно удерживает каждый плеер от получения из create до успешного release,
включая настройку listeners и проигрывание: это предотвращает преждевременное
завершение звука сборщиком мусора, описанное в
[контракте MediaPlayer](https://developer.android.com/reference/android/media/MediaPlayer).
ID звука — канал либо `dm:<ID>`; enabled/mutedChannels — снимки настроек для
новых сигналов. Смена настроек не останавливает уже играющие сигналы.

Completion/error/ошибка настройки или запуска запрашивает release своего
плеера. Исключение сохраняет владельца с признаком незавершённой очистки;
следующий callback либо play повторяет попытку. В начале play повторяются
только такие release, включая случаи disabled/mute и последующий отказ create.
Неудача одного плеера не мешает очистить остальные или запустить новый сигнал;
ещё играющие плееры остаются нетронутыми. После успешного release владелец
удаляется, поздние callbacks игнорируются. Автоматического таймера очистки нет.
Логи Ringback/Sound отмечают успех только после возврата release без исключения;
при отказе показывают ресурс/канал, идентификатор плеера и причину попытки.

`BatteryOptimizationPermission` читает power allowlist Android для текущего пакета:
true/false — ответ ОС, null — ошибка чтения сервиса/статуса с ERROR-диагностикой.
Флаг в DataStore не сохраняется и результат не кэшируется. Это исключение
из оптимизации, а не снятие всех фоновых ограничений; контракт ОС описан в
[PowerManager](https://developer.android.com/reference/android/os/PowerManager#isIgnoringBatteryOptimizations(java.lang.String)).

Компонент `BatteryOptimizationSetting` используется в первоначальной настройке
и настройках приложения. Он перечитывает статус на каждом RESUME. При null
показывает ошибку и «Повторить»: эта кнопка только повторяет чтение. При false
кнопка «Разрешить» сначала перечитывает ОС, затем открывает пакетный запрос,
а при отказе запуска — общий список исключений. Успешный startActivity означает
лишь открытие системного экрана; выдача исключения определяется после возврата.
Неизвестный результат не считается отказом пользователя. Pending запроса
переживает пересоздание UI, оба неудачных запуска снимают его с видимой ошибкой.
Настройка не перезапускает ядро и не блокирует завершение первоначальной настройки.

Android HeadlessService — foreground service с уведомлением и общим native-ядром,
а не Linux control-loop. Он запускает NetworkChangeMonitor и NetworkActivityMonitor.
Foreground подтверждается до запуска native; отказ завершает ожидание startup ошибкой.
CoreStartup ждёт ready/onReady до 10 секунд. Restart и неожиданный stop закрывают прежний
deferred готовности; новый либо сохранённый после ошибки core должен подтвердить ready за тот же срок.
Отмена отдельного await не отменяет startup/recovery. Таймаут завершает Service с диагностикой.
Уведомления и независимые части onDestroy логируют ошибки отдельно. CoreShutdown
переживает Service и повторяет отказ stop с backoff 250 мс..30 с; новый init ждёт успешный stop.
Переход UI в фон передаёт client_activity в uasync. Standby чередует ACTIVE/SLEEP;
сам event loop продолжает работать. Ожидания SLEEP будятся после перехода в ACTIVE,
сетевой activity может разбудить после min_sleep. Контракт отмены: `libutun_lite/standby.h`.

AppEventHandler передаёт каждый native event одним main-заданием и независимо вызывает
обработчики владельцев. Сбой одного обработчика не отменяет остальные и доставку ViewModel.
SharedFlow не повторяет старые команды/ACK новой ViewModel; начальный снимок читается
при DB_READY или проверке уже готового ядра. Emit при заполнении буфера ждёт на main coroutine,
без блокировки native-потока. Отказ main-поста и ошибка обработки конкретного события логируются.

AudioRecorderManager сохраняется в process registry с начала захвата либо playback до release ACK.
ViewModel.onCleared запрашивает асинхронную очистку, рабочий поток переживает UI.
Native cancel и Android release повторяются независимо; неудачный AudioTrack/decoder close
остаётся у playback worker также после completion callback. Capture cleanup не держит playback lock.
NativeLib запрещает новый voice capture во время core teardown/звонка; stop ядра и подготовка
звонка ждут освобождения этих владельцев до доступа к native recorder/звонковому I/O.

Bluetooth использует общий C BLE transport и отдельный Android HandlerThread.
Application наблюдает подтверждённый Flow настроек; вкладка меняет отдельные флаги атомарно
через SettingsOperations, включая закрытие UI. Ошибка DataStore сохраняет прежний снимок.
Разрешения и системный запрос включения адаптера имеют видимую ошибку запуска.
JNI передаёт команды в Handler и возвращает отказ, если очередь их не приняла.
Detach снимает C transport до уничтожения instance/uasync; поздние SDK-события отсекаются generation.
Отказ stopScan/stopAdvertising/GATT close сохраняет точный SDK handle в BleRadioCleanup
и повторяет release с backoff 250 мс..30 с. Новый запуск ждёт очистки, после успешного retry
последняя политика применяется повторно, если native core ещё готов.

DEBUG_* фильтруются общими уровнями/категориями. Bridge переводит их в logcat и,
при включённой подписке, Kotlin LogManager. Debug hook также пишет в UDP-буфер:
16KiB threshold либо timer 200мс, неблокирующая отправка без повторов. ERROR вызывает flush.
Target — числовой IPv4; attach/detach таймера выполняет uasync worker при каждом запуске/рестарте.
Ошибки самого UDP-логгера идут в отдельный utun-udplog, минуя debug hook.

LogManager хранит последние 100000 строк в памяти процесса, в порядке приёма.
Прямой Kotlin addLog сохраняет важность logcat, но не проходит native-фильтр
и не доставляет строку в native-файл/UDP. videoDebug/videoTrace проходят общий
video-фильтр через JNI. JNI callback получает уровни bridge 0=TRACE..4=ERROR,
а не числовые DEBUG_LEVEL_*; неизвестный уровень фиксируется предупреждением.

Строки логов передаются через `onNativeLog(int, byte[], byte[])` как обычный UTF-8:
это сохраняет кириллицу/emoji без ограничений Modified UTF-8 `NewStringUTF`.
Повреждённые UTF-8-байты Kotlin отображает как U+FFFD. На каждый вызов JNI берёт
свой local ref callback под mutex; замена/очистка global ref не инвалидирует
уже начатую доставку. Java-код вызывается после освобождения mutex. Указатель
C log callback читается/меняется атомарно; отключение не ждёт начатых вызовов.
init включает пересылку, Activity подписывается на экране логов и отключает её
при выходе; stop снимает C callback, destroy дополнительно освобождает global ref.

Ошибки JNI-доставки и повторный вход в callback того же потока фиксируются
напрямую в logcat, минуя debug hook, чтобы исключить рекурсию. Проверяются
attach, ссылки, method lookup, выделение/копирование массивов и Java callback.
Все свои local refs освобождаются; detach выполняется только после своего attach.
Исключение от собственной операции/callback описывается и снимается; исключение,
уже существовавшее при входе в logger, сохраняется, доставка этой строки пропускается.
Неудачная доставка не повторяется. Native logcat/файл/UDP работают независимо.

Экран логов показывает последние 10000 строк истории, обновляя снимок не чаще
кадра. StateFlow передаёт объединяемую ревизию, строки читаются из общей истории:
задержка UI и открытие экрана после последнего сообщения не теряют строки
в отдельной очереди событий. Лимиты истории/экрана всё равно вытесняют старые строки.
Фильтр выбирает точный уровень; пауза отключает только прокрутку, сбор продолжается.
Автопрокрутка следует последней видимой строке после фильтрации и возобновления.

## Linux headless и проверки

Каталог `headless/` — отдельный каркас: headless_main запускает select-loop и JSON-line
control socket, но не создаёт uasync/instance и не запускает чат. `-p` меняет порт,
`-l` только печатает имя файла. ping/status/connections/debug_level возвращают заглушки,
subscribe/quit управляют клиентом, ряд команд даёт not implemented. Текстовые команды
вида `send <channel> <text>` этот каркас не поддерживает. JOIN/settings требуют уже
запущенное ядро и исполнения в его потоке; каркас не заменяет рабочий headless API.

Для Linux сетевых сценариев используются основной `src/utun`,
`[chatserver] headless_control_bind=127.0.0.1:9999` и `tools/chatcli`.
Для APK: `cd tools/chatgui-android && ./gradlew assembleDebug`.
`:app:testDebugUnitTest` включает `:router-tests:test`: настоящие
IncomingCallManager/receiver/RingtonePlayer проверяются на JVM с моделями
Android-служб. Сценарии включают отказы до/после эффекта acquire/release,
notify/cancel и отмены PendingIntent, stop/release проигрывателя, request/abandon
audio focus, импульса/cancel вибрации и постановки таймера. Проверяются независимая
повторная очистка, замена вызова и устаревшие callbacks. Эти модели не проверяют
поведение системных служб/OEM на устройстве.
Для CallRingback/NotificationSound проверяются также запрет смены фазы при
неосвобождённом loop, независимость перекрывающихся сигналов, очистка без
установленных callbacks и повтор release при выключенном звуке/ошибке create.
RadioPttLifecycleTest использует настоящие RadioAudioEngine, MicrophoneCapture и
MediaAudioController с host-границами Android/JNI. Проверяется не только UI-флаг,
но и команда native capture stop либо упорядоченный radioTalkEnd при сохранённом
измерителе: отказ/обычный хвост таймера, повторное нажатие, независимые владельцы,
отмена, смена группы, RECORD_AUDIO и запрет фоновых PTT-вызовов. PCM-подписки и
workers настоящие; звукового оборудования, DSP и сетевой передачи в модели нет.
RadioAudioLifecycleTest проверяет отказы регистрации/очистки receiver и AudioTrack,
зависимость release от detach lease, interruption, ошибки native/start/settings,
отложенные и повторно входящие callbacks, восстановление после звонка, отказ
перенастройки с последующим старым ACK и отказы
таймеров ACK/автоматики. Проверяется и сохранение владельцев после частичного
эффекта API, и продолжение независимой очистки перед повторной попыткой.
RadioWorkerLifecycleTest проверяет исключения и неверные результаты pull/write,
целостность частичных PCM-записей и заполнение тишиной, запуск capture с частичным
native-эффектом, отказ feed/PTT/границы, отсутствие детектора, interruption и
отказ main-очереди. Барьеры потоков воспроизводят ошибки старых playback/feed/start
после запроса новой группы. Проверяются освобождение подписок/encoder/track и
сохранение нового PTT. Android/JNI в этих сценариях моделируются на границах;
DSP, звуковое оборудование и сетевой эфир не запускаются.
CallAudioLifecycleTest компилирует настоящие CallAudioEngine/CallAudioOutput и workers.
Проверяются отказы release до/после эффекта, частичная подготовка AudioRecord/AudioTrack,
регистрация/удаление communication listener, native stop, восстановление режима,
поздний Telecom handoff и запрет stop вне main/нулевого ID. Барьер capture доказывает,
что до выхода worker устройства/native сохраняются и новая сессия не запускается.
Android hardware и DSP моделируются; AAudio/OEM поведение на устройстве не проверяется.
`:video-tests:test` проверяет настоящий CallVideoPlayer, uAV2 KEY framing и смену поколений
на JVM. Управляемые границы MediaCodec/probe/JNI позволяют воспроизвести отказы частичного
configure/start/stop/release, Surface callback, playout destroy, join и retry Handler.
Проверяются независимое освобождение, запрет release Surface до codec, старый detach,
ACK показа/PTS и EOS drain без смены Surface. OEM/реальное декодирование остаются вне модели.
StreamPreviewLifecycleTest проверяет реальные preview/leases: потерю/частичный эффект
callback, отменённые команды, независимые и конкурентные владельцы, физические
отказы release до/после эффекта, retry Handler, detach/visibility и поздние update.
Также проверяются onRelease без destroy, позднее появление texture после dispose и destroy прежнего output.
QrScannerSessionTest проверяет настоящего владельца CameraX/ML Kit с управляемыми
use cases, Task и SDK отказами. Dispose во время распознавания, поздняя camera future,
частичный bind, отказ listener и независимый cleanup не требуют камеры для воспроизведения.
RadioCallLifecycleTest использует настоящие RadioCallController, IncomingCallManager,
CoreActivity и гудки. Управляемые Android/JNI/repository-границы проверяют все
команды сигналинга, pending START, валидацию событий/полной identity, активацию,
старые callbacks/таймеры, PHONE_BUSY/CONTINUE, uint64 ID, отказы активности и
публикации результатов. IncomingCallLifecycleTest также использует настоящий
рационный контроллер при доставке DECLINE. Радиоэфир и C-сигналинг в JVM не
запускаются; native-протокол проверяется отдельным test_radio.
CallServiceLifecycleTest в `:call-service-tests` использует настоящие CallService,
CallController, CallState, CoreActivity и router. Управляемые OS/DSP/camera/Telecom
границы воспроизводят частичные startup/stop, независимую очистку, повтор после
onDestroy, отказ очереди retry и отмену Deferred. Проверяются запрет другого
media/core teardown, ожидание восстановления рации, старая identity callbacks,
повторное использование notification tokens, camera flag и conversation clock.
Настоящие codec/камеры, Telecom и ограничения FGS/OEM в JVM не запускаются.
`:app:testDebugUnitTest` включает этот модуль как зависимость задачи, без host-классов в APK.
StreamServiceLifecycleTest использует настоящие StreamService/StreamController и управляемый capture:
частичный старт, повтор cleanup после destroy, pending SDK ACK, отказы lock/native stop/tokens/foreground,
FIN, повреждённые stats, исключение poll и старые notification IDs. StreamCaptureEngineLifecycleTest
в `:video-tests` запускает настоящий capture worker с управляемыми Camera2/MediaCodec/microphone/JNI:
поздние/старые callbacks, отмену switch, отказ encoder/Surface/subscription/Opus и запрет ACK по таймауту.
StreamVideoPlaybackLifecycleTest и StreamPlayerEngineLifecycleTest используют настоящие video/viewer/mixer workers:
cleanup decoder/DSP/signal, GAP+KEY одного batch, fallback, ограниченный join и запрет второго AudioTrack до release.
Это проверки владения и порядка операций; реальное декодирование, capture rate и OEM остаются вне JVM-модели.
TelecomLifecycleTest в том же модуле компилирует настоящие TelecomCallManager/UtunConnection/UtunConnectionService:
account/SDK failures, дубликаты/late owners, destroy и повтор, uint64 extras, malformed requests,
оба порядка endpoint outcome/confirmation, legacy masks, stale callbacks и исключения listener.
OS Telecom IPC и фактические аудиомаршруты заменены управляемыми границами.
RadioOverlayLifecycleTest использует настоящие overlay service, RadioAudioEngine,
микрофонные подписки и MediaAudioController. Host-модели service/window IPC
проверяют частичные add/remove, повторную очистку, отказы channel/promote/stop,
старые touch/layout/destroy callbacks, PTT/latch/drag, отзыв разрешений и
изолированные ID уведомлений. Settings ACK позволяет выполнять настоящие PCM
workers через управляемую JNI-границу. WindowManager/foreground restrictions,
отрисовка и OEM-поведение на устройстве в JVM не проверяются.
На Linux `:app:testDebugUnitTest` также запускает 25 fault-сценариев настоящего
JNI logger через `:app:testJniLog`; окружение и границы модели: `tests/jni_log/README.md`.
Проверки native bridge/standby доступны в `tests/Makefile.am`;
проверка независимости media workers на устройстве описана в
`../tests/stream_worker_smoke/README.md` и не проверяет сетевую доставку/UI.
