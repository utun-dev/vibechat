#include "file_io.h"
#include "platform_compat.h"
#include "debug_config.h"
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>

#ifdef _WIN32
static SRWLOCK temp_mutex = SRWLOCK_INIT;
#define TEMP_LOCK() AcquireSRWLockExclusive(&temp_mutex)
#define TEMP_UNLOCK() ReleaseSRWLockExclusive(&temp_mutex)
#else
#include <pthread.h>
static pthread_mutex_t temp_mutex = PTHREAD_MUTEX_INITIALIZER;
#define TEMP_LOCK() pthread_mutex_lock(&temp_mutex)
#define TEMP_UNLOCK() pthread_mutex_unlock(&temp_mutex)
#endif

/* Реестр заимствует стабильный адрес владельца, не выделяет память и общий для workers/instances. */
static struct file_io_temp *owned_temps;

static void file_io_temp_untrack(struct file_io_temp *output) {
    struct file_io_temp **p = &owned_temps;
    while (*p && *p != output) p = &(*p)->next_owned;
    if (*p) *p = output->next_owned;
    else DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file temporary owner missing from registry path=%s", output->temporary);
    output->next_owned = NULL;
}

#ifdef _WIN32
/* Общие пути API — UTF-8; ANSI Win32 API не применяется. */
int file_io_wide(const char *path, wchar_t wide[FILE_IO_PATH_MAX]) {
    if (path && *path && strlen(path) < FILE_IO_PATH_MAX
        && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, FILE_IO_PATH_MAX)) return 0;
    DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file invalid UTF-8/oversized path winerr=%lu", GetLastError());
    return -1;
}
#endif

int file_io_inspect(const char *path, int follow, struct file_io_info *info) {
    if (!path || !*path || strlen(path) >= FILE_IO_PATH_MAX || !info) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file inspect invalid path/output"); return -1;
    }
    struct file_io_info result = {0};
#ifdef _WIN32
    wchar_t wide[FILE_IO_PATH_MAX];
    if (file_io_wide(path, wide)) return -1;
    DWORD attributes = GetFileAttributesW(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return 1;
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file inspect failed path=%s winerr=%lu", path, error); return -1;
    }
    if (!follow && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) result.type = FILE_IO_LINK;
    else {
        HANDLE handle = CreateFileW(wide, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        BY_HANDLE_FILE_INFORMATION data;
        int valid = handle != INVALID_HANDLE_VALUE && GetFileInformationByHandle(handle, &data);
        if (valid) {
            result.type = GetFileType(handle) != FILE_TYPE_DISK ? FILE_IO_OTHER
                : (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? FILE_IO_DIRECTORY : FILE_IO_REGULAR;
            uint64_t size = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
            valid = size <= INT64_MAX;
            result.size = (int64_t)size;
            result.identity[0] = data.dwVolumeSerialNumber;
            result.identity[1] = ((uint64_t)data.nFileIndexHigh << 32) | data.nFileIndexLow;
        }
        DWORD error = GetLastError();
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        if (!valid) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file handle inspect failed path=%s winerr=%lu", path, error); return -1; }
    }
#else
    struct stat data;
    if ((follow ? stat(path, &data) : lstat(path, &data))) {
        if (errno == ENOENT) return 1;
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file inspect failed path=%s errno=%d", path, errno); return -1;
    }
    result.type = S_ISREG(data.st_mode) ? FILE_IO_REGULAR : S_ISDIR(data.st_mode) ? FILE_IO_DIRECTORY
        : S_ISLNK(data.st_mode) ? FILE_IO_LINK : FILE_IO_OTHER;
    if (data.st_size < 0 || (uint64_t)data.st_size > INT64_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file size out of range path=%s", path); return -1;
    }
    result.size = (int64_t)data.st_size;
    result.identity[0] = data.st_dev; result.identity[1] = data.st_ino;
#endif
    *info = result;
    return 0;
}

FILE *file_io_open_read(const char *path) {
    struct file_io_info info;
    if (file_io_inspect(path, 1, &info) || info.type != FILE_IO_REGULAR) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file source is not regular/readable path=%s", path ? path : "(null)"); return NULL;
    }
#ifdef _WIN32
    wchar_t wide[FILE_IO_PATH_MAX];
    if (file_io_wide(path, wide)) return NULL;
    FILE *file = _wfopen(wide, L"rb");
#else
    FILE *file = fopen(path, "rb");
#endif
    if (!file) DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file open read failed path=%s errno=%d", path, errno);
    return file;
}

FILE *file_io_open_update(const char *path) {
    struct file_io_info info;
    if (file_io_inspect(path, 0, &info) || info.type != FILE_IO_REGULAR) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file update requires regular file path=%s", path ? path : "(null)"); return NULL;
    }
