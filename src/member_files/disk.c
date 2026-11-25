#ifndef _WIN32
#define _FILE_OFFSET_BITS 64
#endif
#include "disk.h"
#include "../../lib/debug_config.h"
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>
#include <sys/stat.h>
#include <openssl/evp.h>
#ifndef _WIN32
#include <dirent.h>
#endif

static int disk_error(const char* operation, const char* path) {
    int error = errno;
    int status = error == ENOENT ? MF_NOT_FOUND : error == EEXIST ? MF_EXISTS :
                 error == ENOSPC ? MF_NO_SPACE : error == ENOTDIR ? MF_INVALID_PATH : MF_IO_ERROR;
    if (error == ENOTEMPTY || (error == EEXIST && !strcmp(operation, "remove"))) status = MF_DIRECTORY_NOT_EMPTY;
#ifdef EDQUOT
    if (error == EDQUOT) status = MF_NO_SPACE;
#endif
#ifdef ELOOP
    if (error == ELOOP) status = MF_INVALID_PATH;
#endif
    if (status == MF_NOT_FOUND || status == MF_EXISTS)
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: disk %s path=%s status=%s errno=%d (%s)", operation,
                    path ? path : "", mf_status_text(status), error, strerror(error));
    if (status != MF_NOT_FOUND && status != MF_EXISTS)
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: disk %s path=%s errno=%d (%s)", operation, path ? path : "", error, strerror(error));
    return status;
}

#ifdef _WIN32
struct disk_parent { HANDLE held[66]; unsigned count; wchar_t path[4096]; };

static int disk_wide(const char* path, wchar_t* wide, size_t cap) {
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, (int)cap)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: invalid UTF-8 path winerr=%lu", GetLastError()); return MF_INVALID_PATH;
    }
    return MF_OK;
}

static int disk_win_error(const char* operation) {
    DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS)
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: %s winerr=%lu", operation, e);
    if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND && e != ERROR_ALREADY_EXISTS && e != ERROR_FILE_EXISTS)
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: %s winerr=%lu", operation, e);
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return MF_NOT_FOUND;
    if (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS) return MF_EXISTS;
    if (e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL) return MF_NO_SPACE;
    if (e == ERROR_DIR_NOT_EMPTY) return MF_DIRECTORY_NOT_EMPTY;
    return MF_IO_ERROR;
}

static int disk_handle_info(HANDLE h, struct mf_disk_info* out) {
    BY_HANDLE_FILE_INFORMATION i;
    if (!GetFileInformationByHandle(h, &i)) return disk_win_error("GetFileInformationByHandle");
    if ((i.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || i.nNumberOfLinks > 1) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: unsupported file attributes=%lu links=%lu", i.dwFileAttributes, i.nNumberOfLinks);
        return MF_INVALID_PATH;
    }
    memset(out, 0, sizeof(*out));
    out->type = i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? MF_DIRECTORY : MF_FILE;
    out->size = out->type == MF_FILE ? ((uint64_t)i.nFileSizeHigh << 32) | i.nFileSizeLow : 0;
    uint64_t written = ((uint64_t)i.ftLastWriteTime.dwHighDateTime << 32) | i.ftLastWriteTime.dwLowDateTime;
    out->mtime = written > 116444736000000000ULL ? (written - 116444736000000000ULL) / 10000000 : 0;
    out->identity[0] = i.dwVolumeSerialNumber;
    out->identity[1] = ((uint64_t)i.nFileIndexHigh << 32) | i.nFileIndexLow;
    out->identity[2] = written;
    out->identity[3] = ((uint64_t)i.ftCreationTime.dwHighDateTime << 32) | i.ftCreationTime.dwLowDateTime;
    out->identity[4] = out->size;
    return MF_OK;
}

static void disk_parent_close(struct disk_parent* p) {
    while (p->count) if (!CloseHandle(p->held[--p->count])) disk_win_error("close directory");
}

