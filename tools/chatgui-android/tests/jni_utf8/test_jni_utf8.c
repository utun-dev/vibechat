/* Настоящая JVM с -Xcheck:jni: строки с emoji/кириллицей, NUL и сохранение чужого Java exception. */
#include "../../jni_bridge/android_jni_bridge.c"

static int fail_allocation;
static int worker_live;
static int reject_post, submitted, submitted_success, command_status;
static uint64_t submitted_request;
static void (*posted_callback)(void*);
static void* posted_arg;
static char command_group[65], command_json[256];
struct UASYNC* instance_lite_get_uasync(void) { return worker_live ? (struct UASYNC*)(uintptr_t)1 : NULL; }
void* u_malloc_impl(uint32_t size, const char* location) { (void)location; return fail_allocation ? NULL : malloc(size); }
void* u_calloc_impl(uint32_t count, uint32_t size, const char* location) {
    (void)location; return fail_allocation ? NULL : calloc(count, size);
}
void u_free_impl(void* pointer, const char* location) { (void)location; free(pointer); }
/* Очередь управляется тестом: проверяем снимок адресата и ACK после выполнения, без настоящего ядра. */
int instance_lite_post(void (*callback)(void*), void* arg, struct UTUN_INSTANCE** instance, void (*discard)(void*)) {
    (void)discard;
    if (reject_post) return -1;
    *instance = (struct UTUN_INSTANCE*)(uintptr_t)1; posted_callback = callback; posted_arg = arg; return 0;
}
void attachment_send_trampoline(void* arg) { (void)arg; abort(); }
void attachment_send_discard(void* arg) { (void)arg; abort(); }
void chat_event_submitted(struct UTUN_INSTANCE* inst, uint64_t request, int success) {
    (void)inst; submitted++; submitted_request = request; submitted_success = success;
}
void kanban_execute_submit(struct UTUN_INSTANCE* inst, const char* group, const char* json, kanban_execute_done_fn done, void* arg) {
    (void)inst; snprintf(command_group, sizeof(command_group), "%s", group); snprintf(command_json, sizeof(command_json), "%s", json);
    const struct kanban_execute_result result = {.status=command_status, .committed=!command_status,
        .json=command_status ? "{\"ok\":false}" : "{\"ok\":true}"};
    done(arg, &result);
}
int __android_log_print(int priority, const char* tag, const char* format, ...) {
    (void)priority; (void)tag;
    va_list args; va_start(args, format); vfprintf(stderr, format, args); va_end(args); fputc('\n', stderr); return 0;
}
#define CHECK(value) do { if (!(value)) { fprintf(stderr, "JNI UTF-8 failed line=%d: %s\n", __LINE__, #value); return 1; } } while (0)

