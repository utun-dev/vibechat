/* file_io — синхронные файловые операции: UTF-8 пути, идентичность и публикация через соседний temp.
 * inspect различает найденный/отсутствующий файл и ошибку; paths сравнивает идентичность файлов.
 * Temp обнулить → open → писать в file → finish → publish; discard обязателен также при ошибке.
 * Активный temp автоматически защищён от remove_orphan во всех потоках до publish/discard, включая время после finish.
 * Пока temp активен, структуру нельзя копировать/перемещать; с одним temp работает один владелец.
 * Исходник и каталоги должны оставаться стабильными на время операции. Это не защита от подмены родительских каталогов.
 * FILE_IO_DURABLE сохраняет данные при finish и имя при publish. POSIX temp имеет права 0600.
 * После успешного rename temp отсоединён даже при ошибке синхронизации каталога: отката публикации нет. */
#ifndef FILE_IO_H
#define FILE_IO_H
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#define FILE_IO_PATH_MAX 4096
#define FILE_IO_DURABLE 1u
enum file_io_type { FILE_IO_REGULAR = 1, FILE_IO_DIRECTORY, FILE_IO_LINK, FILE_IO_OTHER };
struct file_io_info {
    int type;                    // обычный файл, каталог, ссылка/reparse point или другой объект
    int64_t size;                // размер в байтах, без сужения до int
    uint64_t identity[3];        // идентификатор файла внутри файловой системы, сравнивать весь массив
};
struct file_io_temp {
    char temporary[FILE_IO_PATH_MAX]; // только собственный temp; пусто после discard/publish
    FILE *file;                      // открытый wb+; NULL после finish
    struct file_io_temp *next_owned;  // внутренний реестр живых temp; изменяет только file_io под блокировкой
    unsigned flags;                  // режим, заданный open: 0 либо FILE_IO_DURABLE на весь срок операции
};

// path — UTF-8; follow!=0 разрешает конечную ссылку. info обязателен, заполняется только при успехе.
int file_io_inspect(const char *path, int follow, struct file_io_info *info);
#ifdef _WIN32
// Преобразовать непустой UTF-8 путь в wide[FILE_IO_PATH_MAX]; 0/-1.
int file_io_wide(const char *path, wchar_t wide[FILE_IO_PATH_MAX]);
#endif
// Открыть обычный файл для чтения; путь UTF-8. NULL при ошибке, fclose принадлежит вызывающему.
FILE *file_io_open_read(const char *path);
// Открыть существующий regular-файл для чтения/записи без усечения; конечные ссылки запрещены. UTF-8, NULL при ошибке.
FILE *file_io_open_update(const char *path);
// Источник должен быть regular; назначение отсутствует или regular без конечной ссылки. 1 — один файл, 0 — разные, -1 — ошибка.
int file_io_paths(const char *source, const char *destination);
// Сохранить существующий закрытый regular-файл без конечной ссылки; UTF-8, 0/-1.
int file_io_sync_file(const char *path);
// Сохранить каталог, содержащий path; без разделителя используется '.', для '/file' — '/'. UTF-8, 0/-1.
int file_io_sync_directory(const char *path);
// Переместить отдельный готовый файл с durable заменой destination; caller владеет source. 0/-1.
// Ошибка после rename не откатывает замену; сохраняется каталог назначения. Файлы/каталоги не подменяются извне.
int file_io_publish(const char *source, const char *destination);
// Создать эксклюзивный temp рядом с destination; структура заранее обнулена, даже после ошибки вызвать discard.
// flags=0 — flush/close и atomic rename; FILE_IO_DURABLE — также sync файла и каталога назначения.
int file_io_temp_open(struct file_io_temp *output, const char *destination, unsigned flags);
// Проверить flush/[sync]/close, обнулить file при любом результате; temp остаётся собственностью output.
int file_io_temp_finish(struct file_io_temp *output);
// Закрыть/удалить собственный temp; повторный вызов допустим. Ошибки очистки логируются.
void file_io_temp_discard(struct file_io_temp *output);
// Повторно проверить paths и заменить destination закрытым соседним temp; одинаковые файлы отклоняются. 0/-1.
// При ошибке до rename temporary сохраняется; после rename он пуст, в том числе при отказе directory sync.
int file_io_temp_publish(struct file_io_temp *output, const char *source, const char *destination);
/* Удалить уже признанный сиротой файл, атомарно проверив владение file_io_temp во всех потоках.
 * 1 — удалён, 0 — активный temp или уже отсутствует, -1 — ошибка (логируется).
 * path должен иметь то же нормализованное написание, что и путь при temp_open; рабочий каталог не меняется. */
int file_io_remove_orphan(const char *path);
#endif
