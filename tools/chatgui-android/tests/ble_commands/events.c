/* JVM-тест настоящего BleRadio: перехватываем только исходящие JNI-события, без библиотеки ядра и Bluetooth ОС. */
#include <jni.h>

JNIEXPORT void JNICALL Java_com_utun_chat_data_BleRadio_nativeEvent(JNIEnv* env, jclass cls, jlong generation,
        jlong token, jint type, jint arg, jstring text, jbyteArray bytes) {
    (void)cls;
    jclass sink = (*env)->FindClass(env, "com/utun/chat/data/BleRadioCommandsTest");
    if (!sink) return; /* Исключение JVM делает тест неуспешным. */
    jmethodID record = (*env)->GetStaticMethodID(env, sink, "recordEvent", "(JJIILjava/lang/String;[B)V");
    if (record) (*env)->CallStaticVoidMethod(env, sink, record, generation, token, type, arg, text, bytes);
    (*env)->DeleteLocalRef(env, sink);
}
