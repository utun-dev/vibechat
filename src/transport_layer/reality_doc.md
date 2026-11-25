# Внешняя оболочка REALITY для STCP

`reality.c/h` собирает и проверяет Hello, `reality_record.c/h` шифрует записи,
`reality_io.c/h` ведёт handshake/поток STCP, `reality_relay.c/h` передаёт неавторизованное
подключение на `dest`. Неизменяемый профиль `reality_fingerprint.c/h` сейчас один: OpenSSL, id=0.
Это собственный протокол uTun с TLS-подобным фреймированием; обычный TLS-клиент не получает доступ к STCP.
Все состояния принадлежат uasync-потоку соединения/instance.

## Hello и сессия

Клиент использует static public key сервера, short_id, version и непустое SNI длиной до 255 байт.
В SessionId спрятаны version[3], reserved=0, Unix timestamp[4] и short_id[8], зашифрованные AES-256-GCM.
AuthKey выводится HKDF-SHA256 из X25519(client ephemeral, server static), salt=Random[0:20], info=`REALITY`.
Сервер проверяет структуру, GCM, точную версию, список 1..64 short_id и окно времени 0..3600 секунд.
`*_build_at` получает явное Unix-время, обычные `*_build` используют `time(NULL)`.
На сетевом пути сервер использует время instance с поправкой NTP.

Выходной буфер ClientHello должен вмещать `REALITY_MAX_CH_SIZE`=65535 байт,
ServerHello — `REALITY_MAX_SH_SIZE`=512. Прямой серверный codec принимает ровно одну полную
нормализованную запись с ClientHello. I/O собирает Hello из нескольких записей, сохраняя исходные wire-байты для relay.
Размер результата действителен при `REALITY_OK`; после ошибки содержимое выхода не использовать.

Обёртки `reality_*_hello_build*` не сохраняют сессию и не проверяют повтор Hello.
Для обмена защищёнными записями нужен обнулённый `reality_session`: client_start → client_accept
либо server_start, затем проверка обоих Finished. При любой ошибке нужен session_cleanup.
Контекст одноразовый; перед повторным start старый контекст освобождается.
После вывода traffic-ключей ephemeral private/AuthKey стираются; handshake_done освобождает transcript
и Finished-ключи, сохраняя traffic. Cleanup стирает весь контекст.

## Защищённый поток

Стороны выводят общий root из AuthKey и ephemeral X25519 обеих сторон, привязывая его к SHA256(CH || SH).
Точные handshake-байты с четырёхбайтовыми handshake-заголовками входят в transcript; record-заголовки не входят.
После ServerHello сервер передаёт CCS и зашифрованные EE, cover Certificate/CertificateVerify и Finished;
клиент проверяет Finished и отвечает CCS/Finished. Cover-данные не являются PKI.
Только после этого ready разрешает внутренний STCP-handshake.

Запись: `17 03 03 | length_be16 | AES-128-GCM(content || inner_type || zero_padding) | tag[16]`.
Пятибайтовый заголовок — AAD. Nonce — base_iv XOR (zero[4] || sequence_be64).
Направления имеют отдельные secret/key/iv/sequence; успешный seal/open увеличивает sequence.
Предел inner=16385, content до 16384 байт с учётом padding; пустой content допустим только для APPLICATION.
Предел одного ключа — 2^20 записей; последняя доступная запись используется для KeyUpdate.
Обновление меняет secret/key/iv, сбрасывает sequence и увеличивает диагностический generation.
Ошибки аутентификации/формата требуют закрытия сессии; повторная отправка использует уже готовые wire-байты.

`reality_io_send` забирает выделенный через u_malloc буфер только при успехе (0/1).
При -1 буфер остаётся caller, но предыдущие записи уже могли попасть в очередь: соединение закрывается.
`read` возвращает 1 при приёме байтов, 0 при временном отсутствии, -1 при EOF/ошибке/close_notify.
Пауза чтения сохраняет wire-буфер; resume продолжает разбор и сохраняет штатный finish при close_notify.
Очистка I/O освобождает ключи/буферы, но fd закрывает владелец STCP.

## Replay, fallback и остановка

У instance отдельный кеш до 1024 нормализованных ClientHello; запись живёт до timestamp+window.
Повтор и заполненный кеш отклоняются. Кеш проверяется только после авторизации Hello и до своего ServerHello.
После постановки собственного ServerHello в очередь fallback запрещён.
До него неавторизованный вход передаётся relay со всеми уже прочитанными исходными байтами.

Relay забирает client fd даже при синхронной ошибке; prefix копируется, владение входным буфером не меняется.
Dest — IPv4 или hostname[:port], порт по умолчанию 443, явный порт 1..65535; IPv6 dest не поддерживается.
DNS асинхронный, только A; используется первый адрес. Ошибки после старта закрывают relay с логом.
Backpressure ограничивает каждый поток одним pending-буфером; half-close сохраняет ответ противоположной стороны.
HUP дочитывает оставшиеся данные/EOF. Idle timeout сбрасывается при чтении и успешной отправке;
0 отключает его, допустимый максимум — INT_MAX/10000 секунд.

Закрытый relay обычно освобождается через call_soon. При отказе выделения он остаётся в списке owner до cleanup.
`reality_relay_shutdown` синхронно закрывает активные и освобождает закрытые контексты, отменяя call_soon.
`reality_owner_cleanup` дополнительно освобождает replay-кеш. Оба вызываются до уничтожения UASYNC,
вне relay callbacks; активные состояния STCP очищаются отдельно. Предупреждение выше 64 relay не является лимитом.
Диагностика — категория `reality`: INFO для авторизации/закрытия, DEBUG для состояний и KeyUpdate, TRACE для записей.
