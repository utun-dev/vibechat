# Проверка JNI-доставки логов

Linux host, без телефона и запущенного ядра. Нужны Python 3, C99-компилятор,
заголовки OpenSSL и JDK с `include/jni.h`. Используется настоящий
`jni_bridge/android_jni_bridge.c`: неиспользуемые функции отсекает ELF linker.
JNI function tables моделируют JVM, локальные ссылки и принудительные отказы;
Android logcat заменён только проверяющим приёмником диагностики.

Из корня репозитория:

```sh
python3 tools/chatgui-android/tests/jni_log/run.py
# Явный JDK / другой компилятор:
CC=clang python3 tools/chatgui-android/tests/jni_log/run.py --java-home /path/to/jdk
```

В Android Gradle на Linux задача `:app:testJniLog` запускается перед JVM-тестами
приложения, включая `:app:testDebugUnitTest`. На других ОС эта host-задача пропускается.
Android `AttachCurrentThread` принимает `JNIEnv**`, JDK — `void**` с тем же ABI;
только это различие типов допускается host-компилятором. Настоящая NDK-сборка
проверяет Android-заголовки отдельно. Host-модели не включаются в APK.

25 сценариев проверяют GetEnv/attach/detach, получение callback/class/method,
создание/копирование массивов, исходный UTF-8 с кириллицей/emoji, освобождение
всех локальных ссылок и продолжение после ошибки. Отдельно проверяются
Java-исключение callback, сохранение чужого pending exception, подавление
рекурсии и удержание local ref при замене global callback.
Для операций, допускающих `NULL` без Java-исключения, проверяются оба отказа.
Проверяются также пустые строки, 1024 последовательные доставки в уже attached
потоке и переключение подписки через настоящий JNI entry point.

Тест не запускает ART и не проверяет сборщик мусора Android; JVM-тест
`NativeLogContractTest` дополнительно сопоставляет C lookup с дескриптором
скомпилированного Kotlin `onNativeLog(Int, ByteArray?, ByteArray?)`.
Правила JNI: [Android JNI tips](https://developer.android.com/ndk/guides/jni-tips),
[JNI function specification](https://docs.oracle.com/en/java/javase/17/docs/specs/jni/functions.html).
