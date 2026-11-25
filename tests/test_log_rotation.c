/* Реальные файлы: лимит/поколения, новый запуск, reopen, UTF-8 пути, потоки и отказ rename/remove. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "test_utils.h"
#include "../lib/debug_config.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>

static char directory[512] = "/tmp/utun-log-XXXXXX";
static char path[1024];

static FILE* open_file(const char* name, const char* mode) {
#ifdef _WIN32
    wchar_t wide[2048], wide_mode[16];
    assert(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wide, 2048));
    assert(MultiByteToWideChar(CP_UTF8, 0, mode, -1, wide_mode, 16));
    return _wfopen(wide, wide_mode);
#else
    return fopen(name, mode);
#endif
}

static void remove_file(const char* name) {
#ifdef _WIN32
    wchar_t wide[2048];
    assert(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wide, 2048));
    int result = _wremove(wide);
#else
    int result = remove(name);
#endif
    assert(!result || errno == ENOENT);
}

static void filename(char* name, size_t size, unsigned generation) {
    if (generation) snprintf(name, size, "%s.%u", path, generation);
    else snprintf(name, size, "%s", path);
}

static size_t contents(unsigned generation, char* text, size_t capacity) {
    char name[1100]; filename(name, sizeof(name), generation);
    FILE* file = open_file(name, "rb");
    if (!file) { assert(errno == ENOENT); text[0] = 0; return 0; }
    size_t size = fread(text, 1, capacity - 1, file);
    assert(!ferror(file) && feof(file)); assert(!fclose(file)); text[size] = 0;
    return size;
}

static void setup(const char* name, unsigned files) {
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    debug_enable_timestamp(0); debug_enable_file_line(0); debug_enable_function_name(0);
    assert(!debug_set_file_rotation(4096, files));
    debug_enable_file_output(path, 0); assert(g_debug_config.file_output);
}

static void cleanup(void) {
    debug_disable_file_output();
    for (unsigned i = 0; i < 100; ++i) { char name[1100]; filename(name, sizeof(name), i); remove_file(name); }
}

static void retention(void) {
    setup("журнал.log", 5);
    char payload[3501]; memset(payload, 'x', sizeof(payload) - 1); payload[sizeof(payload) - 1] = 0;
    for (int i = 0; i < 20; ++i) DEBUG_INFO(DEBUG_CATEGORY_SYS, "record=%02d %s", i, payload);
    debug_disable_file_output();
    int expected = 10;
    for (int generation = 4; generation >= 0; --generation) {
        char text[9000]; size_t size = contents(generation, text, sizeof(text));
        assert(size >= 4096 && size <= 4096 + 4095);
        char* line = text;
        if (strstr(line, "Log rotated:") == line + strlen("[INFO] [sys] ")) line = strchr(line, '\n') + 1;
        for (int n = 0; n < 2; ++n) {
            int number = -1; assert(sscanf(line, "[INFO] [sys] record=%d", &number) == 1 && number == expected++);
            char* end = strchr(line, '\n'); assert(end); line = end + 1;
        }
        assert(!*line);
    }
    char text[9000]; assert(!contents(5, text, sizeof(text)));
    cleanup();
}

static void startup_and_reopen(void) {
    for (int run = 0; run < 8; ++run) {
        setup("startup.log", 5);
        assert(!debug_rotate_log());
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "launch=%d", run);
        debug_disable_file_output();
    }
    for (unsigned i = 0; i < 5; ++i) {
        char text[9000], marker[32]; assert(contents(i, text, sizeof(text)));
        snprintf(marker, sizeof(marker), "launch=%u", 7 - i); assert(strstr(text, marker));
    }
    cleanup();
    setup("reopen.log", 5);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "first");
    debug_enable_file_output(path, 0); assert(!debug_reopen_log());
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "second");
    char text[9000]; assert(contents(0, text, sizeof(text)) && strstr(text, "first") && strstr(text, "second"));
    assert(!contents(1, text, sizeof(text)));
    cleanup();
}

static void* writer(void* arg) {
    int thread = (int)(intptr_t)arg;
    char payload[801]; memset(payload, 'a' + thread, sizeof(payload) - 1); payload[sizeof(payload) - 1] = 0;
    for (int n = 0; n < 20; ++n) DEBUG_INFO(DEBUG_CATEGORY_SYS, "thread=%d record=%d %s", thread, n, payload);
    return NULL;
}

static void* reopener(void* arg) {
    (void)arg;
    for (int n = 0; n < 20; ++n) assert(!debug_reopen_log());
    return NULL;
}

static void concurrent_writes(void) {
    setup("threads.log", 99);
    pthread_t threads[5];
    for (int i = 0; i < 4; ++i) assert(!pthread_create(&threads[i], NULL, writer, (void*)(intptr_t)i));
    assert(!pthread_create(&threads[4], NULL, reopener, NULL));
    for (int i = 0; i < 5; ++i) assert(!pthread_join(threads[i], NULL));
    debug_disable_file_output();
    int seen[4][20] = {{0}}, count = 0;
    for (unsigned generation = 0; generation < 99; ++generation) {
        char text[10000]; if (!contents(generation, text, sizeof(text))) continue;
        for (char* line = text; *line;) {
            char* end = strchr(line, '\n'); assert(end); *end = 0;
            if (strstr(line, "thread=")) {
                int thread, record, offset = 0;
                assert(sscanf(line, "[INFO] [sys] thread=%d record=%d %n", &thread, &record, &offset) == 2);
                assert(thread >= 0 && thread < 4 && record >= 0 && record < 20 && !seen[thread][record]++);
                assert(strlen(line + offset) == 800);
                for (char* p = line + offset; *p; ++p) assert(*p == 'a' + thread);
                ++count;
            }
            line = end + 1;
        }
    }
    assert(count == 80);
    cleanup();
}

static void rotation_failure(void) {
    setup("failure.log", 5);
    char blocked[1100], child[1200]; filename(blocked, sizeof(blocked), 4);
#ifdef _WIN32
    assert(!_mkdir(blocked));
#else
    assert(!mkdir(blocked, 0700));
#endif
    snprintf(child, sizeof(child), "%s/keep", blocked);
    FILE* file = open_file(child, "wb"); assert(file && !fclose(file));
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "before failure");
    assert(debug_rotate_log() == -1);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "after failure");
    char text[9000]; assert(contents(0, text, sizeof(text)));
    assert(strstr(text, "before failure") && strstr(text, "after failure") && strstr(text, "rotate remove oldest failed"));
    remove_file(child); assert(!test_rmdir(blocked));
    assert(!debug_rotate_log());
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "recovered");
    assert(contents(1, text, sizeof(text)) && strstr(text, "before failure"));
    assert(contents(0, text, sizeof(text)) && strstr(text, "recovered"));
    cleanup();
}

#ifdef _WIN32
static void windows_reader_blocks_rename(void) {
    setup("held.log", 5);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "before locked rename");
    wchar_t wide[2048];
    assert(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 2048));
    HANDLE reader = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    assert(reader != INVALID_HANDLE_VALUE);
    assert(debug_rotate_log() == -1);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "after locked rename");
    char text[9000]; assert(contents(0, text, sizeof(text)));
    assert(strstr(text, "before locked rename") && strstr(text, "after locked rename") && strstr(text, "rotate rename current failed"));
    assert(CloseHandle(reader));
    assert(!debug_rotate_log());
    assert(contents(1, text, sizeof(text)) && strstr(text, "after locked rename"));
    cleanup();
}
#endif

int main(void) {
    assert(!test_mkdtemp(directory));
    retention(); startup_and_reopen(); concurrent_writes(); rotation_failure();
#ifdef _WIN32
    windows_reader_blocks_rename();
#endif
    assert(!test_rmdir(directory));
    puts("PASS log rotation: size, five files, startup, reopen, UTF-8, concurrent writers, failure recovery");
    return 0;
}