static void* config_from_worker(void* arg) {
    int* valid = arg;
    JNIEnv* env = NULL;
    if ((*g_jvm)->GetEnv(g_jvm, (void**)&env, JNI_VERSION_1_6) != JNI_EDETACHED) return NULL;
    const char* value = jni_cfg_get_string("worker");
    *valid = value && !strcmp(value, "worker Имя 😀") &&
        (*g_jvm)->GetEnv(g_jvm, (void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED;
    return NULL;
}

int main(void) {
    debug_config_init(); debug_set_category_level(DEBUG_CATEGORY_KANBAN, DEBUG_LEVEL_INFO);
    JavaVM* vm = NULL; JNIEnv* env = NULL;
    JavaVMOption option = {.optionString = "-Xcheck:jni"};
    JavaVMInitArgs args = {.version = JNI_VERSION_1_6, .nOptions = 1, .options = &option};
    CHECK(JNI_CreateJavaVM(&vm, (void**)&env, &args) == JNI_OK);
    const jchar characters[] = {0x0418, 0x043c, 0x044f, ' ', 0xd83d, 0xde00, '/', '"', '\n'};
    const char* expected = "Имя 😀/\"\n";
    jstring original = (*env)->NewString(env, characters, sizeof(characters) / sizeof(characters[0])); CHECK(original);
    for (int i = 0; i < 1024; i++) {
        char* bytes = jni_utf8(env, original); CHECK(bytes && !strcmp(bytes, expected));
        jstring restored = jni_text(env, bytes); u_free(bytes); CHECK(restored);
        CHECK((*env)->GetStringLength(env, restored) == sizeof(characters) / sizeof(characters[0]));
        const jchar* actual = (*env)->GetStringChars(env, restored, NULL); CHECK(actual && !memcmp(actual, characters, sizeof(characters)));
        (*env)->ReleaseStringChars(env, restored, actual); (*env)->DeleteLocalRef(env, restored);
    }
    fail_allocation = 1; CHECK(jni_utf8(env, original) == NULL); fail_allocation = 0;
    char* retried = jni_utf8(env, original); CHECK(retried && !strcmp(retried, expected)); u_free(retried);
    const jchar nul[] = {'a', 0, 'b'};
    jstring embedded = (*env)->NewString(env, nul, 3); CHECK(embedded && !jni_utf8(env, embedded));
    CHECK(!jni_utf8(env, NULL) && !jni_text(env, NULL));
    jclass exception = (*env)->FindClass(env, "java/lang/IllegalStateException"); CHECK(exception);
    CHECK(!(*env)->ThrowNew(env, exception, "preserved"));
    CHECK(!jni_utf8(env, original) && !jni_text(env, expected) && (*env)->ExceptionCheck(env));
    (*env)->ExceptionClear(env);
    g_jvm = vm;
    jclass sink_class = (*env)->FindClass(env, "EventSink"); CHECK(sink_class);
    jmethodID constructor = (*env)->GetMethodID(env, sink_class, "<init>", "()V"); CHECK(constructor);
    jobject sink = (*env)->NewObject(env, sink_class, constructor); CHECK(!(*env)->ExceptionCheck(env) && sink);
    g_config_callback = (*env)->NewGlobalRef(env, sink); CHECK(g_config_callback);
    for (int i = 0; i < 1024; i++) CHECK(!strcmp(jni_cfg_get_string("main"), "main Имя 😀"));
    CHECK(!jni_cfg_get_string("throw") && !(*env)->ExceptionCheck(env));
    CHECK(!strcmp(jni_cfg_get_string("retry"), "retry Имя 😀"));
    int worker_valid = 0; pthread_t worker;
    CHECK(!pthread_create(&worker, NULL, config_from_worker, &worker_valid) && !pthread_join(worker, NULL) && worker_valid);
    (*env)->DeleteGlobalRef(env, g_config_callback); g_config_callback = NULL;
    jfieldID calls = (*env)->GetFieldID(env, sink_class, "calls", "I"); CHECK(calls);
    jfieldID type = (*env)->GetFieldID(env, sink_class, "type", "I"); CHECK(type);
    jfieldID size = (*env)->GetFieldID(env, sink_class, "bytes", "I"); CHECK(size);
    jfieldID fail = (*env)->GetFieldID(env, sink_class, "fail", "Z"); CHECK(fail);
    Java_com_utun_chat_data_NativeLib_nativeSetEventCallback(env, NULL, sink); CHECK(!(*env)->ExceptionCheck(env));
    const uint8_t payload[] = {1, 2, 3};
    jni_on_event(7, payload, sizeof(payload));
    CHECK((*env)->GetIntField(env, sink, calls) == 1 && (*env)->GetIntField(env, sink, type) == 7);
    CHECK((*env)->GetIntField(env, sink, size) == sizeof(payload));
    (*env)->SetBooleanField(env, sink, fail, JNI_TRUE);
    jni_on_event(8, payload, sizeof(payload)); CHECK(!(*env)->ExceptionCheck(env));
    (*env)->SetBooleanField(env, sink, fail, JNI_FALSE);
    jni_on_event(9, NULL, 0); CHECK((*env)->GetIntField(env, sink, calls) == 3 && !(*env)->GetIntField(env, sink, size));
    jobject replacement = (*env)->NewObject(env, sink_class, constructor); CHECK(!(*env)->ExceptionCheck(env) && replacement);
    Java_com_utun_chat_data_NativeLib_nativeSetEventCallback(env, NULL, replacement); CHECK(!(*env)->ExceptionCheck(env));
    jni_on_event(10, payload, sizeof(payload));
    CHECK((*env)->GetIntField(env, replacement, calls) == 1 && (*env)->GetIntField(env, sink, calls) == 3);
    CHECK(!(*env)->ThrowNew(env, exception, "preserved event exception"));
    jni_on_event(11, payload, sizeof(payload)); CHECK((*env)->ExceptionCheck(env)); (*env)->ExceptionClear(env);
    jni_on_event(12, NULL, 3); CHECK((*env)->GetIntField(env, replacement, calls) == 1);
    Java_com_utun_chat_data_NativeLib_nativeSetEventCallback(env, NULL, NULL); CHECK(!(*env)->ExceptionCheck(env));
    jni_on_event(13, NULL, 0); CHECK(!(*env)->ExceptionCheck(env));
    (*env)->DeleteLocalRef(env, replacement); (*env)->DeleteLocalRef(env, sink); (*env)->DeleteLocalRef(env, sink_class);
    CHECK(!mkdir("база😀", 0700));
    FILE* database = fopen("база😀/chats.db", "wb"); CHECK(database && fputs("owned database", database) >= 0 && !fclose(database));
    jstring directory = jni_text(env, "база😀"); CHECK(directory);
    worker_live = 1;
    jstring busy = Java_com_utun_chat_data_NativeLib_nativeClearDatabase(env, NULL, directory); CHECK(busy);
    (*env)->DeleteLocalRef(env, busy);
    CHECK(!access("база😀/chats.db", F_OK)); worker_live = 0;
    CHECK(Java_com_utun_chat_data_NativeLib_nativeClearDatabase(env, NULL, directory) == NULL);
    CHECK(access("база😀/chats.db", F_OK) && errno == ENOENT);
    CHECK(Java_com_utun_chat_data_NativeLib_nativeClearDatabase(env, NULL, directory) == NULL);
    (*env)->DeleteLocalRef(env, directory); CHECK(!rmdir("база😀"));

    const char* key = "bb5c672482b0dcca91a21a4ed63b15afde8aa1378da72cd01b349589d6e7dd6a";
    jstring group = jni_text(env, key), card = jni_text(env, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    jstring source = jni_text(env, "/cache/Фото😀.jpg"), file_type = jni_text(env, "application/octet-stream");
    jstring photo_type = jni_text(env, "image/jpeg"), video_type = jni_text(env, "video/mp4"), png_type = jni_text(env, "image/png");
    jstring types[] = {file_type, photo_type, png_type, video_type};
    const int kinds[] = {ATTACHMENT_FILE, ATTACHMENT_JPEG, ATTACHMENT_PNG, ATTACHMENT_VIDEO};
    for (unsigned i = 0; i < 4; i++) {
        CHECK(Java_com_utun_chat_data_NativeLib_nativeKanbanAttachmentSend(env, NULL, group, card, source, types[i],
            i ? 640 : 0, i ? 480 : 0, i == 3 ? 1234 : 0, 100 + i) == JNI_TRUE);
        struct attachment_send_req* captured = posted_arg;
        CHECK(posted_callback == attachment_send_trampoline && captured->temporary_source && !captured->is_dm);
        CHECK(!strcmp(captured->target, key) && !strcmp(captured->card, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
        CHECK(!strcmp(captured->source, "/cache/Фото😀.jpg") && !strcmp(captured->info.name, "Фото😀.jpg"));
        CHECK(captured->info.kind == kinds[i] && captured->request_id == 100 + i && !attachment_validate(&captured->info));
        u_free(captured); posted_arg = NULL;
    }
    CHECK(Java_com_utun_chat_data_NativeLib_nativeKanbanAttachmentSend(env, NULL, group, card, source, photo_type, 65537, 480, 0, 1) == JNI_FALSE);
    CHECK(Java_com_utun_chat_data_NativeLib_nativeKanbanAttachmentSend(env, NULL, group, card, source, photo_type, 0, 480, 0, 1) == JNI_FALSE);
    CHECK(Java_com_utun_chat_data_NativeLib_nativeKanbanAttachmentSend(env, NULL, group, card, source, video_type, 640, 480, 0, 1) == JNI_FALSE);
    CHECK(Java_com_utun_chat_data_NativeLib_nativeKanbanAttachmentSend(env, NULL, group, file_type, source, file_type, 0, 0, 0, 1) == JNI_FALSE);
    reject_post = 1;
    CHECK(Java_com_utun_chat_data_NativeLib_nativeKanbanAttachmentSend(env, NULL, group, card, source, file_type, 0, 0, 0, 1) == JNI_FALSE);
    CHECK(!posted_arg); reject_post = 0;
    jstring body = jni_text(env, "{\"action\":\"message_create\",\"entity\":\"card\",\"text\":\"Привет😀\"}");
    CHECK(Java_com_utun_chat_data_NativeLib_nativeKanbanMessageSend(env, NULL, group, 234, body) == JNI_TRUE && !submitted);
    posted_callback(posted_arg); posted_arg = NULL;
    CHECK(submitted == 1 && submitted_request == 234 && submitted_success && !strcmp(command_group, key));
    CHECK(!strcmp(command_json, "{\"action\":\"message_create\",\"entity\":\"card\",\"text\":\"Привет😀\"}"));
    command_status = -1;
    CHECK(Java_com_utun_chat_data_NativeLib_nativeKanbanMessageSend(env, NULL, group, 235, body) == JNI_TRUE && submitted == 1);
    posted_callback(posted_arg); posted_arg = NULL;
    CHECK(submitted == 2 && submitted_request == 235 && !submitted_success);
    (*env)->DeleteLocalRef(env, body); (*env)->DeleteLocalRef(env, group); (*env)->DeleteLocalRef(env, card);
    (*env)->DeleteLocalRef(env, source); for (unsigned i = 0; i < 4; i++) (*env)->DeleteLocalRef(env, types[i]);
    jstring empty = jni_text(env, ""); CHECK(empty && !(*env)->GetStringLength(env, empty));
    jstring object_id = jni_text(env, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    const char group_key[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    jstring group_ed = jni_text(env, group_key), label = jni_text(env, "Команда 😀 / @");
    for (int kind = OBJECT_LINK_CHAT_MESSAGE; kind <= OBJECT_LINK_CARD_MESSAGE; kind++) {
        jstring url = Java_com_utun_chat_data_NativeLib_buildObjectLink(env, NULL, group_ed,
            kind == OBJECT_LINK_CHAT_MESSAGE ? empty : object_id, kind == OBJECT_LINK_CARD ? empty : object_id, label, label, label);
        CHECK(url && !(*env)->ExceptionCheck(env));
        char* bytes = jni_utf8(env, url); CHECK(bytes && strstr(bytes, "Команда-😀-%2F-%40@") && strstr(bytes, group_key));
        struct object_link_target target; CHECK(!object_link_parse(bytes, strlen(bytes), &target, NULL, 0));
        CHECK(!strcmp(target.group, group_key) && target.kind == kind); u_free(bytes);
        jstring decoded = Java_com_utun_chat_data_NativeLib_decodeObjectLink(env, NULL, url); CHECK(decoded);
        bytes = jni_utf8(env, decoded); CHECK(bytes && strstr(bytes, group_key)); u_free(bytes);
        (*env)->DeleteLocalRef(env, decoded); (*env)->DeleteLocalRef(env, url);
    }
    CHECK(!Java_com_utun_chat_data_NativeLib_decodeObjectLink(env, NULL, label));
    fail_allocation = 1;
    CHECK(!Java_com_utun_chat_data_NativeLib_buildObjectLink(env, NULL, group_ed, empty, object_id, label, label, label));
    fail_allocation = 0;
    (*env)->DeleteLocalRef(env, group_ed); (*env)->DeleteLocalRef(env, label); (*env)->DeleteLocalRef(env, object_id);
    (*env)->DeleteLocalRef(env, empty); (*env)->DeleteLocalRef(env, exception);
    (*env)->DeleteLocalRef(env, embedded); (*env)->DeleteLocalRef(env, original);
    CHECK(!(*env)->ExceptionCheck(env) && (*vm)->DestroyJavaVM(vm) == JNI_OK);
    puts("JNI UTF-8/events/Kanban: strings, callbacks, attachment targets/metadata/refusals and commit acknowledgements PASS");
    return 0;
}
