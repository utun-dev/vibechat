#include "db_storage.h"
#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include "../lib/file_io.h"
#endif

int db_storage_clear(const char* directory) {
    if (!directory || !*directory) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Database reset refused: empty directory"); return -EINVAL;
    }
    size_t length = strlen(directory);
    if (length > SIZE_MAX - sizeof("/chats.db-journal")) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Database reset path overflow bytes=%zu", length); return -ENAMETOOLONG;
    }
    size_t capacity = length + sizeof("/chats.db-journal");
    char* path = u_malloc(capacity);
    if (!path) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Database reset path allocation failed bytes=%zu", capacity); return -ENOMEM; }
    const char* names[] = {"chats.db-wal", "chats.db-shm", "chats.db-journal", "chats.db"};
    unsigned removed = 0, missing = 0;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(path, capacity, "%s/%s", directory, names[i]);
#ifdef _WIN32
        wchar_t wide[FILE_IO_PATH_MAX];
        if (file_io_wide(path, wide)) { u_free(path); return -EINVAL; }
        int result = _wunlink(wide);
#else
        int result = unlink(path);
#endif
        if (!result) removed++;
        else if (errno == ENOENT) missing++;
        else {
            int error = errno;
            DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Database reset failed file=%s errno=%d: %s; new startup refused",
                path, error, strerror(error));
            u_free(path); return -error;
        }
    }
    u_free(path);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Database reset completed directory=%s removed=%u missing=%u; configuration and identity retained",
        directory, removed, missing);
    return 0;
}
