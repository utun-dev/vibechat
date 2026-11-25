# Проверка строк JNI

`python3 tools/chatgui-android/tests/jni_utf8/run.py` из корня запускает настоящий
код преобразования Android bridge в JVM с `-Xcheck:jni`. Нужны Linux, Python 3,
C99-компилятор, OpenSSL headers и JDK с javac/libjvm. Телефон не требуется.

1024 преобразования UTF-16 → обычный UTF-8 → UTF-16 проверяют кириллицу, surrogate pair emoji,
кавычки и перевод строки. Проверяются отказ выделения C-буфера с повтором, отклонение
внутреннего NUL, пустая строка, NULL и сохранение уже ожидающего Java exception.
JVM проверяет корректность вызовов JNI; ART и Android SDK этим тестом не запускаются.
Тот же запуск проверяет регистрацию/замену настоящего Java event callback, доставку payload,
исключение обработчика с последующей успешной доставкой и сохранение чужого pending exception.
Gradle запускает проверку как `:app:testJniUtf8` перед `:app:testDebugUnitTest` на Linux.
Проверяются также повторные config callbacks, attach/detach native-потока и удаление БД
по явно переданному пути с кириллицей/emoji через настоящий JNI entry point.
