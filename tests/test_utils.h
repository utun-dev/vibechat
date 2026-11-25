/* Небольшие файловые/env helpers тестов POSIX/Windows; возврат 0 — успех, -1 либо код CRT — ошибка.
 * При UTUN_TEST_TEMP_REGISTRY созданные каталоги регистрируются для runner: он сохраняет логи и удаляет данные,
 * в том числе после аварии/timeout. Без runner созданными каталогами/файлами владеет caller.
 * POSIX mkdtemp требует writable шаблон с конечными XXXXXX. Windows игнорирует шаблон и возвращает путь в TEMP:
 * буфер caller должен вмещать MAX_PATH, для переносимого кода передавать ненулевой буфер не меньше этого размера.
 * Windows использует общие rand/srand и статический буфер в каждой единице трансляции; параллельные вызовы не поддерживаются. */

#ifndef TEST_UTILS_H
#define TEST_UTILS_H

#include "../lib/platform_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <dirent.h>
#endif

/* Зарегистрировать только что созданный временный файл/каталог для runner; 0/-1 с диагностикой.
 * Вызывать до работы с данными. Без UTUN_TEST_TEMP_REGISTRY регистрация не требуется. */
static inline int test_track_temporary(const char* path) {
    const char* registry = getenv("UTUN_TEST_TEMP_REGISTRY");
    if (!registry || !*registry) return 0;
    if (strchr(path, '\n') || strchr(path, '\r')) {
        fprintf(stderr, "[ERROR] temporary registry: newline in path\n"); return -1;
    }
    FILE* file = fopen(registry, "ab");
    if (!file) { fprintf(stderr, "[ERROR] temporary registry: open %s: %s\n", registry, strerror(errno)); return -1; }
    int written = fprintf(file, "%s\n", path), closed = fclose(file);
    if (written < 0 || closed) {
        fprintf(stderr, "[ERROR] temporary registry: write path=%s registry=%s: %s\n", path, registry, strerror(errno)); return -1;
    }
    return 0;
}

#ifdef _WIN32
#include <windows.h>
#include <direct.h>

// Временный путь текущей единицы трансляции; успешный test_mkdtemp копирует его в буфер caller.
static char test_temp_dir[MAX_PATH];

// До 100 попыток создать TEMP/utun_test_XXXXXXXX; 0 — создан (путь копируется при template_str!=NULL), -1 — отказ.
static inline int test_mkdtemp(char *template_str) {
    char tmp_path[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp_path);
    
    // Generate unique directory name
    srand((unsigned int)GetTickCount());
    int attempts = 0;
    while (attempts < 100) {
        snprintf(test_temp_dir, sizeof(test_temp_dir), "%s\\utun_test_%08x", 
                 tmp_path, (unsigned int)rand());
        if (_mkdir(test_temp_dir) == 0) {
            if (test_track_temporary(test_temp_dir)) { _rmdir(test_temp_dir); return -1; }
            // Copy path back to caller's buffer if provided
            if (template_str) {
                size_t len = strlen(test_temp_dir);
                memcpy(template_str, test_temp_dir, len + 1);  // Include null terminator
            }
            return 0;  // Success
        }
        attempts++;
    }
    return -1;  // Failed after 100 attempts
}

// Изменить CRT _environ, видимый getenv; overwrite=0 сохраняет существующую переменную. Возврат — _putenv_s.
static inline int test_setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value);   /* 0 = success, errno_t != 0 = error */
}

// Удалить один файл (не дерево каталогов), возврат _unlink: 0/-1.
static inline int test_unlink(const char *path) {
    return _unlink(path);
}

// Удалить пустой каталог, возврат _rmdir: 0/-1.
static inline int test_rmdir(const char *path) {
    return _rmdir(path);
}

#else
// POSIX: helpers делегируют штатным API; errno остаётся у вызывающего.
#include <unistd.h>

// Создать каталог по mutable шаблону XXXXXX и записать путь в тот же буфер; 0/-1.
static inline int test_mkdtemp(char *template_str) {
    if (!mkdtemp(template_str)) return -1;
    if (test_track_temporary(template_str)) { rmdir(template_str); return -1; }
    return 0;
}

