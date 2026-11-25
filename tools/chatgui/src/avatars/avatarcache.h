/* Дисковый кеш аватарок: исходник, WebP-превью и JSON-манифест с именами SHA-256.<расширение>.
 * Синхронный файловый/декодирующий API; AvatarManager вызывает его последовательно в worker.
 * Каталог должен существовать до prepare/storeObject/publish; создать его можно через initialize. */
#pragma once
#include <QImage>
#include <QString>
#include <QStringList>
#include <QRectF>

namespace AvatarCache {
constexpr int MaxBytes = 1024 * 1024;              /* Предел превью: 1 MiB. */
constexpr int OriginalMaxBytes = 32 * 1024 * 1024; /* Предел исходника: 32 MiB. */
constexpr qint64 DiskLimit = 100 * 1024 * 1024;    /* Целевой объём каталога при prune, без гарантии для pinned. */
struct Record {
    QString original, preview;                    /* Имена объектов с расширениями, без пути. */
    QSize size;                                   /* Размер исходного изображения после EXIF-поворота. */
    QRectF crop;                                  /* Выбранный квадрат в нормализованных координатах size. */
    qint64 originalBytes = 0, previewBytes = 0;     /* Заявленные размеры объектов в байтах. */
};
struct Result {
    QImage image;                                 /* Декодированные пиксели; для одного манифеста остаётся пустым. */
    QString hash, error;                          /* SHA-256 объекта без суффикса; пустой error означает успех. */
    Record record;                                /* Заполняется при подготовке/чтении манифеста. */
};
/* Создаёт directory, удаляет остатки *.part* и *.download*, вызывает prune; false при ошибке создания каталога. */
bool initialize(const QString& directory, const QStringList& pinned = {});
/* path — файл манифеста, hash — ожидаемый SHA-256 без суффикса. Проверяет его и соседнее превью 256x256 с размером из JSON. */
Result load(const QString& path, const QString& hash);
/* Читает JPEG/PNG/WebP до 16*1024*1024 пикселей, сохраняет исходные байты, превью 256x256 и манифест.
 * crop — квадрат после EXIF в координатах [0,1]; невалидный QRectF выбирает центральный квадрат.
 * Частичная ошибка не откатывает уже записанные объекты; результат проверяется по error. */
Result prepare(const QString& source, const QString& directory, const QRectF& crop = {});
/* Проверяет temporary как объект с именем hash (здесь включая расширение), атомарно копирует в directory.
 * Пытается удалить temporary и при ошибке проверки/копирования; исходный файл не должен быть файлом назначения. */
Result publish(const QString& temporary, const QString& directory, const QString& hash);
/* path — полный путь, file — ожидаемое имя SHA-256.<json|jpg|png|webp>. Проверяет размер/хеш и содержимое.
 * Изображение декодируется с EXIF и уменьшением до 2048 по длинной стороне; успех обновляет mtime файла. */
Result readObject(const QString& path, const QString& file);
/* Атомарная запись bytes под именем file через QSaveFile, без проверки имени/хеша/формата.
 * true при успехе; false записывает причину в error (при успехе прежний error не очищается). */
bool storeObject(const QString& directory, const QString& file, const QByteArray& bytes, QString& error);
/* Удаляет старейшие по mtime допустимые объекты до DiskLimit. pinned — хеши манифестов без .json;
 * сами манифесты и объекты из валидных pinned-манифестов сохраняются. Посторонние файлы учитываются, но не удаляются. */
void prune(const QString& directory, const QStringList& pinned);
}