/* Каждый родитель закреплён без SHARE_DELETE до завершения операции над листом. */
static int disk_parent_open(struct mf_disk_root* root, const char* path, struct disk_parent* p) {
    memset(p, 0, sizeof(*p));
    char full[4096];
    int n = snprintf(full, sizeof(full), "%s/%s", root->path, path);
    if (n < 0 || (size_t)n >= sizeof(full) || disk_wide(full, p->path, 4096)) return MF_INVALID_PATH;
    size_t begin = strlen(root->path) + 1;
    char relative[MF_PATH_STORAGE_MAX];
    if (strlen(path) >= sizeof(relative)) return MF_INVALID_PATH;
    snprintf(relative, sizeof(relative), "%s", path);
    for (char* s = relative; *s; s++) {
        if (*s != '/') continue;
        size_t at = begin + (size_t)(s - relative);
        char saved = full[at]; full[at] = 0;
        wchar_t dir[4096];
        int result = disk_wide(full, dir, 4096); full[at] = saved;
        if (result || p->count >= 66) { disk_parent_close(p); return MF_INVALID_PATH; }
        HANDLE h = CreateFileW(dir, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (h == INVALID_HANDLE_VALUE) { result = disk_win_error("open parent"); disk_parent_close(p); return result; }
        p->held[p->count++] = h;
        struct mf_disk_info info;
        result = disk_handle_info(h, &info);
        if (result || info.type != MF_DIRECTORY) { disk_parent_close(p); return result ? result : MF_INVALID_PATH; }
    }
    return MF_OK;
}

static int disk_root_open(struct mf_disk_root* root, const char* path, int create) {
    wchar_t wide[4096];
    if (strlen(path) >= sizeof(root->path) || disk_wide(path, wide, 4096)) return MF_INVALID_PATH;
    if (create && !CreateDirectoryW(wide, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return disk_win_error("create root");
    root->handle = CreateFileW(wide, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (root->handle == INVALID_HANDLE_VALUE) return disk_win_error("open root");
    struct mf_disk_info info;
    int rc = disk_handle_info(root->handle, &info);
    if (rc || info.type != MF_DIRECTORY) { CloseHandle(root->handle); return rc ? rc : MF_INVALID_PATH; }
    snprintf(root->path, sizeof(root->path), "%s", path);
    return MF_OK;
}

void mf_disk_root_close(struct mf_disk_root* root) {
    if (!CloseHandle(root->handle)) disk_win_error("close root");
}

int mf_disk_open(struct mf_disk_root* root, const char* path, int create, int writable, int* fd) {
    struct disk_parent p; int rc = disk_parent_open(root, path, &p);
    if (rc) return rc;
    HANDLE h = CreateFileW(p.path, GENERIC_READ | (writable ? GENERIC_WRITE : 0), FILE_SHARE_READ, NULL,
                          create ? CREATE_NEW : OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) rc = disk_win_error("open file");
    else {
        struct mf_disk_info i; rc = disk_handle_info(h, &i);
        if (!rc && i.type != MF_FILE) rc = MF_INVALID_PATH;
        if (!rc) {
            *fd = _open_osfhandle((intptr_t)h, _O_BINARY | (writable ? _O_RDWR : _O_RDONLY));
            if (*fd < 0) rc = disk_error("open fd", path);
        }
        if (rc) CloseHandle(h);
    }
    disk_parent_close(&p); return rc;
}

int mf_disk_stat(struct mf_disk_root* root, const char* path, struct mf_disk_info* info) {
    struct disk_parent p; int rc = disk_parent_open(root, path, &p);
    if (rc) return rc;
    HANDLE h = CreateFileW(p.path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                          OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) rc = disk_win_error("stat");
    else { rc = disk_handle_info(h, info); CloseHandle(h); }
    disk_parent_close(&p); return rc;
}

int mf_disk_info(int fd, struct mf_disk_info* info) { return disk_handle_info((HANDLE)_get_osfhandle(fd), info); }

int mf_disk_mkdir(struct mf_disk_root* root, const char* path, int* created) {
    if (created) *created = 0;
    struct disk_parent p; int rc = disk_parent_open(root, path, &p);
    if (rc) return rc;
    if (!CreateDirectoryW(p.path, NULL)) rc = disk_win_error("mkdir");
    else if (created) *created = 1;
    disk_parent_close(&p); return rc;
}

int mf_disk_remove(struct mf_disk_root* root, const char* path, int directory) {
    struct disk_parent p; int rc = disk_parent_open(root, path, &p);
    if (rc) return rc;
    if (!(directory ? RemoveDirectoryW(p.path) : DeleteFileW(p.path))) rc = disk_win_error("remove");
    disk_parent_close(&p); return rc;
}

int mf_disk_rename(struct mf_disk_root* root, const char* from, const char* to) {
    struct disk_parent a, b; int rc = disk_parent_open(root, from, &a);
    if (rc) return rc;
    rc = disk_parent_open(root, to, &b);
    if (!rc) {
        if (!MoveFileExW(a.path, b.path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) rc = disk_win_error("publish/rename");
        disk_parent_close(&b);
    }
    disk_parent_close(&a); return rc;
}

int mf_disk_list(struct mf_disk_root* root, const char* path, mf_disk_entry_fn cb, void* arg) {
    char probe[MF_PATH_MAX + 100]; snprintf(probe, sizeof(probe), "%s%s_", path, *path ? "/" : "");
    struct disk_parent p; int rc = disk_parent_open(root, probe, &p);
    if (rc) return rc;
    size_t n = wcslen(p.path); p.path[n - 1] = L'*';
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(p.path, &data);
    if (find == INVALID_HANDLE_VALUE) {
        rc = GetLastError() == ERROR_FILE_NOT_FOUND ? MF_OK : disk_win_error("list");
        disk_parent_close(&p); return rc;
    }
    for (;;) {
        if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) goto next;
        char name[MF_NAME_MAX * 4 + 1], child[MF_PATH_MAX + 100];
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, data.cFileName, -1, name, sizeof(name), NULL, NULL)) {
            rc = MF_INVALID_PATH; break;
        }
        if (snprintf(child, sizeof(child), "%s/%s", path, name) >= (int)sizeof(child)) { rc = MF_INVALID_PATH; break; }
        struct mf_disk_info info; rc = mf_disk_stat(root, child, &info);
        if (rc) break;
        int result = cb(arg, name, &info);
        if (result) { rc = result < 0 ? -result : MF_OK; break; }
next:
        if (!FindNextFileW(find, &data)) {
            if (GetLastError() != ERROR_NO_MORE_FILES) rc = disk_win_error("FindNextFileW");
            break;
        }
    }
    FindClose(find); disk_parent_close(&p); return rc;
}

int mf_disk_resize(int fd, uint64_t size) {
    if (_chsize_s(fd, size)) return disk_error("resize", NULL);
    return MF_OK;
}
int mf_disk_sync(int fd) { return _commit(fd) ? disk_error("sync", NULL) : MF_OK; }
int mf_disk_close(int fd) { return _close(fd) ? disk_error("close", NULL) : MF_OK; }

int mf_disk_local_open(const char* path, int create, int* fd) {
    wchar_t wide[4096]; if (disk_wide(path, wide, 4096)) return MF_INVALID_PATH;
    *fd = _wopen(wide, _O_BINARY | (create ? _O_RDWR | _O_CREAT | _O_EXCL : _O_RDONLY), _S_IREAD | _S_IWRITE);
    return *fd < 0 ? disk_error("local open", path) : MF_OK;
}
int mf_disk_local_publish(const char* from, const char* to) {
    wchar_t a[4096], b[4096]; if (disk_wide(from, a, 4096) || disk_wide(to, b, 4096)) return MF_INVALID_PATH;
    return MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? MF_OK : disk_win_error("local publish");
}
int mf_disk_local_remove(const char* path) {
    wchar_t wide[4096]; if (disk_wide(path, wide, 4096)) return MF_INVALID_PATH;
    if (DeleteFileW(wide) || GetLastError() == ERROR_FILE_NOT_FOUND) return MF_OK;
    return disk_win_error("local remove");
}
#else
struct disk_parent { int fd; char leaf[MF_NAME_MAX + 1]; };

static int disk_parent_open(struct mf_disk_root* root, const char* path, struct disk_parent* p) {
    char copy[MF_PATH_STORAGE_MAX];
    if (strlen(path) >= sizeof(copy)) return MF_INVALID_PATH;
    snprintf(copy, sizeof(copy), "%s", path);
    p->fd = dup(root->fd);
    if (p->fd < 0) return disk_error("dup root", path);
    char* component = copy;
    for (char* slash; (slash = strchr(component, '/')); component = slash + 1) {
        *slash = 0;
        int next = openat(p->fd, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(p->fd);
        if (next < 0) return disk_error("open parent", path);
        p->fd = next;
    }
    if (strlen(component) > MF_NAME_MAX) { close(p->fd); return MF_INVALID_PATH; }
    snprintf(p->leaf, sizeof(p->leaf), "%s", component);
    return MF_OK;
}

static int disk_posix_info(const struct stat* st, struct mf_disk_info* i) {
    if ((!S_ISREG(st->st_mode) && !S_ISDIR(st->st_mode)) || (S_ISREG(st->st_mode) && st->st_nlink != 1)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: unsupported file mode=%lo links=%llu",
            (unsigned long)st->st_mode, (unsigned long long)st->st_nlink);
        return MF_INVALID_PATH;
    }
    memset(i, 0, sizeof(*i)); i->type = S_ISDIR(st->st_mode) ? MF_DIRECTORY : MF_FILE;
    i->size = i->type == MF_FILE ? (uint64_t)st->st_size : 0; i->mtime = (uint64_t)st->st_mtime;
    i->identity[0] = st->st_dev; i->identity[1] = st->st_ino;
    i->identity[2] = st->st_mtim.tv_sec; i->identity[3] = st->st_mtim.tv_nsec;
    i->identity[4] = st->st_ctim.tv_sec; i->identity[5] = st->st_ctim.tv_nsec;
    return MF_OK;
}

static int disk_root_open(struct mf_disk_root* root, const char* path, int create) {
    if (strlen(path) >= sizeof(root->path)) return MF_INVALID_PATH;
    if (create && mkdir(path, 0700) && errno != EEXIST) return disk_error("create root", path);
    root->fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root->fd < 0) return disk_error("open root", path);
    snprintf(root->path, sizeof(root->path), "%s", path); return MF_OK;
}
void mf_disk_root_close(struct mf_disk_root* root) { if (close(root->fd)) disk_error("close root", root->path); }

int mf_disk_open(struct mf_disk_root* root, const char* path, int create, int writable, int* fd) {
    struct disk_parent p; int rc = disk_parent_open(root, path, &p);
    if (rc) return rc;
    *fd = openat(p.fd, p.leaf, O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (writable ? O_RDWR : O_RDONLY) |
                 (create ? O_CREAT | O_EXCL : 0), 0600);
    if (*fd < 0) rc = disk_error("open file", path);
    else {
        struct mf_disk_info info; rc = mf_disk_info(*fd, &info);
        if (!rc && info.type != MF_FILE) rc = MF_INVALID_PATH;
        if (rc) { close(*fd); *fd = -1; }
    }
    close(p.fd); return rc;
}
int mf_disk_stat(struct mf_disk_root* root, const char* path, struct mf_disk_info* info) {
    struct disk_parent p; int rc = disk_parent_open(root, path, &p);
    if (rc) return rc;
    struct stat st;
    if (fstatat(p.fd, p.leaf[0] ? p.leaf : ".", &st, AT_SYMLINK_NOFOLLOW)) rc = disk_error("stat", path);
    else rc = disk_posix_info(&st, info);
    close(p.fd); return rc;
}
int mf_disk_info(int fd, struct mf_disk_info* info) {
    struct stat st;
    return fstat(fd, &st) ? disk_error("fstat", NULL) : disk_posix_info(&st, info);
}
int mf_disk_mkdir(struct mf_disk_root* root, const char* path, int* created) {
    if (created) *created = 0;
    struct disk_parent p; int rc = disk_parent_open(root, path, &p);
    if (rc) return rc;
    if (mkdirat(p.fd, p.leaf, 0700)) rc = disk_error("mkdir", path);
    else {
        if (created) *created = 1;
        if (fsync(p.fd)) rc = disk_error("sync mkdir", path);
    }
    close(p.fd); return rc;
}
int mf_disk_remove(struct mf_disk_root* root, const char* path, int directory) {
    struct disk_parent p; int rc = disk_parent_open(root, path, &p);
    if (rc) return rc;
    if (unlinkat(p.fd, p.leaf, directory ? AT_REMOVEDIR : 0)) rc = disk_error("remove", path);
    else if (fsync(p.fd)) rc = disk_error("sync remove", path);
    close(p.fd); return rc;
}
int mf_disk_rename(struct mf_disk_root* root, const char* from, const char* to) {
    struct disk_parent a, b; int rc = disk_parent_open(root, from, &a);
    if (rc) return rc;
    rc = disk_parent_open(root, to, &b);
    if (!rc) {
        if (renameat(a.fd, a.leaf, b.fd, b.leaf)) rc = disk_error("rename", to);
        else if (fsync(b.fd) || fsync(a.fd)) rc = disk_error("sync rename", to);
        close(b.fd);
    }
    close(a.fd); return rc;
}
int mf_disk_list(struct mf_disk_root* root, const char* path, mf_disk_entry_fn cb, void* arg) {
    char probe[MF_PATH_MAX + 100];
    if (snprintf(probe, sizeof(probe), "%s%s_", path, *path ? "/" : "") >= (int)sizeof(probe)) return MF_INVALID_PATH;
    struct disk_parent p; int rc = disk_parent_open(root, probe, &p);
    if (rc) return rc;
    DIR* dir = fdopendir(p.fd);
    if (!dir) { rc = disk_error("list", path); close(p.fd); return rc; }
    struct dirent* e;
    for (;;) {
        errno = 0; e = readdir(dir);
        if (!e) { if (errno) rc = disk_error("readdir", path); break; }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        struct stat st; struct mf_disk_info info;
        if (fstatat(dirfd(dir), e->d_name, &st, AT_SYMLINK_NOFOLLOW)) { rc = disk_error("list stat", e->d_name); break; }
        rc = disk_posix_info(&st, &info);
        if (rc) { DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: unsafe directory entry path=%s name=%s", path, e->d_name); break; }
        int result = cb(arg, e->d_name, &info);
        if (result) { rc = result < 0 ? -result : MF_OK; break; }
    }
    if (closedir(dir) && !rc) rc = disk_error("closedir", path);
    return rc;
}
int mf_disk_resize(int fd, uint64_t size) { return ftruncate(fd, (off_t)size) ? disk_error("resize", NULL) : MF_OK; }
int mf_disk_sync(int fd) { return fsync(fd) ? disk_error("sync", NULL) : MF_OK; }
int mf_disk_close(int fd) { return close(fd) ? disk_error("close", NULL) : MF_OK; }
int mf_disk_local_open(const char* path, int create, int* fd) {
    *fd = open(path, O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | (create ? O_RDWR | O_CREAT | O_EXCL : O_RDONLY), 0600);
    return *fd < 0 ? disk_error("local open", path) : MF_OK;
}
int mf_disk_local_publish(const char* from, const char* to) { return rename(from, to) ? disk_error("local publish", to) : MF_OK; }
int mf_disk_local_remove(const char* path) { return !unlink(path) || errno == ENOENT ? MF_OK : disk_error("local remove", path); }
#endif

int mf_disk_root_open(struct mf_disk_root* root, const char* path) { return disk_root_open(root, path, 1); }
int mf_disk_root_open_existing(struct mf_disk_root* root, const char* path) { return disk_root_open(root, path, 0); }

int mf_disk_read(int fd, uint64_t offset, void* data, size_t size) {
    size_t done = 0;
    while (done < size) {
#ifdef _WIN32
        if (_lseeki64(fd, (int64_t)(offset + done), SEEK_SET) < 0) return disk_error("seek read", NULL);
        int n = _read(fd, (uint8_t*)data + done, (unsigned)(size - done));
#else
        ssize_t n = pread(fd, (uint8_t*)data + done, size - done, (off_t)(offset + done));
#endif
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: short/read error offset=%llu size=%zu done=%zu errno=%d",
                      (unsigned long long)offset, size, done, errno); return MF_IO_ERROR; }
        done += (size_t)n;
    }
    return MF_OK;
}
int mf_disk_write(int fd, uint64_t offset, const void* data, size_t size) {
    size_t done = 0;
    while (done < size) {
#ifdef _WIN32
        if (_lseeki64(fd, (int64_t)(offset + done), SEEK_SET) < 0) return disk_error("seek write", NULL);
        int n = _write(fd, (const uint8_t*)data + done, (unsigned)(size - done));
#else
        ssize_t n = pwrite(fd, (const uint8_t*)data + done, size - done, (off_t)(offset + done));
#endif
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return disk_error("write", NULL);
        done += (size_t)n;
    }
    return MF_OK;
}

/* Полное хеширование выполняется только worker, с ограниченным буфером. */
int mf_disk_hash(int fd, uint64_t size, uint8_t hash[32]) {
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: hash start fd=%d bytes=%llu", fd, (unsigned long long)size);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    int rc = MF_IO_ERROR;
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) goto out;
    uint8_t buffer[65536];
    for (uint64_t off = 0; off < size;) {
        size_t n = size - off < sizeof(buffer) ? (size_t)(size - off) : sizeof(buffer);
        rc = mf_disk_read(fd, off, buffer, n);
        if (rc || EVP_DigestUpdate(ctx, buffer, n) != 1) { rc = MF_IO_ERROR; goto out; }
        off += n;
    }
    unsigned n = 0;
    rc = EVP_DigestFinal_ex(ctx, hash, &n) == 1 && n == 32 ? MF_OK : MF_IO_ERROR;
out:
    EVP_MD_CTX_free(ctx);
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: hash done fd=%d status=%s", fd, mf_status_text(rc));
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: hash failed size=%llu status=%d", (unsigned long long)size, rc);
    return rc;
}