// Изменить environment через setenv; overwrite=0 сохраняет существующее значение, возврат 0/-1.
static inline int test_setenv(const char *name, const char *value, int overwrite) {
    return setenv(name, value, overwrite);
}

// Удалить один файл/симлинк через unlink, не рекурсивно; 0/-1.
static inline int test_unlink(const char *path) {
    return unlink(path);
}

// Удалить пустой каталог через rmdir; 0/-1.
static inline int test_rmdir(const char *path) {
    return rmdir(path);
}

#endif

/* Удалить принадлежащее тесту дерево, включая корень, не переходя по симлинкам/reparse points.
 * Вызывать после остановки workers и закрытия файлов; 0 — удалено, -1 — ошибка с путём в stderr. */
static inline int test_remove_tree(const char* path) {
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "[ERROR] cleanup: attributes path=%s error=%lu\n", path, (unsigned long)GetLastError()); return -1;
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        if (!test_unlink(path)) return 0;
    } else if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        char pattern[1024];
        if (snprintf(pattern, sizeof(pattern), "%s/*", path) >= (int)sizeof(pattern)) {
            fprintf(stderr, "[ERROR] cleanup: directory path too long: %s\n", path); return -1;
        }
        WIN32_FIND_DATAA entry; HANDLE entries = FindFirstFileA(pattern, &entry);
        if (entries == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND && !test_rmdir(path)) return 0;
            fprintf(stderr, "[ERROR] cleanup: enumerate path=%s error=%lu\n", path, (unsigned long)GetLastError()); return -1;
        }
        int result = 0;
        do {
            if (!strcmp(entry.cFileName, ".") || !strcmp(entry.cFileName, "..")) continue;
            char child[1024];
            if (snprintf(child, sizeof(child), "%s/%s", path, entry.cFileName) >= (int)sizeof(child)) {
                fprintf(stderr, "[ERROR] cleanup: child path too long: %s/%s\n", path, entry.cFileName); result = -1; break;
            }
            if (test_remove_tree(child)) { result = -1; break; }
        } while (FindNextFileA(entries, &entry));
        DWORD error = GetLastError();
        if (!result && error != ERROR_NO_MORE_FILES) {
            fprintf(stderr, "[ERROR] cleanup: enumerate path=%s error=%lu\n", path, (unsigned long)error); result = -1;
        }
        if (!FindClose(entries)) {
            fprintf(stderr, "[ERROR] cleanup: close enumeration path=%s error=%lu\n", path, (unsigned long)GetLastError()); result = -1;
        }
        if (result) return -1;
        if (!test_rmdir(path)) return 0;
    } else if (!test_rmdir(path)) return 0;
#else
    struct stat info;
    if (lstat(path, &info)) {
        fprintf(stderr, "[ERROR] cleanup: stat path=%s: %s\n", path, strerror(errno)); return -1;
    }
    if (!S_ISDIR(info.st_mode)) {
        if (!test_unlink(path)) return 0;
    } else {
        DIR* entries = opendir(path);
        if (!entries) { fprintf(stderr, "[ERROR] cleanup: open path=%s: %s\n", path, strerror(errno)); return -1; }
        struct dirent* entry; int result = 0; errno = 0;
        while ((entry = readdir(entries))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            char child[1024];
            if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= (int)sizeof(child)) {
                fprintf(stderr, "[ERROR] cleanup: child path too long: %s/%s\n", path, entry->d_name); result = -1; break;
            }
            if (test_remove_tree(child)) { result = -1; break; }
            errno = 0;
        }
        if (!result && errno) { fprintf(stderr, "[ERROR] cleanup: read path=%s: %s\n", path, strerror(errno)); result = -1; }
        if (closedir(entries)) { fprintf(stderr, "[ERROR] cleanup: close path=%s: %s\n", path, strerror(errno)); result = -1; }
        if (result) return -1;
        if (!test_rmdir(path)) return 0;
    }
#endif
    fprintf(stderr, "[ERROR] cleanup: remove path=%s: %s\n", path, strerror(errno)); return -1;
}

#endif // TEST_UTILS_H
