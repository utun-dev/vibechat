/* Синхронные примитивы диска для storage и клиентского worker; не вызывать из uasync callbacks.
 * Возврат — MF_OK либо положительный mf_status, не errno/-1. Аргументы/выходы обязательны, кроме отмеченных NULL.
 * Серверные path/from/to — уже проверенные относительные пути; сам слой не запрещает '.'/'..'.
 * На POSIX пути разрешаются
 * относительно открытого каталога через *at/O_NOFOLLOW. Windows удерживает каталоги
 * без FILE_SHARE_DELETE и отвергает reparse points; имена преобразуются UTF-8→UTF-16.
 * fd принадлежит вызывающему; read/write работают по 64-битному смещению.
 * Локальные пути приложения не ограничены корнем storage; их родители/назначение остаются ответственностью caller. */
#ifndef MEMBER_FILES_DISK_H
#define MEMBER_FILES_DISK_H
#include "protocol.h"
#include "../../lib/platform_compat.h"

struct mf_disk_info {
    uint64_t size; // Байты regular-файла; каталог — 0.
    uint64_t mtime; // Unix seconds; для version используются более точные identity.
    uint64_t identity[6]; // Непрозрачный снимок inode/file ID и времён; платформенно зависим, не wire.
    uint8_t type; // MF_FILE/MF_DIRECTORY; другие типы и regular hardlink отклоняются.
};
struct mf_disk_root {
    char path[2048]; // Заимствованный при open путь копируется; предел2047 байт + NUL.
#ifdef _WIN32
    HANDLE handle; // Собственный handle корня, закрывается после всех операций.
#else
    int fd; // Собственный directory fd; операции разрешают имена относительно него.
#endif
};
/* Создать корень, если отсутствует (не создаёт родителей), и закрепить его. Close нужен только после успеха. */
int mf_disk_root_open(struct mf_disk_root* root, const char* path);
/* Закрепить существующий каталог без создания; последний компонент не может быть ссылкой/reparse point. */
int mf_disk_root_open_existing(struct mf_disk_root* root, const char* path);
void mf_disk_root_close(struct mf_disk_root* root);
/* Открыть regular-файл без усечения. create!=0 — эксклюзивное создание, writable!=0 — read/write.
 * fd использовать/закрывать только после MF_OK; отказ не гарантирует значение *fd. */
int mf_disk_open(struct mf_disk_root* root, const char* path, int create, int writable, int* fd);
/* Снимок без чтения содержимого: stat — по имени, info — уже открытого fd. Выход использовать только при успехе. */
int mf_disk_stat(struct mf_disk_root* root, const char* path, struct mf_disk_info* info);
int mf_disk_info(int fd, struct mf_disk_info* info);
/* Создать один каталог. created, если не NULL, показывает факт создания даже при последующей ошибке sync. */
int mf_disk_mkdir(struct mf_disk_root* root, const char* path, int* created);
/* Удалить файл либо пустой каталог. Ошибка sync на POSIX возможна уже после удаления. */
int mf_disk_remove(struct mf_disk_root* root, const char* path, int directory);
/* Заменяющий rename: запрет замены при обычном RENAME проверяет storage. На POSIX sync обоих родителей;
 * ошибка sync не означает откат уже выполненного rename. */
int mf_disk_rename(struct mf_disk_root* root, const char* from, const char* to);
/* name/info заимствованы на время callback; 0 — продолжить, >0 — успешная ранняя остановка, <0 — -mf_status. */
typedef int (*mf_disk_entry_fn)(void* arg, const char* name, const struct mf_disk_info* info);
/* Перечислить записи в порядке файловой системы, без '.'/'..'. Пустой path означает корень. */
int mf_disk_list(struct mf_disk_root* root, const char* path, mf_disk_entry_fn cb, void* arg);
/* Точный диапазон: 0 байт допустимо, короткий read — ошибка. Caller гарантирует offset+size <= INT64_MAX. */
int mf_disk_read(int fd, uint64_t offset, void* data, size_t size);
int mf_disk_write(int fd, uint64_t offset, const void* data, size_t size);
/* Изменить длину (size<=INT64_MAX), синхронизировать содержимое либо закрыть fd; каждый вызов независим. */
int mf_disk_resize(int fd, uint64_t size);
int mf_disk_sync(int fd);
int mf_disk_close(int fd);
/* Локальный source (create=0) либо эксклюзивный read/write temp (create!=0), без усечения.
 * Тип проверяет caller через info; POSIX O_NONBLOCK позволяет отвергнуть FIFO до блокирующего чтения. */
int mf_disk_local_open(const char* path, int create, int* fd);
/* Заменить local назначение: POSIX rename без sync каталога; Windows MOVEFILE_WRITE_THROUGH. */
int mf_disk_local_publish(const char* from, const char* to);
/* Удалить собственный local temp; отсутствие самого файла допустимо. Владение проверяет caller. */
int mf_disk_local_remove(const char* path);
/* SHA256 ровно size байт от нуля, включая пустой файл; ошибка делает hash непригодным. fd остаётся у caller. */
int mf_disk_hash(int fd, uint64_t size, uint8_t hash[32]);
#endif