#ifdef _WIN32
    wchar_t wide[FILE_IO_PATH_MAX];
    if (file_io_wide(path, wide)) return NULL;
    FILE *file = _wfopen(wide, L"r+b");
#else
    FILE *file = fopen(path, "r+b");
#endif
    if (!file) DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file update open failed path=%s errno=%d", path, errno);
    return file;
}

int file_io_paths(const char *source, const char *destination) {
    struct file_io_info input, output;
    if (file_io_inspect(source, 1, &input) || input.type != FILE_IO_REGULAR) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file invalid source path=%s", source ? source : "(null)"); return -1;
    }
    int status = file_io_inspect(destination, 0, &output);
    if (status == 1) return 0;
    if (status < 0) return -1;
    if (output.type != FILE_IO_REGULAR) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file destination is not regular path=%s type=%d", destination, output.type); return -1;
    }
    return !memcmp(input.identity, output.identity, sizeof(input.identity));
}

/* Синхронизировать именно открытый файл; повторного открытия имени нет. */
static int file_io_sync_descriptor(FILE *file, const char *path) {
#ifdef _WIN32
    int result = _commit(_fileno(file));
#else
    int result = fsync(fileno(file));
#endif
    if (result) {
        int error = errno ? errno : EIO;
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file sync failed path=%s errno=%d", path, error);
        errno = error;
    }
    return result ? -1 : 0;
}

int file_io_sync_file(const char *path) {
    FILE *file = file_io_open_update(path);
    if (!file) return -1;
    int result = file_io_sync_descriptor(file, path);
    if (fclose(file)) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file sync close failed path=%s errno=%d", path, errno); result = -1;
    }
    return result;
}

int file_io_sync_directory(const char *path) {
    if (!path || !*path || strlen(path) >= FILE_IO_PATH_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file directory sync invalid path"); return -1;
    }
#ifndef _WIN32
    char parent[FILE_IO_PATH_MAX];
    memcpy(parent, path, strlen(path) + 1);
    char *slash = strrchr(parent, '/');
    if (!slash) memcpy(parent, ".", 2);
    else if (slash == parent) slash[1] = 0;
    else *slash = 0;
    int fd = open(parent, O_RDONLY | O_DIRECTORY);
    if (fd < 0) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file directory open failed path=%s errno=%d", parent, errno); return -1; }
    int error = fsync(fd) ? (errno ? errno : EIO) : 0;
    if (close(fd) && !error) error = errno ? errno : EIO;
    if (error) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file directory sync/close failed path=%s errno=%d", parent, error); return -1; }
#endif
    return 0;
}

/* Только rename; владелец temp снимает регистрацию до длительного directory sync. */
static int file_io_rename(const char *source, const char *destination, unsigned flags) {
#ifdef _WIN32
    wchar_t from[FILE_IO_PATH_MAX], to[FILE_IO_PATH_MAX];
    if (file_io_wide(source, from) || file_io_wide(destination, to)) return -1;
    DWORD options = MOVEFILE_REPLACE_EXISTING | ((flags & FILE_IO_DURABLE) ? MOVEFILE_WRITE_THROUGH : 0);
    if (!MoveFileExW(from, to, options)) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file rename failed source=%s destination=%s winerr=%lu", source, destination, GetLastError());
        return -1;
    }
#else
    (void)flags;
    if (rename(source, destination)) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file rename failed source=%s destination=%s errno=%d", source, destination, errno); return -1;
    }
#endif
    return 0;
}

int file_io_publish(const char *source, const char *destination) {
    if (file_io_paths(source, destination)) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file publish invalid source/destination"); return -1;
    }
    if (file_io_rename(source, destination, FILE_IO_DURABLE)) return -1;
    if (file_io_sync_directory(destination)) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file installed but directory durability unconfirmed destination=%s", destination); return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "file durably installed destination=%s", destination);
    return 0;
}

