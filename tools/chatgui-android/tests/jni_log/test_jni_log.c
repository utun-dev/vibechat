/* Компилируем настоящий JNI-код, включая static callback, без тестовых веток в production.
 * Неиспользуемые функции bridge отсекает ELF linker; JNI-модель ниже проверяет порядок вызовов и владение ссылками. */
#include "../../jni_bridge/android_jni_bridge.c"

enum fault { OK, GET_ENV, ATTACH, LOCAL_REF, OBJECT_CLASS, METHOD, CATEGORY, MESSAGE, COPY_CATEGORY, COPY_MESSAGE,
             CALLBACK, DETACH, RECURSION, REPLACE };
struct ref { int live, kind; jsize length; unsigned char bytes[256]; };
static struct ref refs[16], global_ref;
static enum fault fault;
static int pending, throw_on_failure, illegal_calls, calls, attaches, detaches, descriptions, diagnostics, nested;
static int already_attached, arrays, copies, legacy_strings, payload_ok;
static int normal_logs;
static char last_diagnostic[512], method_signature[128];
static JNIEnv test_env;
static const char category[] = "чат", message[] = "Ошибка 😀";
static const char* expected_category;
static const char* expected_message;

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "[FAIL] %s:%d: %s\n", __func__, __LINE__, #test); return 1; } } while (0)

int __android_log_print(int priority, const char* tag, const char* format, ...) {
    (void)tag;
    if (priority == ANDROID_LOG_INFO || priority == ANDROID_LOG_DEBUG) { normal_logs++; return 0; }
    if (priority != ANDROID_LOG_ERROR && priority != ANDROID_LOG_WARN) illegal_calls++;
    va_list ap; va_start(ap, format);
    vsnprintf(last_diagnostic, sizeof(last_diagnostic), format, ap);
    va_end(ap); diagnostics++;
    return 0;
}

static void check_pending(void) { if (pending) illegal_calls++; }
static void fail(void) { if (throw_on_failure) pending = 1; }

static jobject new_ref(int kind) {
    for (unsigned i = 0; i < sizeof(refs) / sizeof(refs[0]); i++) {
        if (!refs[i].live) { memset(&refs[i], 0, sizeof(refs[i])); refs[i].live = 1; refs[i].kind = kind; return (jobject)&refs[i]; }
    }
    illegal_calls++; return NULL;
}

static int live_refs(void) {
    int count = 0;
    for (unsigned i = 0; i < sizeof(refs) / sizeof(refs[0]); i++) count += refs[i].live;
    return count;
}

static jobject JNICALL local_ref(JNIEnv* env, jobject object) {
    (void)env; check_pending();
    if (fault == LOCAL_REF) { fail(); return NULL; }
    if (!object || !((struct ref*)object)->live) { illegal_calls++; return NULL; }
    return new_ref(3);
}

static void JNICALL delete_ref(JNIEnv* env, jobject object) {
    (void)env;
    if (!object || !((struct ref*)object)->live) { illegal_calls++; return; }
    ((struct ref*)object)->live = 0;
}

static jclass JNICALL object_class(JNIEnv* env, jobject object) {
    (void)env; check_pending();
    if (!object || !((struct ref*)object)->live) illegal_calls++;
    if (fault == OBJECT_CLASS) { fail(); return NULL; }
    if (fault == REPLACE) { global_ref.live = 0; g_log_callback = NULL; }
    return (jclass)new_ref(1);
}

static jmethodID JNICALL method(JNIEnv* env, jclass cls, const char* name, const char* signature) {
    (void)env; check_pending();
    if (!cls || !((struct ref*)cls)->live || strcmp(name, "onNativeLog")) illegal_calls++;
    snprintf(method_signature, sizeof(method_signature), "%s", signature);
    if (fault == METHOD || !cls) { fail(); return NULL; }
    return (jmethodID)&method_signature;
}

static jbyteArray JNICALL byte_array(JNIEnv* env, jsize length) {
    (void)env; check_pending(); arrays++;
    if ((fault == CATEGORY && arrays == 1) || (fault == MESSAGE && arrays == 2)) { fail(); return NULL; }
    if (length < 0 || length > 256) { illegal_calls++; return NULL; }
    struct ref* ref = (struct ref*)new_ref(2);
    if (ref) ref->length = length;
    return (jbyteArray)ref;
}

static void JNICALL copy_bytes(JNIEnv* env, jbyteArray array, jsize start, jsize length, const jbyte* bytes) {
    (void)env; check_pending(); copies++;
    if ((fault == COPY_CATEGORY && copies == 1) || (fault == COPY_MESSAGE && copies == 2)) { fail(); return; }
    struct ref* ref = (struct ref*)array;
    if (!ref || !ref->live || start || length != ref->length) { illegal_calls++; return; }
    memcpy(ref->bytes, bytes, (size_t)length);
}

