# Проверка команд BLE после закрытия линка

Из `tools/chatgui-android/`:

```sh
./gradlew :ble-command-tests:test
```

Linux/JDK/`cc`: Gradle собирает `events.c` в отдельную JNI-библиотеку теста.
Она записывает исходящие `nativeEvent` настоящего `BleRadio` в JUnit-приёмник.
SDK-модели в `tests/ble_commands/src/android/` управляют очередью Handler и ответами GATT.
Отдельный JVM-модуль исполняет тест с собственным classpath; модели не входят в APK
и обычные unit-тесты. `testDebugUnitTest` также запускает эту проверку. Новых зависимостей нет.

Тесты вызывают production `command`, `close`, `pump`, `complete` и callback сервера.
Reflection используется только для начального состояния и private-входов SDK;
приложение не получает тестовых методов. Имя класса callback соответствует текущему
анонимному классу из `start()`; при изменении этой функции обновить имя в тесте.

Проверяются локальное/C-закрытие перед исполнением DATA/INVITE/ACK, отсутствие
лишнего completion, работа другого соединения, смена generation, поздний callback
закрытого writer и новое соединение с тем же адресом. Существующий watchdog writer
остаётся взведённым до callback. Нарушение инварианта на живом линке сохраняет ERROR/failed.
Дополнительно проверяются обновление полного имени/повтор прежней видимости без смены
advertising handle и закрытие profile-only клиента без disconnect общего входящего ACL.

Границы проверки: JNI здесь не исполняет C transport и Android SDK не обращается
к оборудованию. Общий lifecycle отдельно проверяет `test_ble_transport`; APK-сборка
проверяет реальные SDK/JNI ABI. Этот тест не воспроизводит падение системной Bluetooth-службы.