int file_io_temp_open(struct file_io_temp *output, const char *destination, unsigned flags) {
    if (!output || output->file || output->temporary[0] || !destination || !*destination
        || (flags & ~FILE_IO_DURABLE) || strlen(destination) + 38 >= FILE_IO_PATH_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file temporary invalid owner/path"); return -1;
    }
    output->flags = flags;
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        uint8_t random[16]; char suffix[33];
        if (random_bytes(random, sizeof(random))) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file temporary RNG failed"); return -1; }
        for (unsigned i = 0; i < sizeof(random); i++) snprintf(suffix + i * 2, 3, "%02x", random[i]);
        char temporary[FILE_IO_PATH_MAX];
        snprintf(temporary, sizeof(temporary), "%s.tmp.%s", destination, suffix);
#ifdef _WIN32
        wchar_t wide[FILE_IO_PATH_MAX];
        if (file_io_wide(temporary, wide)) return -1;
        TEMP_LOCK();
        int fd = _wopen(wide, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
        TEMP_LOCK();
        int fd = open(temporary, O_CREAT | O_EXCL | O_RDWR, 0600);
#endif
        if (fd < 0) {
            int error = errno; TEMP_UNLOCK();
            if (error == EEXIST) continue;
            DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file temporary open failed destination=%s errno=%d", destination, error); return -1;
        }
        memcpy(output->temporary, temporary, strlen(temporary) + 1);
        output->next_owned = owned_temps; owned_temps = output;
        TEMP_UNLOCK(); /* Файл виден очистке только вместе с зарегистрированным владельцем. */
#ifdef _WIN32
        output->file = _fdopen(fd, "wb+");
        if (!output->file) _close(fd);
#else
        output->file = fdopen(fd, "wb+");
        if (!output->file) close(fd);
#endif
        if (!output->file) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file fdopen failed path=%s errno=%d", output->temporary, errno); return -1; }
        DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "file temporary opened path=%s durable=%u", output->temporary, flags & FILE_IO_DURABLE);
        return 0;
    }
    DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file temporary collision limit destination=%s", destination); return -1;
}

int file_io_temp_finish(struct file_io_temp *output) {
    if (!output || !output->file) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file finish without open file"); return -1; }
    int error = fflush(output->file) ? (errno ? errno : EIO) : 0;
    if (!error && (output->flags & FILE_IO_DURABLE) && file_io_sync_descriptor(output->file, output->temporary))
        error = errno ? errno : EIO;
    if (fclose(output->file) && !error) error = errno ? errno : EIO;
    output->file = NULL;
    if (error) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file flush/sync/close failed path=%s errno=%d", output->temporary, error); return -1; }
    DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "file temporary finished path=%s durable=%u", output->temporary, output->flags & FILE_IO_DURABLE);
    return 0;
}

void file_io_temp_discard(struct file_io_temp *output) {
    if (!output) return;
    if (output->file) {
        if (fclose(output->file)) DEBUG_WARN(DEBUG_CATEGORY_SYS, "file cleanup close failed errno=%d", errno);
        output->file = NULL;
    }
    if (!output->temporary[0]) return;
#ifdef _WIN32
    wchar_t wide[FILE_IO_PATH_MAX];
    int valid = file_io_wide(output->temporary, wide) == 0;
    TEMP_LOCK();
    int error = valid ? _wremove(wide) : -1;
#else
    TEMP_LOCK();
    int error = remove(output->temporary);
#endif
    if (error && errno != ENOENT) DEBUG_WARN(DEBUG_CATEGORY_SYS, "file cleanup remove failed path=%s errno=%d", output->temporary, errno);
    file_io_temp_untrack(output);
    output->temporary[0] = 0;
    TEMP_UNLOCK();
}

int file_io_temp_publish(struct file_io_temp *output, const char *source, const char *destination) {
    if (!output || output->file || !output->temporary[0] || file_io_paths(source, destination)) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file publish invalid state/paths"); return -1;
    }
    TEMP_LOCK();
    if (file_io_rename(output->temporary, destination, output->flags)) { TEMP_UNLOCK(); return -1; }
    file_io_temp_untrack(output);
    output->temporary[0] = 0;
    TEMP_UNLOCK();
    if ((output->flags & FILE_IO_DURABLE) && file_io_sync_directory(destination)) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file temporary installed but directory durability unconfirmed destination=%s", destination); return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "file published destination=%s durable=%u", destination, output->flags & FILE_IO_DURABLE);
    return 0;
}

int file_io_remove_orphan(const char *path) {
    if (!path || !*path) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "file orphan removal without path"); return -1; }
#ifdef _WIN32
    wchar_t wide[FILE_IO_PATH_MAX];
    if (file_io_wide(path, wide)) return -1;
#endif
    TEMP_LOCK();
    for (struct file_io_temp *owner = owned_temps; owner; owner = owner->next_owned) {
        if (!strcmp(owner->temporary, path)) {
            TEMP_UNLOCK();
            DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "file orphan cleanup keeps active temporary path=%s", path);
            return 0;
        }
    }
#ifdef _WIN32
    int result = _wremove(wide);
#else
    int result = remove(path);
#endif
    int error = errno; TEMP_UNLOCK();
    if (!result) return 1;
    if (error == ENOENT) return 0;
    DEBUG_WARN(DEBUG_CATEGORY_SYS, "file orphan removal failed path=%s errno=%d", path, error);
    return -1;
}
