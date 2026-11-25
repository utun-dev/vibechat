/* Локальные превью вложений: первый кадр видео и уменьшенные JPEG/PNG.
 * Cache::load/prune выполняют I/O и декодирование: вызывать только в worker. Один Cache безопасен для двух workers.
 * Запись <SHA-256>.preview: UTNPREV1 (8 байт), время создания UTC мс (int64 little endian), затем JPEG/PNG.
 * Чтение не продлевает срок. Очистка удаляет только собственные записи старше семи суток, оригиналы не меняются.
 * RAM ограничена 32 MiB; публикация через QSaveFile, очистка и чтение/запись сериализованы mutex. */
#pragma once
#include <QCache>
#include <QImage>
#include <QMutex>
#include <QString>

namespace MediaPreviewCache {
constexpr int MaxSide = 600;
constexpr qint64 LifetimeMs = 7LL * 24 * 60 * 60 * 1000;
enum class Kind { Image, Video };
enum class Origin { Generated, Memory, Disk };
struct Result {
    QImage image; // Готовые пиксели <=600 по длинной стороне; пусто при ошибке исходника.
    QString file, error; // Путь записи кеша и причина ошибки; ошибка записи кеша не мешает вернуть image.
    qint64 created = 0; // Неизменное время создания, UTC мс.
    Origin origin = Origin::Generated; // Откуда получен результат; полезно для диагностики и проверок.
    bool retryable = false; // Исходник изменился во время подготовки: можно повторить с новым fingerprint.
};
struct Pruned {
    int removed = 0, errors = 0; // Удалённые записи и ошибки чтения/удаления.
    qint64 bytes = 0; // Освобождённые байты на диске.
};
class Cache {
public:
    explicit Cache(const QString& directory); // Только сохраняет directory; каталог создаётся в worker.
    // Проверяет path/size/mtime/kind; читает RAM/диск либо создаёт превью. now=0 — текущее UTC; другое значение удобно для теста TTL.
    Result load(const QString& path, Kind kind, qint64 now = 0);
    Pruned prune(qint64 now = 0); // Чистит записи строго старше LifetimeMs, включая просроченные элементы RAM.
private:
    struct Entry { QImage image; qint64 created; }; // Shared QImage и дата создания, не дата обращения.
    QString m_directory; // Отдельный каталог профиля; файлы вне него не затрагиваются.
    QMutex m_mutex; // Защищает RAM, публикацию и удаление; декодирование исходников идёт вне lock.
    QCache<QString, Entry> m_memory{32 * 1024 * 1024}; // Ключ — fingerprint исходника, стоимость — размер decoded pixels.
};
}
