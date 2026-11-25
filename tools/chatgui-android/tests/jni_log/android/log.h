/* Host-модель только вывода Android; JNI-типы и ABI берутся из установленного JDK. */
#ifndef UTUN_TEST_ANDROID_LOG_H
#define UTUN_TEST_ANDROID_LOG_H
#define ANDROID_LOG_VERBOSE 2
#define ANDROID_LOG_DEBUG 3
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_WARN 5
#define ANDROID_LOG_ERROR 6
int __android_log_print(int priority, const char* tag, const char* format, ...);
#endif
