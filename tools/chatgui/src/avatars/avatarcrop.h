/* Модальный выбор квадрата аватара в GUI-потоке: неподвижная круглая рамка, фото перемещается ЛКМ.
 * Вертикальное колесо (angle/pixel delta) меняет zoom в 1.15 раза, диапазон 1..min(16, меньшая сторона оригинала).
 * JPEG/PNG/WebP читается в локальном worker: файл <=32 MiB, <=16 MP; preview <=1600px, без увеличения исходника.
 * Функция ждёт worker перед возвратом; уничтожение parent безопасно отменяет выбор, queued preview не обращается к удалённому окну. */
#pragma once
#include <QRectF>
#include <QString>
class QWidget;
/* source — локальный файл; parent — необязательный заимствованный владелец окна.
 * true: crop заменён нормализованным QRectF в [0,1] после EXIF, квадратным в пикселях оригинала (не меньше1px).
 * false: Cancel/уничтожение parent/ошибка загрузки или выделения, crop не меняется.
 * Только выбирает координаты: не создаёт кеш/файлы и не публикует аватар; передать crop в AvatarCache::prepare. */
bool selectAvatarCrop(const QString& source, QRectF& crop, QWidget* parent);