/* Старый callback тоже выполняется моделью: отрицательный контроль проверяет реальную прежнюю ошибку, а не падает на NULL vtable. */
static jstring JNICALL legacy_string(JNIEnv* env, const char* text) {
    (void)env; check_pending(); legacy_strings++;
    struct ref* ref = (struct ref*)new_ref(4);
    if (!ref) return NULL;
    ref->length = (jsize)strlen(text); memcpy(ref->bytes, text, (size_t)ref->length);
    return (jstring)ref;
}

static void JNICALL call(JNIEnv* env, jobject object, jmethodID mid, ...) {
    (void)env; check_pending(); calls++;
    if (!object || !((struct ref*)object)->live || !mid) illegal_calls++;
    va_list ap; va_start(ap, mid);
    int level = va_arg(ap, int);
    struct ref* cat = (struct ref*)va_arg(ap, jobject);
    struct ref* msg = (struct ref*)va_arg(ap, jobject);
    va_end(ap);
    payload_ok = level == BLEV_ERROR && cat && msg && cat->kind == 2 && msg->kind == 2 &&
        cat->length == (jsize)strlen(expected_category) && msg->length == (jsize)strlen(expected_message) &&
        !memcmp(cat->bytes, expected_category, strlen(expected_category)) &&
        !memcmp(msg->bytes, expected_message, strlen(expected_message));
    if (fault == CALLBACK) pending = 1;
    if (fault == RECURSION && !nested++) jni_on_log(BLEV_ERROR, category, message);
}

static jboolean JNICALL exception_check(JNIEnv* env) { (void)env; return pending ? JNI_TRUE : JNI_FALSE; }
static void JNICALL exception_describe(JNIEnv* env) { (void)env; descriptions++; pending = 0; }
static void JNICALL exception_clear(JNIEnv* env) { (void)env; pending = 0; }

static jint JNICALL get_env(JavaVM* vm, void** output, jint version) {
    (void)vm;
    if (version != JNI_VERSION_1_6) illegal_calls++;
    if (fault == GET_ENV) return JNI_EVERSION;
    if (!already_attached) return JNI_EDETACHED;
    *output = &test_env; return JNI_OK;
}

static jint JNICALL attach(JavaVM* vm, void** output, void* args) {
    (void)vm; (void)args; attaches++;
    if (fault == ATTACH) return JNI_ERR;
    *output = &test_env; return JNI_OK;
}

static jint JNICALL detach(JavaVM* vm) { (void)vm; detaches++; return fault == DETACH ? JNI_ERR : JNI_OK; }

static const struct JNINativeInterface_ functions = {
    .NewLocalRef = local_ref, .DeleteLocalRef = delete_ref, .GetObjectClass = object_class, .GetMethodID = method,
    .NewByteArray = byte_array, .SetByteArrayRegion = copy_bytes, .NewStringUTF = legacy_string, .CallVoidMethod = call,
    .ExceptionCheck = exception_check, .ExceptionDescribe = exception_describe, .ExceptionClear = exception_clear
};
static const struct JNIInvokeInterface_ vm_functions = { .GetEnv = get_env, .AttachCurrentThread = attach, .DetachCurrentThread = detach };
static JavaVM test_vm = &vm_functions;

static void reset(enum fault next) {
    memset(refs, 0, sizeof(refs)); memset(&global_ref, 0, sizeof(global_ref)); global_ref.live = 1;
    pending = illegal_calls = calls = attaches = detaches = descriptions = diagnostics = nested = 0;
    arrays = copies = legacy_strings = payload_ok = normal_logs = 0;
    last_diagnostic[0] = method_signature[0] = 0;
    throw_on_failure = already_attached = 1;
    fault = next; test_env = &functions; g_jvm = &test_vm; g_log_callback = (jobject)&global_ref;
    expected_category = category; expected_message = message;
}

static int success(int detached) {
    reset(OK); already_attached = !detached;
    jni_on_log(BLEV_ERROR, category, message);
    CHECK(calls == 1 && payload_ok && !legacy_strings && !illegal_calls && !live_refs() && !pending && !diagnostics);
    CHECK(attaches == detached && detaches == detached);
    CHECK(!strcmp(method_signature, "(I[B[B)V"));
    return 0;
}

