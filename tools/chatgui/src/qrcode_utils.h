/* QR для произвольного текста и чтение штрихкодов из QImage через ZXingQt.
 * Результаты принадлежат caller; модуль не проверяет utun-ссылки и ничего не открывает. */
#pragma once

#include <QImage>
#include <QString>
#include <QStringList>

// Сгенерировать QR: moduleSize>0 задаёт scale ZXing, иначе остаётся значение библиотеки; при ошибке пустой QImage.
QImage generateQRCode(const QString& text, int moduleSize = 8);

// Тексты всех valid результатов ReadBarcodes с настройками по умолчанию, не только QR; без результатов пустой список.
QStringList readQRCodes(const QImage& image);
