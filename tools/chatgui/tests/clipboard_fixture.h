/* Изображение с градиентом и воспроизводимым шумом для сравнения PNG/JPEG в GUI и настоящем ядре. */
#pragma once
#include <QImage>

// density: 0..1024, больше — хуже сжатие PNG; alpha добавляет прозрачность и полупрозрачность.
inline QImage clipboardCodecFixture(int density, bool alpha = false) {
    QImage image(320, 240, alpha ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    unsigned state = 1;
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
        state = state * 1664525u + 1013904223u;
        const int noise = int((state >> 16) & 1023) < density ? int(state >> 31) : 0;
        image.setPixel(x, y, qRgba((x / 2 + noise) & 255, (y / 2 + noise) & 255, ((x + y) / 4 + noise) & 255,
                                  alpha ? (x < 16 && y < 16 ? 0 : 127) : 255));
    }
    return image;
}