static int failure(enum fault next, const char* phase, int throws) {
    reset(next); throw_on_failure = throws;
    if (next == ATTACH || next == DETACH) already_attached = 0;
    jni_on_log(BLEV_ERROR, category, message);
    CHECK(diagnostics && strstr(last_diagnostic, phase));
    CHECK(!illegal_calls && !live_refs() && !pending);
    CHECK(calls == (next == CALLBACK || next == DETACH));
    CHECK(detaches == (next == DETACH));
    CHECK(descriptions == (throws && next != GET_ENV && next != ATTACH && next != DETACH));
    /* Отказ не отравляет следующий callback и не оставляет защиту рекурсии включённой. */
    fault = OK; already_attached = 1;
    jni_on_log(BLEV_ERROR, category, message);
    CHECK(payload_ok && !illegal_calls && !live_refs() && !pending);
    return 0;
}

static int preexisting_exception(void) {
    reset(OK); pending = 1;
    jni_on_log(BLEV_ERROR, category, message);
    CHECK(pending && !descriptions && !illegal_calls && !calls && diagnostics && !live_refs());
    return 0;
}

static int recursive_callback(void) {
    reset(RECURSION);
    jni_on_log(BLEV_ERROR, category, message);
    CHECK(calls == 1 && diagnostics && strstr(last_diagnostic, "recursive") && !illegal_calls && !live_refs());
    return 0;
}

static int replaced_callback(void) {
    reset(REPLACE);
    jni_on_log(BLEV_ERROR, category, message);
    CHECK(calls == 1 && payload_ok && !illegal_calls && !live_refs());
    return 0;
}

static int empty_fields(void) {
    reset(OK); expected_category = expected_message = "";
    jni_on_log(BLEV_ERROR, "", "");
    CHECK(calls == 1 && payload_ok && arrays == 2 && !copies && !illegal_calls && !live_refs() && !diagnostics);
    return 0;
}

static int repeated_callbacks(void) {
    reset(OK);
    for (unsigned i = 0; i < 1024; i++) {
        jni_on_log(BLEV_ERROR, category, message);
        CHECK(payload_ok && !live_refs() && !illegal_calls && !pending && !diagnostics);
    }
    CHECK(calls == 1024 && !attaches && !detaches);
    return 0;
}

static int subscription(void) {
    reset(OK);
    Java_com_utun_chat_data_NativeLib_nativeSubscribeLogs(&test_env, NULL, JNI_FALSE);
    utun_bridge_log(BLEV_ERROR, category, message);
    CHECK(!calls);
    Java_com_utun_chat_data_NativeLib_nativeSubscribeLogs(&test_env, NULL, JNI_TRUE);
    utun_bridge_log(BLEV_ERROR, category, message);
    CHECK(calls == 1 && payload_ok && !illegal_calls && !live_refs());
    Java_com_utun_chat_data_NativeLib_nativeSubscribeLogs(&test_env, NULL, JNI_FALSE);
    utun_bridge_log(BLEV_ERROR, category, message);
    CHECK(calls == 1 && normal_logs == 3 && !diagnostics);
    return 0;
}

static int disabled_and_invalid(void) {
    reset(OK); g_log_callback = NULL;
    jni_on_log(BLEV_ERROR, category, message);
    CHECK(!calls && !diagnostics && !live_refs());
    reset(OK); g_jvm = NULL;
    jni_on_log(BLEV_ERROR, category, message);
    CHECK(!calls && diagnostics && !live_refs());
    reset(OK);
    jni_on_log(BLEV_ERROR, NULL, message);
    CHECK(!calls && diagnostics && !live_refs());
    reset(OK);
    jni_on_log(BLEV_ERROR, category, NULL);
    CHECK(!calls && diagnostics && !live_refs());
    return 0;
}

int main(void) {
    struct { enum fault fault; const char* phase; } cases[] = {
        {GET_ENV, "GetEnv"}, {ATTACH, "AttachCurrentThread"}, {LOCAL_REF, "NewLocalRef"},
        {OBJECT_CLASS, "GetObjectClass"}, {METHOD, "GetMethodID"}, {CATEGORY, "category"}, {MESSAGE, "message"},
        {COPY_CATEGORY, "category"}, {COPY_MESSAGE, "message"}, {CALLBACK, "onNativeLog"}, {DETACH, "DetachCurrentThread"}
    };
    int failed = success(0) + success(1) + preexisting_exception() + recursive_callback() + replaced_callback() + disabled_and_invalid();
    failed += empty_fields() + repeated_callbacks() + subscription();
    int count = 9;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        failed += failure(cases[i].fault, cases[i].phase, 1); count++;
        if (cases[i].fault >= LOCAL_REF && cases[i].fault <= MESSAGE) {
            failed += failure(cases[i].fault, cases[i].phase, 0); count++;
        }
    }
    printf("JNI log scenarios: %d passed, %d failed (%d total)\n", count - failed, failed, count);
    return failed ? 1 : 0;
}
